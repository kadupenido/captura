#include "device_config.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <cstring>

#include "config.h"
#include "log.h"

static const char* const kNvsNamespace = "dev_cfg";
static const char* const kNvsUpdatedAt = "updated_at";

static DeviceConfig s_cfg;
static bool s_nvsReady = false;

const DeviceConfig& deviceConfig() {
  return s_cfg;
}

void deviceConfigApplyDefaults() {
  s_cfg = DeviceConfig {};
}

static void copyStringField(char* dest, size_t destSize, const char* src) {
  if (destSize == 0) {
    return;
  }
  strncpy(dest, src != nullptr ? src : "", destSize - 1);
  dest[destSize - 1] = '\0';
}

static bool parseApiBody(const String& body, DeviceConfig& out) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    logPrintf("Device config: JSON invalido (%s).\n", err.c_str());
    return false;
  }

  DeviceConfig parsed {};
  parsed.soil1_dry_mv = doc["soil1_dry_mv"] | parsed.soil1_dry_mv;
  parsed.soil1_wet_mv = doc["soil1_wet_mv"] | parsed.soil1_wet_mv;
  parsed.soil2_dry_mv = doc["soil2_dry_mv"] | parsed.soil2_dry_mv;
  parsed.soil2_wet_mv = doc["soil2_wet_mv"] | parsed.soil2_wet_mv;
  parsed.altitude_local = doc["altitude_local"] | parsed.altitude_local;
  parsed.manual_irrigation_max_s = doc["manual_irrigation_max_s"] | parsed.manual_irrigation_max_s;
  parsed.pump_sample_interval_s = doc["pump_sample_interval_s"] | parsed.pump_sample_interval_s;
  parsed.deep_sleep_enabled = doc["deep_sleep_enabled"] | parsed.deep_sleep_enabled;
  parsed.capture_interval_seconds = doc["capture_interval_seconds"] | parsed.capture_interval_seconds;
  parsed.deep_sleep_seconds = doc["deep_sleep_seconds"] | parsed.deep_sleep_seconds;
  parsed.samples_per_api_upload = doc["samples_per_api_upload"] | parsed.samples_per_api_upload;
  parsed.http_timeout_ms = doc["http_timeout_ms"] | parsed.http_timeout_ms;
  parsed.http_max_retries = doc["http_max_retries"] | parsed.http_max_retries;
  parsed.wifi_timeout_ms = doc["wifi_timeout_ms"] | parsed.wifi_timeout_ms;
  parsed.cold_boot_usb_wait_ms = doc["cold_boot_usb_wait_ms"] | parsed.cold_boot_usb_wait_ms;
  parsed.ntp_sync_wait_ms = doc["ntp_sync_wait_ms"] | parsed.ntp_sync_wait_ms;
  parsed.ntp_min_valid_year = doc["ntp_min_valid_year"] | parsed.ntp_min_valid_year;
  parsed.ntp_gmt_offset_sec = doc["ntp_gmt_offset_sec"] | parsed.ntp_gmt_offset_sec;
  parsed.ntp_daylight_offset_sec = doc["ntp_daylight_offset_sec"] | parsed.ntp_daylight_offset_sec;
  parsed.pending_batch_max_items = doc["pending_batch_max_items"] | parsed.pending_batch_max_items;
  parsed.pending_batch_max_bytes = doc["pending_batch_max_bytes"] | parsed.pending_batch_max_bytes;
  parsed.pending_max_bytes = doc["pending_max_bytes"] | parsed.pending_max_bytes;
  parsed.pending_max_lines = doc["pending_max_lines"] | parsed.pending_max_lines;

  const char* ntpPrimary = doc["ntp_server_primary"] | parsed.ntp_server_primary;
  const char* ntpSecondary = doc["ntp_server_secondary"] | parsed.ntp_server_secondary;
  copyStringField(parsed.ntp_server_primary, sizeof(parsed.ntp_server_primary), ntpPrimary);
  copyStringField(parsed.ntp_server_secondary, sizeof(parsed.ntp_server_secondary), ntpSecondary);

  const char* updatedAt = doc["updated_at"] | "";
  copyStringField(parsed.updated_at, sizeof(parsed.updated_at), updatedAt);

  out = parsed;
  return true;
}

static void saveToNvs(const DeviceConfig& cfg) {
  if (!s_nvsReady) {
    return;
  }
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    return;
  }

  prefs.putFloat("s1_dry", cfg.soil1_dry_mv);
  prefs.putFloat("s1_wet", cfg.soil1_wet_mv);
  prefs.putFloat("s2_dry", cfg.soil2_dry_mv);
  prefs.putFloat("s2_wet", cfg.soil2_wet_mv);
  prefs.putInt("altitude", cfg.altitude_local);
  prefs.putInt("man_irr_max", cfg.manual_irrigation_max_s);
  prefs.putInt("pump_samp", cfg.pump_sample_interval_s);
  prefs.putBool("deep_sleep", cfg.deep_sleep_enabled);
  prefs.putInt("cap_int", cfg.capture_interval_seconds);
  prefs.putInt("sleep_s", cfg.deep_sleep_seconds);
  prefs.putInt("samples", cfg.samples_per_api_upload);
  prefs.putInt("http_to", cfg.http_timeout_ms);
  prefs.putInt("http_ret", cfg.http_max_retries);
  prefs.putInt("wifi_to", cfg.wifi_timeout_ms);
  prefs.putInt("usb_wait", cfg.cold_boot_usb_wait_ms);
  prefs.putString("ntp1", cfg.ntp_server_primary);
  prefs.putString("ntp2", cfg.ntp_server_secondary);
  prefs.putInt("ntp_wait", cfg.ntp_sync_wait_ms);
  prefs.putInt("ntp_year", cfg.ntp_min_valid_year);
  prefs.putInt("ntp_gmt", cfg.ntp_gmt_offset_sec);
  prefs.putInt("ntp_dst", cfg.ntp_daylight_offset_sec);
  prefs.putInt("batch_n", cfg.pending_batch_max_items);
  prefs.putInt("batch_b", cfg.pending_batch_max_bytes);
  prefs.putInt("pend_b", cfg.pending_max_bytes);
  prefs.putInt("pend_n", cfg.pending_max_lines);
  prefs.putString(kNvsUpdatedAt, cfg.updated_at);
  prefs.end();
}

