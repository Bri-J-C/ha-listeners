/**
 * Web Server Module
 *
 * HTTP server for configuration and OTA updates.
 * Includes HTTP Basic Authentication and CSRF protection.
 */

#include "webserver.h"
#include "settings.h"
#include "diagnostics.h"
#include "protocol.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_random.h"
#include "mbedtls/base64.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

static const char *TAG = "webserver";
static httpd_handle_t server = NULL;

// CSRF token (generated once at server start, stable for session)
static char csrf_token[33] = {0};

// Generate a random CSRF token
static void generate_csrf_token(void)
{
    uint32_t random_bytes[4];
    for (int i = 0; i < 4; i++) {
        random_bytes[i] = esp_random();
    }
    snprintf(csrf_token, sizeof(csrf_token),
             "%08lx%08lx%08lx%08lx",
             (unsigned long)random_bytes[0], (unsigned long)random_bytes[1],
             (unsigned long)random_bytes[2], (unsigned long)random_bytes[3]);
}

// Verify CSRF token from form submission
static bool verify_csrf_token(const char *token)
{
    if (!token || strlen(csrf_token) == 0) {
        return false;
    }
    return (strcmp(csrf_token, token) == 0);
}

// Check HTTP Basic Authentication
// Returns true if authenticated, false otherwise
static bool check_basic_auth(httpd_req_t *req)
{
    const settings_t *s = settings_get();

    // If no password is set, allow access (first-time setup)
    if (strlen(s->web_admin_password) == 0) {
        return true;
    }

    // Get Authorization header
    char auth_header[256] = {0};
    size_t auth_len = httpd_req_get_hdr_value_len(req, "Authorization");

    if (auth_len == 0 || auth_len >= sizeof(auth_header)) {
        return false;
    }

    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }

    // Check for "Basic " prefix
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        return false;
    }

    // Decode base64 credentials
    const char *encoded = auth_header + 6;
    unsigned char decoded[128] = {0};
    size_t decoded_len = 0;

    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                              (const unsigned char *)encoded, strlen(encoded)) != 0) {
        return false;
    }
    decoded[decoded_len] = '\0';

    // Parse username:password
    char *colon = strchr((char *)decoded, ':');
    if (!colon) {
        return false;
    }

    char *password = colon + 1;

    // Verify password (username is "admin")
    return settings_verify_web_password(password);
}

// Send 401 Unauthorized response
static esp_err_t send_auth_required(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"Intercom Settings\"");
    httpd_resp_set_type(req, "text/html");
    const char *msg = "<!DOCTYPE html><html><body><h1>Authentication Required</h1>"
                      "<p>Please log in with username 'admin' and your password.</p></body></html>";
    httpd_resp_send(req, msg, strlen(msg));
    return ESP_OK;
}

// HTML-encode a string to prevent XSS attacks
static void html_encode(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    while (*src && i < dst_size - 6) {  // Reserve space for longest entity
        switch (*src) {
            case '<':
                memcpy(dst + i, "&lt;", 4);
                i += 4;
                break;
            case '>':
                memcpy(dst + i, "&gt;", 4);
                i += 4;
                break;
            case '&':
                memcpy(dst + i, "&amp;", 5);
                i += 5;
                break;
            case '"':
                memcpy(dst + i, "&quot;", 6);
                i += 6;
                break;
            case '\'':
                memcpy(dst + i, "&#39;", 5);
                i += 5;
                break;
            default:
                dst[i++] = *src;
                break;
        }
        src++;
    }
    dst[i] = '\0';
}

