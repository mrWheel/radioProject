#include "web_gui.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "station_store.h"
#include "radio_audio.h"
#include "radio_settings.h"
#include "radio_storage.h"
#include "esp_err.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

//-- Not just "WS clients": this caps httpd's total concurrent sockets
//-- (also used for config.max_open_sockets below). A single browser page
//-- load alone opens ~5 concurrent connections (index.html, style.css,
//-- app.js, favicon.ico, /ws), so 4 was too low even for one client and
//-- silently starved a 2nd browser's page load (stuck on "Loading.." since
//-- app.js itself never finished fetching).
#define WEB_GUI_MAX_CLIENTS 8
#define WEB_GUI_WS_QUEUE_LEN 8
//-- Mirrors station_store's own 32KB cap on stations.json, so an oversized
//-- upload is rejected up front instead of allocating an unbounded buffer.
#define WEB_GUI_STATIONS_IMPORT_MAX_LEN 32768

static const char* TAG = "WEB_GUI";
static httpd_handle_t s_server = NULL;
static size_t s_current_station_index = 0;
static char s_current_line1[160] = "";
static char s_current_line2[160] = "";
static char s_current_line3[160] = "";
static QueueHandle_t s_ws_queue = NULL;
static TaskHandle_t s_ws_worker_task = NULL;
static web_gui_state_applied_cb_t s_state_applied_cb = NULL;
static void* s_state_applied_ctx = NULL;
static web_gui_audio_owner_cb_t s_audio_owner_cb = NULL;
static void* s_audio_owner_ctx = NULL;
static web_gui_audio_owner_t s_audio_owner = WEB_GUI_AUDIO_OWNER_DEVICE;
//-- Only one browser may be connected to /ws at a time; -1 means no client.
static int s_active_ws_fd = -1;

//-- Commands received over the /ws endpoint are queued here so the WebSocket
//-- callback never performs long-running station/filesystem operations itself.
typedef struct
{
  char type[24];
  char name[128];
  char url[1200];
  char codec[8];
  char owner[16];
  int value;
} web_gui_ws_cmd_t;

static size_t web_gui_station_index(size_t index)
{
  size_t count = station_store_count();
  if (count == 0)
  {
    return 0;
  }
  if (index >= count)
  {
    return count - 1;
  }
  return index;
}

//-- Streams a file from the mounted LittleFS partition (RADIO_STORAGE_PATH)
//-- as the HTTP response body, in bounded chunks so large files never need
//-- a single large heap allocation. Responds 404 instead of crashing when
//-- the requested file is missing from the filesystem.
static esp_err_t web_gui_serve_file(httpd_req_t* req, const char* filename,
                                    const char* content_type)
{
  char path[160];
  snprintf(path, sizeof(path), "%s/%s", RADIO_STORAGE_PATH, filename);

  FILE* f = fopen(path, "r");
  if (!f)
  {
    ESP_LOGW(TAG, "Static file not found: %s", path);
    return httpd_resp_send_404(req);
  }

  httpd_resp_set_type(req, content_type);
  //-- Firmware/UI assets change across dev builds; without this, a browser
  //-- that already visited this device's IP keeps serving a stale cached
  //-- index.html/app.js/style.css forever, silently diverging in behavior
  //-- from a browser that never cached it (this previously left one client
  //-- stuck showing old UI/logic while another worked fine).
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  char chunk[1024];
  size_t read_len;
  esp_err_t err = ESP_OK;
  while ((read_len = fread(chunk, 1, sizeof(chunk), f)) > 0)
  {
    err = httpd_resp_send_chunk(req, chunk, read_len);
    if (err != ESP_OK)
    {
      break;
    }
  }
  fclose(f);
  if (err == ESP_OK)
  {
    err = httpd_resp_send_chunk(req, NULL, 0);
  }
  return err;
}

static esp_err_t web_gui_response_json(httpd_req_t* req, const cJSON* json)
{
  char* payload = cJSON_PrintUnformatted(json);
  if (!payload)
  {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unable to encode JSON response");
  }
  httpd_resp_set_type(req, "application/json");
  esp_err_t err = httpd_resp_send(req, payload, strlen(payload));
  free(payload);
  return err;
}

