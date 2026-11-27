/*
 * WLED bridge: read first, write explicit command fields only.
 * Network I/O runs in a dedicated worker, never on the Matter or Arduino loop.
 */
#include "wled_output.h"
#include "matter_manager.h"
#include "web_ui.h"
#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

WledOutput wledOutput;
namespace {
constexpr int TIMEOUT_MS = 1000;
constexpr uint32_t POLL_MS = 1000;
constexpr uint32_t RETRY_MS = 5000;
constexpr size_t MAX_RESPONSE = 32768;

struct Device {
  LightConfig cfg;
  LightState desired;
  uint8_t mask;
  uint8_t segment;
  bool online;
  uint32_t nextPoll;
};
struct Job {
  LightConfig cfg;
  LightState desired;
  uint8_t index, mask, segment;
};
struct Result {
  LightState actual;
  uint8_t index, segment;
  bool ok;
};
Device devices[MAX_LIGHTS] = {};
uint8_t deviceCount = 0, cursor = 0;
QueueHandle_t jobs = nullptr, results = nullptr;
bool busy = false;

struct Response { String data; bool overflow = false; };
esp_err_t httpEvent(esp_http_client_event_t* e) {
  if (e->event_id != HTTP_EVENT_ON_DATA || !e->user_data) return ESP_OK;
  auto* r = static_cast<Response*>(e->user_data);
  if (e->data_len < 0 || r->data.length() + size_t(e->data_len) > MAX_RESPONSE) {
    r->overflow = true;
    return ESP_FAIL;
  }
  if (!r->data.concat(static_cast<const char*>(e->data), e->data_len)) {
    r->overflow = true;
    return ESP_FAIL;
  }
  return ESP_OK;
}

bool request(const LightConfig& cfg, const char* payload, Response* response) {
  char url[128];
  snprintf(url, sizeof(url), "http://%s:%u/json/state", cfg.wledHost, cfg.wledPort ? cfg.wledPort : 80);
  esp_http_client_config_t c = {};
  c.url = url;
  c.method = payload ? HTTP_METHOD_POST : HTTP_METHOD_GET;
  c.timeout_ms = TIMEOUT_MS;
  c.disable_auto_redirect = true;
  c.event_handler = httpEvent;
  c.user_data = response;
  auto client = esp_http_client_init(&c);
  if (!client) return false;
  if (payload) {
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));
  }
  const esp_err_t err = esp_http_client_perform(client);
  const int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  return err == ESP_OK && status == 200 && (!response || !response->overflow);
}

bool readState(const LightConfig& cfg, LightState& state, uint8_t& segment) {
  Response response;
  if (!request(cfg, nullptr, &response)) return false;
  JsonDocument doc;
  if (deserializeJson(doc, response.data)) return false;
  if (!doc["on"].is<bool>() || !doc["bri"].is<uint8_t>()) return false;
  JsonArrayConst segments = doc["seg"].as<JsonArrayConst>();
  if (segments.isNull() || !segments.size()) return false;
  int mainId = doc["mainseg"] | 0;
  JsonObjectConst selected = segments[0].as<JsonObjectConst>();
  for (JsonObjectConst item : segments) {
    if ((item["id"] | -1) == mainId) { selected = item; break; }
  }
  JsonArrayConst color = selected["col"][0].as<JsonArrayConst>();
  if (color.size() < 3 || !color[0].is<uint8_t>() || !color[1].is<uint8_t>() || !color[2].is<uint8_t>()) return false;
  segment = selected["id"] | 0;
  state = {};
  state.powerOn = doc["on"].as<bool>();
  unsigned bri = doc["bri"].as<unsigned>();
  state.brightness = static_cast<uint8_t>(bri > 254 ? 254 : (bri ? bri : 1));
  state.red = color[0].as<uint8_t>();
  state.green = color[1].as<uint8_t>();
  state.blue = color[2].as<uint8_t>();
  state.white = cfg.type == LIGHT_TYPE_RGBW ? (color[3] | 0) : 0;
  return true;
}

bool writeCommand(const Job& job, uint8_t segment, bool actualOn) {
  JsonDocument doc;
  if (job.mask & WLED_CMD_POWER) doc["on"] = job.desired.powerOn;
  if (job.mask & WLED_CMD_BRIGHTNESS) {
    // WLED treats nonzero bri alone as on. Preserve power for level-only changes.
    doc["bri"] = job.desired.brightness >= 254 ? 255 : max(1, int(job.desired.brightness));
    if (!(job.mask & WLED_CMD_POWER)) doc["on"] = actualOn;
  }
  if (job.mask & WLED_CMD_COLOR) {
    JsonObject seg = doc["seg"].to<JsonArray>().add<JsonObject>();
    seg["id"] = segment;
    JsonArray col = seg["col"].to<JsonArray>().add<JsonArray>();
    col.add(job.desired.red); col.add(job.desired.green); col.add(job.desired.blue);
    if (job.cfg.type == LIGHT_TYPE_RGBW) col.add(job.desired.white);
    // Do not set fx, palette, speed, intensity, or secondary colors.
  }
  char payload[256];
  if (measureJson(doc) >= sizeof(payload)) return false;
  serializeJson(doc, payload, sizeof(payload));
  return request(job.cfg, payload, nullptr);
}

