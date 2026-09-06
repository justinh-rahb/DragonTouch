#include "dt_portal.h"
#include "dt_printers.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "dc_moonraker.h"
#include "dc_wifi.h"
#include "dc_portal.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/task.h"


static const char *TAG = "dt_portal";


static const char PAGE_HTML[] =
"<!doctype html>"
"<html><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>DragonTouch Printer Setup</title>"
"<style>"
":root{color-scheme:dark}"
"*{box-sizing:border-box}"
"body{margin:0;background:#111318;color:#eef2f7;"
"font-family:system-ui,-apple-system,sans-serif}"
"main{max-width:680px;margin:auto;padding:28px 18px}"
".card{background:#1b1f27;border:1px solid #303744;"
"border-radius:16px;padding:20px;margin:14px 0}"
"h1{margin:0 0 4px}h2{margin:0 0 14px;font-size:18px}"
"p,small{color:#aeb7c5;line-height:1.45}"
"label{display:block;margin:13px 0 6px}"
"input{width:100%;padding:12px;border-radius:10px;"
"border:1px solid #3b4555;background:#101319;color:#fff;font-size:16px}"
"button,a{display:inline-block;border:0;border-radius:10px;"
"padding:12px 16px;font-size:15px;font-weight:650;text-decoration:none}"
"button{background:#5b8cff;color:white;cursor:pointer}"
".danger{background:#7c3030}"
".secondary{background:#2a303a;color:#eef2f7}"
".row{display:flex;gap:10px;flex-wrap:wrap;margin-top:16px}"
"#status{padding:10px 12px;border-radius:10px;background:#12161c;"
"font-family:ui-monospace,monospace;white-space:pre-wrap}"
"</style></head><body><main>"
"<h1>DragonTouch</h1>"
"<p>Klipper / Moonraker printer configuration.</p>"
"<section class='card'><h2>Printer</h2>"
"<label>Hostname or IP</label>"
"<input id='host' maxlength='63' placeholder='printer.local or 192.168.1.50'>"
"<label>Moonraker port</label>"
"<input id='port' type='number' min='1' max='65535' value='7125'>"
"<label>API key</label>"
"<input id='key' type='password' maxlength='64' "
"placeholder='Optional; blank preserves the saved key'>"
"<label><input id='clearKey' type='checkbox' style='width:auto'> "
"Clear saved API key</label>"
"<div class='row'>"
"<button onclick='savePrinter()'>Save printer</button>"
"<button class='danger' onclick='clearPrinter()'>Clear printer</button>"
"</div></section>"
"<section class='card'><h2>Dragon portal</h2>"
"<p>Wi-Fi provisioning, recovery and family maintenance are provided by "
"dragon-core's shared captive portal.</p>"
"<a class='secondary' href='/'>Open main portal</a>"
"</section>"
"<section class='card'><h2>Status</h2><div id='status'>Loading...</div></section>"
"<script>"
"const q=id=>document.getElementById(id);"
"async function load(){try{"
"const r=await fetch('/api/dragontouch/printer',{cache:'no-store'});"
"const j=await r.json();if(!r.ok)throw new Error(j.error||r.statusText);"
"q('host').value=j.host||'';q('port').value=j.port||7125;"
"q('status').textContent="
"'configured: '+(j.configured?'yes':'no')+'\\n'+"
"'host: '+(j.host||'-')+'\\n'+"
"'port: '+(j.port||7125)+'\\n'+"
"'api key: '+(j.api_key_set?'saved':'not set');"
"}catch(e){q('status').textContent='error: '+e;}}"
"async function savePrinter(){"
"const body={host:q('host').value.trim(),port:Number(q('port').value||7125),"
"clear_api_key:q('clearKey').checked};"
"if(q('key').value)body.api_key=q('key').value;"
"try{const r=await fetch('/api/dragontouch/printer',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
"const j=await r.json();if(!r.ok)throw new Error(j.error||r.statusText);"
"q('key').value='';q('clearKey').checked=false;await load();"
"q('status').textContent+='\\nSaved; DragonTouch is restarting.';"
"}catch(e){q('status').textContent='save failed: '+e;}}"
"async function clearPrinter(){"
"if(!confirm('Clear the saved Moonraker printer?'))return;"
"try{const r=await fetch('/api/dragontouch/printer',{method:'POST',"
"headers:{'Content-Type':'application/json'},body:'{\"clear\":true}'});"
"const j=await r.json();if(!r.ok)throw new Error(j.error||r.statusText);"
"await load();}catch(e){q('status').textContent='clear failed: '+e;}}"
"load();"
"</script></main></body></html>";