static void web_gui_fill_state(cJSON* data)
{
  size_t station_index = web_gui_station_index(s_current_station_index);
  size_t station_count = station_store_count();
  cJSON_AddNumberToObject(data, "stationIndex", (double)station_index);
  cJSON_AddNumberToObject(data, "stationCount", (double)station_count);
  cJSON_AddNumberToObject(data, "volume", (double)radio_audio_get_volume());
  cJSON_AddBoolToObject(data, "playing", station_count > 0 && !radio_audio_is_paused());
  cJSON_AddBoolToObject(data, "streamConnected", true);
  cJSON_AddStringToObject(data, "audioOwner",
                          s_audio_owner == WEB_GUI_AUDIO_OWNER_BROWSER ? "browser" : "device");
  cJSON_AddStringToObject(data, "line1", s_current_line1);
  cJSON_AddStringToObject(data, "line2", s_current_line2);
  cJSON_AddStringToObject(data, "line3", s_current_line3);

  const radio_station_t* station = station_store_get(station_index);
  if (station)
  {
    cJSON* station_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(station_obj, "name", station->name);
    cJSON_AddStringToObject(station_obj, "url", station->url);
    cJSON_AddStringToObject(station_obj, "codec",
                            station->codec == RADIO_CODEC_AAC ? "aac" : "mp3");
    cJSON_AddItemToObject(data, "station", station_obj);
  }
}

//-- Fills a radio_station_t from name/url/codec strings, defaulting codec to mp3.
static void web_gui_build_station(radio_station_t* station, const char* name, const char* url,
                                  const char* codec)
{
  memset(station, 0, sizeof(*station));
  strlcpy(station->name, name ? name : "", sizeof(station->name));
  strlcpy(station->url, url ? url : "", sizeof(station->url));
  station->codec = (codec && strcasecmp(codec, "aac") == 0) ? RADIO_CODEC_AAC : RADIO_CODEC_MP3;
}