static bool loadFromNvs(DeviceConfig& cfg) {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) {
    return false;
  }

  const String updatedAt = prefs.getString(kNvsUpdatedAt, "");
  if (updatedAt.length() == 0) {
    prefs.end();
    return false;
  }

  deviceConfigApplyDefaults();
  cfg = s_cfg;

  cfg.soil1_dry_mv = prefs.getFloat("s1_dry", cfg.soil1_dry_mv);
  cfg.soil1_wet_mv = prefs.getFloat("s1_wet", cfg.soil1_wet_mv);
  cfg.soil2_dry_mv = prefs.getFloat("s2_dry", cfg.soil2_dry_mv);
  cfg.soil2_wet_mv = prefs.getFloat("s2_wet", cfg.soil2_wet_mv);
  cfg.altitude_local = prefs.getInt("altitude", cfg.altitude_local);
  cfg.manual_irrigation_max_s = prefs.getInt("man_irr_max", cfg.manual_irrigation_max_s);
  cfg.pump_sample_interval_s = prefs.getInt("pump_samp", cfg.pump_sample_interval_s);
  cfg.deep_sleep_enabled = prefs.getBool("deep_sleep", cfg.deep_sleep_enabled);
  cfg.capture_interval_seconds = prefs.getInt("cap_int", cfg.capture_interval_seconds);
  cfg.deep_sleep_seconds = prefs.getInt("sleep_s", cfg.deep_sleep_seconds);
  cfg.samples_per_api_upload = prefs.getInt("samples", cfg.samples_per_api_upload);
  cfg.http_timeout_ms = prefs.getInt("http_to", cfg.http_timeout_ms);
  cfg.http_max_retries = prefs.getInt("http_ret", cfg.http_max_retries);
  cfg.wifi_timeout_ms = prefs.getInt("wifi_to", cfg.wifi_timeout_ms);
  cfg.cold_boot_usb_wait_ms = prefs.getInt("usb_wait", cfg.cold_boot_usb_wait_ms);

  const String ntp1 = prefs.getString("ntp1", cfg.ntp_server_primary);
  const String ntp2 = prefs.getString("ntp2", cfg.ntp_server_secondary);
  copyStringField(cfg.ntp_server_primary, sizeof(cfg.ntp_server_primary), ntp1.c_str());
  copyStringField(cfg.ntp_server_secondary, sizeof(cfg.ntp_server_secondary), ntp2.c_str());

  cfg.ntp_sync_wait_ms = prefs.getInt("ntp_wait", cfg.ntp_sync_wait_ms);
  cfg.ntp_min_valid_year = prefs.getInt("ntp_year", cfg.ntp_min_valid_year);
  cfg.ntp_gmt_offset_sec = prefs.getInt("ntp_gmt", cfg.ntp_gmt_offset_sec);
  cfg.ntp_daylight_offset_sec = prefs.getInt("ntp_dst", cfg.ntp_daylight_offset_sec);
  cfg.pending_batch_max_items = prefs.getInt("batch_n", cfg.pending_batch_max_items);
  cfg.pending_batch_max_bytes = prefs.getInt("batch_b", cfg.pending_batch_max_bytes);
  cfg.pending_max_bytes = prefs.getInt("pend_b", cfg.pending_max_bytes);
  cfg.pending_max_lines = prefs.getInt("pend_n", cfg.pending_max_lines);
  copyStringField(cfg.updated_at, sizeof(cfg.updated_at), updatedAt.c_str());

  prefs.end();
  return true;
}

bool deviceConfigLoadNvs() {
  deviceConfigApplyDefaults();
  Preferences prefs;
  s_nvsReady = prefs.begin(kNvsNamespace, false);
  if (!s_nvsReady) {
    logPrintf("Device config: NVS indisponivel; usando defaults.\n");
    return false;
  }
  prefs.end();

  DeviceConfig loaded {};
  if (loadFromNvs(loaded)) {
    s_cfg = loaded;
    return true;
  }
  return false;
}

bool deviceConfigSyncFromApi() {
  char url[192];
  snprintf(url, sizeof(url), "%s/device/config", API_BASE_URL);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(s_cfg.http_timeout_ms);
  http.addHeader("Authorization", "Bearer " API_TOKEN);
  const int code = http.GET();
  if (code != 200) {
    logPrintf("Device config: GET /device/config falhou (%d).\n", code);
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  DeviceConfig remote {};
  if (!parseApiBody(body, remote)) {
    return false;
  }

  if (remote.updated_at[0] != '\0' && strcmp(remote.updated_at, s_cfg.updated_at) == 0) {
    return true;
  }

  s_cfg = remote;
  saveToNvs(s_cfg);
  logPrintf("Device config: sincronizado (updated_at=%s).\n", s_cfg.updated_at);
  return true;
}
