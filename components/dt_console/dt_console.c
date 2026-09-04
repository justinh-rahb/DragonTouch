// SPDX-License-Identifier: MIT
#include "dt_console.h"

#include "dc_registry.h"
#include "dc_peer.h"

#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "dt_console";

static const char *kind_str(uint8_t k)
{
    switch (k) {
        case DC_PEER_KIND_BREATH: return "breath";
        case DC_PEER_KIND_VENT:   return "vent";
        case DC_PEER_KIND_WHEEZE: return "wheeze";
        case DC_PEER_KIND_TOUCH:  return "touch";
        default:                  return "unknown";
    }
}

// Decode a device's latest status blob into a human "summary" string for display.
static void status_summary(const dc_registry_entry_t *e, char *out, size_t n)
{
    if (!e->has_status) { snprintf(out, n, "(no status yet)"); return; }
    switch (e->status_cap) {
        case DC_PEER_CAP_HEATER: {
            const dc_peer_heater_t *h = &e->status.heater;
            const char *m = h->mode == 1 ? "power_on" : h->mode == 2 ? "auto"
                          : h->mode == 3 ? "drying" : "off";
            char chamber[16] = "--";
            if (h->chamber_dc != DC_PEER_TEMP_UNKNOWN)
                snprintf(chamber, sizeof chamber, "%.1f", h->chamber_dc / 10.0);
            snprintf(out, n, "%s  target %.1fC  chamber %sC%s%s%s", m,
                     h->target_dc / 10.0, chamber,
                     (h->flags & DC_PEER_HEATER_DEMAND) ? "  demand" : "",
                     (h->flags & DC_PEER_HEATER_FAULT) ? "  FAULT" : "",
                     (h->flags & DC_PEER_HEATER_INHIBITED) ? "  inhibited" : "");
            break;
        }
        case DC_PEER_CAP_VENT: {
            const dc_peer_vent_t *v = &e->status.vent;
            const char *ps = v->printer_state == DC_PEER_PRINTER_IDLE ? "idle"
                           : v->printer_state == DC_PEER_PRINTER_PRINTING ? "printing"
                           : v->printer_state == DC_PEER_PRINTER_PAUSED ? "paused"
                           : v->printer_state == DC_PEER_PRINTER_ERROR ? "error"
                           : v->printer_state == DC_PEER_PRINTER_OFFLINE ? "standalone" : "unknown";
            snprintf(out, n, "%s  %s  printer:%s%s", v->mode ? "manual" : "auto",
                     v->target ? "CLOSED" : "OPEN", ps,
                     (v->flags & DC_PEER_VENT_RUNNING) ? "  moving" : "");
            break;
        }
        case DC_PEER_CAP_DRYER: {
            const dc_peer_dryer_t *d = &e->status.dryer;
            const char *m = d->mode == 2 ? "drying" : d->mode == 1 ? "idle" : "off";
            char amb[16] = "--";
            if (d->ambient_dc != DC_PEER_TEMP_UNKNOWN)
                snprintf(amb, sizeof amb, "%.1f", d->ambient_dc / 10.0);
            char rh[8] = "--";
            if (d->humidity_pct != DC_PEER_RH_UNKNOWN) snprintf(rh, sizeof rh, "%u", d->humidity_pct);
            snprintf(out, n, "%s  %uC/%uh  amb %sC  rh %s%%  %lus left", m,
                     d->set_temp_c, d->set_time_h, amb, rh, (unsigned long)d->remaining_sec);
            break;
        }
        default:
            snprintf(out, n, "(cap %u)", e->status_cap);
            break;
    }
}

// ---- JSON: GET /api/devices ----

static esp_err_t devices_get(httpd_req_t *req)
{
    dc_registry_entry_t tbl[DC_REGISTRY_MAX];
    int n = dc_registry_get(tbl, DC_REGISTRY_MAX);
    int64_t now = esp_timer_get_time();

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < n; i++) {
        dc_registry_entry_t *e = &tbl[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", e->id);
        cJSON_AddStringToObject(o, "kind", kind_str(e->kind));
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "firmware", e->fw);
        char ip[16]; snprintf(ip, sizeof ip, "%u.%u.%u.%u", e->ip[0], e->ip[1], e->ip[2], e->ip[3]);
        cJSON_AddStringToObject(o, "ip", ip);
        cJSON_AddBoolToObject(o, "online", !dc_registry_entry_stale(e, now));
        cJSON_AddNumberToObject(o, "last_seen_ms_ago", (double)(now - e->last_seen_us) / 1000.0);
        char summary[96]; status_summary(e, summary, sizeof summary);
        cJSON_AddStringToObject(o, "status", summary);
        cJSON_AddItemToArray(arr, o);
    }
    cJSON_AddNumberToObject(root, "count", n);
    char *body = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, body ? body : "{}");
    cJSON_free(body);
    cJSON_Delete(root);
    return ESP_OK;
}