//-- Shared by the HTTP /api/command handler and the WebSocket worker task so both
//-- transports apply commands identically. Must only be called outside of the
//-- WebSocket receive callback, since it performs blocking station/filesystem work.
static void web_gui_apply_command(const char* type, const char* name, const char* url,
                                  const char* codec, const char* owner, int value, cJSON* root,
                                  cJSON* response_data)
{
  cJSON_AddStringToObject(root, "type", "ack");

  if (strcmp(type, "getState") == 0)
  {
    cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("state"));
    web_gui_fill_state(response_data);
  }
  else if (strcmp(type, "audioOwnerSet") == 0)
  {
    web_gui_audio_owner_t next_owner = WEB_GUI_AUDIO_OWNER_DEVICE;
    if (owner && strcasecmp(owner, "browser") == 0)
    {
      next_owner = WEB_GUI_AUDIO_OWNER_BROWSER;
    }
    web_gui_set_audio_owner(next_owner);
    if (s_audio_owner_cb)
    {
      s_audio_owner_cb(s_audio_owner, s_audio_owner_ctx);
    }
    cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("state"));
    web_gui_fill_state(response_data);
  }
  else if (strcmp(type, "stationPrevious") == 0)
  {
    size_t count = station_store_count();
    if (count > 0)
    {
      if (s_current_station_index == 0)
      {
        s_current_station_index = count - 1;
      }
      else
      {
        s_current_station_index--;
      }
      const radio_station_t* station = station_store_get(s_current_station_index);
      if (station)
      {
        radio_audio_play(station);
        radio_settings_save(s_current_station_index);
      }
    }
    cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("state"));
    web_gui_fill_state(response_data);
  }
  else if (strcmp(type, "stationNext") == 0)
  {
    size_t count = station_store_count();
    if (count > 0)
    {
      s_current_station_index = (s_current_station_index + 1) % count;
      const radio_station_t* station = station_store_get(s_current_station_index);
      if (station)
      {
        radio_audio_play(station);
        radio_settings_save(s_current_station_index);
      }
    }
    cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("state"));
    web_gui_fill_state(response_data);
  }
  else if (strcmp(type, "play") == 0)
  {
    radio_audio_set_paused(false);
    cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("state"));
    web_gui_fill_state(response_data);
  }
  else if (strcmp(type, "pause") == 0)
  {
    radio_audio_set_paused(true);
    cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("state"));
    web_gui_fill_state(response_data);
  }
  else if (strcmp(type, "volumeSet") == 0)
  {
    int clamped = value < 0 ? 0 : (value > 100 ? 100 : value);
    radio_audio_set_volume(clamped);
    cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("state"));
    web_gui_fill_state(response_data);
  }
  else if (strcmp(type, "stationAdd") == 0)
  {
    if (!name || !url || strlen(name) == 0 || strlen(url) == 0)
    {
      cJSON_AddStringToObject(response_data, "message", "Station name and URL are required");
      cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("error"));
    }
    else
    {
      radio_station_t station;
      web_gui_build_station(&station, name, url, codec);
      if (station_store_add(&station) && station_store_save() == ESP_OK)
      {
        s_current_station_index = web_gui_station_index(station_store_count() - 1);
        cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("state"));
        web_gui_fill_state(response_data);
      }
      else
      {
        cJSON_AddStringToObject(response_data, "message", "Unable to save station");
        cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("error"));
      }
    }
  }
  else if (strcmp(type, "stationEdit") == 0)
  {
    if (!name || !url || strlen(name) == 0 || strlen(url) == 0)
    {
      cJSON_AddStringToObject(response_data, "message", "Station name and URL are required");
      cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("error"));
    }
    else
    {
      radio_station_t station;
      web_gui_build_station(&station, name, url, codec);
      size_t index = web_gui_station_index(s_current_station_index);
      if (station_store_edit(index, &station) && station_store_save() == ESP_OK)
      {
        cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("state"));
        web_gui_fill_state(response_data);
      }
      else
      {
        cJSON_AddStringToObject(response_data, "message", "Unable to save station");
        cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("error"));
      }
    }
  }
  else if (strcmp(type, "stationDelete") == 0)
  {
    size_t index = web_gui_station_index(s_current_station_index);
    if (station_store_count() == 0 || !station_store_delete(index) ||
        station_store_save() != ESP_OK)
    {
      cJSON_AddStringToObject(response_data, "message", "Unable to delete station");
      cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("error"));
    }
    else
    {
      s_current_station_index = web_gui_station_index(s_current_station_index);
      cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("state"));
      web_gui_fill_state(response_data);
    }
  }
  else
  {
    cJSON_AddStringToObject(response_data, "message", "Unsupported command");
    cJSON_ReplaceItemInObject(root, "type", cJSON_CreateString("error"));
  }

  //-- Mirror a successful, state-changing web GUI command onto the physical
  //-- display/EC11 tracking (app_main), so a station/volume change made from
  //-- the browser doesn't leave the device's own state stale. getState never
  //-- mutates anything, so it's excluded even though it also returns "state".
  if (s_state_applied_cb && strcmp(type, "getState") != 0)
  {
    cJSON* result_type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (result_type && cJSON_IsString(result_type) &&
        strcmp(result_type->valuestring, "state") == 0)
    {
      s_state_applied_cb(s_current_station_index, radio_audio_get_volume(), s_state_applied_ctx);
    }
  }
}

void web_gui_set_audio_owner(web_gui_audio_owner_t owner)
{
  s_audio_owner = owner;
}

web_gui_audio_owner_t web_gui_get_audio_owner(void)
{
  return s_audio_owner;
}

static esp_err_t web_gui_handle_command(httpd_req_t* req)
{
  uint8_t buffer[1024] = {0};
  int received = httpd_req_recv(req, (char*)buffer, sizeof(buffer) - 1);
  if (received <= 0)
  {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No command payload received");
  }
  buffer[received] = '\0';

  cJSON* msg = cJSON_Parse((const char*)buffer);
  if (!msg)
  {
    cJSON* root = cJSON_CreateObject();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "message", "Invalid JSON payload");
    esp_err_t err = web_gui_response_json(req, root);
    cJSON_Delete(root);
    return err;
  }

  cJSON* type_obj = cJSON_GetObjectItemCaseSensitive(msg, "type");
  cJSON* data = cJSON_GetObjectItemCaseSensitive(msg, "data");
  cJSON* root = cJSON_CreateObject();
  cJSON* response_data = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "data", response_data);

  const char* type = (type_obj && cJSON_IsString(type_obj)) ? type_obj->valuestring : "";
  const char* name = NULL;
  const char* url = NULL;
  const char* codec = NULL;
  const char* owner = NULL;
  int value = 0;
  if (data)
  {
    cJSON* name_obj = cJSON_GetObjectItemCaseSensitive(data, "name");
    cJSON* url_obj = cJSON_GetObjectItemCaseSensitive(data, "url");
    cJSON* codec_obj = cJSON_GetObjectItemCaseSensitive(data, "codec");
    cJSON* owner_obj = cJSON_GetObjectItemCaseSensitive(data, "owner");
    cJSON* value_obj = cJSON_GetObjectItemCaseSensitive(data, "value");
    if (name_obj && cJSON_IsString(name_obj))
      name = name_obj->valuestring;
    if (url_obj && cJSON_IsString(url_obj))
      url = url_obj->valuestring;
    if (codec_obj && cJSON_IsString(codec_obj))
      codec = codec_obj->valuestring;
    if (owner_obj && cJSON_IsString(owner_obj))
      owner = owner_obj->valuestring;
    if (value_obj && cJSON_IsNumber(value_obj))
      value = value_obj->valueint;
  }
  web_gui_apply_command(type, name, url, codec, owner, value, root, response_data);

  esp_err_t err = web_gui_response_json(req, root);
  cJSON_Delete(msg);
  cJSON_Delete(root);
  return err;
}

