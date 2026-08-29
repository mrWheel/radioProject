#include "esp32_s3_piggyback.h"
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_timer.h"
#include "esp_check.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

uint8_t tft_ec11_font_row(char c, uint8_t row);

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t s_w=320, s_h=240, s_bg=TFT_EC11_BLACK, s_fg=TFT_EC11_WHITE;
static uint8_t s_scale=1, s_rotation=1;
static QueueHandle_t s_queue;
static volatile uint8_t s_encoder_state;
static volatile int s_encoder_accum;
static volatile int64_t s_button_press_start_us[2];
static volatile bool s_button_pressed[2];
static volatile bool s_button_long_reported[2];
static TaskHandle_t s_button_monitor_task;
static const char *TAG="tft_ec11";
static uint16_t *s_transfer;
static SemaphoreHandle_t s_transfer_done;

static bool IRAM_ATTR transfer_done_callback(esp_lcd_panel_io_handle_t io,
                                              esp_lcd_panel_io_event_data_t *edata,
                                              void *user_ctx)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_transfer_done, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
}

static esp_err_t draw_pixels(int x, int y, int w, int h, uint16_t color)
{
    if (!s_panel || w<=0 || h<=0 || x>=s_w || y>=s_h || x+w<=0 || y+h<=0) return ESP_ERR_INVALID_ARG;
    if (x<0) { w+=x; x=0; }
    if (y<0) { h+=y; y=0; }
    if (x+w>s_w) { w=s_w-x; }
    if (y+h>s_h) { h=s_h-y; }
    const int chunk_rows=16;
    if (!s_transfer) s_transfer=heap_caps_malloc(320*chunk_rows*2, MALLOC_CAP_DMA);
    if (!s_transfer) return ESP_ERR_NO_MEM;
    for (int i=0;i<w*chunk_rows;i++) s_transfer[i]=(uint16_t)((color>>8)|(color<<8));
    esp_err_t e=ESP_OK;
    for (int yy=0; yy<h && e==ESP_OK; yy+=chunk_rows) {
        int rows=(h-yy>chunk_rows)?chunk_rows:h-yy;
        e=esp_lcd_panel_draw_bitmap(s_panel,x,y+yy,x+w,y+yy+rows,s_transfer);
        if (e == ESP_OK && xSemaphoreTake(s_transfer_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
            e = ESP_ERR_TIMEOUT;
        }
    }
    return e;
}

esp_err_t tft_ec11_set_rotation(uint8_t r)
{
    if (!s_panel || r>3) return ESP_ERR_INVALID_ARG;
    s_rotation=r;
    bool swap=(r&1), mx=(r==1||r==2), my=(r==2||r==3);
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_swap_xy(s_panel,swap));
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_mirror(s_panel,mx,my));
    s_w=swap?320:240; s_h=swap?240:320;
    return ESP_OK;
}

esp_err_t tft_ec11_init(void)
{
    if (s_panel) return ESP_ERR_INVALID_STATE;
    gpio_config_t out={.pin_bit_mask=(1ULL<<CONFIG_TFT_EC11_PIN_BL),.mode=GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&out),TAG,"backlight gpio");
    gpio_set_level(CONFIG_TFT_EC11_PIN_BL,0);
    spi_bus_config_t bus={.sclk_io_num=CONFIG_TFT_EC11_PIN_SCLK,.mosi_io_num=CONFIG_TFT_EC11_PIN_MOSI,
        .miso_io_num=-1,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=320*20*2};
    esp_err_t e=spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO); if(e!=ESP_OK) return e;
    s_transfer_done = xSemaphoreCreateBinary();
    if (!s_transfer_done) { e=ESP_ERR_NO_MEM; goto fail_bus; }
    esp_lcd_panel_io_spi_config_t io={.dc_gpio_num=CONFIG_TFT_EC11_PIN_DC,.cs_gpio_num=CONFIG_TFT_EC11_PIN_CS,
        .pclk_hz=CONFIG_TFT_EC11_SPI_CLOCK_HZ,.lcd_cmd_bits=8,.lcd_param_bits=8,.spi_mode=0,.trans_queue_depth=1,
        .on_color_trans_done=transfer_done_callback};
    e=esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,&io,&s_io); if(e!=ESP_OK) goto fail_bus;
    esp_lcd_panel_dev_config_t cfg={
        .reset_gpio_num=CONFIG_TFT_EC11_PIN_RST,
#if defined(CONFIG_TFT_EC11_BGR) && CONFIG_TFT_EC11_BGR
        .rgb_ele_order=LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order=LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel=16
    };
    e=esp_lcd_new_panel_st7789(s_io,&cfg,&s_panel); if(e!=ESP_OK) goto fail_io;
    if((e=esp_lcd_panel_reset(s_panel))!=ESP_OK || (e=esp_lcd_panel_init(s_panel))!=ESP_OK) goto fail_panel;
