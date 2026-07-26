/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Protocentral Electronics
 *
 * wifi_server.c - Multi-Client TCP Server Implementation
 *
 * Ported from the HealthyPi 6 ESP32-C6 app (app_esp32c6_esp_idf) and relicensed
 * MIT by the copyright holder.
 *
 * Accepts TCP connections on port 5000 and broadcasts biomedical
 * data packets (OpenView protocol) to all connected clients.
 * 
 * Architecture:
 *  - Main server task: accept() loop, manages client list
 *  - Send operation: Non-blocking, iterates connected clients
 *  - Client cleanup: Removes disconnected sockets from list
 */

#include "wifi_server.h"
#include "esp_log.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/inet.h"
#include <errno.h>
#include <string.h>
#include <sys/time.h>
#include <fcntl.h>

#define TAG "wifi_server"

/* ============================================================================
 * Async Send Queue - Decouples TCP send from SPI callback
 * ============================================================================
 *
 * The SPI callback runs at high priority and must not block on TCP sends.
 * We use a queue to pass data to a separate sender task that handles TCP.
 *
 * Queue item size: 800 bytes (16 packets × 50 bytes v2 format) + 4 bytes length = 804 bytes
 * Queue depth: 64 items = ~52KB buffer (holds ~2 seconds of data at 31 batches/sec)
 *
 * LOSSLESS MODE: Increased from 32 to 64 for ~2 second buffer. Combined with:
 *   - Packer queue: 16 batches = 512ms
 *   - This queue: 64 batches = 2048ms
 *   - TCP socket buffer: 128KB = ~3.5 seconds
 *   - Blocking sends: up to 500ms per batch
 * Total buffering: ~6 seconds of congestion can be absorbed without loss.
 *
 * v2 UPDATE: Packet size increased from 46 to 50 bytes (added 4-byte sequence number)
 */
#define WIFI_SEND_QUEUE_DEPTH   64
#define WIFI_SEND_QUEUE_ITEM_SIZE 804  /* Max batch size + length field (v2 format) */

typedef struct {
    uint16_t len;
    uint8_t data[802];  /* Max 16 × 50 = 800 bytes (v2 format), +2 padding */
} wifi_send_item_t;

static QueueHandle_t g_send_queue = NULL;
static TaskHandle_t g_sender_task_handle = NULL;

/* Forward declarations for sender tasks */
static void wifi_sender_task(void *arg);
static void wifi_udp_sender_task(void *arg);

/* ============================================================================
 * Client Management
 * ============================================================================
 */

typedef struct {
    int socket_fd;              /* Socket file descriptor, or -1 if inactive */
    uint32_t connect_time_ms;   /* Time connected (for tracking) */
    uint32_t packets_received;  /* Packets sent to this client */
} wifi_server_client_t;

/* ============================================================================
 * Global State
 * ============================================================================
 */

static struct {
    bool initialized;
    bool running;
    int server_socket_fd;
    TaskHandle_t server_task_handle;
    SemaphoreHandle_t clients_mutex;
    wifi_server_client_t clients[WIFI_SERVER_MAX_CLIENTS];

    /* Cached client count - updated atomically when clients connect/disconnect.
     * Allows lock-free check in hot path (wifi_server_send_packet).
     * OPTIMIZATION: Avoids mutex in SPI callback which runs at 31 Hz. */
    volatile int cached_client_count;

    /* Protocol mode: TCP (default) or UDP */
    wifi_server_mode_t mode;

    /* UDP-specific state */
    int udp_socket_fd;
    struct sockaddr_in udp_target_addr;
    bool udp_target_set;

    /* Statistics */
    uint32_t packets_sent;
    uint32_t packets_dropped;
    uint32_t bytes_sent;
    uint32_t total_connections;
} g_server = {
    .initialized = false,
    .running = false,
    .server_socket_fd = -1,
    .server_task_handle = NULL,
    .clients_mutex = NULL,
    .cached_client_count = 0,
    .mode = WIFI_SERVER_MODE_TCP,  /* Default to TCP for reliable delivery */
    .udp_socket_fd = -1,
    .udp_target_set = false,
};

/* ============================================================================
 * Helper Functions
 * ============================================================================
 */

static void client_list_init(void)
{
    for (int i = 0; i < WIFI_SERVER_MAX_CLIENTS; i++) {
        g_server.clients[i].socket_fd = -1;
        g_server.clients[i].connect_time_ms = 0;
        g_server.clients[i].packets_received = 0;
    }
}