static esp_err_t root_get_handler(httpd_req_t* req)
{
  return web_gui_serve_file(req, "index.html", "text/html");
}

static esp_err_t style_get_handler(httpd_req_t* req)
{
  return web_gui_serve_file(req, "style.css", "text/css");
}

static esp_err_t app_js_get_handler(httpd_req_t* req)
{
  return web_gui_serve_file(req, "app.js", "application/javascript");
}

static esp_err_t favicon_get_handler(httpd_req_t* req)
{
  return web_gui_serve_file(req, "favicon.ico", "image/x-icon");
}

static esp_err_t state_get_handler(httpd_req_t* req)
{
  cJSON* root = cJSON_CreateObject();
  cJSON* data = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "state");
  cJSON_AddItemToObject(root, "data", data);
  web_gui_fill_state(data);
  esp_err_t err = web_gui_response_json(req, root);
  cJSON_Delete(root);
  return err;
}

static esp_err_t command_post_handler(httpd_req_t* req)
{
  return web_gui_handle_command(req);
}

static esp_err_t web_gui_ws_send_json(httpd_req_t* req, cJSON* json)
{
  char* payload = cJSON_PrintUnformatted(json);
  if (!payload)
  {
    return ESP_ERR_NO_MEM;
  }
  httpd_ws_frame_t pkt = {0};
  pkt.type = HTTPD_WS_TYPE_TEXT;
  pkt.payload = (uint8_t*)payload;
  pkt.len = strlen(payload);
  esp_err_t err = httpd_ws_send_frame(req, &pkt);
  free(payload);
  return err;
}

static esp_err_t web_gui_ws_reply_error(httpd_req_t* req, const char* message)
{
  cJSON* root = cJSON_CreateObject();
  cJSON* data = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "error");
  cJSON_AddItemToObject(root, "data", data);
  cJSON_AddStringToObject(data, "message", message);
  esp_err_t err = web_gui_ws_send_json(req, root);
  cJSON_Delete(root);
  return err;
}

//-- Sends a JSON message to every currently connected WebSocket client, so that
//-- state changes (station/volume/track) reach all open browser sessions.
static void web_gui_ws_broadcast(cJSON* root)
{
  if (!s_server)
  {
    return;
  }
  char* payload = cJSON_PrintUnformatted(root);
  if (!payload)
  {
    return;
  }
  int client_fds[WEB_GUI_MAX_CLIENTS];
  size_t fd_count = WEB_GUI_MAX_CLIENTS;
  if (httpd_get_client_list(s_server, &fd_count, client_fds) == ESP_OK)
  {
    for (size_t i = 0; i < fd_count; ++i)
    {
      int fd = client_fds[i];
      if (httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET)
      {
        continue;
      }
      httpd_ws_frame_t pkt = {0};
      pkt.type = HTTPD_WS_TYPE_TEXT;
      pkt.payload = (uint8_t*)payload;
      pkt.len = strlen(payload);
      httpd_ws_send_frame_async(s_server, fd, &pkt);
    }
  }
  free(payload);
}

