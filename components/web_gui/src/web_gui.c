#include "web_gui.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "station_store.h"
#include "radio_audio.h"
#include "esp_err.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "WEB_GUI";
static httpd_handle_t s_server = NULL;
static size_t s_current_station_index = 0;

static size_t web_gui_station_index(size_t index)
{
    size_t count = station_store_count();
    if (count == 0) {
        return 0;
    }
    if (index >= count) {
        return count - 1;
    }
    return index;
}

static const char *s_html =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "  <meta charset=\"utf-8\">\n"
    "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "  <title>Internet Radio</title>\n"
    "  <style>\n"
    "    :root { --bg:#f5f5f7; --panel:rgba(255,255,255,0.72); --text:#1d2733; --muted:#5c6b7a; --line:rgba(15,23,42,0.12); --accent:#2f80ed; --shadow:rgba(15,23,42,0.12); }\n"
    "    * { box-sizing: border-box; } body { margin:0; min-height:100vh; font-family:-apple-system,BlinkMacSystemFont,'SF Pro Display','Segoe UI',sans-serif; background:linear-gradient(180deg,#f4f4f8,#e9edf4); color:var(--text); display:flex; align-items:center; justify-content:center; padding:24px; }\n"
    "    .window { width:min(720px,100%); background:var(--panel); border:1px solid var(--line); border-radius:24px; box-shadow:0 20px 50px var(--shadow); backdrop-filter:blur(18px); overflow:hidden; }\n"
    "    .titlebar { display:flex; align-items:center; justify-content:space-between; padding:14px 18px; border-bottom:1px solid var(--line); font-weight:600; }\n"
    "    .traffic { display:flex; gap:8px; } .traffic span { width:12px; height:12px; display:inline-block; border-radius:50%; background:#f0b646; } .traffic span:nth-child(1){background:#ff5f57;} .traffic span:nth-child(2){background:#febc2e;} .traffic span:nth-child(3){background:#28c840;}\n"
    "    .content { padding:24px; } .station-name { text-align:center; font-size:clamp(1.8rem,3vw,2.6rem); font-weight:700; letter-spacing:-.04em; margin:12px 0 18px; }\n"
    "    .meta { min-height:88px; text-align:center; color:var(--muted); display:grid; place-content:center; gap:6px; }\n"
    "    .meta .artist { font-size:1.1rem; font-weight:600; } .meta .track { font-size:1.3rem; font-weight:500; }\n"
    "    .controls { display:grid; gap:16px; margin-top:18px; } .nav-row { display:flex; justify-content:center; flex-wrap:wrap; gap:16px; }\n"
    "    button { appearance:none; border:1px solid var(--line); border-radius:14px; background:rgba(255,255,255,0.6); color:var(--text); padding:12px 18px; font-size:1rem; min-height:44px; cursor:pointer; }\n"
    "    .primary { background:linear-gradient(180deg, rgba(47,128,237,0.12), rgba(47,128,237,0.04)); border-color:rgba(47,128,237,0.3); }\n"
    "    .slider-wrap { display:grid; gap:10px; padding:18px 20px; border:1px solid var(--line); border-radius:18px; background:rgba(255,255,255,0.38); }\n"
    "    .slider-row { display:flex; justify-content:space-between; align-items:center; gap:16px; } input[type=range] { width:100%; accent-color:var(--accent); }\n"
    "    .volume-value { min-width:54px; text-align:right; font-weight:700; color:var(--muted); }\n"
    "    .status { margin-top:12px; min-height:20px; color:var(--muted); font-size:0.9rem; }\n"
    "    .modal { position:fixed; inset:0; display:none; align-items:center; justify-content:center; background:rgba(15,23,42,0.2); padding:20px; }\n"
    "    .modal.open { display:flex; } .modal-card { width:min(500px,100%); background:rgba(255,255,255,0.9); border:1px solid var(--line); border-radius:20px; padding:20px; box-shadow:0 30px 80px rgba(15,23,42,0.18); }\n"
    "    .field { display:grid; gap:8px; margin-bottom:14px; } input { width:100%; min-height:44px; border-radius:12px; border:1px solid rgba(15,23,42,0.15); padding:10px 12px; font:inherit; }\n"
    "    .modal-actions { display:flex; justify-content:flex-end; gap:12px; margin-top:16px; }\n"
    "    @media (max-width:560px) { body { padding:12px; } .content { padding:16px; } .nav-row, .modal-actions, .slider-row { flex-direction:column; } .modal-actions button { width:100%; } }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <div class=\"window\">\n"
    "    <div class=\"titlebar\">\n"
    "      <div class=\"traffic\"><span></span><span></span><span></span></div>\n"
    "      <div>Internet Radio</div>\n"
    "      <div></div>\n"
    "    </div>\n"
    "    <div class=\"content\">\n"
    "      <div id=\"stationName\" class=\"station-name\">Loading…</div>\n"
    "      <div class=\"meta\">\n"
    "        <div id=\"artist\" class=\"artist\">No artist</div>\n"
    "        <div id=\"track\" class=\"track\">No track information</div>\n"
    "      </div>\n"
    "      <div class=\"controls\">\n"
    "        <div class=\"nav-row\">\n"
    "          <button id=\"prevBtn\" type=\"button\">◀ Previous</button>\n"
    "          <button id=\"nextBtn\" type=\"button\">Next ▶</button>\n"
    "        </div>\n"
    "        <div class=\"slider-wrap\">\n"
    "          <div class=\"slider-row\">\n"
    "            <span>Volume</span>\n"
    "            <div id=\"volumeValue\" class=\"volume-value\">0%</div>\n"
    "          </div>\n"
    "          <input id=\"volumeSlider\" type=\"range\" min=\"0\" max=\"100\" value=\"0\" />\n"
    "        </div>\n"
    "        <button id=\"manageBtn\" class=\"primary\" type=\"button\">Manage Stations</button>\n"
    "      </div>\n"
    "      <div id=\"status\" class=\"status\">Connecting…</div>\n"
    "    </div>\n"
    "  </div>\n"
    "  <div id=\"stationModal\" class=\"modal\">\n"
    "    <div class=\"modal-card\">\n"
    "      <h3 id=\"stationModalTitle\">Add Station</h3>\n"
    "      <div class=\"field\">\n"
    "        <label for=\"stationNameInput\">Station Name</label>\n"
    "        <input id=\"stationNameInput\" type=\"text\" maxlength=\"128\" />\n"
    "      </div>\n"
    "      <div class=\"field\">\n"
    "        <label for=\"stationUrlInput\">Stream URL</label>\n"
    "        <input id=\"stationUrlInput\" type=\"url\" maxlength=\"1024\" />\n"
    "      </div>\n"
    "      <div class=\"modal-actions\">\n"
    "        <button id=\"cancelStationBtn\" type=\"button\">Cancel</button>\n"
    "        <button id=\"saveStationBtn\" class=\"primary\" type=\"button\">Save</button>\n"
    "      </div>\n"
    "    </div>\n"
    "  </div>\n"
    "  <script>\n"
    "    const state = { stationIndex: 0, stationCount: 0, volume: 0, artist: '', track: '', playing: false, streamConnected: false, station: null };\n"
    "    const api = { async getState() { const res = await fetch('/api/state'); return res.ok ? res.json() : null; }, async send(type, data) { const res = await fetch('/api/command', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ type, data }) }); return res.ok ? res.json() : { type: 'error', data: { message: 'Request failed' } }; } };\n"
    "    function setStatus(msg) { document.getElementById('status').textContent = msg; }\n"
    "    function applyState(data) { if (!data) return; state.stationIndex = Number(data.stationIndex ?? 0); state.stationCount = Number(data.stationCount ?? 0); state.volume = Number(data.volume ?? 0); state.playing = !!data.playing; state.streamConnected = !!data.streamConnected; state.station = data.station || null; if (state.station && state.station.name) document.getElementById('stationName').textContent = state.station.name; if (data.artist) state.artist = data.artist; if (data.track) state.track = data.track; document.getElementById('artist').textContent = state.artist || 'No artist'; document.getElementById('track').textContent = state.track || 'No track information'; const slider = document.getElementById('volumeSlider'); slider.value = String(state.volume); document.getElementById('volumeValue').textContent = state.volume + '%'; setStatus(state.streamConnected ? 'Connected' : 'Waiting for stream'); }\n"
    "    async function refreshState() { const payload = await api.getState(); if (payload) applyState(payload.data || payload); }\n"
    "    document.getElementById('prevBtn').addEventListener('click', async () => { const res = await api.send('stationPrevious'); if (res && res.data) applyState(res.data); });\n"
    "    document.getElementById('nextBtn').addEventListener('click', async () => { const res = await api.send('stationNext'); if (res && res.data) applyState(res.data); });\n"
    "    document.getElementById('manageBtn').addEventListener('click', () => { document.getElementById('stationModal').classList.add('open'); });\n"
    "    document.getElementById('cancelStationBtn').addEventListener('click', () => document.getElementById('stationModal').classList.remove('open'));\n"
    "    document.getElementById('saveStationBtn').addEventListener('click', async () => { const name = document.getElementById('stationNameInput').value.trim(); const url = document.getElementById('stationUrlInput').value.trim(); if (!name || !url) { setStatus('Name and URL are required'); return; } const res = await api.send('stationAdd', { name, url }); document.getElementById('stationModal').classList.remove('open'); if (res && res.data) applyState(res.data); });\n"
    "    document.getElementById('volumeSlider').addEventListener('input', async (event) => { const value = Number(event.target.value); document.getElementById('volumeValue').textContent = value + '%'; const res = await api.send('volumeSet', { value }); if (res && res.data) applyState(res.data); });\n"
    "    document.addEventListener('keydown', async (event) => { if (['INPUT','TEXTAREA','SELECT'].includes(document.activeElement.tagName)) return; if (event.key === 'ArrowUp') { event.preventDefault(); const res = await api.send('stationPrevious'); if (res && res.data) applyState(res.data); } if (event.key === 'ArrowDown') { event.preventDefault(); const res = await api.send('stationNext'); if (res && res.data) applyState(res.data); } });\n"
    "    refreshState();\n"
    "  </script>\n"
    "</body>\n"
    "</html>\n";