static esp_err_t send_json(
    httpd_req_t *req,
    cJSON *root,
    const char *status
)
{
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }

    httpd_resp_set_type(req, "application/json");

    char *text = cJSON_PrintUnformatted(root);
    if (text == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = httpd_resp_sendstr(req, text);
    cJSON_free(text);
    return err;
}


static esp_err_t send_ok_and_restart(httpd_req_t *req)
{
    cJSON *reply = cJSON_CreateObject();

    if (reply == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(reply, "ok", true);
    cJSON_AddBoolToObject(reply, "restarting", true);

    esp_err_t err = send_json(req, reply, NULL);
    cJSON_Delete(reply);

    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }

    return err;
}


static esp_err_t send_error(
    httpd_req_t *req,
    const char *status,
    const char *message
)
{
    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "error", message);
    esp_err_t err = send_json(req, root, status);
    cJSON_Delete(root);
    return err;
}


static esp_err_t recv_json(
    httpd_req_t *req,
    cJSON **out
)
{
    *out = NULL;

    if (req->content_len <= 0 || req->content_len > 1024) {
        return ESP_ERR_INVALID_SIZE;
    }

    char *body = calloc(1, (size_t)req->content_len + 1);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;

    while (received < (size_t)req->content_len) {
        int n = httpd_req_recv(
            req,
            body + received,
            req->content_len - received
        );

        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (n <= 0) {
            free(body);
            return ESP_FAIL;
        }

        received += (size_t)n;
    }

    cJSON *root = cJSON_Parse(body);
    free(body);

    if (root == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out = root;
    return ESP_OK;
}


static bool host_valid(const char *host)
{
    if (host == NULL || host[0] == '\0') {
        return false;
    }

    size_t len = strlen(host);

    if (len > 63 || strstr(host, "://") != NULL || strchr(host, '/') != NULL) {
        return false;
    }

    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)host[i];

        if (isspace(c) || iscntrl(c)) {
            return false;
        }
    }

    return true;
}


static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    return httpd_resp_send(
        req,
        PAGE_HTML,
        HTTPD_RESP_USE_STRLEN
    );
}


static esp_err_t printer_get(httpd_req_t *req)
{
    dc_moonraker_config_t cfg = {0};

    esp_err_t cfg_err = dc_moonraker_get_config(&cfg);

    if (cfg.port == 0) {
        cfg.port = 7125;
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddBoolToObject(
        root,
        "configured",
        cfg_err == ESP_OK && cfg.host[0] != '\0'
    );

    cJSON_AddStringToObject(root, "host", cfg.host);
    cJSON_AddNumberToObject(root, "port", cfg.port);
    cJSON_AddBoolToObject(root, "api_key_set", cfg.api_key[0] != '\0');

    esp_err_t err = send_json(req, root, NULL);
    cJSON_Delete(root);
    return err;
}


static esp_err_t printer_post(httpd_req_t *req)
{
    cJSON *root = NULL;

    esp_err_t err = recv_json(req, &root);

    if (err != ESP_OK) {
        return send_error(
            req,
            "400 Bad Request",
            "invalid JSON body"
        );
    }

    cJSON *clear = cJSON_GetObjectItemCaseSensitive(root, "clear");

    if (cJSON_IsTrue(clear)) {
        cJSON_Delete(root);

        err = dc_moonraker_clear_config();

        if (err != ESP_OK) {
            return send_error(
                req,
                "500 Internal Server Error",
                "failed to clear Moonraker config"
            );
        }

        return send_ok_and_restart(req);
    }

    cJSON *host = cJSON_GetObjectItemCaseSensitive(root, "host");
    cJSON *port = cJSON_GetObjectItemCaseSensitive(root, "port");
    cJSON *api_key = cJSON_GetObjectItemCaseSensitive(root, "api_key");
    cJSON *clear_key = cJSON_GetObjectItemCaseSensitive(root, "clear_api_key");

    if (!cJSON_IsString(host) || !host_valid(host->valuestring)) {
        cJSON_Delete(root);
        return send_error(
            req,
            "400 Bad Request",
            "host must be a hostname or IP without a URL scheme"
        );
    }

    int port_value = 7125;

    if (port != NULL) {
        if (
            !cJSON_IsNumber(port) ||
            port->valuedouble < 1 ||
            port->valuedouble > 65535
        ) {
            cJSON_Delete(root);
            return send_error(
                req,
                "400 Bad Request",
                "port must be between 1 and 65535"
            );
        }

        port_value = port->valueint;
    }

    if (
        api_key != NULL &&
        (
            !cJSON_IsString(api_key) ||
            strlen(api_key->valuestring) > 64
        )
    ) {
        cJSON_Delete(root);
        return send_error(
            req,
            "400 Bad Request",
            "API key must be at most 64 characters"
        );
    }

    dc_moonraker_config_t cfg = {0};

    /*
     * Preserve an existing API key unless explicitly replaced or cleared.
     */
    (void)dc_moonraker_get_config(&cfg);

    snprintf(cfg.host, sizeof(cfg.host), "%s", host->valuestring);
    cfg.port = (uint16_t)port_value;

    if (cJSON_IsTrue(clear_key)) {
        cfg.api_key[0] = '\0';
    } else if (
        cJSON_IsString(api_key) &&
        api_key->valuestring[0] != '\0'
    ) {
        snprintf(
            cfg.api_key,
            sizeof(cfg.api_key),
            "%s",
            api_key->valuestring
        );
    }

    cJSON_Delete(root);

    err = dc_moonraker_set_config(&cfg);

    /*
     * DT_PRINTER_LIST
     *
     * Anything configured through the portal also joins the on-screen
     * picker, so pointing the portal at a second printer once is enough to
     * make it switchable from the header thereafter.
     */
    if (err == ESP_OK) {
        (void)dt_printers_add(cfg.host, cfg.port, cfg.api_key);
    }

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "dc_moonraker_set_config failed: %s",
            esp_err_to_name(err)
        );

        return send_error(
            req,
            "500 Internal Server Error",
            "failed to save Moonraker config"
        );
    }

    ESP_LOGI(
        TAG,
        "Moonraker config saved: %s:%u",
        cfg.host,
        (unsigned)cfg.port
    );

    return send_ok_and_restart(req);
}


