#include <stdlib.h>
#include <string.h>
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
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
    .spi_clock_hz = 30 * 1000 * 1000,
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
 * Image protocol parser state
 * ----------------------------------------------------------------------- */
typedef enum {
    IMG_STATE_HEADER,        /* 2-byte frame header                     */
    IMG_STATE_FULL_FRAME,    /* raw RGB565 strips (type 0xFF)            */
    IMG_STATE_PATCH_HEADER,  /* 8-byte patch coords (type 0xDF)          */
    IMG_STATE_PATCH_DATA,    /* patch pixel data                         */
} img_parse_state_t;

/* Largest patch the host will ever send: full width × one strip height. */
#define MAX_PATCH_BYTES  (ILI9341_WIDTH * STRIP_HEIGHT * 2)  /* 19 200 B */

static img_parse_state_t s_img_state      = IMG_STATE_HEADER;
static uint8_t           s_img_hdr[2];
static uint8_t           s_img_hdr_pos    = 0;

/* Delta-frame patch state */
static uint8_t           s_patches_remaining = 0;
static uint8_t           s_patch_hdr[8];
static uint8_t           s_patch_hdr_pos     = 0;
static uint16_t          s_patch_x0, s_patch_y0, s_patch_x1, s_patch_y1;
static uint8_t          *s_patch_buf         = NULL; /* DMA-capable, app_main alloc */
static size_t            s_patch_need        = 0;   /* bytes expected for this patch */
static size_t            s_patch_recv        = 0;   /* bytes received so far         */

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