static esp_err_t web_gui_response_json(httpd_req_t *req, const cJSON *json)
{
    char *payload = cJSON_PrintUnformatted(json);
    if (!payload) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unable to encode JSON response");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, payload, strlen(payload));
    free(payload);
    return err;
}

static void web_gui_fill_state(cJSON *data)
{
    size_t station_index = web_gui_station_index(s_current_station_index);
    size_t station_count = station_store_count();
    cJSON_AddNumberToObject(data, "stationIndex", (double)station_index);
    cJSON_AddNumberToObject(data, "stationCount", (double)station_count);
    cJSON_AddNumberToObject(data, "volume", (double)60);
    cJSON_AddBoolToObject(data, "playing", true);
    cJSON_AddBoolToObject(data, "streamConnected", true);
    cJSON_AddStringToObject(data, "artist", "");
    cJSON_AddStringToObject(data, "track", "");

    const radio_station_t *station = station_store_get(station_index);
    if (station) {
        cJSON *station_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(station_obj, "name", station->name);
        cJSON_AddStringToObject(station_obj, "url", station->url);
        cJSON_AddItemToObject(data, "station", station_obj);
    }
}

static esp_err_t web_gui_handle_command(httpd_req_t *req)
{
    uint8_t buffer[1024] = {0};
    int received = httpd_req_recv(req, (char *)buffer, sizeof(buffer) - 1);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No command payload received");
    }
    buffer[received] = '\0';

    cJSON *msg = cJSON_Parse((const char *)buffer);
    if (!msg) {
        cJSON *root = cJSON_CreateObject();
        cJSON *data = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "type", "error");
        cJSON_AddItemToObject(root, "data", data);
        cJSON_AddStringToObject(data, "message", "Invalid JSON payload");
        esp_err_t err = web_gui_response_json(req, root);
        cJSON_Delete(root);
        return err;
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(msg, "type");
    cJSON *data = cJSON_GetObjectItemCaseSensitive(msg, "data");
    cJSON *root = cJSON_CreateObject();
    cJSON *response_data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ack");
    cJSON_AddItemToObject(root, "data", response_data);

    if (type && cJSON_IsString(type) && strcmp(type->valuestring, "getState") == 0) {
        cJSON_AddStringToObject(root, "type", "state");
        web_gui_fill_state(response_data);
    } else if (type && cJSON_IsString(type) && strcmp(type->valuestring, "stationPrevious") == 0) {
        size_t count = station_store_count();
        if (count > 0) {
            if (s_current_station_index == 0) {
                s_current_station_index = count - 1;
            } else {
                s_current_station_index--;
            }
            const radio_station_t *station = station_store_get(s_current_station_index);
            if (station) {
                radio_audio_play(station);
            }
        }
        cJSON_AddStringToObject(root, "type", "state");
        web_gui_fill_state(response_data);
    } else if (type && cJSON_IsString(type) && strcmp(type->valuestring, "stationNext") == 0) {
        size_t count = station_store_count();
        if (count > 0) {
            s_current_station_index = (s_current_station_index + 1) % count;
            const radio_station_t *station = station_store_get(s_current_station_index);
            if (station) {
                radio_audio_play(station);
            }
        }
        cJSON_AddStringToObject(root, "type", "state");
        web_gui_fill_state(response_data);
    } else if (type && cJSON_IsString(type) && strcmp(type->valuestring, "volumeSet") == 0) {
        int value = 0;
        cJSON *value_obj = data ? cJSON_GetObjectItemCaseSensitive(data, "value") : NULL;
        if (value_obj && cJSON_IsNumber(value_obj)) {
            value = (int)value_obj->valueint;
        }
        value = value < 0 ? 0 : value;
        value = value > 100 ? 100 : value;
        radio_audio_set_volume(value);
        cJSON_AddNumberToObject(response_data, "volume", (double)value);
        cJSON_AddStringToObject(root, "type", "volume");
    } else if (type && cJSON_IsString(type) && strcmp(type->valuestring, "stationAdd") == 0) {
        const char *name = data ? cJSON_GetObjectItemCaseSensitive(data, "name") ? cJSON_GetObjectItemCaseSensitive(data, "name")->valuestring : NULL : NULL;
        const char *url = data ? cJSON_GetObjectItemCaseSensitive(data, "url") ? cJSON_GetObjectItemCaseSensitive(data, "url")->valuestring : NULL : NULL;
        if (!name || !url || strlen(name) == 0 || strlen(url) == 0) {
            cJSON_AddStringToObject(response_data, "message", "Station name and URL are required");
            cJSON_AddStringToObject(root, "type", "error");
        } else {
            radio_station_t station = {0};
            strlcpy(station.name, name, sizeof(station.name));
            strlcpy(station.url, url, sizeof(station.url));
            station.codec = RADIO_CODEC_MP3;
            if (station_store_add(&station) && station_store_save() == ESP_OK) {
                s_current_station_index = web_gui_station_index(station_store_count() - 1);
                cJSON_AddStringToObject(root, "type", "state");
                web_gui_fill_state(response_data);
            } else {
                cJSON_AddStringToObject(response_data, "message", "Unable to save station");
                cJSON_AddStringToObject(root, "type", "error");
            }
        }
    } else {
        cJSON_AddStringToObject(response_data, "message", "Unsupported command");
        cJSON_AddStringToObject(root, "type", "error");
    }

    esp_err_t err = web_gui_response_json(req, root);
    cJSON_Delete(msg);
    cJSON_Delete(root);
    return err;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, s_html, strlen(s_html));
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "state");
    cJSON_AddItemToObject(root, "data", data);
    web_gui_fill_state(data);
    esp_err_t err = web_gui_response_json(req, root);
    cJSON_Delete(root);
    return err;
}

static esp_err_t command_post_handler(httpd_req_t *req)
{
    return web_gui_handle_command(req);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    httpd_resp_send_err(req, HTTPD_501_METHOD_NOT_IMPLEMENTED, "WebSocket support is not enabled for this ESP-IDF build");
    return ESP_OK;
}

static const httpd_uri_t uri_get_root = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_get_state = {
    .uri = "/api/state",
    .method = HTTP_GET,
    .handler = state_get_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_post_command = {
    .uri = "/api/command",
    .method = HTTP_POST,
    .handler = command_post_handler,
    .user_ctx = NULL
};

static const httpd_uri_t uri_ws = {
    .uri = "/ws",
    .method = HTTP_GET,
    .handler = ws_handler,
    .user_ctx = NULL
};

esp_err_t web_gui_init(void)
{
    if (s_server) return ESP_OK;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 4;
    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) return err;

    httpd_register_uri_handler(s_server, &uri_get_root);
    httpd_register_uri_handler(s_server, &uri_get_state);
    httpd_register_uri_handler(s_server, &uri_post_command);
    httpd_register_uri_handler(s_server, &uri_ws);
    ESP_LOGI(TAG, "Web GUI started");
    return ESP_OK;
}

void web_gui_deinit(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