//-- Serves the raw stations.json file as a browser download (Manage Stations'
//-- "Download Stations" button), separate from web_gui_serve_file() since it
//-- needs Content-Disposition instead of a page content type.
static esp_err_t stations_export_get_handler(httpd_req_t* req)
{
  char path[160];
  snprintf(path, sizeof(path), "%s/stations.json", RADIO_STORAGE_PATH);

  FILE* f = fopen(path, "r");
  if (!f)
  {
    ESP_LOGW(TAG, "stations.json not found for export: %s", path);
    return httpd_resp_send_404(req);
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"stations.json\"");
  char chunk[1024];
  size_t read_len;
  esp_err_t err = ESP_OK;
  while ((read_len = fread(chunk, 1, sizeof(chunk), f)) > 0)
  {
    err = httpd_resp_send_chunk(req, chunk, read_len);
    if (err != ESP_OK)
    {
      break;
    }
  }
  fclose(f);
  if (err == ESP_OK)
  {
    err = httpd_resp_send_chunk(req, NULL, 0);
  }
  return err;
}

//-- Receives an uploaded stations.json body (Manage Stations' "Upload Stations"
//-- button) and hands it to station_store_import(), which validates before it
//-- touches disk or the live station list. Runs on the httpd worker task, not
//-- the WebSocket callback, so the blocking read/parse/file-write here is fine.
static esp_err_t stations_import_post_handler(httpd_req_t* req)
{
  if (req->content_len == 0 || req->content_len > WEB_GUI_STATIONS_IMPORT_MAX_LEN)
  {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "stations.json must be a non-empty file of at most 32KB");
  }

  size_t content_len = (size_t)req->content_len;
  char* buf = malloc(content_len + 1);
  if (!buf)
  {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
  }

  size_t received_total = 0;
  while (received_total < content_len)
  {
    int received = httpd_req_recv(req, buf + received_total, content_len - received_total);
    if (received <= 0)
    {
      free(buf);
      if (received == HTTPD_SOCK_ERR_TIMEOUT)
      {
        httpd_resp_send_408(req);
      }
      else
      {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed reading upload body");
      }
      return ESP_FAIL;
    }
    received_total += (size_t)received;
  }
  buf[content_len] = '\0';

  esp_err_t import_err = station_store_import(buf);
  free(buf);

  cJSON* root = cJSON_CreateObject();
  cJSON* response_data = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "data", response_data);

  if (import_err == ESP_OK)
  {
    s_current_station_index = web_gui_station_index(s_current_station_index);
    cJSON_AddStringToObject(root, "type", "state");
    web_gui_fill_state(response_data);

    //-- Every other connected browser, plus the physical TFT/EC11 state,
    //-- must also pick up the new station list right away.
    cJSON* broadcast = cJSON_CreateObject();
    cJSON* broadcast_data = cJSON_CreateObject();
    cJSON_AddStringToObject(broadcast, "type", "state");
    cJSON_AddItemToObject(broadcast, "data", broadcast_data);
    web_gui_fill_state(broadcast_data);
    web_gui_ws_broadcast(broadcast);
    cJSON_Delete(broadcast);

    if (s_state_applied_cb)
    {
      s_state_applied_cb(s_current_station_index, radio_audio_get_volume(), s_state_applied_ctx);
    }
  }
  else
  {
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(response_data, "message",
                            import_err == ESP_ERR_NOT_FOUND
                                ? "No valid stations found in uploaded file"
                                : "Unable to import stations.json");
  }

  esp_err_t err = web_gui_response_json(req, root);
  cJSON_Delete(root);
  return err;
}

//-- Runs the queued WebSocket commands outside the WebSocket receive callback,
//-- since station changes and file writes may block on network/filesystem I/O.
static void web_gui_ws_worker_task(void* arg)
{
  (void)arg;
  web_gui_ws_cmd_t cmd;
  for (;;)
  {
    if (xQueueReceive(s_ws_queue, &cmd, portMAX_DELAY) != pdTRUE)
    {
      continue;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON* response_data = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "data", response_data);
    web_gui_apply_command(cmd.type, cmd.name[0] ? cmd.name : NULL, cmd.url[0] ? cmd.url : NULL,
                          cmd.codec[0] ? cmd.codec : NULL, cmd.owner[0] ? cmd.owner : NULL,
                          cmd.value, root, response_data);
    web_gui_ws_broadcast(root);
    cJSON_Delete(root);
  }
}