// HTML page template - Modern gradient design
static const char *HTML_PAGE =
"<!DOCTYPE html>"
"<html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Intercom</title>"
"<style>"
":root{--c:#00D4FF;--p:#6366F1;--bg:#0a0a1a;--card:rgba(255,255,255,0.03);--border:rgba(255,255,255,0.08);}"
"*{box-sizing:border-box}"
"body{font-family:-apple-system,sans-serif;max-width:420px;margin:0 auto;padding:20px;background:var(--bg);"
"background-image:radial-gradient(ellipse at top left,rgba(0,212,255,0.1),transparent 50%%),"
"radial-gradient(ellipse at bottom right,rgba(99,102,241,0.1),transparent 50%%);color:#fff;min-height:100vh;}"
".hdr{display:flex;align-items:center;gap:14px;margin-bottom:24px;}"
".hdr h1{margin:0;font-size:26px;background:linear-gradient(135deg,var(--c),var(--p));"
"-webkit-background-clip:text;-webkit-text-fill-color:transparent;}"
".card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:20px;margin-bottom:16px;}"
".info{display:grid;gap:8px;font-size:14px;}"
".info span{color:rgba(255,255,255,0.5);}"
"h3{color:var(--c);font-size:14px;margin:16px 0 12px;text-transform:uppercase;letter-spacing:1px;}"
"label{display:block;margin:12px 0 6px;color:rgba(255,255,255,0.6);font-size:13px;}"
"input[type=text],input[type=password],input[type=number]{"
"width:100%%;padding:12px;border:1px solid var(--border);border-radius:10px;background:rgba(255,255,255,0.03);color:#fff;font-size:15px;}"
"input:focus{outline:none;border-color:var(--c);box-shadow:0 0 0 3px rgba(0,212,255,0.1);}"
"input[type=checkbox]{margin-right:8px;}"
".btn{width:100%%;padding:14px;border:none;border-radius:12px;font-size:15px;font-weight:600;cursor:pointer;margin-top:16px;"
"background:linear-gradient(135deg,var(--c),var(--p));color:#fff;}"
".btn:hover{opacity:0.9;}"
".btn-link{display:block;text-align:center;padding:14px;border-radius:12px;text-decoration:none;font-weight:600;"
"background:var(--card);border:1px solid var(--border);color:var(--c);margin-bottom:16px;}"
".row{display:flex;gap:12px;}.row>*{flex:1;}"
".danger{border-color:rgba(239,68,68,0.3);}"
".danger h3{color:#EF4444;}"
".danger .btn{background:#EF4444;}"
"input[type=file]{color:rgba(255,255,255,0.5);font-size:13px;}"
"</style></head><body>"
"<div class='hdr'>"
"<svg viewBox='0 0 512 512' width='44' height='44'><defs><linearGradient id='g' x1='0%%' y1='0%%' x2='100%%' y2='100%%'>"
"<stop offset='0%%' stop-color='#00D4FF'/><stop offset='100%%' stop-color='#6366F1'/></linearGradient></defs>"
"<rect x='32' y='32' width='448' height='448' rx='72' fill='none' stroke='url(#g)' stroke-width='48'/>"
"<rect x='116' y='140' width='36' height='140' rx='18' fill='url(#g)'/>"
"<rect x='178' y='110' width='36' height='200' rx='18' fill='url(#g)'/>"
"<rect x='238' y='95' width='36' height='230' rx='18' fill='url(#g)'/>"
"<rect x='298' y='110' width='36' height='200' rx='18' fill='url(#g)'/>"
"<rect x='360' y='140' width='36' height='140' rx='18' fill='url(#g)'/>"
"<path d='M140 370 Q256 440 372 370' fill='none' stroke='url(#g)' stroke-width='32' stroke-linecap='round'/></svg>"
"<h1>Intercom</h1></div>"
"<div class='card'><div class='info'>"
"<div><span>Room</span><br><strong>%s</strong></div>"
"<div><span>IP Address</span><br><strong>%s</strong></div>"
"<div><span>MQTT</span><br><strong>%s</strong></div>"
"<div><span>Version</span><br><strong>" FIRMWARE_VERSION "</strong></div>"
"</div></div>"
"<a href='/diagnostics' class='btn-link'>View Diagnostics</a>"
"<form action='/save' method='POST' class='card'>"
"<input type='hidden' name='csrf' value='%s'>"
"<h3>WiFi</h3>"
"<label>SSID</label><input type='text' name='ssid' value='%s'>"
"<label>Password</label><input type='password' name='pass' placeholder='Leave blank to keep'>"
"<h3>Device</h3>"
"<label>Room Name</label><input type='text' name='room' value='%s' required>"
"<label>Volume (0-100)</label><input type='number' name='vol' min='0' max='100' value='%d'>"
"<h3>Home Assistant</h3>"
"<label><input type='checkbox' name='mqtt_en' value='1' %s> Enable MQTT</label>"
"<label><input type='checkbox' name='mqtt_tls' value='1' %s> Enable TLS (secure)</label>"
"<div class='row'>"
"<div><label>Host</label><input type='text' name='mqtt_host' value='%s' placeholder='192.168.1.x'></div>"
"<div><label>Port</label><input type='number' name='mqtt_port' value='%d'></div></div>"
"<label>Username</label><input type='text' name='mqtt_user' value='%s'>"
"<label>Password</label><input type='password' name='mqtt_pass' placeholder='Leave blank to keep'>"
"<h3>Security</h3>"
"<label>Web Admin Password</label><input type='password' name='web_pass' placeholder='%s'>"
"<button type='submit' class='btn'>Save Settings</button>"
"</form>"
"<form action='/update' method='POST' enctype='multipart/form-data' class='card'>"
"<input type='hidden' name='csrf' value='%s'>"
"<h3>Firmware</h3>"
"<label>Select .bin file</label>"
"<input type='file' name='firmware' accept='.bin'>"
"<button type='submit' class='btn'>Upload</button>"
"</form>"
"<form action='/reset' method='POST' class='card danger'>"
"<input type='hidden' name='csrf' value='%s'>"
"<h3>Factory Reset</h3>"
"<button type='submit' class='btn' onclick=\"return confirm('Reset all settings?');\">Reset Device</button>"
"</form>"
"</body></html>";