#if defined(CONFIG_TFT_EC11_DEFAULT_INVERT) && CONFIG_TFT_EC11_DEFAULT_INVERT
    esp_lcd_panel_invert_color(s_panel, true);
#else
    esp_lcd_panel_invert_color(s_panel, false);
#endif
    esp_lcd_panel_disp_on_off(s_panel,true);
    tft_ec11_set_rotation(CONFIG_TFT_EC11_DEFAULT_ROTATION);
    gpio_set_level(CONFIG_TFT_EC11_PIN_BL,1);
    return tft_ec11_clear();
fail_panel: esp_lcd_panel_del(s_panel); s_panel=NULL;
fail_io: esp_lcd_panel_io_del(s_io); s_io=NULL;
fail_bus: if (s_transfer_done) { vSemaphoreDelete(s_transfer_done); s_transfer_done=NULL; } spi_bus_free(SPI2_HOST); return e;
}

esp_err_t tft_ec11_deinit(void) { tft_ec11_inputs_stop(); if(!s_panel)return ESP_OK; gpio_set_level(CONFIG_TFT_EC11_PIN_BL,0); esp_lcd_panel_del(s_panel); esp_lcd_panel_io_del(s_io); s_panel=NULL;s_io=NULL; if (s_transfer_done) { vSemaphoreDelete(s_transfer_done); s_transfer_done=NULL; } free(s_transfer);s_transfer=NULL; return spi_bus_free(SPI2_HOST); }
esp_err_t tft_ec11_set_inversion(bool v){ return s_panel?esp_lcd_panel_invert_color(s_panel,v):ESP_ERR_INVALID_STATE; }
esp_err_t tft_ec11_set_backlight(bool v){ return gpio_set_level(CONFIG_TFT_EC11_PIN_BL,v); }
uint16_t tft_ec11_width(void){return s_w;} uint16_t tft_ec11_height(void){return s_h;}
void tft_ec11_set_background(uint16_t c){s_bg=c;} void tft_ec11_set_text_style(uint16_t c,uint8_t z){s_fg=c;s_scale=z?z:1;}
tft_ec11_text_style_t tft_ec11_get_text_style(void){return (tft_ec11_text_style_t){s_bg,s_fg,s_scale};}
esp_err_t tft_ec11_clear(void){return draw_pixels(0,0,s_w,s_h,s_bg);}
esp_err_t tft_ec11_fill_rect(int x,int y,int w,int h,uint16_t c){return draw_pixels(x,y,w,h,c);}
esp_err_t tft_ec11_refresh(const uint16_t *fb){ if(!s_panel||!fb)return ESP_ERR_INVALID_ARG; const int rows=16;if(!s_transfer)s_transfer=heap_caps_malloc(320*rows*2,MALLOC_CAP_DMA);if(!s_transfer)return ESP_ERR_NO_MEM;for(int y=0;y<s_h;y+=rows){int n=(s_h-y>rows)?rows:s_h-y;for(int i=0;i<s_w*n;i++){uint16_t p=fb[(size_t)y*s_w+i];s_transfer[i]=(p>>8)|(p<<8);}esp_err_t e=esp_lcd_panel_draw_bitmap(s_panel,0,y,s_w,y+n,s_transfer);if(e!=ESP_OK)return e;if(xSemaphoreTake(s_transfer_done,pdMS_TO_TICKS(1000))!=pdTRUE)return ESP_ERR_TIMEOUT;}return ESP_OK; }