//-- Only one browser session may control the radio at a time. Checked on
//-- every /ws data frame (not ws_post_handshake_cb: registering that hook
//-- disrupted the normal WS receive/reply flow in this IDF build, leaving
//-- the new client's getState reply unanswered and stuck retrying forever).
//-- A no-op once s_active_ws_fd already matches the calling fd.
static void web_gui_ws_check_takeover(httpd_req_t* req)
{
  httpd_handle_t server = req->handle;
  int new_fd = httpd_req_to_sockfd(req);
  if (new_fd == s_active_ws_fd)
  {
    return;
  }

  if (s_active_ws_fd >= 0)
  {
    cJSON* root = cJSON_CreateObject();
    cJSON* data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "disconnected");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "message", "Connection lost or taken over");
    char* payload = cJSON_PrintUnformatted(root);
    if (payload)
    {
      httpd_ws_frame_t pkt = {0};
      pkt.type = HTTPD_WS_TYPE_TEXT;
      pkt.payload = (uint8_t*)payload;
      pkt.len = strlen(payload);
      httpd_ws_send_frame_async(server, s_active_ws_fd, &pkt);
      free(payload);
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "New WS client fd=%d taking over from fd=%d", new_fd, s_active_ws_fd);
    httpd_sess_trigger_close(server, s_active_ws_fd);
  }
  s_active_ws_fd = new_fd;
  ESP_LOGI(TAG, "Active WS client is now fd=%d", new_fd);
}

//-- Clears the tracked active client when its socket actually closes, so a
//-- reconnect after a normal disconnect isn't mistaken for a takeover.
static void web_gui_httpd_close_fn(httpd_handle_t hd, int sockfd)
{
  (void)hd;
  if (sockfd == s_active_ws_fd)
  {
    s_active_ws_fd = -1;
  }
  close(sockfd);
}

//-- WebSocket data handler for /ws. Only receives, validates and queues commands;
//-- the actual station/volume/file work happens in web_gui_ws_worker_task.
static esp_err_t ws_handler(httpd_req_t* req)
{
  web_gui_ws_check_takeover(req);

  httpd_ws_frame_t ws_pkt = {0};
  ws_pkt.type = HTTPD_WS_TYPE_TEXT;
  esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
  if (ret != ESP_OK)
  {
    ESP_LOGW(TAG, "ws recv frame length failed: %s", esp_err_to_name(ret));
    return ret;
  }
  if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE || ws_pkt.len == 0)
  {
    return ESP_OK;
  }
  if (ws_pkt.type != HTTPD_WS_TYPE_TEXT)
  {
    return ESP_OK;
  }

  uint8_t* buf = calloc(1, ws_pkt.len + 1);
  if (!buf)
  {
    return ESP_ERR_NO_MEM;
  }
  ws_pkt.payload = buf;
  ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
  if (ret != ESP_OK)
  {
    ESP_LOGW(TAG, "ws recv frame payload failed: %s", esp_err_to_name(ret));
    free(buf);
    return ret;
  }

  cJSON* msg = cJSON_Parse((const char*)buf);
  free(buf);
  if (!msg)
  {
    return web_gui_ws_reply_error(req, "Invalid JSON payload");
  }

  cJSON* type_obj = cJSON_GetObjectItemCaseSensitive(msg, "type");
  if (!type_obj || !cJSON_IsString(type_obj))
  {
    cJSON_Delete(msg);
    return web_gui_ws_reply_error(req, "Missing command type");
  }
  const char* type = type_obj->valuestring;

  //-- Answered inline, same as getState: lets the browser's heartbeat
  //-- confirm the connection is alive during idle periods (no station/
  //-- volume change to broadcast), without touching the command queue
  //-- or any audio-path state.
  if (strcmp(type, "ping") == 0)
  {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "pong");
    esp_err_t err = web_gui_ws_send_json(req, root);
    cJSON_Delete(root);
    cJSON_Delete(msg);
    return err;
  }

  if (strcmp(type, "getState") == 0)
  {
    cJSON* root = cJSON_CreateObject();
    cJSON* response_data = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "state");
    cJSON_AddItemToObject(root, "data", response_data);
    web_gui_fill_state(response_data);
    esp_err_t err = web_gui_ws_send_json(req, root);
    cJSON_Delete(root);
    cJSON_Delete(msg);
    return err;
  }

  static const char* k_queued_types[] = {"stationPrevious", "stationNext", "play",
                                         "pause",           "volumeSet",   "audioOwnerSet",
                                         "stationAdd",      "stationEdit", "stationDelete"};
  bool known = false;
  for (size_t i = 0; i < sizeof(k_queued_types) / sizeof(k_queued_types[0]); ++i)
  {
    if (strcmp(type, k_queued_types[i]) == 0)
    {
      known = true;
      break;
    }
  }
  if (!known)
  {
    cJSON_Delete(msg);
    return web_gui_ws_reply_error(req, "Unsupported command");
  }

  web_gui_ws_cmd_t cmd = {0};
  strlcpy(cmd.type, type, sizeof(cmd.type));
  cJSON* data = cJSON_GetObjectItemCaseSensitive(msg, "data");
  if (data)
  {
    cJSON* name_obj = cJSON_GetObjectItemCaseSensitive(data, "name");
    cJSON* url_obj = cJSON_GetObjectItemCaseSensitive(data, "url");
    cJSON* codec_obj = cJSON_GetObjectItemCaseSensitive(data, "codec");
    cJSON* owner_obj = cJSON_GetObjectItemCaseSensitive(data, "owner");
    cJSON* value_obj = cJSON_GetObjectItemCaseSensitive(data, "value");
    if (name_obj && cJSON_IsString(name_obj))
      strlcpy(cmd.name, name_obj->valuestring, sizeof(cmd.name));
    if (url_obj && cJSON_IsString(url_obj))
      strlcpy(cmd.url, url_obj->valuestring, sizeof(cmd.url));
    if (codec_obj && cJSON_IsString(codec_obj))
      strlcpy(cmd.codec, codec_obj->valuestring, sizeof(cmd.codec));
    if (owner_obj && cJSON_IsString(owner_obj))
      strlcpy(cmd.owner, owner_obj->valuestring, sizeof(cmd.owner));
    if (value_obj && cJSON_IsNumber(value_obj))
      cmd.value = value_obj->valueint;
  }
  cJSON_Delete(msg);

  if (!s_ws_queue || xQueueSend(s_ws_queue, &cmd, 0) != pdTRUE)
  {
    return web_gui_ws_reply_error(req, "Server busy, try again");
  }
  return ESP_OK;
}