/**
 * Add a new client to the list
 * Returns the slot index, or -1 if list is full
 */
static int client_add(int socket_fd)
{
    xSemaphoreTake(g_server.clients_mutex, portMAX_DELAY);

    for (int i = 0; i < WIFI_SERVER_MAX_CLIENTS; i++) {
        if (g_server.clients[i].socket_fd == -1) {
            g_server.clients[i].socket_fd = socket_fd;
            g_server.clients[i].connect_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
            g_server.clients[i].packets_received = 0;

            /* Update cached client count atomically */
            g_server.cached_client_count++;

            xSemaphoreGive(g_server.clients_mutex);

            ESP_LOGI(TAG, "Client %d added (socket %d), total=%d", i, socket_fd, g_server.cached_client_count);
            g_server.total_connections++;
            return i;
        }
    }

    xSemaphoreGive(g_server.clients_mutex);
    ESP_LOGW(TAG, "Client list full, rejecting socket %d", socket_fd);
    return -1;
}

/**
 * Remove a client from the list by index
 */
static __attribute__((unused)) void client_remove(int index)
{
    if (index < 0 || index >= WIFI_SERVER_MAX_CLIENTS) {
        return;
    }

    xSemaphoreTake(g_server.clients_mutex, portMAX_DELAY);

    if (g_server.clients[index].socket_fd >= 0) {
        int fd = g_server.clients[index].socket_fd;
        close(fd);
        g_server.clients[index].socket_fd = -1;

        /* Update cached client count atomically */
        if (g_server.cached_client_count > 0) {
            g_server.cached_client_count--;
        }

        ESP_LOGI(TAG, "Client %d removed (socket %d), total=%d", index, fd, g_server.cached_client_count);
    }

    xSemaphoreGive(g_server.clients_mutex);
}

/**
 * Get number of active clients
 */
static int client_count(void)
{
    xSemaphoreTake(g_server.clients_mutex, portMAX_DELAY);
    
    int count = 0;
    for (int i = 0; i < WIFI_SERVER_MAX_CLIENTS; i++) {
        if (g_server.clients[i].socket_fd >= 0) {
            count++;
        }
    }
    
    xSemaphoreGive(g_server.clients_mutex);
    return count;
}

/* ============================================================================
 * Server Task
 * ============================================================================
 */

/**
 * Main server task: Listen for connections and accept clients
 * 
 * Blocks on accept() waiting for new connections.
 * Adds accepted clients to the client list.
 */