static const char *HTML_SAVED =
"<!DOCTYPE html><html><head>"
"<meta http-equiv='refresh' content='3;url=/'>"
"<title>Saved</title>"
"<style>body{background:#0a0a1a;color:#fff;font-family:-apple-system,sans-serif;display:flex;flex-direction:column;"
"align-items:center;justify-content:center;min-height:100vh;margin:0;text-align:center;}"
"h1{background:linear-gradient(135deg,#00D4FF,#6366F1);-webkit-background-clip:text;-webkit-text-fill-color:transparent;}"
"p{color:rgba(255,255,255,0.6);}</style></head><body>"
"<h1>Settings Saved</h1><p>Rebooting...</p>"
"</body></html>";

static const char *HTML_OTA_OK =
"<!DOCTYPE html><html><head>"
"<meta http-equiv='refresh' content='10;url=/'>"
"<title>Updated</title>"
"<style>body{background:#0a0a1a;color:#fff;font-family:-apple-system,sans-serif;display:flex;flex-direction:column;"
"align-items:center;justify-content:center;min-height:100vh;margin:0;text-align:center;}"
"h1{background:linear-gradient(135deg,#00D4FF,#6366F1);-webkit-background-clip:text;-webkit-text-fill-color:transparent;}"
"p{color:rgba(255,255,255,0.6);}</style></head><body>"
"<h1>Firmware Updated</h1><p>Rebooting in 10 seconds...</p>"
"</body></html>";

