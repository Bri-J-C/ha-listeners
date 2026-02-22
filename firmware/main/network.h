/**
 * Network Module
 *
 * UDP multicast/unicast for audio streaming.
 */

#ifndef NETWORK_H
#define NETWORK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "protocol.h"

/**
 * Callback for received audio packets.
 */
typedef void (*network_rx_callback_t)(const audio_packet_t *packet, size_t total_len);

/**
 * Initialize network (WiFi and UDP sockets).
 * @param ssid WiFi SSID
 * @param password WiFi password
 * @return ESP_OK on success
 */
esp_err_t network_init(const char *ssid, const char *password);

/**
 * Wait for WiFi connection.
 * @param timeout_ms Timeout in milliseconds
 * @return ESP_OK if connected, ESP_ERR_TIMEOUT if timed out
 */
esp_err_t network_wait_connected(uint32_t timeout_ms);

/**
 * Check if WiFi is connected.
 */
bool network_is_connected(void);

/**
 * Check if AP mode is active (for configuration).
 */
bool network_is_ap_mode(void);

/**
 * Get local IP address as string.
 * @param ip_str Buffer to store IP string (at least 16 bytes)
 */
void network_get_ip(char *ip_str);

/**
 * Set callback for received audio packets.
 */
void network_set_rx_callback(network_rx_callback_t callback);

/**
 * Start receiving audio (joins multicast group).
 */
esp_err_t network_start_rx(void);

/**
 * Stop receiving audio.
 */
void network_stop_rx(void);

/**
 * Send audio packet via multicast (broadcast to all).
 *
 * @param packet Audio packet to send
 * @param len Total packet length
 * @return ESP_OK on success
 */
esp_err_t network_send_multicast(const audio_packet_t *packet, size_t len);

/**
 * Send audio packet via unicast (to specific IP).
 *
 * @param packet Audio packet to send
 * @param len Total packet length
 * @param dest_ip Destination IP address
 * @return ESP_OK on success
 */
esp_err_t network_send_unicast(const audio_packet_t *packet, size_t len, const char *dest_ip);

/**
 * Start mDNS service with given hostname.
 * Device will be accessible at hostname.local
 */
esp_err_t network_start_mdns(const char *hostname);

/**
 * Get TX packet statistics.
 * @param sent    Total packets successfully sent (may be NULL)
 * @param failed  Total packets that failed to send (may be NULL)
 * @param last_errno  errno from last send failure, 0 if none (may be NULL)
 */
void network_get_tx_stats(uint32_t *sent, uint32_t *failed, int *last_errno);

/**
 * Deinitialize network.
 */
void network_deinit(void);

#endif // NETWORK_H
