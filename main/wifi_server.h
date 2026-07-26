/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Protocentral Electronics
 *
 * wifi_server.h - Multi-Client TCP Server for HealthyPi 6
 *
 * Ported from the HealthyPi 6 ESP32-C6 app (app_esp32c6_esp_idf) and relicensed
 * MIT by the copyright holder.
 *
 * Broadcasts OpenView protocol packets to multiple connected clients
 * over TCP on port 5000.
 * 
 * Features:
 * - Up to 4 simultaneous client connections
 * - Non-blocking broadcast to all clients
 * - Automatic client disconnection on send failure
 * - Graceful startup/shutdown
 * - Thread-safe client list management
 */

#ifndef WIFI_SERVER_H
#define WIFI_SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Server configuration */
#define WIFI_SERVER_PORT        5000
#define WIFI_SERVER_UDP_PORT    5001    /* UDP broadcast port */
#define WIFI_SERVER_MAX_CLIENTS 4
#define WIFI_SERVER_BACKLOG     WIFI_SERVER_MAX_CLIENTS
#define WIFI_SERVER_TASK_STACK  4096
#define WIFI_SERVER_TASK_PRIORITY 5

/* Protocol mode selection */
typedef enum {
    WIFI_SERVER_MODE_TCP = 0,   /* TCP streaming (default, reliable) */
    WIFI_SERVER_MODE_UDP = 1,   /* UDP broadcast (low-latency, fire-and-forget) */
} wifi_server_mode_t;

/**
 * Initialize the TCP server
 * 
 * Must be called before wifi_server_start().
 * Initializes internal data structures and creates server resources.
 * 
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
esp_err_t wifi_server_init(void);

/**
 * Start the TCP server
 * 
 * Spawns the server task to listen for client connections.
 * Can only be called after wifi_server_init().
 * 
 * @return ESP_OK on success, ESP_ERR_* on failure
 */
esp_err_t wifi_server_start(void);

/**
 * Stop the TCP server
 * 
 * Closes all client connections and stops listening.
 * Server can be restarted with wifi_server_start() after calling this.
 */
void wifi_server_stop(void);

/**
 * Send a packet to all connected clients
 * 
 * Broadcasts the packet to all active client connections.
 * Silently drops clients that fail to receive (e.g., disconnected).
 * 
 * Non-blocking: Returns immediately after queuing sends.
 * 
 * @param[in] packet Data buffer to send
 * @param[in] len    Number of bytes to send
 * 
 * @return Number of clients successfully sent to, or -1 on error
 */
int wifi_server_send_packet(const uint8_t *packet, size_t len);

/**
 * Get number of currently connected clients
 * 
 * @return Number of active TCP connections (0 to WIFI_SERVER_MAX_CLIENTS)
 */
int wifi_server_get_connected_clients(void);

/**
 * Check if server is currently running
 * 
 * @return true if accepting connections, false otherwise
 */
bool wifi_server_is_running(void);

/**
 * Get server statistics
 * 
 * @param[out] packets_sent    Total packets sent to any client
 * @param[out] packets_dropped Total packets failed to send
 * @param[out] bytes_sent      Total bytes transmitted
 * @param[out] connect_count   Total connections accepted (cumulative)
 */
void wifi_server_get_stats(uint32_t *packets_sent, uint32_t *packets_dropped,
                           uint32_t *bytes_sent, uint32_t *connect_count);

/**
 * Print server statistics to log (ESP_LOGI)
 *
 * Called periodically to monitor health.
 */
void wifi_server_log_stats(void);

/**
 * Set server protocol mode
 *
 * Must be called before wifi_server_start(), or call wifi_server_stop() first.
 * Default is TCP mode.
 *
 * @param[in] mode WIFI_SERVER_MODE_TCP or WIFI_SERVER_MODE_UDP
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if server is running
 */
esp_err_t wifi_server_set_mode(wifi_server_mode_t mode);

/**
 * Get current protocol mode
 *
 * @return Current mode (TCP or UDP)
 */
wifi_server_mode_t wifi_server_get_mode(void);

/**
 * Set UDP target address for broadcast
 *
 * In UDP mode, packets are sent to this address instead of waiting for clients.
 * Default is broadcast (255.255.255.255).
 *
 * @param[in] ip_str IP address string (e.g., "192.168.1.255" for subnet broadcast)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if invalid IP
 */
esp_err_t wifi_server_set_udp_target(const char *ip_str);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SERVER_H */
