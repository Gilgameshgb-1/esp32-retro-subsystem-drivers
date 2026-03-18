#pragma once
// No include guards :D

#include <stdint.h>

#define ILI9341_WIDTH   240
#define ILI9341_HEIGHT  320

/**
 * @brief Pin and bus configuration for the ILI9341 driver.
 *        Pass this to ili9341_init()
 */
typedef struct {
    int pin_mosi;
    int pin_clk;
    int pin_cs;
    int pin_dc;
    int pin_rst;
    int spi_clock_hz;   /*!< SPI clock frequency in Hz, e.g. 26 * 1000 * 1000 */
} ili9341_config_t;

typedef enum {
    ILI9341_ROTATION_0   = 0x48,  /*!< Portrait,           240x320 */
    ILI9341_ROTATION_90  = 0x28,  /*!< Landscape,          320x240 */
    ILI9341_ROTATION_180 = 0x88,  /*!< Portrait  flipped,  240x320 */
    ILI9341_ROTATION_270 = 0xE8,  /*!< Landscape flipped,  320x240 */
} ili9341_rotation_t;

/**
 * @brief Initialize the ILI9341 display.
 *        Configures GPIO, SPI bus, SPI device, and runs the LCD init sequence.
 *
 * @param config  Pointer to a populated ili9341_config_t struct.
 */
void ili9341_init(const ili9341_config_t *config);

/**
 * @brief Fill a rectangular region with a single RGB565 color.
 */
void ili9341_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);

/**
 * @brief Fill the entire screen with a single RGB565 color.
 */
void ili9341_fill_screen(uint16_t color);

/**
 * @brief Set display rotation. Updates internal width/height so all drawing
 *        functions work correctly after the call.
 */
void ili9341_set_rotation(ili9341_rotation_t rotation);

/**
 * @brief Draw a raw RGB565 image from a byte buffer stored in flash.
 *        Bytes must be in big-endian order (high byte first) as produced
 *        by the img2c.py conversion script.
 *
 * @param x, y   Top-left corner on the display.
 * @param w, h   Width and height of the image in pixels.
 * @param data   Pointer to RGB565 pixel data (w * h * 2 bytes).
 */
void ili9341_draw_image(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint8_t *data);

/**
 * @brief Pack 8-bit R, G, B components into a 16-bit RGB565 value.
 */
static inline uint16_t ili9341_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8) << 8) |
           ((uint16_t)(g & 0xFC) << 3) |
           ((uint16_t) b          >> 3);
}
