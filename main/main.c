/* Blink Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "example";

#define BLINK_GPIO 2
#define BUTTON_GPIO 4

static uint8_t s_led_state = 0;
static volatile bool button_pressed = false;
static volatile bool blink_in_progress = false;

static void blink_led(void)
{
    gpio_set_level(BLINK_GPIO, s_led_state);
}

static void configure_led(void)
{
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
}

static void IRAM_ATTR button_isr_handler(void* arg)
{
    if (!blink_in_progress) {
        button_pressed = true;
    }
}

static void configure_button(void)
{
    gpio_reset_pin(BUTTON_GPIO);
    gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
    gpio_pullup_en(BUTTON_GPIO);
    gpio_install_isr_service(0);
    gpio_set_intr_type(BUTTON_GPIO, GPIO_INTR_NEGEDGE);
    gpio_isr_handler_add(BUTTON_GPIO, button_isr_handler, NULL);
}

void app_main(void)
{
    configure_led();
    configure_button();

    while (1) {
        if (button_pressed) {
            button_pressed = false;
            blink_in_progress = true;

            s_led_state = 1;
            blink_led();
            vTaskDelay(pdMS_TO_TICKS(200)); // pdMS_TO_TICKS(200) - freertos macro 
            
            s_led_state = 0;
            blink_led();
            vTaskDelay(pdMS_TO_TICKS(200));
            
            blink_in_progress = false;
        }        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