static const httpd_uri_t uri_get_root = {
    .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL};

static const httpd_uri_t uri_get_index = {
    .uri = "/index.html", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL};

static const httpd_uri_t uri_get_style = {
    .uri = "/style.css", .method = HTTP_GET, .handler = style_get_handler, .user_ctx = NULL};

static const httpd_uri_t uri_get_app_js = {
    .uri = "/app.js", .method = HTTP_GET, .handler = app_js_get_handler, .user_ctx = NULL};

static const httpd_uri_t uri_get_favicon = {
    .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_get_handler, .user_ctx = NULL};

static const httpd_uri_t uri_get_state = {
    .uri = "/api/state", .method = HTTP_GET, .handler = state_get_handler, .user_ctx = NULL};

static const httpd_uri_t uri_post_command = {
    .uri = "/api/command", .method = HTTP_POST, .handler = command_post_handler, .user_ctx = NULL};

static const httpd_uri_t uri_get_stations_export = {.uri = "/api/stations/export",
                                                    .method = HTTP_GET,
                                                    .handler = stations_export_get_handler,
                                                    .user_ctx = NULL};

static const httpd_uri_t uri_post_stations_import = {.uri = "/api/stations/import",
                                                     .method = HTTP_POST,
                                                     .handler = stations_import_post_handler,
                                                     .user_ctx = NULL};

static const httpd_uri_t uri_ws = {.uri = "/ws",
                                   .method = HTTP_GET,
                                   .handler = ws_handler,
                                   .user_ctx = NULL,
                                   .is_websocket = true};