void worker(void*) {
  Job job;
  while (true) {
    if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
    Result result = {};
    result.index = job.index;
    // Read before every command, including recovery after an outage.
    result.ok = readState(job.cfg, result.actual, result.segment);
    if (result.ok && job.mask) {
      const bool sent = wledOutput.isRunning() && writeCommand(job, result.segment, result.actual.powerOn);
      // Never replay a failed/stale command automatically.
      result.ok = sent && readState(job.cfg, result.actual, result.segment);
    }
    xQueueSend(results, &result, portMAX_DELAY);
  }
}

bool sameConfig(const LightConfig& a, const LightConfig& b) {
  return a.active == b.active && a.type == b.type && a.wledPort == b.wledPort &&
      strcmp(a.wledHost, b.wledHost) == 0 && strcmp(a.mac, b.mac) == 0 &&
      strcmp(a.name, b.name) == 0;
}
}

void WledOutput::begin() {
  if (initialized.load() || jobs) return;
  deviceCount = configStore.getLightCount();
  for (uint8_t i = 0; i < deviceCount; ++i) devices[i].cfg = configStore.getLight(i);
  jobs = xQueueCreate(1, sizeof(Job));
  results = xQueueCreate(1, sizeof(Result));
  if (!jobs || !results || xTaskCreate(worker, "wled-io", 8192, nullptr, 2, nullptr) != pdPASS) {
    ESP_LOGE("WLED", "Unable to start WLED I/O worker");
    return;
  }
  initialized.store(true);
}

void WledOutput::update(const LightConfig* lights, const LightState* states, uint8_t count) {
  if (!initialized.load()) return;
  // Config list changes require restart in upstream. Never use shifted indexes.
  if (count != deviceCount) { stop(); return; }
  for (uint8_t i = 0; i < count; ++i)
    if (!sameConfig(lights[i], devices[i].cfg)) { stop(); return; }
  if (!matterIsStarted()) return;
  const uint32_t now = millis();
  const bool wifi = isWiFiConnected();
  for (uint8_t i = 0; i < count; ++i) {
    const uint8_t mask = matterTakeCommands(i);
    if (!wifi || !devices[i].online) {
      devices[i].mask = 0;
      if (!wifi && devices[i].online) {
        devices[i].online = false;
        devices[i].nextPoll = now;
        matterReportReachable(i, false);
      }
      continue;
    }
    if (mask) {
      auto& d = devices[i];
      if (mask & WLED_CMD_POWER) d.desired.powerOn = states[i].powerOn;
      if (mask & WLED_CMD_BRIGHTNESS) d.desired.brightness = states[i].brightness;
      if (mask & WLED_CMD_COLOR) {
        d.desired.red = states[i].red; d.desired.green = states[i].green;
        d.desired.blue = states[i].blue; d.desired.white = states[i].white;
      }
      d.mask |= mask;
    }
  }
  Result result;
  if (xQueueReceive(results, &result, 0) == pdTRUE) {
    busy = false;
    auto& d = devices[result.index];
    const bool ok = result.ok && wifi;
    if (ok) {
      d.segment = result.segment;
      // Do not overwrite a newer queued controller command with old read-back.
      if (!d.mask) matterReportState(result.index, result.actual);
    } else {
      d.mask = 0;
    }
    d.online = ok;
    matterReportReachable(result.index, ok);
    d.nextPoll = now + (ok ? POLL_MS : RETRY_MS);
  }
  if (!wifi || busy || !count) return;
  // Round-robin scheduling gives other devices a turn even if one is offline.
  for (uint8_t offset = 0; offset < count; ++offset) {
    uint8_t i = (cursor + offset) % count;
    auto& d = devices[i];
    if (!d.cfg.active || !d.cfg.wledHost[0]) continue;
    if (!d.mask && int32_t(now - d.nextPoll) < 0) continue;
    Job job = {d.cfg, d.desired, i, d.mask, d.segment};
    if (xQueueSend(jobs, &job, 0) == pdTRUE) {
      d.mask = 0;
      busy = true;
      cursor = (i + 1) % count;
    }
    break;
  }
}

void WledOutput::stop() {
  initialized.store(false);
  // Worker remains alive to finish a read already in flight; no new POSTs.
}