static esp_err_t draw_text_with_style(int x, int y, size_t slots, const char *text,
                                      uint16_t background, uint8_t font_scale,
                                      uint16_t foreground)
{
    if(!text||!slots||!font_scale)return ESP_ERR_INVALID_ARG;
    int cw=6*font_scale,ch=8*font_scale; ESP_RETURN_ON_ERROR(draw_pixels(x,y,(int)slots*cw,ch,background),TAG,"text clear");
    size_t len=strlen(text); if(len>slots)len=slots;
    for(size_t i=0;i<len;i++) for(int row=0;row<7;row++){uint8_t bits=tft_ec11_font_row(text[i],row); for(int col=0;col<5;col++) if(bits&(1<<(4-col))) draw_pixels(x+(int)i*cw+col*font_scale,y+row*font_scale,font_scale,font_scale,foreground);}
    return ESP_OK;
}

esp_err_t tft_ec11_draw_text_simple(int x, int y, const char *text)
{
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    return draw_text_with_style(x, y, strlen(text), text, s_bg, s_scale, s_fg);
}

esp_err_t tft_ec11_draw_text_styled(int x, int y, const char *text, uint16_t background,
                                    uint8_t font_scale, uint16_t foreground)
{
    if (!text) {
        return ESP_ERR_INVALID_ARG;
    }
    return draw_text_with_style(x, y, strlen(text), text, background, font_scale, foreground);
}

esp_err_t tft_ec11_draw_text(int x, int y, size_t slots, const char *text)
{
    return draw_text_with_style(x, y, slots, text, s_bg, s_scale, s_fg);
}

static void emit_button(tft_ec11_event_type_t type, tft_ec11_press_t press, uint32_t ms)
{
    if (!s_queue) {
        return;
    }

    tft_ec11_event_t e = { .type = type, .duration_ms = ms, .press = press };
    xQueueSend(s_queue, &e, 0);
}