static void wifi_server_task(void *arg)
{
    (void)arg;
    
    struct sockaddr_in addr;
    int addr_family = AF_INET;
    int ip_protocol = IPPROTO_IP;
    
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(WIFI_SERVER_PORT);
    
    /* Create server socket */
    g_server.server_socket_fd = socket(addr_family, SOCK_STREAM, ip_protocol);
    if (g_server.server_socket_fd < 0) {
        ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
        g_server.running = false;
        vTaskDelete(NULL);
        return;
    }
    
    /* Bind to port */
    if (bind(g_server.server_socket_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
        close(g_server.server_socket_fd);
        g_server.running = false;
        vTaskDelete(NULL);
        return;
    }
    
    /* Listen for connections */
    if (listen(g_server.server_socket_fd, WIFI_SERVER_BACKLOG) != 0) {
        ESP_LOGE(TAG, "Error during listen: errno %d", errno);
        close(g_server.server_socket_fd);
        g_server.running = false;
        vTaskDelete(NULL);
        return;
    }
    
    ESP_LOGI(TAG, "TCP server listening on port %d, max %d clients",
             WIFI_SERVER_PORT, WIFI_SERVER_MAX_CLIENTS);
    
    g_server.running = true;
    
    /* Accept loop */
    while (g_server.running) {
        struct sockaddr_in6 source_addr;
        socklen_t addr_len = sizeof(source_addr);
        
        int client_sock = accept(g_server.server_socket_fd,
                                 (struct sockaddr *)&source_addr,
                                 &addr_len);
        
        if (client_sock < 0) {
            if (g_server.running) {
                ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            }
            continue;
        }
        
        /* Set TCP_NODELAY to disable Nagle's algorithm for low-latency streaming */
        int flag = 1;
        if (setsockopt(client_sock, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag)) < 0) {
            ESP_LOGW(TAG, "Failed to set TCP_NODELAY: errno %d", errno);
        }

        /* Increase send buffer size for smoother streaming (128KB)
         * Larger buffer allows more data to queue before blocking */
        int sndbuf = 131072;  /* 128KB - holds ~3.5 seconds of data at 36KB/s */
        if (setsockopt(client_sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
            ESP_LOGW(TAG, "Failed to set SO_SNDBUF: errno %d", errno);
        }

        /* TCP Keepalive: Detect dead connections faster
         * Without keepalive, a dead connection may not be detected for minutes,
         * causing sends to block indefinitely. These aggressive settings detect
         * dead connections within ~15 seconds (idle 5s + 3 probes × 3s). */
        int keepalive = 1;
        int keepidle = 5;    /* Start keepalive after 5 seconds of idle */
        int keepintvl = 3;   /* Send keepalive probes every 3 seconds */
        int keepcnt = 3;     /* Close after 3 failed probes */
        setsockopt(client_sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
        setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepidle, sizeof(keepidle));
        setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepintvl, sizeof(keepintvl));
        setsockopt(client_sock, IPPROTO_TCP, TCP_KEEPCNT, &keepcnt, sizeof(keepcnt));

        /* Note: SO_SNDTIMEO not used - we use select() with timeout in send function
         * for more precise control over blocking behavior */

        /* Add to client list */
        int client_id = client_add(client_sock);
        if (client_id < 0) {
            /* List full, close immediately */
            close(client_sock);
            ESP_LOGW(TAG, "Rejected connection (list full)");
        } else {
            ESP_LOGI(TAG, "Client connected (LOSSLESS mode: 128KB buf, keepalive, blocking sends): total = %d", client_count());
        }
    }
    
    /* Cleanup on exit */
    if (g_server.server_socket_fd >= 0) {
        close(g_server.server_socket_fd);
        g_server.server_socket_fd = -1;
    }
    
    ESP_LOGI(TAG, "Server task exiting");
    vTaskDelete(NULL);
}

/* ============================================================================
 * Public API
 * ============================================================================
 */

