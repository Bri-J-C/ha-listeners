/**
 * Web Server Module
 *
 * HTTP server for configuration and OTA updates.
 */

#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <stdbool.h>
#include "esp_err.h"

/**
 * Start the web server.
 * Serves config page at / and handles OTA at /update
 */
esp_err_t webserver_start(void);

/**
 * Stop the web server.
 */
void webserver_stop(void);

/**
 * Check if web server is running.
 */
bool webserver_is_running(void);

#endif // WEBSERVER_H
