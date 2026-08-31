#include "radio_audio.h"
#include "radio_board.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_timer.h"
#include <arpa/inet.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef struct { radio_station_t station; } audio_message_t;
static const char *TAG = "RADIO";
static const char *TAG_HTTP = "RADIO_HTTP";
static const char *TAG_TLS = "RADIO_TLS";
static const char *TAG_STREAM = "RADIO_STREAM";
static const char *TAG_CODEC = "RADIO_CODEC";
static const char *TAG_BUFFER = "RADIO_BUFFER";
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
//-- When paused the decode loop simply stops consuming from the ring buffer;
//-- fetch_task then blocks on a full buffer instead of tearing down the connection
static volatile bool s_paused = false;
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

static void log_station_banner(const radio_station_t *station)
{
	ESP_LOGI(TAG, "============================================================");
	ESP_LOGI(TAG, "Opening radio station");
	ESP_LOGI(TAG, "Name: %s", station->name);
	ESP_LOGI(TAG, "URL : %s", station->url);
	ESP_LOGI(TAG, "Free heap before open: %d bytes", esp_get_free_heap_size());
	ESP_LOGI(TAG, "Minimum free heap so far: %d bytes", heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
	ESP_LOGI(TAG, "============================================================");
}

static void log_url_details(const char *url)
{
	char scheme[16] = {0};
	char host[128] = {0};
	char path[256] = {0};
	char port[16] = {0};
	const char *scheme_sep = strstr(url, "://");
	const char *path_start = NULL;

	if (scheme_sep) {
		size_t scheme_len = (size_t)(scheme_sep - url);
		if (scheme_len < sizeof(scheme)) {
			memcpy(scheme, url, scheme_len);
			scheme[scheme_len] = '\0';
		}
		const char *host_start = scheme_sep + 3;
		const char *port_sep = strchr(host_start, ':');
		const char *slash = strchr(host_start, '/');
		if (slash) {
			path_start = slash;
			if (slash - host_start < (ptrdiff_t)sizeof(host)) {
				memcpy(host, host_start, (size_t)(slash - host_start));
				host[slash - host_start] = '\0';
			}
			if (port_sep && port_sep > host_start && port_sep < slash) {
				size_t port_len = (size_t)(slash - port_sep - 1);
				if (port_len < sizeof(port)) {
					memcpy(port, port_sep + 1, port_len);
					port[port_len] = '\0';
				}
			}
			if (path_start) {
				size_t path_len = strlen(path_start);
				if (path_len < sizeof(path)) {
					memcpy(path, path_start, path_len);
					path[path_len] = '\0';
				}
			}
		} else {
			if (port_sep) {
				size_t host_len = (size_t)(port_sep - host_start);
				if (host_len < sizeof(host)) {
					memcpy(host, host_start, host_len);
					host[host_len] = '\0';
				}
				size_t port_len = strlen(port_sep + 1);
				if (port_len < sizeof(port)) {
					memcpy(port, port_sep + 1, port_len);
					port[port_len] = '\0';
				}
			} else {
				strlcpy(host, host_start, sizeof(host));
			}
		}
	}
	if (scheme[0]) ESP_LOGI(TAG_HTTP, "Scheme   : %s", scheme);
	if (host[0]) ESP_LOGI(TAG_HTTP, "Host     : %s", host);
	if (port[0]) ESP_LOGI(TAG_HTTP, "Port     : %s", port);
	else if (strcmp(scheme, "https") == 0) ESP_LOGI(TAG_HTTP, "Port     : 443");
	else if (strcmp(scheme, "http") == 0) ESP_LOGI(TAG_HTTP, "Port     : 80");
	if (path[0]) ESP_LOGI(TAG_HTTP, "Path     : %s", path);
	if (strstr(url, ".m3u") || strstr(url, ".pls") || strstr(url, ".m3u8") || strstr(url, ".xspf") || strstr(url, ".asx") || strstr(url, "playlist")) {
		ESP_LOGW(TAG_HTTP, "Suspicious playlist-like URL detected: %s", url);
		ESP_LOGW(TAG_HTTP, "This may resolve to a playlist instead of a direct audio stream");
	}
}

static void log_dns_resolution(const char *host)
{
	static const int families[] = {AF_INET, AF_INET6};
	for (size_t i = 0; i < sizeof(families) / sizeof(families[0]); ++i) {
		struct addrinfo hints;
		struct addrinfo *result = NULL;
		int rc;
		char ipbuf[INET6_ADDRSTRLEN];
		int count = 0;
		const char *family_name = families[i] == AF_INET ? "IPv4" : "IPv6";
		memset(&hints, 0, sizeof(hints));
		hints.ai_family = families[i];
		hints.ai_socktype = SOCK_STREAM;
		ESP_LOGI(TAG_HTTP, "Resolving host %s (%s)", host, family_name);
		rc = getaddrinfo(host, NULL, &hints, &result);
		if (rc != 0 || result == NULL) {
			ESP_LOGW(TAG_HTTP, "%s lookup failed for %s", family_name, host);
			continue;
		}
		for (struct addrinfo *ai = result; ai != NULL; ai = ai->ai_next) {
			void *addr = NULL;
			if (ai->ai_family == AF_INET) {
				addr = &((struct sockaddr_in *)ai->ai_addr)->sin_addr;
			} else if (ai->ai_family == AF_INET6) {
				addr = &((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;
			}
			if (!addr) continue;
			if (inet_ntop(ai->ai_family, addr, ipbuf, sizeof(ipbuf)) == NULL) continue;
			ESP_LOGI(TAG_HTTP, "DNS result (%s): %s", family_name, ipbuf);
			count++;
		}
		if (count == 0) {
			ESP_LOGW(TAG_HTTP, "No usable %s addresses returned for %s", family_name, host);
		}
		freeaddrinfo(result);
	}
}

static void log_http_response(esp_http_client_handle_t h)
{
	char *content_type = NULL;
	char *location = NULL;
	char *transfer_encoding = NULL;
	char *content_length = NULL;
	char *icy_metaint = NULL;
	int status = esp_http_client_get_status_code(h);
	ESP_LOGI(TAG_HTTP, "HTTP status: %d", status);
	if (esp_http_client_get_response_header(h, "Content-Type", &content_type) == ESP_OK && content_type) {
		ESP_LOGI(TAG_HTTP, "Content-Type: %s", content_type);
	}
	if (esp_http_client_get_response_header(h, "Location", &location) == ESP_OK && location) {
		ESP_LOGW(TAG_HTTP, "Redirect returned: %s", location);
	}
	if (esp_http_client_get_response_header(h, "Transfer-Encoding", &transfer_encoding) == ESP_OK && transfer_encoding) {
		ESP_LOGI(TAG_HTTP, "Transfer-Encoding: %s", transfer_encoding);
	} else {
		ESP_LOGI(TAG_HTTP, "Transfer-Encoding: none");
	}
	if (esp_http_client_get_response_header(h, "Content-Length", &content_length) == ESP_OK && content_length) {
		ESP_LOGI(TAG_HTTP, "Content-Length: %s", content_length);
	} else {
		ESP_LOGI(TAG_HTTP, "Content-Length: not supplied");
	}
	if (esp_http_client_get_response_header(h, "icy-metaint", &icy_metaint) == ESP_OK && icy_metaint) {
		ESP_LOGI(TAG_HTTP, "icy-metaint: %s", icy_metaint);
	} else {
		ESP_LOGI(TAG_HTTP, "icy-metaint: not supplied");
	}
	if (status >= 300 && status < 400) {
		ESP_LOGW(TAG_HTTP, "HTTP redirect detected; follow-up URL must be inspected");
	}
}

static const char *detect_payload_type(const uint8_t *buf, size_t len, const char *content_type)
{
	if (!buf || len == 0) return "unknown";
	if (content_type && (strstr(content_type, "text/html") || strstr(content_type, "application/json") || strstr(content_type, "application/xml") || strstr(content_type, "text/xml"))) {
		return "web-or-data-page";
	}
	if (content_type && (strstr(content_type, "mpegurl") || strstr(content_type, "mpd") || strstr(content_type, "dash"))) {
		return "HLS/DASH playlist";
	}
	if (len >= 7 && memcmp(buf, "#EXTM3U", 7) == 0) return "HLS/M3U";
	if (len >= 9 && memcmp(buf, "[playlist]", 9) == 0) return "PLS";
	if (len >= 5 && memcmp(buf, "<!DOC", 5) == 0) return "HTML";
	if (len >= 5 && (memcmp(buf, "<html", 5) == 0 || memcmp(buf, "<?xml", 5) == 0)) return "HTML/XML";
	if (len >= 4 && memcmp(buf, "OggS", 4) == 0) return "Ogg";
	if (len >= 4 && memcmp(buf, "fLaC", 4) == 0) return "FLAC";
	if (len >= 3 && (buf[0] == 0xFF && (buf[1] == 0xFB || buf[1] == 0xF3 || buf[1] == 0xF2))) return "MP3";
	if (len >= 2 && (buf[0] == 0xFF && (buf[1] == 0xF1 || buf[1] == 0xF9))) return "AAC";
	if (len >= 11 && strncmp((const char *)buf, "StreamTitle", 11) == 0) return "ICY metadata";
	return "unknown";
}

static void log_payload_probe(const uint8_t *buf, size_t len, const char *content_type)
{
	if (!buf || len == 0) {
		ESP_LOGW(TAG_STREAM, "No initial payload bytes available for inspection");
		return;
	}
	const size_t show = len < 32 ? len : 32;
	const char *kind = detect_payload_type(buf, len, content_type);
	ESP_LOGI(TAG_STREAM, "First %zu payload bytes:", show);
	for (size_t i = 0; i < show; ++i) {
		ESP_LOGI(TAG_STREAM, "%02X", buf[i]);
	}
	if (content_type && (strstr(content_type, "mpegurl") || strstr(content_type, "dash") || strstr(content_type, "mpd") || strstr(content_type, "m3u"))) {
		ESP_LOGW(TAG_STREAM, "Stream is a playlist or HLS/DASH manifest");
		ESP_LOGE(TAG_STREAM, "Current radio pipeline does not support direct HLS/DASH playlists");
	}
	if (strstr(kind, "HLS") || strstr(kind, "DASH") || strstr(kind, "PLS") || strstr(kind, "M3U")) {
		ESP_LOGW(TAG_STREAM, "Playlist-like payload detected: %s", kind);
	}
	if (content_type && strncmp(content_type, "text/html", 9) == 0) {
		ESP_LOGE(TAG_STREAM, "URL returned HTML instead of an audio stream");
	}
	ESP_LOGI(TAG_STREAM, "Payload signature: %s", kind);
}

static void log_stream_summary(const char *station_name, const char *final_url, const char *reason, const char *content_type, bool success, int http_code)
{
	ESP_LOGI(TAG, "============================================================");
	if (success) {
		ESP_LOGI(TAG, "Station started successfully");
	} else {
		ESP_LOGE(TAG, "Station open FAILED");
	}
	ESP_LOGI(TAG, "Name: %s", station_name ? station_name : "unknown");
	ESP_LOGI(TAG, "Final URL: %s", final_url ? final_url : "unknown");
	if (content_type) ESP_LOGI(TAG, "Content-Type: %s", content_type);
	ESP_LOGI(TAG, "HTTP status: %d", http_code);
	if (reason) ESP_LOGI(TAG, "Reason: %s", reason);
	ESP_LOGI(TAG, "============================================================");
}

static void log_retry_event(int attempt, int max_attempts, const char *url, const char *reason)
{
	ESP_LOGW(TAG_HTTP, "Retry %d/%d after 2000 ms for %s", attempt, max_attempts, url);
	if (reason) ESP_LOGW(TAG_HTTP, "Retry reason: %s", reason);
}

static void log_teardown_summary(const radio_station_t *station, const char *reason, size_t bytes_received, int http_code, uint64_t elapsed_us)
{
	ESP_LOGW(TAG, "Stream ended");
	ESP_LOGW(TAG, "Station       : %s", station ? station->name : "unknown");
	ESP_LOGW(TAG, "Final URL     : %s", station ? station->url : "unknown");
	ESP_LOGW(TAG, "Bytes received: %zu", bytes_received);
	ESP_LOGW(TAG, "Uptime        : %llu ms", (unsigned long long)(elapsed_us / 1000ULL));
	ESP_LOGW(TAG, "Last HTTP code: %d", http_code);
	if (reason) ESP_LOGW(TAG, "Last error    : %s", reason);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
	if (!evt) return ESP_ERR_INVALID_ARG;
	switch (evt->event_id) {
	case HTTP_EVENT_ON_CONNECTED:
		ESP_LOGI(TAG_HTTP, "HTTP connection established");
		break;
	case HTTP_EVENT_ON_STATUS_CODE:
		ESP_LOGI(TAG_HTTP, "HTTP status: %d", esp_http_client_get_status_code(evt->client));
		break;
	case HTTP_EVENT_REDIRECT:
		if (evt->client) {
			char *loc = NULL;
			ESP_LOGW(TAG_HTTP, "Redirect detected");
			if (esp_http_client_get_response_header(evt->client, "Location", &loc) == ESP_OK && loc) {
				ESP_LOGW(TAG_HTTP, "Location: %s", loc);
			}
		}
		break;
	case HTTP_EVENT_HEADER_SENT:
		ESP_LOGD(TAG_HTTP, "HTTP headers sent");
		break;
	case HTTP_EVENT_ON_HEADER:
		if (evt->header_key && evt->header_value) {
			ESP_LOGI(TAG_HTTP, "Header: %s = %s", evt->header_key, evt->header_value);
		}
		break;
	case HTTP_EVENT_ON_DATA:
		if (evt->data && evt->data_len > 0) {
			ESP_LOGD(TAG_HTTP, "Received %d bytes of payload data", evt->data_len);
		}
		break;
	case HTTP_EVENT_ERROR:
		ESP_LOGE(TAG_HTTP, "HTTP event error");
		break;
	case HTTP_EVENT_DISCONNECTED:
		ESP_LOGW(TAG_HTTP, "HTTP disconnect event received");
		break;
	case HTTP_EVENT_ON_FINISH:
		ESP_LOGI(TAG_HTTP, "HTTP transfer finished");
		break;
	default:
		break;
	}
	return ESP_OK;
}

//-- Builds an esp_http_client handle for `url`, with TLS certificate
//-- verification either enabled (via the certificate bundle) or disabled.
//-- Used to re-create the handle with a different verification mode when
//-- the verified attempts in fetch_task are exhausted.
static esp_http_client_handle_t create_http_client(const char *url, bool verify_tls)
{
	esp_http_client_config_t hc = {.url = url, .crt_bundle_attach = verify_tls ? esp_crt_bundle_attach : NULL, .timeout_ms = 10000, .buffer_size = 4096, .keep_alive_enable = true, .event_handler = http_event_handler, .user_agent = "ESP32-Internet-Radio/1.0", .max_redirection_count = 5};
	esp_http_client_handle_t h = esp_http_client_init(&hc);
	if (!h) return NULL;
	esp_http_client_set_header(h, "User-Agent", "ESP32-Internet-Radio/1.0");
	esp_http_client_set_header(h, "Accept", "*/*");
	esp_http_client_set_header(h, "Icy-Metadata", "1");
	esp_http_client_set_header(h, "Connection", "close");
	return h;
}

//-- Opens `url` with certificate verification, retrying up to
//-- `verified_attempts` times; if every verified attempt fails on an HTTPS
//-- URL, falls back to one unverified TLS attempt (see create_http_client)
//-- so a station whose chain the bundle can't validate still plays instead
//-- of refusing to open. On success returns the open handle and reports
//-- whether TLS was verified via `*out_tls_verified`; on failure returns
//-- NULL and copies a short reason into `reason_out` (already logged).
static esp_http_client_handle_t open_with_retries(const char *url, bool *out_tls_verified, char *reason_out, size_t reason_cap)
{
	bool is_https = strncmp(url, "https://", 8) == 0;
	bool tls_verified = true;
	esp_http_client_handle_t h = create_http_client(url, tls_verified);
	if (!h) {
		ESP_LOGE(TAG_HTTP, "HTTP client init failed for %s", url);
		strlcpy(reason_out, "client init failed", reason_cap);
		return NULL;
	}
	ESP_LOGI(TAG_HTTP, "Method: GET");
	ESP_LOGI(TAG_HTTP, "Request URL: %s", url);
	ESP_LOGI(TAG_HTTP, "HTTP version: library-managed by esp_http_client");
	ESP_LOGI(TAG_HTTP, "User-Agent: ESP32-Internet-Radio/1.0");
	ESP_LOGI(TAG_HTTP, "Icy-Metadata: 1");
	const int verified_attempts = 3;
	const int total_attempts = is_https ? verified_attempts + 1 : verified_attempts;
	for (int attempt = 1; attempt <= total_attempts; ++attempt) {
		if (esp_http_client_open(h, 0) == ESP_OK) {
			if (is_https && !tls_verified) ESP_LOGW(TAG_TLS, "Connected without TLS certificate verification (bundle rejected server's certificate chain)");
			*out_tls_verified = tls_verified;
			return h;
		}
		int err = esp_http_client_get_errno(h);
		esp_http_client_close(h);
		//-- Some broadcaster CDNs serve a certificate chain that terminates
		//-- at a root CA missing from the certificate bundle (e.g. a legacy
		//-- cross-signed root); every verified attempt against such a
		//-- station fails identically, so retrying with the same config is
		//-- pointless. Switch to one unverified TLS attempt instead. This is
		//-- generic: it engages for any station whose chain the bundle
		//-- can't validate, not a specific host.
		if (attempt == verified_attempts && is_https && tls_verified) {
			ESP_LOGW(TAG_TLS, "Certificate verification failed %d times for %s; falling back to one unverified TLS attempt", attempt, url);
			esp_http_client_cleanup(h);
			tls_verified = false;
			h = create_http_client(url, tls_verified);
			if (!h) {
				ESP_LOGE(TAG_HTTP, "HTTP client re-init failed for %s", url);
				strlcpy(reason_out, "client re-init failed", reason_cap);
				return NULL;
			}
			continue;
		}
		if (attempt < total_attempts) {
			log_retry_event(attempt, total_attempts, url, err != 0 ? strerror(err) : "connection failed");
			vTaskDelay(pdMS_TO_TICKS(2000));
			continue;
		}
		ESP_LOGE(TAG_HTTP, "HTTP open failed for %s", url);
		ESP_LOGE(TAG_TLS, "TLS handshake failed");
		if (err != 0) ESP_LOGE(TAG_HTTP, "errno=%d (%s)", err, strerror(err));
		strlcpy(reason_out, err != 0 ? strerror(err) : "HTTP open failed", reason_cap);
		esp_http_client_cleanup(h);
		return NULL;
	}
	esp_http_client_cleanup(h);
	return NULL;
}

//-- Owns the HTTP/ICY connection and does nothing else: reads network bytes,
//-- strips interleaved ICY metadata, and pushes pure audio bytes into the
//-- stream buffer. Runs on its own core so a slow/blocking read never delays
//-- stream_task's decode+I2S loop.
static void fetch_task(void *arg)
{
	(void)arg;
	radio_station_t station = s_station;
	bool first_payload_logged = false;
	uint64_t start_us = esp_timer_get_time();
	size_t bytes_received = 0;
	int http_code = 0;
	if (s_title_cb) s_title_cb("", s_title_ctx);
	log_station_banner(&station);
	char current_url[RADIO_URL_MAX];
	strlcpy(current_url, station.url, sizeof(current_url));
	char failure_reason_buf[64] = "HTTP open failed";
	esp_http_client_handle_t h = NULL;
	bool tls_verified = true;
	//-- A "livestream-redirect" style API (used by several broadcaster
	//-- CDNs, e.g. streamtheworld.com) answers with an HTTP 3xx pointing at
	//-- the actual stream host instead of audio. esp_http_client only
	//-- follows redirects automatically inside esp_http_client_perform(),
	//-- which this streaming reader doesn't use, so redirects are followed
	//-- explicitly here for any station that needs it.
	const int max_redirects = 5;
	for (int redirect = 0; redirect <= max_redirects; ++redirect) {
		log_url_details(current_url);
		const char *host = NULL;
		const char *colon = strstr(current_url, "://");
		if (colon) {
			host = colon + 3;
			if (strncmp(current_url, "https://", 8) == 0) {
				ESP_LOGI(TAG_TLS, "HTTPS connection requested");
				ESP_LOGI(TAG_TLS, "Certificate bundle attach configured");
			} else if (strncmp(current_url, "http://", 7) == 0) {
				ESP_LOGI(TAG_HTTP, "HTTP connection requested");
			}
		}
		if (host) {
			const char *host_end = strchr(host, '/');
			char host_copy[128] = {0};
			if (host_end) {
				size_t len = (size_t)(host_end - host);
				if (len >= sizeof(host_copy)) len = sizeof(host_copy) - 1;
				memcpy(host_copy, host, len);
				host_copy[len] = '\0';
				log_dns_resolution(host_copy);
			} else {
				strlcpy(host_copy, host, sizeof(host_copy));
				log_dns_resolution(host_copy);
			}
		}

		h = open_with_retries(current_url, &tls_verified, failure_reason_buf, sizeof(failure_reason_buf));
		if (!h) {
			log_stream_summary(station.name, current_url, failure_reason_buf, NULL, false, 0);
			goto done;
		}
		esp_http_client_fetch_headers(h);
		http_code = esp_http_client_get_status_code(h);
		log_http_response(h);

		if (http_code < 300 || http_code >= 400) break;
		char *location = NULL;
		if (redirect == max_redirects || esp_http_client_get_response_header(h, "Location", &location) != ESP_OK || !location) {
			ESP_LOGE(TAG_HTTP, "Unresolved HTTP redirect (status %d) for %s", http_code, current_url);
			log_stream_summary(station.name, current_url, "unresolved HTTP redirect", NULL, false, http_code);
			esp_http_client_close(h);
			esp_http_client_cleanup(h);
			h = NULL;
			goto done;
		}
		ESP_LOGW(TAG_HTTP, "Following redirect to %s", location);
		strlcpy(current_url, location, sizeof(current_url));
		esp_http_client_close(h);
		esp_http_client_cleanup(h);
		h = NULL;
	}

	//-- ICY metadata: server interleaves a length-prefixed "StreamTitle='...';"
	//-- block every icy-metaint bytes of audio when we asked for it above
	size_t meta_int = 0;
	char *meta_hdr = NULL;
	if (esp_http_client_get_response_header(h, "icy-metaint", &meta_hdr) == ESP_OK && meta_hdr) meta_int = (size_t)atoi(meta_hdr);
	ESP_LOGI(TAG_STREAM, "icy-metaint=%d", (int)meta_int);
	uint8_t *meta_buf = meta_int ? malloc(255 * 16 + 1) : NULL;
	if (meta_int && !meta_buf) meta_int = 0;
	size_t meta_remain = meta_int;

	uint8_t *chunk = malloc(FETCH_CHUNK_SIZE);
	if (!chunk) { free(meta_buf); goto done; }

	while (!s_stop_requested) {
		if (meta_int && meta_remain == 0) {
			uint8_t len_byte;
			if (http_read_exact(h, &len_byte, 1) != 0) {
				ESP_LOGE(TAG_STREAM, "ICY metadata length read failed; connection ended unexpectedly");
				break;
			}
			size_t meta_len = (size_t)len_byte * 16;
			if (meta_len) {
				if (http_read_exact(h, meta_buf, meta_len) != 0) {
					ESP_LOGE(TAG_STREAM, "ICY metadata payload read failed");
					break;
				}
				meta_buf[meta_len] = '\0';
				char title[64] = {0};
				parse_icy_title((char *)meta_buf, title, sizeof(title));
				if (title[0] && s_title_cb) s_title_cb(title, s_title_ctx);
				ESP_LOGI(TAG_STREAM, "ICY metadata: %s", title[0] ? title : "<empty>");
			}
			meta_remain = meta_int;
		}

		size_t want = FETCH_CHUNK_SIZE;
		if (meta_int && meta_remain < want) want = meta_remain;
		if (want == 0) continue;
		int n = esp_http_client_read(h, (char *)chunk, want);
		if (n <= 0) {
			ESP_LOGE(TAG_STREAM, "Stream read failed: n=%d errno=%d (%s)", n, errno, errno ? strerror(errno) : "no errno");
			break;
		}
		if (!first_payload_logged) {
			char *content_type = NULL;
			if (esp_http_client_get_response_header(h, "Content-Type", &content_type) == ESP_OK && content_type) {
				log_payload_probe(chunk, (size_t)n, content_type);
			} else {
				log_payload_probe(chunk, (size_t)n, NULL);
			}
			first_payload_logged = true;
		}
		if (meta_int) meta_remain -= (size_t)n;

		//-- Avoid blocking on a full buffer while a station switch is tearing
		//-- down the old stream: a waited send can leave xTaskWaitingToSend
		//-- set while the buffer is reset, which triggers the assert seen in
		//-- production. Poll with a zero timeout and retry briefly instead.
		size_t sent = 0;
		while (sent < (size_t)n && !s_stop_requested) {
			size_t written = xStreamBufferSend(s_sb, chunk + sent, (size_t)n - sent, 0);
			if (written == 0) {
				vTaskDelay(pdMS_TO_TICKS(5));
				continue;
			}
			sent += written;
		}
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
	ESP_LOGI(TAG_CODEC, "Codec selected: %s", station.codec == RADIO_CODEC_AAC ? "AAC" : "MP3");
	esp_audio_simple_dec_handle_t dec = NULL;
	if (esp_audio_simple_dec_open(&dc, &dec) != ESP_AUDIO_ERR_OK) {
		ESP_LOGE(TAG_CODEC, "Decoder open failed for station %s", station.name);
		goto done;
	}
	ESP_LOGI(TAG_CODEC, "Decoder start successful");

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
		if (s_paused) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
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
			//-- A real decode error means frame sync was lost (e.g. a network
			//-- hiccup corrupted/dropped bytes). Skip one byte and retry so the
			//-- decoder resyncs on the next valid frame header within a few
			//-- bytes, instead of staying stuck on the same bad offset until
			//-- the whole `in` buffer wraps (which was the cause of the
			//-- multi-second bursts of "AAC only support 1-2 channel" /
			//-- "Not supported" / decode-error clusters and the audible crackle).
			if (e != ESP_AUDIO_ERR_OK) {
				if (raw.len <= 1) break;
				raw.buffer += 1;
				raw.len -= 1;
				continue;
			}
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
esp_err_t radio_audio_play(const radio_station_t*s){if(!s||!s_queue)return ESP_ERR_INVALID_ARG;s_muted=true;s_paused=false;audio_message_t message={.station=*s};return xQueueSend(s_queue,&message,0)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;}
void radio_audio_set_volume(int p){s_volume=p<0?0:(p>100?100:p);}
int radio_audio_get_volume(void){return s_volume;}
void radio_audio_set_paused(bool paused){s_paused=paused;}
bool radio_audio_is_paused(void){return s_paused;}
void radio_audio_set_title_callback(radio_audio_title_cb_t cb,void *ctx){s_title_cb=cb;s_title_ctx=ctx;}