esp_err_t web_gui_init(void)
{
  if (s_server)
    return ESP_OK;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_open_sockets = WEB_GUI_MAX_CLIENTS;
  //-- Tracks when the single allowed WS client's socket actually closes,
  //-- so a takeover isn't confused with a normal disconnect/reconnect.
  config.close_fn = web_gui_httpd_close_fn;
  //-- HTTPD_DEFAULT_CONFIG()'s max_uri_handlers is 8; this server registers
  //-- 10 (root, index.html, style.css, app.js, favicon.ico, /api/state,
  //-- /api/command, /api/stations/export, /api/stations/import, /ws).
  //-- Leave headroom for future endpoints instead of sizing exactly to
  //-- the current count.
  config.max_uri_handlers = 12;
  esp_err_t err = httpd_start(&s_server, &config);
  if (err != ESP_OK)
    return err;

  //-- httpd_register_uri_handler() fails silently for the caller if left
  //-- unchecked (e.g. when max_uri_handlers is too small for the handler
  //-- count), which previously dropped /ws registration without any error
  //-- at startup and only surfaced later as "URI '/ws' not found" at runtime.
  static const httpd_uri_t* const k_uri_handlers[] = {
      &uri_get_root,     &uri_get_index,           &uri_get_style,
      &uri_get_app_js,   &uri_get_favicon,         &uri_get_state,
      &uri_post_command, &uri_get_stations_export, &uri_post_stations_import,
      &uri_ws,
  };
  for (size_t i = 0; i < sizeof(k_uri_handlers) / sizeof(k_uri_handlers[0]); ++i)
  {
    esp_err_t reg_err = httpd_register_uri_handler(s_server, k_uri_handlers[i]);
    if (reg_err != ESP_OK)
    {
      ESP_LOGE(TAG, "Failed to register URI handler for %s: %s", k_uri_handlers[i]->uri,
               esp_err_to_name(reg_err));
    }
  }

  s_ws_queue = xQueueCreate(WEB_GUI_WS_QUEUE_LEN, sizeof(web_gui_ws_cmd_t));
  if (!s_ws_queue)
  {
    httpd_stop(s_server);
    s_server = NULL;
    return ESP_ERR_NO_MEM;
  }
  if (xTaskCreate(web_gui_ws_worker_task, "web_gui_ws", 6144, NULL, 5, &s_ws_worker_task) != pdPASS)
  {
    vQueueDelete(s_ws_queue);
    s_ws_queue = NULL;
    httpd_stop(s_server);
    s_server = NULL;
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "Web GUI started with WebSocket support on /ws");
  return ESP_OK;
}

void web_gui_notify_title(const char* line1, const char* line2, const char* line3)
{
  bool changed = strcmp(s_current_line1, line1 ? line1 : "") != 0 ||
                 strcmp(s_current_line2, line2 ? line2 : "") != 0 ||
                 strcmp(s_current_line3, line3 ? line3 : "") != 0;
  strlcpy(s_current_line1, line1 ? line1 : "", sizeof(s_current_line1));
  strlcpy(s_current_line2, line2 ? line2 : "", sizeof(s_current_line2));
  strlcpy(s_current_line3, line3 ? line3 : "", sizeof(s_current_line3));
  if (!changed || !s_server)
  {
    return;
  }
  cJSON* root = cJSON_CreateObject();
  cJSON* response_data = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "state");
  cJSON_AddItemToObject(root, "data", response_data);
  web_gui_fill_state(response_data);
  web_gui_ws_broadcast(root);
  cJSON_Delete(root);
}

void web_gui_notify_device_state(size_t station_index)
{
  s_current_station_index = web_gui_station_index(station_index);
  if (!s_server)
  {
    return;
  }
  cJSON* root = cJSON_CreateObject();
  cJSON* response_data = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "type", "state");
  cJSON_AddItemToObject(root, "data", response_data);
  web_gui_fill_state(response_data);
  web_gui_ws_broadcast(root);
  cJSON_Delete(root);
}

void web_gui_set_state_applied_cb(web_gui_state_applied_cb_t cb, void* ctx)
{
  s_state_applied_cb = cb;
  s_state_applied_ctx = ctx;
}

void web_gui_set_audio_owner_cb(web_gui_audio_owner_cb_t cb, void* ctx)
{
  s_audio_owner_cb = cb;
  s_audio_owner_ctx = ctx;
}

void web_gui_deinit(void)
{
  if (s_ws_worker_task)
  {
    vTaskDelete(s_ws_worker_task);
    s_ws_worker_task = NULL;
  }
  if (s_ws_queue)
  {
    vQueueDelete(s_ws_queue);
    s_ws_queue = NULL;
  }
  if (s_server)
  {
    httpd_stop(s_server);
    s_server = NULL;
  }
  s_active_ws_fd = -1;
}
