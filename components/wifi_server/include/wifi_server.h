#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

/**
 * @brief Callback invoked when a chunk of data arrives over TCP.
 */
typedef void (*wifi_server_recv_cb_t)(const uint8_t *data, size_t len, void *arg);

/**
 * @brief Callback invoked when a client connects.  @p sock is the accepted
 *        socket fd — the callee may store it to call shutdown() later, but
 *        must NOT close() it (the server task owns the socket lifecycle).
 */
typedef void (*wifi_server_connect_cb_t)(int sock, void *arg);

/**
 * @brief Callback invoked when a client disconnects.
 */
typedef void (*wifi_server_close_cb_t)(void *arg);

/**
 * @brief TCP server configuration. One instance per port.
 */
typedef struct {
    uint16_t                  port;
    wifi_server_connect_cb_t  on_connect; /*!< Called on client connect. May be NULL. */
    wifi_server_recv_cb_t     on_recv;
    wifi_server_close_cb_t    on_close;   /*!< Called on client disconnect. May be NULL. */
    void                     *cb_arg;
    uint32_t                  stack_size; /*!< Task stack in bytes. 0 = default 4096 */
    int                       core_id;    /*!< CPU core to pin TCP task. 0 or 1. */
} wifi_server_config_t;

/**
 * @brief Connect to Wi-Fi in station mode. Call once before wifi_server_start().
 *        Blocks until IP is obtained or all retries are exhausted.
 */
esp_err_t wifi_connect(const char *ssid, const char *password);

/**
 * @brief Start a TCP server on the given port.
 *        Can be called multiple times to open multiple ports simultaneously.
 *        Each call spawns an independent server task.
 */
esp_err_t wifi_server_start(const wifi_server_config_t *config);
