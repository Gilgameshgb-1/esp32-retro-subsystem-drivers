#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "audio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx_chan;
static int               s_sample_rate;

/* -----------------------------------------------------------------------
 * I2S init / reconfigure
 * ----------------------------------------------------------------------- */
void audio_init(const audio_config_t *config)
{
    s_sample_rate = config->sample_rate;

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 8;
    chan_cfg.dma_frame_num = 1024;   /* 8 × 1024 × 2 B = 16 KB ≈ 185 ms headroom */
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(config->sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                         I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = config->pin_bclk,
            .ws   = config->pin_lrc,
            .dout = config->pin_din,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    ESP_LOGI(TAG, "I2S initialized: %d Hz, 16-bit mono", config->sample_rate);
}

/* Reconfigure just the clock when the MP3 sample rate differs from init */
static void reconfigure_sample_rate(int new_rate)
{
    if (new_rate == s_sample_rate) return;

    ESP_ERROR_CHECK(i2s_channel_disable(s_tx_chan));
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(new_rate);
    ESP_ERROR_CHECK(i2s_channel_reconfig_std_clock(s_tx_chan, &clk));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    s_sample_rate = new_rate;
    ESP_LOGI(TAG, "I2S sample rate -> %d Hz", new_rate);
}

/* -----------------------------------------------------------------------
 * PCM write
 * ----------------------------------------------------------------------- */
void audio_write(const int16_t *data, size_t len)
{
    /* Loop in case of partial writes (e.g. DMA descriptor boundary) */
    const uint8_t *ptr = (const uint8_t *)data;
    while (len > 0) {
        size_t written = 0;
        i2s_channel_write(s_tx_chan, ptr, len, &written, portMAX_DELAY);
        ptr += written;
        len -= written;
    }
}

/* -----------------------------------------------------------------------
 * Tone generator
 * ----------------------------------------------------------------------- */
void audio_play_tone(uint16_t freq_hz, uint32_t duration_ms, float volume)
{
    int samples_per_period = s_sample_rate / freq_hz;
    int16_t period_buf[samples_per_period];

    for (int i = 0; i < samples_per_period; i++) {
        float t = (float)i / (float)s_sample_rate;
        float sample = sinf(2.0f * M_PI * freq_hz * t) * volume;
        period_buf[i] = (int16_t)(sample * 32767.0f);
    }

    int total_samples = (s_sample_rate * duration_ms) / 1000;
    int periods = total_samples / samples_per_period;

    for (int p = 0; p < periods; p++) {
        audio_write(period_buf, sizeof(period_buf));
    }

    ESP_LOGI(TAG, "Tone: %d Hz, %d ms", (int)freq_hz, (int)duration_ms);
}

/* -----------------------------------------------------------------------
 * PCM clip from flash
 * ----------------------------------------------------------------------- */
void audio_play_pcm(const int16_t *data, size_t num_samples)
{
    const size_t CHUNK = 1024;
    size_t remaining = num_samples;
    const int16_t *ptr = data;

    while (remaining > 0) {
        size_t to_send = (remaining < CHUNK) ? remaining : CHUNK;
        audio_write(ptr, to_send * sizeof(int16_t));
        ptr       += to_send;
        remaining -= to_send;
    }
}

/* -----------------------------------------------------------------------
 * Streaming MP3 decoder
 *
 * Architecture: TCP callback -> stream buffer -> decode task -> I2S
 *
 * The decode task runs independently at high priority.  The TCP task
 * only does xStreamBufferSend (fast, non-blocking relative to I2S),
 * so TCP receive is never stalled waiting for DMA to drain.
 * ----------------------------------------------------------------------- */

#define AUDIO_STREAM_BUF_SIZE  (32 * 1024)   /* compressed data queue   */
#define DECODE_TASK_STACK      (20 * 1024)   /* minimp3 needs ~16 KB    */
#define DECODE_TASK_PRIORITY   6             /* higher than TCP task    */

struct audio_mp3_stream {
    mp3dec_t             dec;
    int16_t              pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    uint8_t              in_buf[AUDIO_MP3_IN_BUF_SIZE];
    size_t               in_used;
    StreamBufferHandle_t stream_buf;
    TaskHandle_t         decode_task;
};

/* Decode and play all complete frames currently in in_buf */
static void decode_frames(audio_mp3_stream_t *s)
{
    mp3dec_frame_info_t info;

    for (;;) {
        int samples = mp3dec_decode_frame(&s->dec,
                                          s->in_buf, (int)s->in_used,
                                          s->pcm, &info);

        if (info.frame_bytes == 0) break;  /* need more data */

        s->in_used -= info.frame_bytes;
        memmove(s->in_buf, s->in_buf + info.frame_bytes, s->in_used);

        if (samples == 0) continue;  /* ID3 / xing header -- skip */

        reconfigure_sample_rate(info.hz);

        /* Downmix stereo to mono -- I2S slot is configured mono */
        if (info.channels == 2) {
            for (int i = 0; i < samples; i++) {
                s->pcm[i] = (int16_t)(((int32_t)s->pcm[i * 2] +
                                        s->pcm[i * 2 + 1]) >> 1);
            }
        }
        audio_write(s->pcm, (size_t)samples * sizeof(int16_t));
    }
}

/* Dedicated decode task -- runs independently of TCP receive */
static void audio_decode_task(void *arg)
{
    audio_mp3_stream_t *s = (audio_mp3_stream_t *)arg;
    uint8_t chunk[1460];

    for (;;) {
        size_t received = xStreamBufferReceive(s->stream_buf,
                                               chunk, sizeof(chunk),
                                               portMAX_DELAY);
        if (received == 0) continue;

        const uint8_t *data = chunk;
        size_t          len  = received;

        while (len > 0) {
            size_t space    = AUDIO_MP3_IN_BUF_SIZE - s->in_used;
            size_t to_copy  = (len < space) ? len : space;

            memcpy(s->in_buf + s->in_used, data, to_copy);
            s->in_used += to_copy;
            data       += to_copy;
            len        -= to_copy;

            decode_frames(s);
        }
    }
}

audio_mp3_stream_t *audio_mp3_stream_alloc(void)
{
    return calloc(1, sizeof(audio_mp3_stream_t));
}

void audio_mp3_stream_free(audio_mp3_stream_t *s)
{
    if (!s) return;
    if (s->decode_task) vTaskDelete(s->decode_task);
    if (s->stream_buf)  vStreamBufferDelete(s->stream_buf);
    free(s);
}

void audio_mp3_stream_init(audio_mp3_stream_t *s)
{
    mp3dec_init(&s->dec);
    s->in_used = 0;

    s->stream_buf = xStreamBufferCreate(AUDIO_STREAM_BUF_SIZE, 1);
    xTaskCreatePinnedToCore(audio_decode_task, "mp3_dec",
                            DECODE_TASK_STACK, s,
                            DECODE_TASK_PRIORITY, &s->decode_task, 1); /* core 1, away from WiFi */
}

void audio_mp3_stream_feed(audio_mp3_stream_t *s, const uint8_t *data, size_t len)
{
    /* Loop: stream buffer send may return a short count */
    while (len > 0) {
        size_t sent = xStreamBufferSend(s->stream_buf, data, len, portMAX_DELAY);
        data += sent;
        len  -= sent;
    }
}

void audio_mp3_stream_flush(audio_mp3_stream_t *s)
{
    while (!xStreamBufferIsEmpty(s->stream_buf)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s->in_used = 0;
    ESP_LOGI(TAG, "MP3 stream done");
}
