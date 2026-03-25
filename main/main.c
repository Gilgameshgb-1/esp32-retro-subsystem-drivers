#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "ili9341.h"
#include "framebuffer.h"
#include "wifi_server.h"
#include "audio.h"

static const char *TAG = "main";

#define STRIP_HEIGHT  40
#define NUM_STRIPS    (ILI9341_HEIGHT / STRIP_HEIGHT)

static const ili9341_config_t display_cfg = {
    .pin_mosi     = 23,
    .pin_clk      = 18,
    .pin_cs       =  5,
    .pin_dc       =  2,
    .pin_rst      =  4,
    .spi_clock_hz = 26 * 1000 * 1000,
};

static const audio_config_t audio_cfg = {
    .pin_din     = 25,
    .pin_bclk    = 26,
    .pin_lrc     = 27,
    .sample_rate = 44100,
};

/* -----------------------------------------------------------------------
 * Display shared state
 * ----------------------------------------------------------------------- */
static framebuffer_t     *s_fb;
static SemaphoreHandle_t  s_strip_ready;
static SemaphoreHandle_t  s_strip_done;
static uint16_t           s_strip_y;
static size_t             s_buf_offset;
static int                s_strip_idx;

/* -----------------------------------------------------------------------
 * Audio shared state
 * ----------------------------------------------------------------------- */

/* 8 KB is sufficient: the TCP receive window is already only ~5760 B
 * so the sender never bursts more than that before waiting for an ACK.
 * Keeping this small ensures xStreamBufferCreate succeeds after the
 * WiFi driver, lwIP, DMA, and framebuffer have already consumed most
 * of the 155 KB DRAM heap. */
#define AUDIO_STREAM_BUF_BYTES  (8 * 1024)

static StreamBufferHandle_t s_audio_sbuf;

/* -----------------------------------------------------------------------
 * Audio write task (core 1)
 * Owns all i2s_channel_write calls so the WiFi/TCP task on core 0 never
 * blocks inside I2S.
 * ----------------------------------------------------------------------- */
static void audio_write_task(void *arg)
{
    (void)arg;
    static uint32_t chunk_words[1024];  /* 4096 bytes, 4-byte aligned */
    uint8_t * const chunk = (uint8_t *)chunk_words;
    for (;;) {
        size_t received = xStreamBufferReceive(s_audio_sbuf, chunk,
                                               sizeof(chunk_words),
                                               portMAX_DELAY);
        if (received >= 2) {
            audio_write((const int16_t *)chunk, received & ~1u);
        }
    }
}

/* -----------------------------------------------------------------------
 * Display refresh task (core 1)
 * ----------------------------------------------------------------------- */
static void display_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_strip_ready, portMAX_DELAY);
        fb_swap(s_fb);
        ili9341_draw_image(0, s_strip_y,
                           fb_get_width(s_fb),
                           fb_get_height(s_fb),
                           fb_get_front_buffer(s_fb));
        xSemaphoreGive(s_strip_done);
    }
}

/* -----------------------------------------------------------------------
 * TCP callbacks
 * ----------------------------------------------------------------------- */

/* Port 8080 -- RGB565 image data, strip by strip */
static void on_image_data(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    size_t strip_bytes = fb_get_size(s_fb);

    while (len > 0) {
        uint8_t *buf = fb_get_back_buffer(s_fb);
        size_t   remaining = strip_bytes - s_buf_offset;
        size_t   to_copy   = (len < remaining) ? len : remaining;

        memcpy(buf + s_buf_offset, data, to_copy);
        s_buf_offset += to_copy;
        data         += to_copy;
        len          -= to_copy;

        if (s_buf_offset == strip_bytes) {
            s_strip_y = s_strip_idx * STRIP_HEIGHT;
            xSemaphoreGive(s_strip_ready);
            xSemaphoreTake(s_strip_done, portMAX_DELAY);
            s_buf_offset = 0;
            s_strip_idx  = (s_strip_idx + 1) % NUM_STRIPS;
        }
    }
}