// ---- HTML: GET / (the emulated screen) ----

static const char PAGE[] =
"<!doctype html><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
"<title>DragonTouch</title><style>"
"body{margin:0;background:#141414;color:#eee;font:14px system-ui,sans-serif}"
"header{padding:14px 18px;border-bottom:1px solid #333;display:flex;align-items:center;gap:10px}"
"header b{color:#EF4444;font-size:16px}"
"#list{padding:14px;display:grid;gap:10px;max-width:760px}"
".card{background:#1e1e1e;border:1px solid #333;border-radius:10px;padding:12px 14px}"
".card.off{opacity:.45}"
".row{display:flex;justify-content:space-between;align-items:baseline;gap:10px}"
".name{font-weight:600;font-size:15px}"
".kind{color:#EF4444;text-transform:uppercase;font-size:11px;letter-spacing:.08em}"
".sub{color:#888;font-size:12px;margin-top:2px}"
".status{margin-top:8px;font-family:ui-monospace,monospace;font-size:13px;color:#ddd}"
".dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:6px;background:#3a3}"
".off .dot{background:#844}"
"#empty{color:#777;padding:20px}"
"</style>"
"<header><b>DragonTouch</b><span style=color:#888>family devices · status only</span></header>"
"<div id=list></div><div id=empty>Scanning for Dragon devices…</div>"
"<script>"
// Device fields (id, name, firmware) come from unauthenticated ESP-NOW frames, so
// escape every interpolated value before it reaches innerHTML — a nearby transmitter
// must not be able to inject markup into this page.
"const esc=s=>String(s==null?'':s).replace(/[&<>\"']/g,c=>'&#'+c.charCodeAt(0)+';');"
"async function tick(){"
"try{const r=await fetch('/api/devices');const d=await r.json();"
"const L=document.getElementById('list'),E=document.getElementById('empty');"
"if(!d.devices.length){E.style.display='';L.innerHTML='';return}E.style.display='none';"
"L.innerHTML=d.devices.map(v=>`<div class='card ${v.online?'':'off'}'>`+"
"`<div class=row><span class=name><span class=dot></span>${esc(v.name||v.id)}</span>`+"
"`<span class=kind>${esc(v.kind)}</span></div>`+"
"`<div class=sub>${esc(v.id)} · ${esc(v.ip)} · fw ${esc(v.firmware||'?')} · ${(v.last_seen_ms_ago/1000).toFixed(1)}s ago</div>`+"
"`<div class=status>${esc(v.status)}</div></div>`).join('')}"
"catch(e){}}"
"tick();setInterval(tick,2000);"
"</script>";

static esp_err_t devices_page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Routes handed to dc_portal (which owns the server, the setup SPA at "/", and OTA).
static const httpd_uri_t s_routes[] = {
    { .uri = "/devices",     .method = HTTP_GET, .handler = devices_page_get },
    { .uri = "/api/devices", .method = HTTP_GET, .handler = devices_get },
};

void dt_console_get_routes(const httpd_uri_t **routes, size_t *count)
{
    if (routes) *routes = s_routes;
    if (count)  *count = sizeof(s_routes) / sizeof(s_routes[0]);
}

// ---- periodic console dump (works even with no browser) ----

static void log_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        dc_registry_entry_t tbl[DC_REGISTRY_MAX];
        int n = dc_registry_get(tbl, DC_REGISTRY_MAX);
        int64_t now = esp_timer_get_time();
        ESP_LOGI(TAG, "-- %d device(s) --", n);
        for (int i = 0; i < n; i++) {
            char s[96]; status_summary(&tbl[i], s, sizeof s);
            ESP_LOGI(TAG, "  [%s] %-18s %-16s %s%s", kind_str(tbl[i].kind),
                     tbl[i].id, tbl[i].name[0] ? tbl[i].name : "?", s,
                     dc_registry_entry_stale(&tbl[i], now) ? "  (stale)" : "");
        }
    }
}

esp_err_t dt_console_start_logger(void)
{
    if (xTaskCreate(log_task, "dt_console", 4096, NULL, 3, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "emulated screen at /devices  (JSON: /api/devices; setup: /setup)");
    return ESP_OK;
}