static esp_err_t product_factory_reset(void *ctx)
{
    (void)ctx;
    return dc_moonraker_clear_config();
}




/*
 * DT_STAGE6B_PORTAL_RESET_REUSE
 *
 * The shared dc_portal reset endpoint performs:
 *   product factory reset -> dc_wifi_clear_creds -> reboot.
 *
 * The touchscreen has no authenticated HTTP session/token of its own, so
 * invoke those same public reset primitives directly rather than creating
 * a second NVS implementation or bypassing the product reset callback.
 */
esp_err_t dt_portal_factory_reset_from_ui(void)
{
    ESP_LOGW(
        TAG,
        "factory reset requested from touchscreen"
    );

    esp_err_t err =
        product_factory_reset(NULL);

    if (err != ESP_OK) {
        return err;
    }

    err =
        dc_wifi_clear_creds();

    if (err != ESP_OK) {
        return err;
    }

    vTaskDelay(
        pdMS_TO_TICKS(300)
    );

    esp_restart();

    return ESP_OK;
}



/*
 * DT_PORTAL_HOME_PAGE
 *
 * DragonTouch's web root is a navigation hub. Operational browser surfaces
 * remain separate so each can stay small and purpose-built.
 */
static esp_err_t home_get(httpd_req_t *req)
{
    static const char page[] =
        "<!doctype html>"
        "<html lang=\"en\">"
        "<head>"
        "<meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta name=\"color-scheme\" content=\"light dark\">"
        "<title>DragonTouch</title>"
        "<style>"
        ":root{color-scheme:light dark;"
        "--bg:light-dark(#f5f5f5,#171717);"
        "--card:light-dark(#fff,#242424);"
        "--fg:light-dark(#18181b,#f4f4f5);"
        "--muted:light-dark(#71717a,#a1a1aa);"
        "--border:light-dark(#d4d4d8,#3f3f46);"
        "--accent:light-dark(#2563eb,#60a5fa);}"
        "*{box-sizing:border-box}"
        "body{margin:0;font-family:-apple-system,BlinkMacSystemFont,\"Segoe UI\",sans-serif;"
        "background:var(--bg);color:var(--fg)}"
        "main{width:min(760px,calc(100% - 32px));margin:0 auto;padding:36px 0 48px}"
        "header{margin-bottom:24px}"
        "h1{font-size:30px;line-height:1.1;margin:0 0 8px}"
        "header p{margin:0;color:var(--muted);font-size:15px}"
        ".grid{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:12px}"
        ".card{display:flex;flex-direction:column;min-height:170px;padding:18px;"
        "border:1px solid var(--border);border-radius:12px;background:var(--card);"
        "color:inherit;text-decoration:none;transition:transform .12s ease,border-color .12s ease}"
        ".card:hover{transform:translateY(-2px);border-color:var(--accent)}"
        ".card:focus-visible{outline:2px solid var(--accent);outline-offset:2px}"
        ".eyebrow{font-size:11px;font-weight:700;letter-spacing:.08em;"
        "text-transform:uppercase;color:var(--accent);margin-bottom:8px}"
        ".card h2{font-size:19px;margin:0 0 8px}"
        ".card p{font-size:14px;line-height:1.45;color:var(--muted);margin:0 0 16px}"
        ".go{margin-top:auto;font-size:14px;font-weight:600;color:var(--accent)}"
        "footer{margin-top:20px;color:var(--muted);font-size:12px}"
        "@media(max-width:650px){.grid{grid-template-columns:1fr}.card{min-height:0}}"
        "</style>"
        "</head>"
        "<body>"
        "<main>"
        "<header>"
        "<h1>DragonTouch</h1>"
        "<p>Printer control display and configuration.</p>"
        "</header>"
        "<section class=\"grid\">"
        "<a class=\"card\" href=\"/dragontouch\">"
        "<span class=\"eyebrow\">Printer</span>"
        "<h2>Printer &amp; Connection</h2>"
        "<p>Configure the Moonraker connection and DragonTouch printer settings.</p>"
        "<span class=\"go\">Open printer setup &rarr;</span>"
        "</a>"
        "<a class=\"card\" href=\"/setup\">"
        "<span class=\"eyebrow\">Device</span>"
        "<h2>Device Setup &amp; Firmware</h2>"
        "<p>Manage Wi-Fi, fallback access point, firmware updates, logs and recovery.</p>"
        "<span class=\"go\">Open device setup &rarr;</span>"
        "</a>"
        "<a class=\"card\" href=\"/console\">"
        "<span class=\"eyebrow\">Diagnostics</span>"
        "<h2>Console</h2>"
        "<p>View the live DragonTouch firmware log for diagnostics and troubleshooting.</p>"
        "<span class=\"go\">Open console &rarr;</span>"
        "</a>"
        "</section>"
        "<footer>DragonTouch local web interface</footer>"
        "</main>"
        "</body>"
        "</html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}


static const httpd_uri_t PRODUCT_ROUTES[] = {
    {
        .uri = "/home",
        .method = HTTP_GET,
        .handler = home_get,
        .user_ctx = NULL,
    },
    {
        .uri = "/dragontouch",
        .method = HTTP_GET,
        .handler = page_get,
        .user_ctx = NULL,
    },
    /*
     * DT_PORTAL_SETTINGS_ALIAS
     *
     * /settings is not a DragonTouch API. Without an explicit product route it
     * falls through dc_portal's wildcard SPA route, whose legacy compatibility
     * surface defaults to DragonBreath when no DragonTouch API-v2 descriptor
     * exists. Route it to the DragonTouch setup page instead.
     */
    {
        .uri = "/settings",
        .method = HTTP_GET,
        .handler = page_get,
        .user_ctx = NULL,
    },
    {
        .uri = "/api/dragontouch/printer",
        .method = HTTP_GET,
        .handler = printer_get,
        .user_ctx = NULL,
    },
    {
        .uri = "/api/dragontouch/printer",
        .method = HTTP_POST,
        .handler = printer_post,
        .user_ctx = NULL,
    },
};


esp_err_t dt_portal_start(void)
{
    const dc_portal_config_t config = {
        .product = "dragontouch",
        .display_name = "DragonTouch",
        .root_redirect = "/home",
        .product_routes = PRODUCT_ROUTES,
        .product_route_count =
            sizeof(PRODUCT_ROUTES) / sizeof(PRODUCT_ROUTES[0]),
        .factory_reset = product_factory_reset,
        .ctx = NULL,
    };

    esp_err_t err = dc_portal_start(&config);

    if (err == ESP_OK) {
        ESP_LOGI(
            TAG,
            "Stage 3 portal active; printer setup at /dragontouch"
        );
    }

    return err;
}
