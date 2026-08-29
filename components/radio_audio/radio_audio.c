#include "radio_audio.h"
#include "radio_board.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include <stdlib.h>
#include <string.h>
typedef struct { radio_station_t station; } audio_message_t;
//-- Raw, ICY-stripped audio bytes cross from fetch_task to stream_task through
//-- this PSRAM ring buffer, so a network/HTTP stall never stalls the I2S feed
//-- directly (~4s of buffering at 128kbps)
#define STREAM_BUF_CAPACITY (64*1024)
#define FETCH_CHUNK_SIZE 4096
static i2s_chan_handle_t s_tx; static TaskHandle_t s_stream_task; static TaskHandle_t s_fetch_task; static QueueHandle_t s_queue; static radio_station_t s_station; static volatile int s_volume=60; static volatile bool s_stream_running; static volatile bool s_fetch_running; static volatile bool s_stop_requested;
static StreamBufferHandle_t s_sb; static StaticStreamBuffer_t s_sb_struct; static uint8_t *s_sb_storage;
//-- Set as soon as a switch is requested so a stale tail of the old station
//-- never bleeds into the new one; cleared only by the new stream once it has
//-- decoded a couple of good frames (see stream_task)
static volatile bool s_muted = true;
static radio_audio_title_cb_t s_title_cb; static void *s_title_ctx;
static void apply_volume(int16_t *pcm,size_t samples){int v=s_volume;for(size_t i=0;i<samples;i++)pcm[i]=(int16_t)(((int32_t)pcm[i]*v)/100);}

//-- Blocks until exactly `len` bytes are read (a single esp_http_client_read()
//-- call may return short of a full ICY metadata block)
static int http_read_exact(esp_http_client_handle_t h, uint8_t *buf, size_t len)
{
	size_t got = 0;
	while (got < len) {
		int n = esp_http_client_read(h, (char *)buf + got, len - got);
		if (n <= 0) return -1;
		got += (size_t)n;
	}
	return 0;
}

static void parse_icy_title(char *meta, char *out, size_t out_cap)
{
	char *start = strstr(meta, "StreamTitle='");
	if (!start) return;
	start += strlen("StreamTitle='");
	char *end = strstr(start, "';");
	if (!end) return;
	size_t title_len = (size_t)(end - start);
	if (title_len >= out_cap) title_len = out_cap - 1;
	memcpy(out, start, title_len);
	out[title_len] = '\0';
}

//-- Owns the HTTP/ICY connection and does nothing else: reads network bytes,
//-- strips interleaved ICY metadata, and pushes pure audio bytes into the
//-- stream buffer. Runs on its own core so a slow/blocking read never delays
//-- stream_task's decode+I2S loop.
static void fetch_task(void *arg)
{
	(void)arg;
	radio_station_t station = s_station;
	if (s_title_cb) s_title_cb("", s_title_ctx);

	esp_http_client_config_t hc = {.url = station.url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 10000, .buffer_size = 4096, .keep_alive_enable = true};
	esp_http_client_handle_t h = esp_http_client_init(&hc);
	if (!h) goto done;
	esp_http_client_set_header(h, "Icy-Metadata", "1");
	if (esp_http_client_open(h, 0) != ESP_OK) goto done;
	esp_http_client_fetch_headers(h);

	//-- ICY metadata: server interleaves a length-prefixed "StreamTitle='...';"
	//-- block every icy-metaint bytes of audio when we asked for it above
	size_t meta_int = 0;
	char *meta_hdr = NULL;
	if (esp_http_client_get_header(h, "icy-metaint", &meta_hdr) == ESP_OK && meta_hdr) meta_int = (size_t)atoi(meta_hdr);
	uint8_t *meta_buf = meta_int ? malloc(255 * 16 + 1) : NULL;
	if (meta_int && !meta_buf) meta_int = 0;
	size_t meta_remain = meta_int;

	uint8_t *chunk = malloc(FETCH_CHUNK_SIZE);
	if (!chunk) { free(meta_buf); goto done; }

	while (!s_stop_requested) {
		if (meta_int && meta_remain == 0) {
			uint8_t len_byte;
			if (http_read_exact(h, &len_byte, 1) != 0) break;
			size_t meta_len = (size_t)len_byte * 16;
			if (meta_len) {
				if (http_read_exact(h, meta_buf, meta_len) != 0) break;
				meta_buf[meta_len] = '\0';
				char title[64] = {0};
				parse_icy_title((char *)meta_buf, title, sizeof(title));
				if (title[0] && s_title_cb) s_title_cb(title, s_title_ctx);
			}
			meta_remain = meta_int;
		}

		size_t want = FETCH_CHUNK_SIZE;
		if (meta_int && meta_remain < want) want = meta_remain;
		if (want == 0) continue;
		int n = esp_http_client_read(h, (char *)chunk, want);
		if (n <= 0) break;
		if (meta_int) meta_remain -= (size_t)n;

		//-- Bounded-timeout send so we keep noticing s_stop_requested even
		//-- while the (4s-deep) buffer is full
		size_t sent = 0;
		while (sent < (size_t)n && !s_stop_requested) sent += xStreamBufferSend(s_sb, chunk + sent, (size_t)n - sent, pdMS_TO_TICKS(100));
	}
	free(chunk);
	free(meta_buf);
done:
	if (h) {
		esp_http_client_close(h);
		esp_http_client_cleanup(h);
	}
	s_fetch_running = false;
	s_fetch_task = NULL;
	vTaskDelete(NULL);
}

