#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ili9341.h"
#include "image.h"

static const ili9341_config_t display_cfg = {
    .pin_mosi     = 23,
    .pin_clk      = 18,
    .pin_cs       =  5,
    .pin_dc       =  2,
    .pin_rst      =  4,
    .spi_clock_hz = 26 * 1000 * 1000,
};

void app_main(void)
{
    ili9341_init(&display_cfg);
    ili9341_fill_screen(ili9341_rgb565(0, 0, 0));
    ili9341_draw_image(0, 0, IMAGE_WIDTH, IMAGE_HEIGHT, image);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
