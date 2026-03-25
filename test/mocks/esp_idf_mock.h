#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/*
 * Minimal mocks for ESP-IDF types and functions so we can compile
 * and test ili9341.c on the host without the real ESP-IDF SDK.
 */

/* ---- esp_err.h ---- */
typedef int esp_err_t;
#define ESP_OK   0
#define ESP_FAIL -1
#define ESP_ERROR_CHECK(x) (void)(x)

/* ---- gpio.h ---- */
#define GPIO_MODE_OUTPUT       1
#define GPIO_PULLUP_DISABLE    0
#define GPIO_PULLDOWN_DISABLE  0
#define GPIO_INTR_DISABLE      0

typedef struct {
    uint64_t pin_bit_mask;
    int      mode;
    int      pull_up_en;
    int      pull_down_en;
    int      intr_type;
} gpio_config_t;

esp_err_t gpio_config(const gpio_config_t *cfg);
esp_err_t gpio_set_level(int gpio_num, uint32_t level);

/* ---- spi_master.h ---- */
#define SPI2_HOST          1
#define SPI_DMA_CH_AUTO    3

typedef void *spi_device_handle_t;

typedef struct {
    int mosi_io_num;
    int miso_io_num;
    int sclk_io_num;
    int quadwp_io_num;
    int quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;

typedef struct {
    int  clock_speed_hz;
    int  mode;
    int  spics_io_num;
    int  queue_size;
} spi_device_interface_config_t;

typedef struct {
    uint32_t       flags;
    uint16_t       cmd;
    uint64_t       addr;
    size_t         length;      /* in bits */
    size_t         rxlength;
    void          *user;
    union {
        const void *tx_buffer;
        uint8_t     tx_data[4];
    };
    union {
        void    *rx_buffer;
        uint8_t  rx_data[4];
    };
} spi_transaction_t;

esp_err_t spi_bus_initialize(int host, const spi_bus_config_t *cfg, int dma_chan);
esp_err_t spi_bus_add_device(int host, const spi_device_interface_config_t *cfg,
                             spi_device_handle_t *handle);
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *t);
esp_err_t spi_device_queue_trans(spi_device_handle_t handle, spi_transaction_t *t, int ticks);
esp_err_t spi_device_get_trans_result(spi_device_handle_t handle, spi_transaction_t **t, int ticks);

/* ---- FreeRTOS ---- */
#define portMAX_DELAY  0xFFFFFFFF
#define pdMS_TO_TICKS(ms) (ms)

static inline void vTaskDelay(uint32_t ticks) { (void)ticks; }

/* ---- esp_log.h ---- */
#define ESP_LOGI(tag, fmt, ...) (void)0
#define ESP_LOGE(tag, fmt, ...) (void)0

/* ---- I2S ---- */
#define I2S_NUM_0           0
#define I2S_ROLE_MASTER     0
#define I2S_GPIO_UNUSED     (-1)
#define I2S_DATA_BIT_WIDTH_16BIT  16
#define I2S_SLOT_MODE_MONO        1

typedef void *i2s_chan_handle_t;

typedef struct {
    int id;
    int role;
    int dma_desc_num;
    int dma_frame_num;
    int auto_clear;
} i2s_chan_config_t;

#define I2S_CHANNEL_DEFAULT_CONFIG(i2s_id, i2s_role) { .id = i2s_id, .role = i2s_role }

typedef struct { int sample_rate_hz; int clk_src; int mclk_multiple; } i2s_std_clk_config_t;
typedef struct { int data_bit_width; int slot_bit_width; int slot_mode; int slot_mask; } i2s_std_slot_config_t;
typedef struct {
    int mclk; int bclk; int ws; int dout; int din;
    struct { int mclk_inv; int bclk_inv; int ws_inv; } invert_flags;
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t  clk_cfg;
    i2s_std_slot_config_t slot_cfg;
    i2s_std_gpio_config_t gpio_cfg;
} i2s_std_config_t;

#define I2S_STD_CLK_DEFAULT_CONFIG(rate) { .sample_rate_hz = rate }
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) { .data_bit_width = bits, .slot_mode = mode }

esp_err_t i2s_new_channel(const i2s_chan_config_t *cfg, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *cfg);
esp_err_t i2s_channel_enable(i2s_chan_handle_t handle);
esp_err_t i2s_channel_write(i2s_chan_handle_t handle, const void *src, size_t size,
                            size_t *bytes_written, uint32_t timeout);

/* ---- Mock tracking ---- */

/* Maximum number of SPI transactions we record */
#define MOCK_MAX_TRANSACTIONS 512

typedef struct {
    size_t      length;      /* in bits, as passed to the driver */
    const void *tx_buffer;
} mock_spi_record_t;

typedef struct {
    /* Polling transaction log (commands, small data) */
    mock_spi_record_t polled[MOCK_MAX_TRANSACTIONS];
    int               polled_count;

    /* Queued/async transaction log (bulk pixel data) */
    mock_spi_record_t queued[MOCK_MAX_TRANSACTIONS];
    int               queued_count;

    /* GPIO tracking */
    int               last_dc_level;
    int               gpio_set_level_count;

    /* Drain tracking */
    int               get_result_count;

    /* I2S tracking */
    int               i2s_init_count;
    size_t            i2s_bytes_written;
    int               i2s_write_count;
    int               i2s_sample_rate;
} mock_state_t;

/* Global mock state -- reset before each test */
extern mock_state_t g_mock;

void mock_reset(void);
