#include "audio.h"

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"

static const char *TAG = "audio";

static i2s_chan_handle_t s_tx_chan;

void audio_init(const audio_config_t *config)
{
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

void audio_write(const int16_t *data, size_t len)
{
    const uint8_t *ptr = (const uint8_t *)data;
    while (len > 0) {
        size_t written = 0;
        i2s_channel_write(s_tx_chan, ptr, len, &written, portMAX_DELAY);
        ptr += written;
        len -= written;
    }
}
