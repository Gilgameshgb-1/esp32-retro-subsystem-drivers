#include "ili9341.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "ili9341";

/* Module-private state */
static spi_device_handle_t s_spi;
static int                 s_pin_dc;
static int                 s_pin_rst;
static uint16_t            s_width  = ILI9341_WIDTH;
static uint16_t            s_height = ILI9341_HEIGHT;

/* --------------------------------------------------------------------------
 * Low-level SPI helpers
 * -------------------------------------------------------------------------- */

// Send a command byte DC has to be low for commands
static void lcd_cmd(uint8_t cmd)
{
    spi_transaction_t t = {
        .length    = 8,
        .tx_buffer = &cmd,
    };
    gpio_set_level(s_pin_dc, 0);
    spi_device_polling_transmit(s_spi, &t);
}

// Send data bytes, DC has to be high for data
static void lcd_data(const uint8_t *data, int len)
{
    if (len == 0) return;
    spi_transaction_t t = {
        .length    = len * 8,
        .tx_buffer = data,
    };
    gpio_set_level(s_pin_dc, 1);
    spi_device_polling_transmit(s_spi, &t);
}

// Helper for a single byte of data
static void lcd_data_byte(uint8_t val)
{
    lcd_data(&val, 1);
}

/* --------------------------------------------------------------------------
 * Hardware control
 * -------------------------------------------------------------------------- */

static void lcd_reset(void)
{
    gpio_set_level(s_pin_rst, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(s_pin_rst, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void lcd_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    lcd_cmd(0x2A);  /* Column address set, datasheet info */
    uint8_t col[] = { x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF };
    lcd_data(col, 4);

    lcd_cmd(0x2B);  /* Row address set, datasheet info */
    uint8_t row[] = { y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF };
    lcd_data(row, 4);

    lcd_cmd(0x2C);  /* Memory write */
}

/* --------------------------------------------------------------------------
 * Initialization
 * -------------------------------------------------------------------------- */

static void gpio_driver_init(int pin_dc, int pin_rst)
{
    gpio_config_t io_conf = {
        .pin_bit_mask  = (1ULL << pin_dc) | (1ULL << pin_rst),
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
}

static void spi_driver_init(const ili9341_config_t *cfg)
{
    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = cfg->pin_mosi,
        .miso_io_num     = -1,
        .sclk_io_num     = cfg->pin_clk,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = ILI9341_HEIGHT * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {
        .clock_speed_hz = cfg->spi_clock_hz,
        .mode           = 0,
        .spics_io_num   = cfg->pin_cs,
        .queue_size     = 7,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &s_spi));

    ESP_LOGI(TAG, "SPI initialized at %d Hz", cfg->spi_clock_hz);
}

static void lcd_init_sequence(void)
{
    lcd_reset();

    lcd_cmd(0x01);                      /* Software reset */
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd(0x11);                      /* Sleep out */
    vTaskDelay(pdMS_TO_TICKS(150));

    lcd_cmd(0x3A); lcd_data_byte(0x55); /* Pixel format: 16-bit RGB565 */
    lcd_cmd(0x36); lcd_data_byte(0x48); /* Memory access control: RGB order */

    lcd_cmd(0x29);                      /* Display on */
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "ILI9341 initialized");
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

void ili9341_init(const ili9341_config_t *config)
{
    s_pin_dc  = config->pin_dc;
    s_pin_rst = config->pin_rst;

    gpio_driver_init(config->pin_dc, config->pin_rst);
    spi_driver_init(config);
    lcd_init_sequence();
}

//fill_rect is mostly a test function to check simple things like the display working
//there is no checking of the parameters, so if you set x+w > width it will just write to the next line and etc
void ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    lcd_set_window(x, y, x + w - 1, y + h - 1);

    uint8_t line_buf[ILI9341_HEIGHT * 2];
    for (int i = 0; i < w; i++) {
        line_buf[i * 2]     = color >> 8;
        line_buf[i * 2 + 1] = color & 0xFF;
    }

    gpio_set_level(s_pin_dc, 1);
    for (int row = 0; row < h; row++) {
        spi_transaction_t t = {
            .length    = w * 16,
            .tx_buffer = line_buf,
        };
        spi_device_polling_transmit(s_spi, &t);
    }
}

//also just a testing function for screen filling
void ili9341_fill_screen(uint16_t color)
{
    ili9341_fill_rect(0, 0, s_width, s_height, color);
}

// potentially not working very well
void ili9341_set_rotation(ili9341_rotation_t rotation)
{
    lcd_cmd(0x36);
    lcd_data_byte((uint8_t)rotation);

    if (rotation == ILI9341_ROTATION_90 || rotation == ILI9341_ROTATION_270) {
        s_width  = ILI9341_HEIGHT;
        s_height = ILI9341_WIDTH;
    } else {
        s_width  = ILI9341_WIDTH;
        s_height = ILI9341_HEIGHT;
    }
}

void ili9341_draw_image(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *data)
{
    lcd_set_window(x, y, x + w - 1, y + h - 1);
    gpio_set_level(s_pin_dc, 1);

    int row_bytes = w * 2;
    for (int row = 0; row < h; row++) {
        // Transaction is defined as follows:
        // currently we hand over a pointer to the start of the
        // entire image data and then what we just do we add to the pointer
        // the length of the buffer we send to the display
        // so in the first loop we send the first row (0*row_bytes) + pointer to pos 0
        // afterwards we move by 1 and etc until all of the frames are sent
        spi_transaction_t t = {
            .length    = row_bytes * 8,
            .tx_buffer = data + row * row_bytes,
        };
        spi_device_polling_transmit(s_spi, &t);
    }
}
