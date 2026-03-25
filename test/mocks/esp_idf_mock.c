#include "esp_idf_mock.h"

mock_state_t g_mock;

void mock_reset(void)
{
    memset(&g_mock, 0, sizeof(g_mock));
}

/* ---- GPIO mocks ---- */

esp_err_t gpio_config(const gpio_config_t *cfg)
{
    (void)cfg;
    return ESP_OK;
}

esp_err_t gpio_set_level(int gpio_num, uint32_t level)
{
    (void)gpio_num;
    g_mock.last_dc_level = (int)level;
    g_mock.gpio_set_level_count++;
    return ESP_OK;
}

/* ---- SPI mocks ---- */

static int s_dummy_handle;

esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *cfg, int dma_chan)
{
    (void)host; (void)cfg; (void)dma_chan;
    return ESP_OK;
}

esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *cfg,
                             spi_device_handle_t *handle)
{
    (void)host; (void)cfg;
    *handle = &s_dummy_handle;
    return ESP_OK;
}

esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *t)
{
    (void)handle;
    if (g_mock.polled_count < MOCK_MAX_TRANSACTIONS) {
        g_mock.polled[g_mock.polled_count].length    = t->length;
        g_mock.polled[g_mock.polled_count].tx_buffer = t->tx_buffer;
        g_mock.polled_count++;
    }
    return ESP_OK;
}

esp_err_t spi_device_queue_trans(spi_device_handle_t handle, spi_transaction_t *t, int ticks)
{
    (void)handle; (void)ticks;
    if (g_mock.queued_count < MOCK_MAX_TRANSACTIONS) {
        g_mock.queued[g_mock.queued_count].length    = t->length;
        g_mock.queued[g_mock.queued_count].tx_buffer = t->tx_buffer;
        g_mock.queued_count++;
    }
    return ESP_OK;
}

esp_err_t spi_device_get_trans_result(spi_device_handle_t handle, spi_transaction_t **t, int ticks)
{
    (void)handle; (void)ticks;
    *t = NULL;
    g_mock.get_result_count++;
    return ESP_OK;
}

/* ---- I2S mocks ---- */

static int s_dummy_i2s_handle;

esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    (void)cfg;
    if (tx) *tx = &s_dummy_i2s_handle;
    if (rx) *rx = NULL;
    g_mock.i2s_init_count++;
    return ESP_OK;
}

esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *cfg)
{
    (void)handle;
    g_mock.i2s_sample_rate = cfg->clk_cfg.sample_rate_hz;
    return ESP_OK;
}

esp_err_t i2s_channel_enable(i2s_chan_handle_t handle)
{
    (void)handle;
    return ESP_OK;
}

esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *src, size_t size,
                            size_t *bytes_written, uint32_t timeout)
{
    (void)handle; (void)src; (void)timeout;
    *bytes_written = size;
    g_mock.i2s_bytes_written += size;
    g_mock.i2s_write_count++;
    return ESP_OK;
}
