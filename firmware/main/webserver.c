/**
 * Web Server Module
 *
 * HTTP server for configuration and OTA updates.
 */

#include "webserver.h"
#include "settings.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static const char *TAG = "webserver";
static httpd_handle_t server = NULL;

// HTML page template
static const char *HTML_PAGE =
"<!DOCTYPE html>"
"<html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Intercom Setup</title>"
"<style>"
"body{font-family:sans-serif;max-width:400px;margin:20px auto;padding:0 10px;}"
"h1{color:#333;font-size:24px;}"
"form{background:#f5f5f5;padding:20px;border-radius:8px;margin:10px 0;}"
"label{display:block;margin:10px 0 5px;font-weight:bold;}"
"input[type=text],input[type=password],input[type=number]{"
"width:100%%;padding:8px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;}"
"input[type=checkbox]{margin-right:8px;}"
"input[type=submit]{background:#007bff;color:white;border:none;padding:10px 20px;"
"border-radius:4px;cursor:pointer;margin-top:10px;}"
"input[type=submit]:hover{background:#0056b3;}"
".info{background:#e7f3ff;padding:10px;border-radius:4px;margin:10px 0;}"
".danger{background:#ffe0e0;}"
".danger input[type=submit]{background:#dc3545;}"
".row{display:flex;gap:10px;}"
".row>*{flex:1;}"
"</style></head><body>"
"<h1>Intercom Setup</h1>"
"<div class='info'>"
"<strong>Room:</strong> %s<br>"
"<strong>IP:</strong> %s<br>"
"<strong>MQTT:</strong> %s<br>"
"<strong>Version:</strong> 1.2.0 by guywithacomputer"
"</div>"
"<form action='/save' method='POST'>"
"<h3>WiFi Settings</h3>"
"<label>SSID</label><input type='text' name='ssid' value='%s'>"
"<label>Password (only fill to change WiFi)</label><input type='password' name='pass' placeholder='Leave blank to keep current'>"
"<h3>Device Settings</h3>"
"<label>Room Name</label><input type='text' name='room' value='%s' required>"
"<label>Volume (0-100)</label><input type='number' name='vol' min='0' max='100' value='%d'>"
"<h3>Home Assistant (MQTT)</h3>"
"<label><input type='checkbox' name='mqtt_en' value='1' %s>Enable MQTT</label>"
"<div class='row'>"
"<div><label>Host</label><input type='text' name='mqtt_host' value='%s' placeholder='192.168.1.x'></div>"
"<div><label>Port</label><input type='number' name='mqtt_port' value='%d'></div>"
"</div>"
"<label>Username</label><input type='text' name='mqtt_user' value='%s'>"
"<label>Password</label><input type='password' name='mqtt_pass' placeholder='Leave blank to keep current'>"
"<input type='submit' value='Save Settings'>"
"</form>"
"<form action='/update' method='POST' enctype='multipart/form-data'>"
"<h3>Firmware Update</h3>"
"<label>Select firmware.bin file</label>"
"<input type='file' name='firmware' accept='.bin'>"
"<input type='submit' value='Upload Firmware'>"
"</form>"
"<form action='/reset' method='POST' class='danger'>"
"<h3>Factory Reset</h3>"
"<input type='submit' value='Reset All Settings' onclick=\"return confirm('Are you sure?');\">"
"</form>"
"</body></html>";

static const char *HTML_SAVED =
"<!DOCTYPE html><html><head>"
"<meta http-equiv='refresh' content='3;url=/'>"
"<title>Saved</title></head><body>"
"<h1>Settings Saved!</h1><p>Rebooting...</p>"
"</body></html>";

static const char *HTML_OTA_OK =
"<!DOCTYPE html><html><head>"
"<meta http-equiv='refresh' content='10;url=/'>"
"<title>Update OK</title></head><body>"
"<h1>Firmware Updated!</h1><p>Rebooting in 10 seconds...</p>"
"</body></html>";

// Get IP address string
static void get_ip_string(char *buf, size_t len)
{
    extern void network_get_ip(char *ip_str);
    network_get_ip(buf);
}

// Get current WiFi SSID (from settings or fallback)
static const char* get_current_ssid(void)
{
    const settings_t *s = settings_get();
    if (s->configured && strlen(s->wifi_ssid) > 0) {
        return s->wifi_ssid;
    }
    // Return the fallback SSID that was used
    return "wifi";  // Must match DEFAULT_WIFI_SSID in main.c
}

// URL decode helper
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    char a, b;
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if ((*src == '%') && ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            dst[i++] = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

// Parse form value
static bool get_form_value(const char *body, const char *key, char *value, size_t value_size)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);

    const char *start = strstr(body, search);
    if (!start) return false;

    start += strlen(search);
    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    if (len >= value_size) len = value_size - 1;

    char encoded[256];
    strncpy(encoded, start, len);
    encoded[len] = '\0';

    url_decode(value, encoded, value_size);
    return true;
}