//-- Buttons are fully polled (debounce, edge detection, LONG timing); ISR handles only the encoder
static void button_task(void *arg)
{
    const int pins[2] = { CONFIG_TFT_EC11_PIN_ENC_BUTTON, CONFIG_TFT_EC11_PIN_AUX_BUTTON };
    bool raw_pressed[2] = { s_button_pressed[0], s_button_pressed[1] };
    int64_t last_change_us[2] = { esp_timer_get_time(), esp_timer_get_time() };

    while (true) {
        int64_t now_us = esp_timer_get_time();

        for (int idx = 0; idx < 2; idx++) {
            bool level_pressed = !gpio_get_level(pins[idx]);

            if (level_pressed != raw_pressed[idx]) {
                raw_pressed[idx] = level_pressed;
                last_change_us[idx] = now_us;
            }

            bool settled = (now_us - last_change_us[idx]) >= ((int64_t)CONFIG_TFT_EC11_DEBOUNCE_MS * 1000LL);
            tft_ec11_event_type_t type = idx == 0 ? TFT_EC11_EVENT_ENCODER_BUTTON : TFT_EC11_EVENT_AUX_BUTTON;

            if (settled && raw_pressed[idx] != s_button_pressed[idx]) {
                s_button_pressed[idx] = raw_pressed[idx];

                if (s_button_pressed[idx]) {
                    s_button_press_start_us[idx] = now_us;
                    s_button_long_reported[idx] = false;
                    emit_button(type, TFT_EC11_PRESS_DOWN, 0);
                } else if (!s_button_long_reported[idx]) {
                    uint32_t duration_ms = (uint32_t)((now_us - s_button_press_start_us[idx]) / 1000ULL);
                    tft_ec11_press_t press = duration_ms >= CONFIG_TFT_EC11_MEDIUM_MS ? TFT_EC11_PRESS_MEDIUM : TFT_EC11_PRESS_SHORT;
                    emit_button(type, press, duration_ms);
                }
            }

            if (s_button_pressed[idx] && !s_button_long_reported[idx]) {
                uint32_t held_ms = (uint32_t)((now_us - s_button_press_start_us[idx]) / 1000ULL);
                if (held_ms >= CONFIG_TFT_EC11_LONG_MS) {
                    s_button_long_reported[idx] = true;
                    emit_button(type, TFT_EC11_PRESS_LONG, held_ms);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void IRAM_ATTR input_isr_handler(void *arg)
{
    uint8_t now_state = (gpio_get_level(CONFIG_TFT_EC11_PIN_ENC_A) << 1) | gpio_get_level(CONFIG_TFT_EC11_PIN_ENC_B);
    static const int8_t q[16] = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };
    int delta = q[(s_encoder_state << 2) | now_state];
    s_encoder_state = now_state;

    if (delta != 0) {
        s_encoder_accum += delta;
        if (s_encoder_accum >= CONFIG_TFT_EC11_ENCODER_TRANSITIONS || s_encoder_accum <= -CONFIG_TFT_EC11_ENCODER_TRANSITIONS) {
            tft_ec11_event_t e = {
                .type = TFT_EC11_EVENT_ROTATE,
                .steps = s_encoder_accum > 0 ? 1 : -1
            };
            BaseType_t higher = pdFALSE;
            xQueueSendFromISR(s_queue, &e, &higher);
            s_encoder_accum = 0;
        }
    }
}

esp_err_t tft_ec11_inputs_start(size_t n)
{
    if (s_queue) {
        return ESP_ERR_INVALID_STATE;
    }

    s_queue = xQueueCreate(n ? n : 16, sizeof(tft_ec11_event_t));
    if (!s_queue) {
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t encoder_pins = {
        .pin_bit_mask = (1ULL << CONFIG_TFT_EC11_PIN_ENC_A) |
                        (1ULL << CONFIG_TFT_EC11_PIN_ENC_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };
    ESP_RETURN_ON_ERROR(gpio_config(&encoder_pins), TAG, "encoder gpio config");

    gpio_config_t button_pins = {
        .pin_bit_mask = (1ULL << CONFIG_TFT_EC11_PIN_ENC_BUTTON) |
                        (1ULL << CONFIG_TFT_EC11_PIN_AUX_BUTTON),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button_pins), TAG, "button gpio config");

    s_encoder_state = (gpio_get_level(CONFIG_TFT_EC11_PIN_ENC_A) << 1) | gpio_get_level(CONFIG_TFT_EC11_PIN_ENC_B);
    s_encoder_accum = 0;
    s_button_pressed[0] = !gpio_get_level(CONFIG_TFT_EC11_PIN_ENC_BUTTON);
    s_button_pressed[1] = !gpio_get_level(CONFIG_TFT_EC11_PIN_AUX_BUTTON);
    s_button_press_start_us[0] = esp_timer_get_time();
    s_button_press_start_us[1] = esp_timer_get_time();
    s_button_long_reported[0] = false;
    s_button_long_reported[1] = false;

    if (gpio_install_isr_service(0) != ESP_OK) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    gpio_isr_handler_add(CONFIG_TFT_EC11_PIN_ENC_A, input_isr_handler, (void *)(intptr_t)CONFIG_TFT_EC11_PIN_ENC_A);
    gpio_isr_handler_add(CONFIG_TFT_EC11_PIN_ENC_B, input_isr_handler, (void *)(intptr_t)CONFIG_TFT_EC11_PIN_ENC_B);

    xTaskCreate(button_task, "btn_task", 2048, NULL, 5, &s_button_monitor_task);

    return ESP_OK;
}

void tft_ec11_inputs_stop(void)
{
    if (!s_queue) {
        return;
    }

    if (s_button_monitor_task) {
        vTaskDelete(s_button_monitor_task);
        s_button_monitor_task = NULL;
    }

    gpio_isr_handler_remove(CONFIG_TFT_EC11_PIN_ENC_A);
    gpio_isr_handler_remove(CONFIG_TFT_EC11_PIN_ENC_B);
    gpio_uninstall_isr_service();

    vQueueDelete(s_queue);
    s_queue = NULL;
}

QueueHandle_t tft_ec11_event_queue(void)
{
    return s_queue;
}

bool tft_ec11_get_event(tft_ec11_event_t *e, TickType_t w)
{
    return s_queue && e && xQueueReceive(s_queue, e, w) == pdTRUE;
}