static const char *HTML_DIAG_HEADER =
"<!DOCTYPE html><html><head>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<meta http-equiv='refresh' content='5'>"
"<title>Diagnostics</title>"
"<style>"
":root{--c:#00D4FF;--p:#6366F1;--bg:#0a0a1a;--card:rgba(255,255,255,0.03);--border:rgba(255,255,255,0.08);}"
"*{box-sizing:border-box}"
"body{font-family:-apple-system,sans-serif;max-width:700px;margin:0 auto;padding:20px;background:var(--bg);"
"background-image:radial-gradient(ellipse at top left,rgba(0,212,255,0.1),transparent 50%%),"
"radial-gradient(ellipse at bottom right,rgba(99,102,241,0.1),transparent 50%%);color:#fff;min-height:100vh;}"
".hdr{display:flex;align-items:center;gap:12px;margin-bottom:8px;}"
".hdr h1{margin:0;font-size:24px;background:linear-gradient(135deg,var(--c),var(--p));"
"-webkit-background-clip:text;-webkit-text-fill-color:transparent;}"
".card{background:var(--card);border:1px solid var(--border);border-radius:16px;padding:20px;margin:16px 0;}"
"h3{color:var(--c);font-size:13px;margin:0 0 16px;text-transform:uppercase;letter-spacing:1px;}"
".stat{display:inline-block;margin:0 24px 16px 0;}"
".stat-value{font-size:28px;font-weight:700;background:linear-gradient(135deg,var(--c),var(--p));"
"-webkit-background-clip:text;-webkit-text-fill-color:transparent;}"
".stat-label{font-size:11px;color:rgba(255,255,255,0.5);text-transform:uppercase;letter-spacing:0.5px;}"
".warn{color:#F59E0B;}.error{color:#EF4444;}.ok{color:#10B981;}"
"a{color:var(--c);text-decoration:none;}"
"a:hover{text-decoration:underline;}"
".back{display:inline-flex;align-items:center;gap:6px;margin-bottom:16px;font-size:14px;}"
".reset-reason{padding:10px 16px;border-radius:10px;display:inline-block;font-weight:500;}"
".reset-power{background:rgba(0,212,255,0.1);color:var(--c);}"
".reset-sw{background:rgba(245,158,11,0.1);color:#F59E0B;}"
".reset-crash{background:rgba(239,68,68,0.1);color:#EF4444;}"
".reset-wdt{background:rgba(239,68,68,0.1);color:#EF4444;}"
"</style></head><body>"
"<div class='hdr'>"
"<svg viewBox='0 0 512 512' width='36' height='36'><defs><linearGradient id='g' x1='0%%' y1='0%%' x2='100%%' y2='100%%'>"
"<stop offset='0%%' stop-color='#00D4FF'/><stop offset='100%%' stop-color='#6366F1'/></linearGradient></defs>"
"<rect x='32' y='32' width='448' height='448' rx='72' fill='none' stroke='url(#g)' stroke-width='48'/>"
"<rect x='116' y='140' width='36' height='140' rx='18' fill='url(#g)'/>"
"<rect x='178' y='110' width='36' height='200' rx='18' fill='url(#g)'/>"
"<rect x='238' y='95' width='36' height='230' rx='18' fill='url(#g)'/>"
"<rect x='298' y='110' width='36' height='200' rx='18' fill='url(#g)'/>"
"<rect x='360' y='140' width='36' height='140' rx='18' fill='url(#g)'/>"
"<path d='M140 370 Q256 440 372 370' fill='none' stroke='url(#g)' stroke-width='32' stroke-linecap='round'/></svg>"
"<h1>Diagnostics</h1></div>"
"<a href='/' class='back'>&#8592; Back to Settings</a>";

static const char *HTML_DIAG_FOOTER =
"<p style='color:rgba(255,255,255,0.3);font-size:12px;text-align:center;margin-top:24px;'>Auto-refresh every 5 seconds</p>"
"<script>window.onload=function(){var l=document.getElementById('logbox');if(l)l.scrollTop=l.scrollHeight;}</script>"
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
    return "your_wifi_ssid";  // Must match DEFAULT_WIFI_SSID in main.c
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
    // Check authentication
    if (!check_basic_auth(req)) {
        return send_auth_required(req);
    }

    const settings_t *s = settings_get();
    char ip[16] = "0.0.0.0";
    get_ip_string(ip, sizeof(ip));

    char *html = malloc(8192);  // Increased size for security fields and CSRF tokens
    if (!html) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    const char *mqtt_status = s->mqtt_enabled ?
        (strlen(s->mqtt_host) > 0 ? "Enabled" : "Not configured") : "Disabled";

    // HTML-encode user-supplied values to prevent XSS
    char safe_room[SETTINGS_ROOM_MAX * 6];
    char safe_ssid[SETTINGS_SSID_MAX * 6];
    char safe_mqtt_host[SETTINGS_MQTT_HOST_MAX * 6];
    char safe_mqtt_user[SETTINGS_MQTT_USER_MAX * 6];

    html_encode(safe_room, s->room_name, sizeof(safe_room));
    html_encode(safe_ssid, get_current_ssid(), sizeof(safe_ssid));
    html_encode(safe_mqtt_host, s->mqtt_host, sizeof(safe_mqtt_host));
    html_encode(safe_mqtt_user, s->mqtt_user, sizeof(safe_mqtt_user));

    snprintf(html, 8192, HTML_PAGE,
             safe_room, ip, mqtt_status,
             csrf_token,  // CSRF token for save form
             safe_ssid, safe_room, s->volume,
             s->mqtt_enabled ? "checked" : "",
             s->mqtt_tls_enabled ? "checked" : "",
             safe_mqtt_host, s->mqtt_port, safe_mqtt_user,
             strlen(s->web_admin_password) > 0 ? "Leave blank to keep" : "Set admin password",
             csrf_token,  // CSRF token for firmware form
             csrf_token); // CSRF token for reset form

    // Set security headers
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self'; style-src 'unsafe-inline'; script-src 'unsafe-inline'");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    free(html);

    return ESP_OK;
}