// GET / - serve config page
static esp_err_t root_handler(httpd_req_t *req)
{
    const settings_t *s = settings_get();
    char ip[16] = "0.0.0.0";
    get_ip_string(ip, sizeof(ip));

    char *html = malloc(4096);
    if (!html) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const char *mqtt_status = s->mqtt_enabled ?
        (strlen(s->mqtt_host) > 0 ? "Enabled" : "Not configured") : "Disabled";

    snprintf(html, 4096, HTML_PAGE,
             s->room_name, ip, mqtt_status,
             get_current_ssid(), s->room_name, s->volume,
             s->mqtt_enabled ? "checked" : "",
             s->mqtt_host, s->mqtt_port, s->mqtt_user);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    free(html);

    return ESP_OK;
}

// POST /save - save settings
static esp_err_t save_handler(httpd_req_t *req)
{
    char body[1024] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char ssid[64], pass[128], room[64], vol[8];
    char mqtt_host[64], mqtt_port[8], mqtt_user[32], mqtt_pass[64], mqtt_en[4];

    // Only update WiFi if password is provided (intentional change)
    if (get_form_value(body, "pass", pass, sizeof(pass)) && strlen(pass) > 0) {
        if (get_form_value(body, "ssid", ssid, sizeof(ssid))) {
            settings_set_wifi(ssid, pass);
            ESP_LOGI(TAG, "WiFi credentials updated");
        }
    }

    // Room name can be changed independently
    if (get_form_value(body, "room", room, sizeof(room))) {
        settings_set_room(room);
    }

    // Volume can be changed independently
    if (get_form_value(body, "vol", vol, sizeof(vol))) {
        settings_set_volume(atoi(vol));
    }

    // MQTT settings
    bool mqtt_enabled = get_form_value(body, "mqtt_en", mqtt_en, sizeof(mqtt_en));
    settings_set_mqtt_enabled(mqtt_enabled);

    // Update MQTT connection settings if host is provided
    if (get_form_value(body, "mqtt_host", mqtt_host, sizeof(mqtt_host)) && strlen(mqtt_host) > 0) {
        uint16_t port = 1883;
        if (get_form_value(body, "mqtt_port", mqtt_port, sizeof(mqtt_port))) {
            port = atoi(mqtt_port);
        }

        get_form_value(body, "mqtt_user", mqtt_user, sizeof(mqtt_user));

        // Only update MQTT password if provided
        const settings_t *s = settings_get();
        if (get_form_value(body, "mqtt_pass", mqtt_pass, sizeof(mqtt_pass)) && strlen(mqtt_pass) > 0) {
            settings_set_mqtt(mqtt_host, port, mqtt_user, mqtt_pass);
        } else {
            settings_set_mqtt(mqtt_host, port, mqtt_user, s->mqtt_password);
        }
    }

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_SAVED, strlen(HTML_SAVED));

    // Reboot after 1 second
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// POST /reset - factory reset
static esp_err_t reset_handler(httpd_req_t *req)
{
    settings_reset();

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_SAVED, strlen(HTML_SAVED));

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// POST /update - OTA firmware update
static esp_err_t ota_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "OTA update started, size=%d", req->content_len);

    esp_ota_handle_t ota_handle;
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);

    if (!update_partition) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char *buf = malloc(1024);
    if (!buf) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int received = 0;
    int total = req->content_len;
    bool header_skipped = false;

    while (received < total) {
        int ret = httpd_req_recv(req, buf, 1024);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }

        char *data = buf;
        int data_len = ret;

        // Skip multipart header on first chunk
        if (!header_skipped) {
            char *bin_start = memmem(buf, ret, "\r\n\r\n", 4);
            if (bin_start) {
                bin_start += 4;
                data_len = ret - (bin_start - buf);
                data = bin_start;
                header_skipped = true;
            } else {
                received += ret;
                continue;
            }
        }

        // Check for multipart boundary at end
        char *boundary = memmem(data, data_len, "\r\n------", 8);
        if (boundary) {
            data_len = boundary - data;
        }

        if (data_len > 0) {
            err = esp_ota_write(ota_handle, data, data_len);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
                break;
            }
        }

        received += ret;

        // Progress logging
        if (received % 51200 < 1024) {
            ESP_LOGI(TAG, "OTA progress: %d/%d bytes", received, total);
        }
    }

    free(buf);

    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
        return ESP_FAIL;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA end failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful!");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_OTA_OK, strlen(HTML_OTA_OK));

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

esp_err_t webserver_start(void)
{
    if (server) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register handlers
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = root_handler};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = save_handler};
    httpd_uri_t reset = {.uri = "/reset", .method = HTTP_POST, .handler = reset_handler};
    httpd_uri_t ota = {.uri = "/update", .method = HTTP_POST, .handler = ota_handler};

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &reset);
    httpd_register_uri_handler(server, &ota);

    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}

void webserver_stop(void)
{
    if (server) {
        httpd_stop(server);
        server = NULL;
        ESP_LOGI(TAG, "Web server stopped");
    }
}

bool webserver_is_running(void)
{
    return server != NULL;
}