esp_err_t wifi_server_init(void)
{
    if (g_server.initialized) {
        return ESP_OK;
    }

    if (g_server.clients_mutex == NULL) {
        g_server.clients_mutex = xSemaphoreCreateMutex();
        if (g_server.clients_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Create async send queue */
    if (g_send_queue == NULL) {
        g_send_queue = xQueueCreate(WIFI_SEND_QUEUE_DEPTH, sizeof(wifi_send_item_t));
        if (g_send_queue == NULL) {
            ESP_LOGE(TAG, "Failed to create send queue");
            return ESP_ERR_NO_MEM;
        }
    }

    client_list_init();
    g_server.initialized = true;

    ESP_LOGI(TAG, "WiFi server initialized (async queue depth=%d)", WIFI_SEND_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t wifi_server_start(void)
{
    if (!g_server.initialized) {
        ESP_LOGE(TAG, "Server not initialized. Call wifi_server_init() first.");
        return ESP_ERR_INVALID_STATE;
    }

    if (g_server.running) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    BaseType_t result;

    if (g_server.mode == WIFI_SERVER_MODE_UDP) {
        /* ===== UDP Mode: Create UDP socket and start sender task ===== */

        /* Set default broadcast target if not configured
         * Use subnet broadcast (192.168.4.255) for AP mode - more reliable than global broadcast */
        if (!g_server.udp_target_set) {
            g_server.udp_target_addr.sin_family = AF_INET;
            /* Use AP subnet broadcast (192.168.4.255) for better reliability */
            inet_aton("192.168.4.255", &g_server.udp_target_addr.sin_addr);
            g_server.udp_target_addr.sin_port = htons(WIFI_SERVER_UDP_PORT);
            ESP_LOGI(TAG, "UDP target defaulting to AP subnet broadcast (192.168.4.255:%d)", WIFI_SERVER_UDP_PORT);
        }

        /* Create UDP socket */
        g_server.udp_socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (g_server.udp_socket_fd < 0) {
            ESP_LOGE(TAG, "Failed to create UDP socket: errno %d", errno);
            return ESP_ERR_INVALID_STATE;
        }

        /* Enable broadcast if target is broadcast address */
        int broadcast_enable = 1;
        if (setsockopt(g_server.udp_socket_fd, SOL_SOCKET, SO_BROADCAST,
                       &broadcast_enable, sizeof(broadcast_enable)) < 0) {
            ESP_LOGW(TAG, "Failed to enable broadcast: errno %d", errno);
        }

        /* Set non-blocking mode */
        int flags = fcntl(g_server.udp_socket_fd, F_GETFL, 0);
        fcntl(g_server.udp_socket_fd, F_SETFL, flags | O_NONBLOCK);

        g_server.running = true;

        ESP_LOGI(TAG, "UDP server started on port %d (target: %s:%d)",
                 WIFI_SERVER_UDP_PORT,
                 inet_ntoa(g_server.udp_target_addr.sin_addr),
                 ntohs(g_server.udp_target_addr.sin_port));

        /* Start UDP sender task */
        result = xTaskCreate(
            wifi_udp_sender_task,
            "udp_sender",
            4096,
            NULL,
            7,  /* Priority 7 - higher than packer (6) and SPI (5) */
            &g_sender_task_handle
        );

        if (result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create UDP sender task");
            close(g_server.udp_socket_fd);
            g_server.udp_socket_fd = -1;
            g_server.running = false;
            return ESP_ERR_NO_MEM;
        }

    } else {
        /* ===== TCP Mode: Start TCP server and sender tasks ===== */

        result = xTaskCreate(
            &wifi_server_task,
            "wifi_server",
            WIFI_SERVER_TASK_STACK,
            NULL,
            WIFI_SERVER_TASK_PRIORITY,
            &g_server.server_task_handle
        );

        if (result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create server task");
            return ESP_ERR_NO_MEM;
        }

        /* Wait for server task to start and bind */
        int wait_ticks = 0;
        while (!g_server.running && wait_ticks < 50) {  /* 5 second timeout */
            vTaskDelay(pdMS_TO_TICKS(100));
            wait_ticks++;
        }

        if (!g_server.running) {
            ESP_LOGE(TAG, "Server failed to start");
            return ESP_ERR_INVALID_STATE;
        }

        /* Start async TCP sender task at higher priority (7) to ensure TCP keeps up
         * Priority hierarchy: SPI task (5) < Packer (6) < TCP Sender (7)
         * Sender needs highest priority to drain queue before it fills */
        result = xTaskCreate(
            wifi_sender_task,
            "wifi_sender",
            4096,
            NULL,
            7,  /* Priority 7 - higher than packer (6) and SPI (5) */
            &g_sender_task_handle
        );

        if (result != pdPASS) {
            ESP_LOGW(TAG, "Failed to create sender task - using sync sends");
            /* Non-fatal: server will still work but may block SPI */
        }
    }

    return ESP_OK;
}

void wifi_server_stop(void)
{
    if (!g_server.running) {
        return;
    }

    g_server.running = false;

    /* Close UDP socket if in UDP mode */
    if (g_server.udp_socket_fd >= 0) {
        close(g_server.udp_socket_fd);
        g_server.udp_socket_fd = -1;
    }

    /* Close TCP server socket to unblock accept() */
    if (g_server.server_socket_fd >= 0) {
        shutdown(g_server.server_socket_fd, SHUT_RDWR);
        close(g_server.server_socket_fd);
        g_server.server_socket_fd = -1;
    }

    /* Close all client sockets (TCP mode) */
    xSemaphoreTake(g_server.clients_mutex, portMAX_DELAY);
    for (int i = 0; i < WIFI_SERVER_MAX_CLIENTS; i++) {
        if (g_server.clients[i].socket_fd >= 0) {
            shutdown(g_server.clients[i].socket_fd, SHUT_RDWR);
            close(g_server.clients[i].socket_fd);
            g_server.clients[i].socket_fd = -1;
        }
    }
    /* Reset cached count */
    g_server.cached_client_count = 0;
    xSemaphoreGive(g_server.clients_mutex);

    /* Wait for tasks to exit */
    vTaskDelay(pdMS_TO_TICKS(150));
    g_server.server_task_handle = NULL;
    g_sender_task_handle = NULL;

    ESP_LOGI(TAG, "Server stopped");
}

/* ============================================================================
 * Internal Send Function (called from sender task)
 * ============================================================================
 *
 * LOSSLESS MODE: For biomedical data where every sample matters, we use
 * blocking sends with reasonable timeouts. This trades latency for reliability.
 *
 * Data flow buffering (total ~2 seconds):
 *   1. Packer queue: 16 batches = 512ms
 *   2. TCP send queue: 32 batches = 1024ms
 *   3. TCP socket buffer: 128KB = ~3.5 seconds
 *   4. This function: up to 500ms blocking per batch
 *
 * With ~2 seconds of buffering, WiFi congestion up to 2 seconds can be absorbed
 * without packet loss. Only sustained congestion will cause drops.
 */
static int wifi_server_send_packet_internal(const uint8_t *packet, size_t len)
{
    if (!g_server.running || !packet || len == 0) {
        return -1;
    }

    xSemaphoreTake(g_server.clients_mutex, portMAX_DELAY);

    int sent_count = 0;

    for (int i = 0; i < WIFI_SERVER_MAX_CLIENTS; i++) {
        if (g_server.clients[i].socket_fd < 0) {
            continue;  /* Inactive client */
        }

        /* LOSSLESS SEND: Block until all data is sent or timeout.
         * For 250 Hz streaming (4ms per sample), we can tolerate up to 500ms
         * latency spikes if it means no data loss. The queue absorbs the delay.
         *
         * Strategy:
         *   1. First try non-blocking send (fast path for normal conditions)
         *   2. If socket buffer full, wait with select() + retry
         *   3. Max 500ms total wait time before giving up
         */
        size_t total_sent = 0;
        bool send_failed = false;
        int64_t start_time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
        const int64_t max_wait_ms = 500;  /* 500ms max blocking time */

        while (total_sent < len) {
            /* Check timeout */
            int64_t elapsed_ms = (xTaskGetTickCount() * portTICK_PERIOD_MS) - start_time_ms;
            if (elapsed_ms > max_wait_ms) {
                ESP_LOGW(TAG, "Send timeout after %lldms (sent %zu/%zu bytes)", elapsed_ms, total_sent, len);
                send_failed = true;
                break;
            }

            int sent = send(g_server.clients[i].socket_fd,
                           packet + total_sent,
                           len - total_sent, MSG_DONTWAIT);

            if (sent > 0) {
                total_sent += sent;
            } else if (sent < 0) {
                int err = errno;
                if (err == EPIPE || err == ECONNRESET || err == EBADF) {
                    /* Client disconnected */
                    close(g_server.clients[i].socket_fd);
                    g_server.clients[i].socket_fd = -1;
                    if (g_server.cached_client_count > 0) {
                        g_server.cached_client_count--;
                    }
                    ESP_LOGI(TAG, "Client %d disconnected (errno %d), total=%d", i, err, g_server.cached_client_count);
                    send_failed = true;
                    break;
                } else if (err == EAGAIN || err == EWOULDBLOCK) {
                    /* Socket buffer full - use select() to wait for writability
                     * This is more efficient than polling with vTaskDelay */
                    fd_set write_fds;
                    FD_ZERO(&write_fds);
                    FD_SET(g_server.clients[i].socket_fd, &write_fds);

                    /* Wait up to 50ms for socket to become writable */
                    struct timeval tv = { .tv_sec = 0, .tv_usec = 50000 };  /* 50ms */
                    int sel_ret = select(g_server.clients[i].socket_fd + 1, NULL, &write_fds, NULL, &tv);

                    if (sel_ret < 0) {
                        ESP_LOGW(TAG, "select() failed: errno %d", errno);
                        send_failed = true;
                        break;
                    }
                    /* sel_ret == 0 means timeout, loop will retry */
                    /* sel_ret > 0 means socket is writable, loop will retry send */
                } else {
                    ESP_LOGW(TAG, "Send to client %d failed: errno %d", i, err);
                    send_failed = true;
                    break;
                }
            }
        }

        if (send_failed || total_sent < len) {
            g_server.packets_dropped++;
        } else {
            /* Full send successful */
            sent_count++;
            g_server.clients[i].packets_received++;
            g_server.packets_sent++;
            g_server.bytes_sent += total_sent;
        }
    }

    xSemaphoreGive(g_server.clients_mutex);
    return sent_count;
}

/* ============================================================================
 * Async Sender Task
 * ============================================================================
 *
 * Runs in a separate task to avoid blocking SPI processing.
 * Receives batches from queue and sends them to all connected clients.
 */
static void wifi_sender_task(void *arg)
{
    (void)arg;
    wifi_send_item_t item;
    uint32_t batches_sent = 0;
    uint32_t last_stats_time = 0;

    ESP_LOGI(TAG, "WiFi sender task started (queue depth=%d)", WIFI_SEND_QUEUE_DEPTH);

    while (g_server.running) {
        /* Block waiting for first item (short timeout for responsiveness) */
        if (xQueueReceive(g_send_queue, &item, pdMS_TO_TICKS(10)) == pdTRUE) {
            wifi_server_send_packet_internal(item.data, item.len);
            batches_sent++;

            /* Drain remaining items without blocking (burst mode)
             * This reduces task switch overhead when queue backs up */
            while (xQueueReceive(g_send_queue, &item, 0) == pdTRUE) {
                wifi_server_send_packet_internal(item.data, item.len);
                batches_sent++;
            }
        }

        /* Log stats every 5 seconds */
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_stats_time >= 5000) {
            UBaseType_t queue_waiting = uxQueueMessagesWaiting(g_send_queue);
            ESP_LOGI(TAG, "📊 Sender: %lu batches/5s, queue=%u/%d, dropped=%lu",
                     batches_sent, (unsigned)queue_waiting, WIFI_SEND_QUEUE_DEPTH,
                     g_server.packets_dropped);
            batches_sent = 0;
            last_stats_time = now;
        }
    }

    ESP_LOGI(TAG, "WiFi sender task exiting");
    vTaskDelete(NULL);
}

/* ============================================================================
 * UDP Send Implementation
 * ============================================================================
 *
 * UDP provides lower latency than TCP by eliminating:
 *  - Connection establishment (3-way handshake)
 *  - Acknowledgments and retransmission
 *  - Flow control and congestion control
 *  - In-order delivery guarantees
 *
 * For biomedical streaming at 500 Hz, occasional packet loss is acceptable
 * (interpolation can fill gaps), but latency spikes are problematic.
 */
static int wifi_server_send_udp_internal(const uint8_t *packet, size_t len)
{
    if (!g_server.running || g_server.udp_socket_fd < 0 || !packet || len == 0) {
        return -1;
    }

    /* Send UDP datagram - fire and forget, no acknowledgment */
    int sent = sendto(g_server.udp_socket_fd, packet, len, MSG_DONTWAIT,
                      (struct sockaddr *)&g_server.udp_target_addr,
                      sizeof(g_server.udp_target_addr));

    if (sent > 0) {
        g_server.packets_sent++;
        g_server.bytes_sent += sent;
        return 1;
    } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGD(TAG, "UDP send failed: errno %d", errno);
        }
        g_server.packets_dropped++;
        return -1;
    }
}

/* UDP sender task - similar to TCP but simpler (no client management) */
static void wifi_udp_sender_task(void *arg)
{
    (void)arg;
    wifi_send_item_t item;
    uint32_t batches_sent = 0;
    uint32_t last_stats_time = 0;

    ESP_LOGI(TAG, "UDP sender task started (target=%s:%d, queue depth=%d)",
             inet_ntoa(g_server.udp_target_addr.sin_addr),
             ntohs(g_server.udp_target_addr.sin_port),
             WIFI_SEND_QUEUE_DEPTH);

    while (g_server.running) {
        /* Block waiting for first item */
        if (xQueueReceive(g_send_queue, &item, pdMS_TO_TICKS(10)) == pdTRUE) {
            wifi_server_send_udp_internal(item.data, item.len);
            batches_sent++;

            /* Drain remaining items without blocking (burst mode) */
            while (xQueueReceive(g_send_queue, &item, 0) == pdTRUE) {
                wifi_server_send_udp_internal(item.data, item.len);
                batches_sent++;
            }
        }

        /* Log stats every 5 seconds */
        uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
        if (now - last_stats_time >= 5000) {
            UBaseType_t queue_waiting = uxQueueMessagesWaiting(g_send_queue);
            ESP_LOGI(TAG, "📊 UDP Sender: %lu batches/5s, queue=%u/%d, dropped=%lu",
                     batches_sent, (unsigned)queue_waiting, WIFI_SEND_QUEUE_DEPTH,
                     g_server.packets_dropped);
            batches_sent = 0;
            last_stats_time = now;
        }
    }

    ESP_LOGI(TAG, "UDP sender task exiting");
    vTaskDelete(NULL);
}

/* ============================================================================
 * Public API: Queue-based async send
 * ============================================================================
 */
int wifi_server_send_packet(const uint8_t *packet, size_t len)
{
    if (!g_server.running || !packet || len == 0 || g_send_queue == NULL) {
        return -1;
    }

    /* In TCP mode, skip if no clients connected.
     * In UDP mode, always send (broadcast doesn't require connected clients). */
    if (g_server.mode == WIFI_SERVER_MODE_TCP && g_server.cached_client_count == 0) {
        return 0;
    }

    /* Validate length fits in queue item */
    if (len > sizeof(((wifi_send_item_t *)0)->data)) {
        ESP_LOGW(TAG, "Packet too large for queue: %zu > %zu", len, sizeof(((wifi_send_item_t *)0)->data));
        g_server.packets_dropped++;
        return -1;
    }

    /* Queue the packet for async send (non-blocking) */
    wifi_send_item_t item;
    item.len = (uint16_t)len;
    memcpy(item.data, packet, len);

    if (xQueueSend(g_send_queue, &item, 0) != pdTRUE) {
        /* Queue full - drop packet */
        g_server.packets_dropped++;
        ESP_LOGD(TAG, "Send queue full, dropping packet (%zu bytes)", len);
        return -1;
    }

    return 1;  /* Queued successfully */
}

int wifi_server_get_connected_clients(void)
{
    /* Safety check: if server not initialized, return 0 */
    if (!g_server.initialized) {
        return 0;
    }
    /* Use cached count for lock-free access in hot path (SPI callback @ 31 Hz)
     * This is a critical optimization - avoids mutex contention that was causing
     * 5-10% packet loss. The cached count is updated atomically on connect/disconnect. */
    return g_server.cached_client_count;
}

bool wifi_server_is_running(void)
{
    return g_server.running;
}

void wifi_server_get_stats(uint32_t *packets_sent, uint32_t *packets_dropped,
                           uint32_t *bytes_sent, uint32_t *connect_count)
{
    if (packets_sent) *packets_sent = g_server.packets_sent;
    if (packets_dropped) *packets_dropped = g_server.packets_dropped;
    if (bytes_sent) *bytes_sent = g_server.bytes_sent;
    if (connect_count) *connect_count = g_server.total_connections;
}

void wifi_server_log_stats(void)
{
    int active_clients = client_count();
    const char *mode_str = (g_server.mode == WIFI_SERVER_MODE_UDP) ? "UDP" : "TCP";
    ESP_LOGI(TAG, "📊 Stats [%s]: sent=%lu dropped=%lu bytes=%lu clients=%d/%d total_conn=%lu",
             mode_str,
             g_server.packets_sent,
             g_server.packets_dropped,
             g_server.bytes_sent,
             active_clients,
             WIFI_SERVER_MAX_CLIENTS,
             g_server.total_connections);
}

/* ============================================================================
 * Mode Control API
 * ============================================================================
 */

esp_err_t wifi_server_set_mode(wifi_server_mode_t mode)
{
    if (g_server.running) {
        ESP_LOGE(TAG, "Cannot change mode while server is running");
        return ESP_ERR_INVALID_STATE;
    }

    g_server.mode = mode;
    ESP_LOGI(TAG, "Server mode set to %s", mode == WIFI_SERVER_MODE_UDP ? "UDP" : "TCP");
    return ESP_OK;
}

wifi_server_mode_t wifi_server_get_mode(void)
{
    return g_server.mode;
}

esp_err_t wifi_server_set_udp_target(const char *ip_str)
{
    if (!ip_str) {
        return ESP_ERR_INVALID_ARG;
    }

    struct in_addr addr;
    if (inet_aton(ip_str, &addr) == 0) {
        ESP_LOGE(TAG, "Invalid IP address: %s", ip_str);
        return ESP_ERR_INVALID_ARG;
    }

    g_server.udp_target_addr.sin_family = AF_INET;
    g_server.udp_target_addr.sin_addr = addr;
    g_server.udp_target_addr.sin_port = htons(WIFI_SERVER_UDP_PORT);
    g_server.udp_target_set = true;

    ESP_LOGI(TAG, "UDP target set to %s:%d", ip_str, WIFI_SERVER_UDP_PORT);
    return ESP_OK;
}