/* Audio control state -- written by command task (core 0), read by audio task (core 1) */
static volatile bool    s_audio_paused  = false;
static volatile bool    s_audio_stopped = false;
static volatile uint8_t s_audio_volume  = 100;  /* 0–100 */
static volatile int     s_audio_client_sock = -1; /* fd of active audio TCP client, -1 = none */

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
    static const int16_t silence[256] = {0};  /* ~5.8 ms of silence */

    for (;;) {
        if (s_audio_stopped) {
            xStreamBufferReset(s_audio_sbuf);
            audio_write(silence, sizeof(silence));
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (s_audio_paused) {
            /* Don't consume the buffer; write silence to keep I2S clock alive */
            audio_write(silence, sizeof(silence));
            continue;
        }

        /* Wait up to 20 ms so we can re-check control state promptly */
        size_t received = xStreamBufferReceive(s_audio_sbuf, chunk,
                                               sizeof(chunk_words),
                                               pdMS_TO_TICKS(20));
        if (received < 2) continue;

        /* Apply volume scaling */
        uint8_t vol = s_audio_volume;
        if (vol < 100) {
            int16_t *samples = (int16_t *)chunk;
            size_t n = received / 2;
            for (size_t i = 0; i < n; i++) {
                samples[i] = (int16_t)((samples[i] * vol) / 100);
            }
        }

        audio_write((const int16_t *)chunk, received & ~1u);
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

/* Port 8080 -- framed RGB565 data.
 *
 * Frame wire format:
 *   Byte 0 : type   — 0xFF full frame | 0xDF delta frame
 *   Byte 1 : n      — strip count (0xFF) or patch count (0xDF)
 *
 * 0xFF: followed by NUM_STRIPS × strip_bytes of raw RGB565.
 * 0xDF: followed by n patch records, each:
 *         [x0 y0 x1 y1] as 4 × uint16_t little-endian
 *         (x1-x0)*(y1-y0)*2 bytes of RGB565 big-endian pixel data
 *       n == 0 is a valid "no change" frame — nothing follows the header.
 */
static void on_image_data(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;

    while (len > 0) {

        /* ----------------------------------------------------------------
         * HEADER — 2 bytes: [type, n]
         * ---------------------------------------------------------------- */
        if (s_img_state == IMG_STATE_HEADER) {
            s_img_hdr[s_img_hdr_pos++] = *data++;
            len--;
            if (s_img_hdr_pos < 2) continue;

            s_img_hdr_pos = 0;
            uint8_t type = s_img_hdr[0];
            uint8_t n    = s_img_hdr[1];

            if (type == 0xFF) {
                s_img_state = IMG_STATE_FULL_FRAME;
            } else if (type == 0xDF) {
                if (n == 0) {
                    /* No-change frame — stay in HEADER for the next one */
                } else {
                    s_patches_remaining = n;
                    s_patch_hdr_pos     = 0;
                    s_img_state         = IMG_STATE_PATCH_HEADER;
                }
            } else {
                ESP_LOGW(TAG, "[img] unknown frame type 0x%02X", type);
            }
            continue;
        }

        /* ----------------------------------------------------------------
         * FULL FRAME — raw RGB565 strips, same as original path
         * ---------------------------------------------------------------- */
        if (s_img_state == IMG_STATE_FULL_FRAME) {
            size_t  strip_bytes = fb_get_size(s_fb);
            uint8_t *buf        = fb_get_back_buffer(s_fb);
            size_t   remaining  = strip_bytes - s_buf_offset;
            size_t   to_copy    = (len < remaining) ? len : remaining;

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

                if (s_strip_idx == 0) {
                    s_img_state = IMG_STATE_HEADER;
                }
            }
            continue;
        }

        /* ----------------------------------------------------------------
         * PATCH HEADER — 8 bytes: x0 y0 x1 y1 (uint16_t LE each)
         * ---------------------------------------------------------------- */
        if (s_img_state == IMG_STATE_PATCH_HEADER) {
            s_patch_hdr[s_patch_hdr_pos++] = *data++;
            len--;
            if (s_patch_hdr_pos < 8) continue;

            s_patch_hdr_pos = 0;
            memcpy(&s_patch_x0, &s_patch_hdr[0], 2);
            memcpy(&s_patch_y0, &s_patch_hdr[2], 2);
            memcpy(&s_patch_x1, &s_patch_hdr[4], 2);
            memcpy(&s_patch_y1, &s_patch_hdr[6], 2);

            s_patch_need = (size_t)(s_patch_x1 - s_patch_x0)
                         * (size_t)(s_patch_y1 - s_patch_y0) * 2;
            s_patch_recv = 0;
            s_img_state  = IMG_STATE_PATCH_DATA;
            continue;
        }

        /* ----------------------------------------------------------------
         * PATCH DATA — accumulate then draw
         * ---------------------------------------------------------------- */
        if (s_img_state == IMG_STATE_PATCH_DATA) {
            size_t remaining = s_patch_need - s_patch_recv;
            size_t to_copy   = (len < remaining) ? len : remaining;

            memcpy(s_patch_buf + s_patch_recv, data, to_copy);
            s_patch_recv += to_copy;
            data         += to_copy;
            len          -= to_copy;

            if (s_patch_recv == s_patch_need) {
                uint16_t w = s_patch_x1 - s_patch_x0;
                uint16_t h = s_patch_y1 - s_patch_y0;
                ili9341_draw_image(s_patch_x0, s_patch_y0, w, h, s_patch_buf);

                s_patches_remaining--;
                if (s_patches_remaining == 0) {
                    s_img_state = IMG_STATE_HEADER;
                } else {
                    s_patch_hdr_pos = 0;
                    s_img_state     = IMG_STATE_PATCH_HEADER;
                }
            }
            continue;
        }
    }
}

/* Reset all parser state so the next connection starts clean. */
static void on_image_close(void *arg)
{
    (void)arg;
    s_buf_offset        = 0;
    s_strip_idx         = 0;
    s_img_state         = IMG_STATE_HEADER;
    s_img_hdr_pos       = 0;
    s_patches_remaining = 0;
    s_patch_hdr_pos     = 0;
    s_patch_recv        = 0;
}

static void on_audio_connect(int sock, void *arg)
{
    (void)arg;
    s_audio_client_sock = sock;
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
 * 16 x 512 = 8192 bytes = stream buffer capacity = ~93 ms of silence.
 * Skip when paused/stopped to avoid blocking on a full or reset buffer. */
static void on_audio_close(void *arg)
{
    (void)arg;
    s_audio_client_sock = -1;
    if (s_audio_paused || s_audio_stopped) return;
    static const int16_t silence[256] = {0};
    for (int i = 0; i < 16; i++) {
        xStreamBufferSend(s_audio_sbuf, silence, sizeof(silence), portMAX_DELAY);
    }
}

/* Port 8082 -- plain-text control commands */
static void on_command_recv(const uint8_t *data, size_t len, void *arg)
{
    (void)arg;
    char cmd[32];
    size_t n = len < sizeof(cmd) - 1 ? len : sizeof(cmd) - 1;
    memcpy(cmd, data, n);
    cmd[n] = '\0';
    /* Trim trailing whitespace / newlines */
    for (int i = (int)n - 1; i >= 0 && (cmd[i] == '\n' || cmd[i] == '\r' || cmd[i] == ' '); i--) {
        cmd[i] = '\0';
    }

    if (strcmp(cmd, "PLAY") == 0) {
        s_audio_stopped = false;
        s_audio_paused  = false;
        ESP_LOGI(TAG, "Command: PLAY");
    } else if (strcmp(cmd, "PAUSE") == 0) {
        s_audio_paused = true;
        ESP_LOGI(TAG, "Command: PAUSE");
    } else if (strcmp(cmd, "STOP") == 0) {
        s_audio_stopped = true;
        s_audio_paused  = false;
        /* Kick the sender off so the Python process exits immediately */
        int sock = s_audio_client_sock;
        if (sock >= 0) shutdown(sock, SHUT_RDWR);
        ESP_LOGI(TAG, "Command: STOP");
    } else if (strncmp(cmd, "VOLUME ", 7) == 0) {
        int vol = atoi(cmd + 7);
        if (vol >= 0 && vol <= 100) {
            s_audio_volume = (uint8_t)vol;
            ESP_LOGI(TAG, "Command: VOLUME %d", vol);
        } else {
            ESP_LOGW(TAG, "VOLUME out of range: %d", vol);
        }
    } else {
        ESP_LOGW(TAG, "Unknown command: %s", cmd);
    }
}

static void on_command_close(void *arg) { (void)arg; }

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

    s_patch_buf = heap_caps_malloc(MAX_PATCH_BYTES, MALLOC_CAP_DMA);
    if (!s_patch_buf) { ESP_LOGE(TAG, "Patch buffer alloc failed"); return; }

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
        .on_connect = on_audio_connect,
        .on_recv    = on_audio_data,
        .on_close   = on_audio_close,
        .cb_arg     = NULL,
        .stack_size = 8192,
        .core_id    = 1,
    });

    /* Command server: core 0 */
    wifi_server_start(&(wifi_server_config_t){
        .port       = 8082,
        .on_recv    = on_command_recv,
        .on_close   = on_command_close,
        .cb_arg     = NULL,
        .stack_size = 4096,
        .core_id    = 0,
    });

    fill_screen_solid(ili9341_rgb565(0, 0, 255));  /* blue = ready */
    ESP_LOGI(TAG, "Ready -- image: 8080 | audio: 8081 | cmd: 8082");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