/* Reset strip state so the next connection always starts at the top of the frame */
static void on_image_close(void *arg)
{
    (void)arg;
    s_buf_offset = 0;
    s_strip_idx  = 0;
}

/* Port 8081 -- raw 16-bit mono PCM at 44100 Hz.
 * Block until all bytes are queued.  When the stream buffer fills,
 * recv() stalls, TCP window closes to zero, and the sender pauses --
 * natural flow control with no data ever dropped. */
static void on_audio_data(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    while (len > 0) {
        size_t sent = xStreamBufferSend(s_audio_sbuf, data, len, portMAX_DELAY);
        data += sent;
        len  -= sent;
    }
}

/* Queue silence to flush the I2S DMA buffer after playback ends.
 * 16 x 512 = 8192 bytes = stream buffer capacity = ~93 ms of silence. */
static void on_audio_close(void *arg)
{
    (void)arg;
    static const int16_t silence[256] = {0};
    for (int i = 0; i < 16; i++) {
        xStreamBufferSend(s_audio_sbuf, silence, sizeof(silence), portMAX_DELAY);
    }
}

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static void fill_back_buffer_solid(uint16_t color)
{
    uint8_t *buf = fb_get_back_buffer(s_fb);
    size_t   sz  = fb_get_size(s_fb);
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    for (size_t i = 0; i < sz; i += 2) {
        buf[i]     = hi;
        buf[i + 1] = lo;
    }
}

static void fill_screen_solid(uint16_t color)
{
    for (int strip = 0; strip < NUM_STRIPS; strip++) {
        fill_back_buffer_solid(color);
        s_strip_y = strip * STRIP_HEIGHT;
        xSemaphoreGive(s_strip_ready);
        xSemaphoreTake(s_strip_done, portMAX_DELAY);
    }
}

/* -----------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */
void app_main(void)
{
    /* Display */
    ili9341_init(&display_cfg);
    ili9341_fill_screen(ili9341_rgb565(0, 0, 0));

    s_fb = fb_alloc(ILI9341_WIDTH, STRIP_HEIGHT);
    if (!s_fb) { ESP_LOGE(TAG, "Framebuffer alloc failed"); return; }

    s_strip_ready = xSemaphoreCreateBinary();
    s_strip_done  = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(display_refresh_task, "refresh", 4096, NULL, 5, NULL, 1);

    fill_screen_solid(ili9341_rgb565(0, 255, 0));  /* green = connecting */

    /* Wi-Fi */
    if (wifi_connect(CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD) != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi failed");
        fill_screen_solid(ili9341_rgb565(255, 0, 0));
        return;
    }

    /* Audio: init I2S, create stream buffer, start write task on core 1 */
    audio_init(&audio_cfg);
    s_audio_sbuf = xStreamBufferCreate(AUDIO_STREAM_BUF_BYTES, 1);
    if (!s_audio_sbuf) {
        ESP_LOGE(TAG, "Audio stream buffer alloc failed");
        fill_screen_solid(ili9341_rgb565(255, 0, 0));
        return;
    }
    xTaskCreatePinnedToCore(audio_write_task, "audio_write",
                            8192, NULL, 7, NULL, 1);

    /* Image server: core 0 */
    wifi_server_start(&(wifi_server_config_t){
        .port       = 8080,
        .on_recv    = on_image_data,
        .on_close   = on_image_close,
        .cb_arg     = NULL,
        .stack_size = 8192,
        .core_id    = 0,
    });

    /* Audio server: core 1 -- keeps TCP recv away from the WiFi/lwIP core */
    wifi_server_start(&(wifi_server_config_t){
        .port       = 8081,
        .on_recv    = on_audio_data,
        .on_close   = on_audio_close,
        .cb_arg     = NULL,
        .stack_size = 8192,
        .core_id    = 1,
    });

    fill_screen_solid(ili9341_rgb565(0, 0, 255));  /* blue = ready */
    ESP_LOGI(TAG, "Ready -- image: port 8080 | audio: port 8081");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