// POST /save - save settings
static esp_err_t save_handler(httpd_req_t *req)
{
    // Check authentication
    if (!check_basic_auth(req)) {
        return send_auth_required(req);
    }

    char body[1024] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Verify CSRF token
    char csrf[64] = {0};
    if (!get_form_value(body, "csrf", csrf, sizeof(csrf)) || !verify_csrf_token(csrf)) {
        ESP_LOGW(TAG, "CSRF token validation failed");
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_send(req, "CSRF validation failed", -1);
        return ESP_FAIL;
    }

    char ssid[64], pass[128], room[64], vol[8];
    char mqtt_host[64], mqtt_port[8], mqtt_user[32], mqtt_pass[64], mqtt_en[4], mqtt_tls[4];
    char web_pass[64];

    // Only update WiFi if password is provided (intentional change)
    if (get_form_value(body, "pass", pass, sizeof(pass)) && strlen(pass) > 0) {
        if (get_form_value(body, "ssid", ssid, sizeof(ssid))) {
            settings_set_wifi(ssid, pass);
            ESP_LOGI(TAG, "WiFi credentials updated");
        }
    }

    // Room name can be changed independently — strip leading/trailing whitespace
    if (get_form_value(body, "room", room, sizeof(room))) {
        // Trim leading whitespace
        char *trimmed = room;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        // Trim trailing whitespace
        int tlen = strlen(trimmed);
        while (tlen > 0 && (trimmed[tlen-1] == ' ' || trimmed[tlen-1] == '\t')) {
            trimmed[--tlen] = '\0';
        }
        if (tlen > 0) settings_set_room(trimmed);
    }

    // Volume can be changed independently
    if (get_form_value(body, "vol", vol, sizeof(vol))) {
        settings_set_volume(atoi(vol));
    }

    // MQTT settings
    bool mqtt_enabled = get_form_value(body, "mqtt_en", mqtt_en, sizeof(mqtt_en));
    settings_set_mqtt_enabled(mqtt_enabled);

    // MQTT TLS setting
    bool tls_enabled = get_form_value(body, "mqtt_tls", mqtt_tls, sizeof(mqtt_tls));
    settings_set_mqtt_tls_enabled(tls_enabled);

    // Update MQTT connection settings if host is provided
    if (get_form_value(body, "mqtt_host", mqtt_host, sizeof(mqtt_host)) && strlen(mqtt_host) > 0) {
        uint16_t port = tls_enabled ? 8883 : 1883;  // Default TLS port
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

    // Web admin password - only update if provided
    if (get_form_value(body, "web_pass", web_pass, sizeof(web_pass)) && strlen(web_pass) > 0) {
        settings_set_web_admin_password(web_pass);
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
    // Check authentication
    if (!check_basic_auth(req)) {
        return send_auth_required(req);
    }

    char body[256] = {0};
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret > 0) {
        // Verify CSRF token
        char csrf[64] = {0};
        if (!get_form_value(body, "csrf", csrf, sizeof(csrf)) || !verify_csrf_token(csrf)) {
            ESP_LOGW(TAG, "CSRF token validation failed for reset");
            httpd_resp_set_status(req, "403 Forbidden");
            httpd_resp_send(req, "CSRF validation failed", -1);
            return ESP_FAIL;
        }
    }

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
    // Check authentication
    if (!check_basic_auth(req)) {
        return send_auth_required(req);
    }

    ESP_LOGI(TAG, "OTA update started, size=%d", req->content_len);

    // Extract the multipart boundary from Content-Type header.
    // We build an end-of-part marker "\r\n--<boundary>" to use instead of
    // a hardcoded pattern that can appear in firmware binary data.
    char content_type[256] = {0};
    char end_marker[128] = "\r\n------";  // fallback
    int marker_len = 8;
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type)) == ESP_OK) {
        char *bnd = strstr(content_type, "boundary=");
        if (bnd) {
            bnd += 9;
            int built = snprintf(end_marker, sizeof(end_marker), "\r\n--%s", bnd);
            if (built > 0 && built < (int)sizeof(end_marker)) {
                marker_len = built;
            }
        }
    }
    ESP_LOGI(TAG, "OTA boundary marker: '%s' (%d bytes)", end_marker, marker_len);

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
    bool done = false;

    while (received < total && !done) {
        int ret = httpd_req_recv(req, buf, 1024);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            break;
        }

        char *data = buf;
        int data_len = ret;

        // Skip multipart header on first chunk (find end of part header)
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

        // Check for multipart end boundary using the actual boundary string.
        // This prevents false matches against firmware binary data.
        char *boundary_end = memmem(data, data_len, end_marker, marker_len);
        if (boundary_end) {
            data_len = boundary_end - data;
            done = true;  // Stop after writing this chunk
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

// GET /diagnostics - system diagnostics page
static esp_err_t diagnostics_handler(httpd_req_t *req)
{
    // Check authentication
    if (!check_basic_auth(req)) {
        return send_auth_required(req);
    }

    const char *reset_reason = diagnostics_get_reset_reason();
    uint32_t uptime = diagnostics_get_uptime();
    uint32_t heap = esp_get_free_heap_size();
    uint32_t min_heap = esp_get_minimum_free_heap_size();

    // Determine reset reason CSS class
    const char *reset_class = "reset-power";
    if (strstr(reset_reason, "Crash") || strstr(reset_reason, "Panic")) {
        reset_class = "reset-crash";
    } else if (strstr(reset_reason, "watchdog") || strstr(reset_reason, "Watchdog")) {
        reset_class = "reset-wdt";
    } else if (strstr(reset_reason, "Software") || strstr(reset_reason, "Brownout")) {
        reset_class = "reset-sw";
    }

    // Get logs HTML
    char *logs_html = diagnostics_get_logs_html();

    // Calculate buffer size
    size_t buf_size = 4096 + (logs_html ? strlen(logs_html) : 0);
    char *html = malloc(buf_size);
    if (!html) {
        if (logs_html) free(logs_html);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Format uptime
    uint32_t days = uptime / 86400;
    uint32_t hours = (uptime % 86400) / 3600;
    uint32_t mins = (uptime % 3600) / 60;
    uint32_t secs = uptime % 60;

    // Build page
    int len = snprintf(html, buf_size,
        "%s"
        "<div class='card'>"
        "<h3>System Status</h3>"
        "<div class='stat'><div class='stat-value'>%lud %luh %lum %lus</div><div class='stat-label'>Uptime</div></div>"
        "<div class='stat'><div class='stat-value'>%lu</div><div class='stat-label'>Free Heap (bytes)</div></div>"
        "<div class='stat'><div class='stat-value'>%lu</div><div class='stat-label'>Min Heap (bytes)</div></div>"
        "</div>"
        "<div class='card'>"
        "<h3>Last Reset</h3>"
        "<span class='reset-reason %s'>%s</span>"
        "</div>"
        "<div class='card'>"
        "<h3>Recent Logs</h3>"
        "%s"
        "</div>"
        "%s",
        HTML_DIAG_HEADER,
        days, hours, mins, secs,
        heap, min_heap,
        reset_class, reset_reason,
        logs_html ? logs_html : "<p>No logs available</p>",
        HTML_DIAG_FOOTER);

    if (logs_html) free(logs_html);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, len);
    free(html);

    return ESP_OK;
}

// GET /diagnostics/json - diagnostics as JSON
static esp_err_t diagnostics_json_handler(httpd_req_t *req)
{
    // Check authentication
    if (!check_basic_auth(req)) {
        return send_auth_required(req);
    }

    char *json = diagnostics_get_json();
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, strlen(json));
    free(json);

    return ESP_OK;
}

esp_err_t webserver_start(void)
{
    if (server) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    // Generate CSRF token once at startup (stable for entire session)
    generate_csrf_token();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 12288;  // 12KB for complex page rendering
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.recv_wait_timeout = 60;  // 60s for OTA uploads over slow WiFi
    config.send_wait_timeout = 60;

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
    httpd_uri_t diag = {.uri = "/diagnostics", .method = HTTP_GET, .handler = diagnostics_handler};
    httpd_uri_t diag_json = {.uri = "/diagnostics/json", .method = HTTP_GET, .handler = diagnostics_json_handler};

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
    httpd_register_uri_handler(server, &reset);
    httpd_register_uri_handler(server, &ota);
    httpd_register_uri_handler(server, &diag);
    httpd_register_uri_handler(server, &diag_json);

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