//-- Decode+I2S only: pulls ICY-stripped bytes from the stream buffer instead
//-- of reading the network directly, so a network hiccup drains the buffer's
//-- slack instead of stalling the I2S feed
static void stream_task(void *arg)
{
	(void)arg;
	radio_station_t station = s_station;

	esp_audio_simple_dec_cfg_t dc = {.dec_type = station.codec == RADIO_CODEC_AAC ? ESP_AUDIO_SIMPLE_DEC_TYPE_AAC : ESP_AUDIO_SIMPLE_DEC_TYPE_MP3, .use_frame_dec = false};
	esp_audio_simple_dec_handle_t dec = NULL;
	if (esp_audio_simple_dec_open(&dc, &dec) != ESP_AUDIO_ERR_OK) goto done;

	//-- in_cap has headroom beyond one buffer receive so a codec frame split
	//-- across two receives can be carried over instead of being silently
	//-- dropped (was the cause of the periodic audio glitches: any unconsumed
	//-- tail of `in` used to be discarded when the next read overwrote it from
	//-- offset 0)
	const size_t in_cap = 16384;
	uint8_t *in = malloc(in_cap), *out = malloc(16384);
	uint32_t out_size = 16384;
	if (!in || !out) goto decode_done;
	size_t in_len = 0;
	bool configured = false;
	bool warmed_up = false;
	int good_frames = 0;

	while (!s_stop_requested) {
		size_t want = in_cap - in_len;
		size_t n = xStreamBufferReceive(s_sb, in + in_len, want, pdMS_TO_TICKS(50));
		if (n == 0) {
			if (s_stop_requested) break;
			//-- Upstream fetch_task ended (or never started) and nothing is
			//-- left buffered: nothing more will ever arrive
			if (!s_fetch_running && xStreamBufferIsEmpty(s_sb)) break;
			continue;
		}
		in_len += n;

		esp_audio_simple_dec_raw_t raw = {.buffer = in, .len = (uint32_t)in_len};
		do {
			esp_audio_simple_dec_out_t frame = {.buffer = out, .len = out_size};
			esp_audio_err_t e = esp_audio_simple_dec_process(dec, &raw, &frame);
			if (e == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
				uint8_t *b = realloc(out, frame.needed_size);
				if (!b) break;
				out = b;
				out_size = frame.needed_size;
				continue;
			}
			if (e != ESP_AUDIO_ERR_OK) break;
			if (frame.decoded_size) {
				esp_audio_simple_dec_info_t info;
				if (!configured && esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK) {
					i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(info.sample_rate);
					ESP_ERROR_CHECK(i2s_channel_disable(s_tx));
					ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(s_tx, &clk));
					ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
					configured = true;
				}
				apply_volume((int16_t *)out, frame.decoded_size / 2);

				//-- The byte stream is entered mid-MP3-frame, so the decoder's
				//-- first output can be a burst of noise before it locks onto
				//-- frame sync; stay muted for a couple of frames to skip that.
				if (s_muted && !warmed_up && ++good_frames >= 2) {
					warmed_up = true;
					s_muted = false;
				}
				if (!s_muted) {
					size_t written;
					i2s_channel_write(s_tx, out, frame.decoded_size, &written, portMAX_DELAY);
				}
			}
			if (raw.consumed == 0) break;
			raw.buffer += raw.consumed;
			raw.len -= raw.consumed;
		} while (raw.len && !s_stop_requested);

		if (raw.len && raw.buffer != in) memmove(in, raw.buffer, raw.len);
		in_len = raw.len;
		if (in_len >= in_cap) in_len = 0;
	}
	free(in);
	free(out);
