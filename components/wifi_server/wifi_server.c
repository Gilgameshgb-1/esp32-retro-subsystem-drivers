#include "wifi_server.h"

#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"

static const char *TAG = "wifi_server";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define MAX_RETRY           5

static EventGroupHandle_t s_wifi_events;
static int                s_retry_count;

/* -----------------------------------------------------------------------
 * Wi-Fi event handler
 * ----------------------------------------------------------------------- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *event_data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Retrying (%d/%d)", s_retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* -----------------------------------------------------------------------
 * TCP server task -- one per port, owns its config copy
 * ----------------------------------------------------------------------- */
static void tcp_server_task(void *arg)
{
    wifi_server_config_t cfg = *(wifi_server_config_t *)arg;
    free(arg);

    uint8_t rx_buf[1460];

    for (;;) {
        int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock < 0) {
            ESP_LOGE(TAG, "[%d] socket() failed: errno %d", cfg.port, errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        int opt = 1;
        setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr = {
            .sin_family      = AF_INET,
            .sin_port        = htons(cfg.port),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };

        if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
            listen(listen_sock, 1) != 0) {
            ESP_LOGE(TAG, "[%d] bind/listen failed: errno %d", cfg.port, errno);
            close(listen_sock);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "Listening on port %d", cfg.port);

        for (;;) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_sock = accept(listen_sock,
                                     (struct sockaddr *)&client_addr,
                                     &client_len);
            if (client_sock < 0) {
                ESP_LOGE(TAG, "[%d] accept() failed", cfg.port);
                break;
            }

            ESP_LOGI(TAG, "[%d] Client connected from " IPSTR,
                     cfg.port, IP2STR((esp_ip4_addr_t *)&client_addr.sin_addr));

            if (cfg.on_connect) cfg.on_connect(client_sock, cfg.cb_arg);

            int len;
            while ((len = recv(client_sock, rx_buf, sizeof(rx_buf), 0)) > 0) {
                if (cfg.on_recv) {
                    cfg.on_recv(rx_buf, (size_t)len, cfg.cb_arg);
                }
            }

            ESP_LOGI(TAG, "[%d] Client disconnected", cfg.port);
            if (cfg.on_close) cfg.on_close(cfg.cb_arg);
            close(client_sock);
        }

        close(listen_sock);
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
esp_err_t wifi_connect(const char *ssid, const char *password)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t sta_cfg = { 0 };
    strncpy((char *)sta_cfg.sta.ssid,     ssid,     sizeof(sta_cfg.sta.ssid) - 1);
    strncpy((char *)sta_cfg.sta.password, password, sizeof(sta_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Connecting to %s ...", ssid);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        /* Disable modem sleep so the radio stays awake continuously.
         * With the default WIFI_PS_MIN_MODEM the chip sleeps for 3 beacon
         * intervals (~307 ms on most APs).  During that sleep, the AP buffers
         * incoming TCP data; on wake-up the entire backlog arrives as a single
         * burst that can keep the WiFi interrupt handler busy for >300 ms and
         * trip the interrupt watchdog (IWDT) → rst:0x3 (SW_RESET).
         * WIFI_PS_NONE keeps the radio awake and makes packet delivery smooth. */
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to connect to %s", ssid);
    return ESP_FAIL;
}

esp_err_t wifi_server_start(const wifi_server_config_t *config)
{
    /* Heap-allocate a copy so each task owns its own config */
    wifi_server_config_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) return ESP_ERR_NO_MEM;
    *cfg = *config;

    uint32_t stack = cfg->stack_size ? cfg->stack_size : 4096;
    int core = (cfg->core_id == 1) ? 1 : 0;
    xTaskCreatePinnedToCore(tcp_server_task, "tcp_srv", stack, cfg, 4, NULL, core);
    return ESP_OK;
}