decode_done:
	esp_audio_simple_dec_close(dec);
done:
	s_stream_running = false;
	s_stream_task = NULL;
	vTaskDelete(NULL);
}

static void audio_task(void *arg)
{
	audio_message_t message;
	(void)arg;
	for (;;) {
		if (xQueueReceive(s_queue, &message, portMAX_DELAY) != pdTRUE) continue;
		if (s_stream_running || s_fetch_running) {
			s_stop_requested = true;
			for (int wait = 0; wait < 100 && (s_stream_running || s_fetch_running); wait++) vTaskDelay(pdMS_TO_TICKS(10));
			xStreamBufferReset(s_sb);
		}
		s_station = message.station;
		s_stop_requested = false;
		//-- decode+I2S pinned to core 1, network fetch pinned to core 0
		//-- (alongside the WiFi driver task) so neither competes with the
		//-- other's scheduling
		s_stream_running = true;
		if (xTaskCreatePinnedToCore(stream_task, "radio_stream", 12288, NULL, 7, &s_stream_task, 1) != pdPASS) s_stream_running = false;
		s_fetch_running = true;
		if (xTaskCreatePinnedToCore(fetch_task, "radio_fetch", 6144, NULL, 6, &s_fetch_task, 0) != pdPASS) s_fetch_running = false;
	}
}

esp_err_t radio_audio_init(void)
{
	if (RADIO_I2S_ENABLE >= 0) {
		gpio_set_direction(RADIO_I2S_ENABLE, GPIO_MODE_OUTPUT);
		gpio_set_level(RADIO_I2S_ENABLE, 1);
	}
	esp_audio_dec_register_default();
	i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
	//-- Default DMA sizing (6*240 frames, ~32ms) is too small: any scheduling
	//-- gap over that underruns the DMA, and with auto_clear off the DMA
	//-- repeats the last stale buffer (audible hum) instead of silence
	cc.dma_desc_num = 8;
	cc.dma_frame_num = 960;
	cc.auto_clear = true;
	ESP_ERROR_CHECK(i2s_new_channel(&cc, &s_tx, NULL));
	i2s_std_config_t sc = {.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100), .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO), .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = RADIO_I2S_BCLK, .ws = RADIO_I2S_WS, .dout = RADIO_I2S_DOUT, .din = I2S_GPIO_UNUSED, .invert_flags = {0}}};
	ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &sc));
	ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
	//-- Backing storage for the network->decoder ring buffer lives in PSRAM;
	//-- it's several seconds of compressed audio, not worth internal SRAM
	s_sb_storage = heap_caps_malloc(STREAM_BUF_CAPACITY + 1, MALLOC_CAP_SPIRAM);
	if (!s_sb_storage) return ESP_ERR_NO_MEM;
	s_sb = xStreamBufferCreateStatic(STREAM_BUF_CAPACITY + 1, 1, s_sb_storage, &s_sb_struct);
	if (!s_sb) return ESP_ERR_NO_MEM;
	s_queue = xQueueCreate(4, sizeof(audio_message_t));
	if (!s_queue) return ESP_ERR_NO_MEM;
	return xTaskCreate(audio_task, "radio_audio", 4096, NULL, 6, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
esp_err_t radio_audio_play(const radio_station_t*s){if(!s||!s_queue)return ESP_ERR_INVALID_ARG;s_muted=true;audio_message_t message={.station=*s};return xQueueSend(s_queue,&message,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;}
void radio_audio_set_volume(int p){s_volume=p<0?0:(p>100?100:p);}
void radio_audio_set_title_callback(radio_audio_title_cb_t cb,void *ctx){s_title_cb=cb;s_title_ctx=ctx;}
