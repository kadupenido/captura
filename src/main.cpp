#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_sleep.h>
#include <Preferences.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_INA219.h>

#include "config.h"
#include "hardware_defaults.h"
#include "device_config.h"
#include "log.h"
#include "pending_queue.h"

#define RTC_MAGIC 0x4D455449  // "METI" meteo dual

// Reducao barometrica isotermica: P_nm = P_local * exp(g*h/(R*T_K)), T_K = temperatura medida.
static constexpr float kGravity = 9.80665f;
static constexpr float kGasConstantDryAir = 287.05f;

RTC_DATA_ATTR uint32_t rtc_magic;
// Histerese da irrigacao por zona (persistente entre wakes).
RTC_DATA_ATTR uint8_t rtc_irrigation_armed_1;
RTC_DATA_ATTR uint8_t rtc_irrigation_armed_2;
// Irrigacao manual: segundos acumulados no ciclo (entram em tempo_irrigacao_s_N).
RTC_DATA_ATTR uint16_t rtc_manual_irrig_s_1;
RTC_DATA_ATTR uint16_t rtc_manual_irrig_s_2;
// Ultimo comando manual executado por zona (evita re-execucao se o ack falhar).
RTC_DATA_ATTR int32_t rtc_last_manual_id_1;
RTC_DATA_ATTR int32_t rtc_last_manual_id_2;
// Bomba manual em andamento (sobrevive a reset por watchdog para retomada).
RTC_DATA_ATTR int32_t rtc_active_manual_id;
RTC_DATA_ATTR uint8_t rtc_active_manual_zone;
RTC_DATA_ATTR uint16_t rtc_active_manual_total_s;
RTC_DATA_ATTR uint16_t rtc_active_manual_elapsed_s;

Adafruit_BME280 bme;
Adafruit_SHT31 sht31 = Adafruit_SHT31();
Adafruit_INA219 inaPainel(INA219_PAINEL_ADDR);
Adafruit_INA219 inaSistema(INA219_SISTEMA_ADDR);

// Irrigacao bloqueia por dezenas de segundos (delay). Desliga loop/core0 WDT apenas
// durante o acionamento do rele; I/O de rede roda com watchdog ativo.
struct PumpWdtGuard {
  PumpWdtGuard() {
    disableLoopWDT();
    disableCore0WDT();
  }
  ~PumpWdtGuard() {
    enableCore0WDT();
    enableLoopWDT();
  }
};

static Preferences s_manualPrefs;
static const char* const kManualPrefsNamespace = "irr_manual";
static bool s_manualPrefsReady = false;

static bool initManualPrefs() {
  s_manualPrefsReady = s_manualPrefs.begin(kManualPrefsNamespace, false);
  return s_manualPrefsReady;
}

static int32_t loadPersistedLastManualId(int zone) {
  if (!s_manualPrefsReady) {
    return 0;
  }
  const char* key = (zone == 1) ? "last_id_z1" : "last_id_z2";
  return static_cast<int32_t>(s_manualPrefs.getUInt(key, 0));
}

static void persistLastManualId(int zone, int32_t id) {
  if (s_manualPrefsReady) {
    const char* key = (zone == 1) ? "last_id_z1" : "last_id_z2";
    s_manualPrefs.putUInt(key, static_cast<uint32_t>(id));
  }
  if (zone == 1) {
    rtc_last_manual_id_1 = id;
  } else {
    rtc_last_manual_id_2 = id;
  }
}

static int loadPersistedLastManualDuration(int zone, int32_t id) {
  const int32_t persistedId = loadPersistedLastManualId(zone);
  if (persistedId != id || id <= 0) {
    return -1;
  }
  if (!s_manualPrefsReady) {
    return -1;
  }
  const char* durKey = (zone == 1) ? "last_dur_z1" : "last_dur_z2";
  return static_cast<int>(s_manualPrefs.getUInt(durKey, 0));
}

static void persistManualCompletion(int zone, int32_t id, int executedS) {
  if (executedS < 0) {
    executedS = 0;
  }
  persistLastManualId(zone, id);
  if (s_manualPrefsReady) {
    const char* durKey = (zone == 1) ? "last_dur_z1" : "last_dur_z2";
    s_manualPrefs.putUInt(durKey, static_cast<uint32_t>(executedS));
  }
}

static void syncManualIdsFromNvsIfNeeded() {
  const int32_t nvs1 = loadPersistedLastManualId(1);
  const int32_t nvs2 = loadPersistedLastManualId(2);
  if (nvs1 > 0) {
    rtc_last_manual_id_1 = nvs1;
  }
  if (nvs2 > 0) {
    rtc_last_manual_id_2 = nvs2;
  }
}

static bool isManualCommandAlreadyFinished(int zone, int32_t cmdId) {
  if (cmdId <= 0) {
    return false;
  }
  const int32_t rtcLast = (zone == 1) ? rtc_last_manual_id_1 : rtc_last_manual_id_2;
  if (cmdId == rtcLast) {
    return true;
  }
  return cmdId == loadPersistedLastManualId(zone);
}

static void clearActiveManualPump() {
  rtc_active_manual_id = 0;
  rtc_active_manual_zone = 0;
  rtc_active_manual_total_s = 0;
  rtc_active_manual_elapsed_s = 0;
}

static void persistActiveManualState() {
  if (!s_manualPrefsReady) {
    return;
  }
  s_manualPrefs.putUInt("act_id", static_cast<uint32_t>(rtc_active_manual_id));
  s_manualPrefs.putUChar("act_zone", rtc_active_manual_zone);
  s_manualPrefs.putUShort("act_tot", rtc_active_manual_total_s);
  s_manualPrefs.putUShort("act_elap", rtc_active_manual_elapsed_s);
}

static void clearPersistedActiveManualState() {
  if (!s_manualPrefsReady) {
    return;
  }
  s_manualPrefs.remove("act_id");
  s_manualPrefs.remove("act_zone");
  s_manualPrefs.remove("act_tot");
  s_manualPrefs.remove("act_elap");
}

static bool restorePersistedActiveManualState() {
  if (!s_manualPrefsReady) {
    return false;
  }
  const int32_t id = static_cast<int32_t>(s_manualPrefs.getUInt("act_id", 0));
  if (id <= 0) {
    return false;
  }
  const uint8_t zone = s_manualPrefs.getUChar("act_zone", 0);
  const uint16_t total = s_manualPrefs.getUShort("act_tot", 0);
  const uint16_t elapsed = s_manualPrefs.getUShort("act_elap", 0);
  if ((zone != 1 && zone != 2) || total == 0 || elapsed > total) {
    clearPersistedActiveManualState();
    return false;
  }
  rtc_active_manual_id = id;
  rtc_active_manual_zone = zone;
  rtc_active_manual_total_s = total;
  rtc_active_manual_elapsed_s = elapsed;
  return true;
}

static bool s_littlefsOk = false;
static bool s_bmeOk = false;
static bool s_shtOk = false;
static bool s_inaPainelOk = false;
static bool s_inaSistemaOk = false;

struct IrrigationZoneConfig {
  float thresholdPct = NAN;
  float hysteresisPct = NAN;
  int pumpDurationS = 0;
  bool active = true;
  bool valid = false;
};

struct IrrigationConfig {
  IrrigationZoneConfig zone1;
  IrrigationZoneConfig zone2;
  bool valid = false;
};

static float clampPct(float v) {
  if (v < 0.0f) return 0.0f;
  if (v > 100.0f) return 100.0f;
  return v;
}

static void setRelayState(int pin, bool on) {
  if (deviceConfig().relay_active_high) {
    digitalWrite(pin, on ? HIGH : LOW);
  } else {
    digitalWrite(pin, on ? LOW : HIGH);
  }
}

static float readAdcMilliVoltsAvg(int pin) {
  uint32_t accumMv = 0;
  int valid = 0;
  for (int i = 0; i < deviceConfig().adc_samples; i++) {
    uint32_t mv = analogReadMilliVolts(pin);
    accumMv += mv;
    valid++;
    delayMicroseconds(200);
  }
  if (valid == 0) {
    return NAN;
  }
  return static_cast<float>(accumMv) / static_cast<float>(valid);
}

static float readAdcRawAvg(int pin) {
  uint32_t accumRaw = 0;
  int valid = 0;
  for (int i = 0; i < deviceConfig().adc_samples; i++) {
    accumRaw += static_cast<uint32_t>(analogRead(pin));
    valid++;
    delayMicroseconds(200);
  }
  if (valid == 0) {
    return NAN;
  }
  return static_cast<float>(accumRaw) / static_cast<float>(valid);
}

static void readSoilReading(int pin, float dryMv, float wetMv, float& outRaw, float& outPct) {
  outRaw = readAdcRawAvg(pin);
  const float mv = readAdcMilliVoltsAvg(pin);
  if (isnan(mv) || fabsf(wetMv - dryMv) < 1.0f) {
    outPct = NAN;
    return;
  }
  outPct = clampPct(((mv - dryMv) / (wetMv - dryMv)) * 100.0f);
}

void resetRtcState() {
  rtc_magic = RTC_MAGIC;
  rtc_manual_irrig_s_1 = 0;
  rtc_manual_irrig_s_2 = 0;
}

void enterDeepSleep() {
  setRelayState(RELAY_PIN_1, false);
  setRelayState(RELAY_PIN_2, false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Wire.end();
  Wire1.end();
  esp_sleep_enable_timer_wakeup((uint64_t)deviceConfig().deep_sleep_seconds * 1000000ULL);
  logPrintf("Entrando em deep sleep por %d segundos...\n", deviceConfig().deep_sleep_seconds);
  Serial.flush();
  delay(50);
  esp_deep_sleep_start();
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);

#ifdef WIFI_STATIC_IP
  IPAddress ip, gateway, subnet;
  ip.fromString(WIFI_STATIC_IP);
  gateway.fromString(WIFI_GATEWAY);
  subnet.fromString(WIFI_SUBNET);
  WiFi.config(ip, gateway, subnet, IPAddress(8, 8, 8, 8));  // Google DNS público
#endif

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < static_cast<unsigned long>(deviceConfig().wifi_timeout_ms)) {
    delay(500);
    Serial.printf(".");
  }
  Serial.println();

  return WiFi.status() == WL_CONNECTED;
}

static bool wallClockLooksSynced() {
  struct tm t {};
  if (!getLocalTime(&t, 50)) {
    return false;
  }
  return (t.tm_year + 1900) >= deviceConfig().ntp_min_valid_year;
}

static void trySyncTimeFromNtp() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (wallClockLooksSynced()) {
    return;
  }
  const DeviceConfig& cfg = deviceConfig();
  configTime(cfg.ntp_gmt_offset_sec, cfg.ntp_daylight_offset_sec, cfg.ntp_server_primary,
             cfg.ntp_server_secondary);
  const uint32_t start = millis();
  while (millis() - start < static_cast<uint32_t>(cfg.ntp_sync_wait_ms) &&
         WiFi.status() == WL_CONNECTED) {
    if (wallClockLooksSynced()) {
      logPrintf("NTP: sincronizado.\n");
      return;
    }
    delay(100);
  }
  logPrintf("NTP: timeout (RTC pode estar invalido neste wake).\n");
}

static void appendCreatedAtUtcIfSynced(JsonDocument& doc) {
  if (!wallClockLooksSynced()) {
    return;
  }
  const time_t now = time(nullptr);
  struct tm* t = gmtime(&now);
  if (t == nullptr) {
    return;
  }
  char buf[32];
  if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", t) == 0) {
    return;
  }
  doc["created_at"] = buf;
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    deviceConfigSyncFromApi();
    trySyncTimeFromNtp();
    return true;
  }
  if (!connectWiFi()) {
    return false;
  }
  deviceConfigSyncFromApi();
  trySyncTimeFromNtp();
  return true;
}

void wifiOffIfConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
}

int sendData(const char* payload, size_t payloadLen) {
  char url[192];
  snprintf(url, sizeof(url), "%s/dados", API_BASE_URL);
  HTTPClient http;
  http.begin(url);
  http.setTimeout(deviceConfig().http_timeout_ms);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " API_TOKEN);

  // HTTPClient::POST exige ponteiro mutavel; o buffer nao e alterado pelo cliente.
  int code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(payload)), payloadLen);
  if (code != 201) {
    logPrintf("Erro HTTP %d: %s\n", code, http.getString().c_str());
  }
  http.end();
  return code;
}

int sendBatchData(const char* payload, size_t payloadLen) {
  char url[192];
  snprintf(url, sizeof(url), "%s/dados/lote", API_BASE_URL);
  HTTPClient http;
  http.begin(url);
  http.setTimeout(deviceConfig().http_timeout_ms);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " API_TOKEN);

  int code = http.POST(reinterpret_cast<uint8_t*>(const_cast<char*>(payload)), payloadLen);
  if (code != 201) {
    logPrintf("Erro HTTP lote %d: %s\n", code, http.getString().c_str());
  }
  http.end();
  return code;
}

int sendPayloadWithRetries(const char* payload, size_t payloadLen) {
  int lastCode = -1;
  for (int attempt = 1; attempt <= deviceConfig().http_max_retries; attempt++) {
    logPrintf("Envio tentativa %d/%d...\n", attempt, deviceConfig().http_max_retries);
    lastCode = sendData(payload, payloadLen);
    if (lastCode == 201) {
      logPrintf("Dados enviados com sucesso.\n");
      return 201;
    }
    if (attempt < deviceConfig().http_max_retries) {
      delay(deviceConfig().http_retry_delay_ms);
    }
  }
  return lastCode;
}

int sendBatchWithRetries(const char* payload, size_t payloadLen) {
  int lastCode = -1;
  for (int attempt = 1; attempt <= deviceConfig().http_max_retries; attempt++) {
    logPrintf("Envio lote tentativa %d/%d...\n", attempt, deviceConfig().http_max_retries);
    lastCode = sendBatchData(payload, payloadLen);
    if (lastCode == 201) {
      logPrintf("Lote enviado com sucesso.\n");
      return 201;
    }
    if (attempt < deviceConfig().http_max_retries) {
      delay(deviceConfig().http_retry_delay_ms);
    }
  }
  return lastCode;
}

// Ponte para PendingSendFn (linhas lidas como String na fila; envio unitario legado).
int sendPayloadWithRetriesForQueue(const String& payload) {
  return sendPayloadWithRetries(payload.c_str(), payload.length());
}

static bool parseZoneConfig(JsonVariant zoneNode, IrrigationZoneConfig& out) {
  if (!zoneNode.is<JsonObject>()) {
    return false;
  }
  const float threshold = zoneNode["threshold_pct"] | NAN;
  const float hysteresis = zoneNode["hysteresis_pct"] | NAN;
  const int duration = zoneNode["pump_duration_s"] | -1;
  const bool active = zoneNode["active"] | true;
  if (isnan(threshold) || isnan(hysteresis) || duration < 0) {
    return false;
  }
  out.thresholdPct = clampPct(threshold);
  out.hysteresisPct = hysteresis < 0.0f ? 0.0f : hysteresis;
  out.pumpDurationS = duration;
  out.active = active;
  out.valid = true;
  return true;
}

static bool fetchIrrigationConfig(IrrigationConfig& cfg) {
  cfg = IrrigationConfig {};

  char url[192];
  snprintf(url, sizeof(url), "%s/irrigation/config", API_BASE_URL);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(deviceConfig().http_timeout_ms);
  http.addHeader("Authorization", "Bearer " API_TOKEN);
  const int code = http.GET();
  if (code != 200) {
    logPrintf("Irrigacao: GET /irrigation/config falhou (%d).\n", code);
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    logPrintf("Irrigacao: JSON invalido em /irrigation/config (%s).\n", err.c_str());
    return false;
  }

  bool ok1 = parseZoneConfig(doc["zone_1"], cfg.zone1);
  bool ok2 = parseZoneConfig(doc["zone_2"], cfg.zone2);
  cfg.valid = ok1 && ok2;
  if (!cfg.valid) {
    logPrintf("Irrigacao: configuracao incompleta.\n");
  }
  return cfg.valid;
}

struct SensorSnapshot {
  float soil1 = NAN;
  float soil2 = NAN;
  float soilAdc1 = NAN;
  float soilAdc2 = NAN;
  bool bmeReadOk = false;
  bool shtReadOk = false;
  bool inaPainelReadOk = false;
  bool inaSistemaReadOk = false;
  float pSeaBme = NAN;
  float temperatura = NAN;
  float umidade = NAN;
  float vPainel = NAN;
  float iPainel = NAN;
  float pPainel = NAN;
  float vSistema = NAN;
  float iSistema = NAN;
  float pSistema = NAN;
};

static SensorSnapshot captureAmbientSnapshot();
static void capturePowerSnapshot(SensorSnapshot& snap);
static SensorSnapshot captureInstantSnapshot();
static void runCaptureCycle();
static bool uploadSnapshotNow(const SensorSnapshot& snap, bool queueOnly = false, int irrigS1 = -1,
                              int irrigS2 = -1);
static int runPump(int relayPin, int durationS, int32_t manualCmdId = 0, int manualZone = 0,
                   int priorElapsedS = 0);
static bool finalizeManualCommand(int zone, int32_t id, int executedS, bool clearActiveState = true);
static void flushPendingQueueWithFallback(const char* context);

struct ManualCommand {
  int32_t id = 0;
  int zone = 0;
  int durationS = 0;
  char status[12] = "pending";
};

// Busca comandos manuais ativos (pending ou running). Retorna -1 em falha, ou 0..2 em out.
static int fetchPendingManualCommands(ManualCommand out[2]) {
  char url[192];
  snprintf(url, sizeof(url), "%s/irrigation/manual/pending", API_BASE_URL);

  HTTPClient http;
  http.begin(url);
  http.setTimeout(deviceConfig().http_timeout_ms);
  http.addHeader("Authorization", "Bearer " API_TOKEN);
  const int code = http.GET();
  if (code != 200) {
    logPrintf("Irrigacao manual: GET /irrigation/manual/pending falhou (%d).\n", code);
    http.end();
    return -1;
  }

  const String body = http.getString();
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    logPrintf("Irrigacao manual: JSON invalido (%s).\n", err.c_str());
    return -1;
  }

  JsonArray commands = doc["commands"].as<JsonArray>();
  if (commands.isNull()) {
    logPrintf("Irrigacao manual: resposta sem campo commands.\n");
    return -1;
  }

  int n = 0;
  for (JsonVariant item : commands) {
    if (n >= 2) {
      break;
    }
    const int32_t id = item["id"] | 0;
    const int zone = item["zone"] | 0;
    const int durationS = item["duration_s"] | 0;
    if (id <= 0 || (zone != 1 && zone != 2) || durationS < 1) {
      logPrintf("Irrigacao manual: comando invalido ignorado.\n");
      continue;
    }
    const char* status = item["status"] | "pending";
    out[n].id = id;
    out[n].zone = zone;
    out[n].durationS = durationS;
    strncpy(out[n].status, status, sizeof(out[n].status) - 1);
    out[n].status[sizeof(out[n].status) - 1] = '\0';
    n++;
  }
  return n;
}

// Marca inicio na API antes de acionar a bomba (pending -> running).
static bool startManualCommand(int32_t id) {
  char url[192];
  snprintf(url, sizeof(url), "%s/irrigation/manual/%ld/start", API_BASE_URL, (long)id);

  for (int attempt = 1; attempt <= deviceConfig().http_max_retries; attempt++) {
    HTTPClient http;
    http.begin(url);
    http.setTimeout(deviceConfig().http_timeout_ms);
    http.addHeader("Authorization", "Bearer " API_TOKEN);
    const int code = http.POST(nullptr, 0);
    http.end();
    if (code == 200) {
      logPrintf("Irrigacao manual: inicio do comando %ld confirmado.\n", (long)id);
      return true;
    }
    if (code == 404 || code == 409) {
      logPrintf("Irrigacao manual: start do comando %ld rejeitado (%d); bomba nao sera acionada.\n",
                    (long)id, code);
      return false;
    }
    logPrintf("Irrigacao manual: start tentativa %d/%d falhou (%d).\n", attempt,
              deviceConfig().http_max_retries, code);
    if (attempt < deviceConfig().http_max_retries) {
      delay(deviceConfig().http_retry_delay_ms);
    }
  }
  return false;
}

// Confirma execucao na API. 404/409 sao terminais: o comando deixou de estar
// pendente no servidor e nao sera re-executado.
static bool ackManualCommand(int32_t id, int executedS) {
  char url[192];
  snprintf(url, sizeof(url), "%s/irrigation/manual/%ld/ack", API_BASE_URL, (long)id);
  char body[48];
  snprintf(body, sizeof(body), "{\"executed_duration_s\":%d}", executedS);

  for (int attempt = 1; attempt <= deviceConfig().http_max_retries; attempt++) {
    HTTPClient http;
    http.begin(url);
    http.setTimeout(deviceConfig().http_timeout_ms);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " API_TOKEN);
    const int code = http.POST(reinterpret_cast<uint8_t*>(body), strlen(body));
    http.end();
    if (code == 200) {
      logPrintf("Irrigacao manual: ack do comando %ld confirmado.\n", (long)id);
      return true;
    }
    if (code == 404 || code == 409) {
      logPrintf("Irrigacao manual: ack do comando %ld respondeu %d; tratado como concluido.\n",
                    (long)id, code);
      return true;
    }
    logPrintf("Irrigacao manual: ack tentativa %d/%d falhou (%d).\n", attempt,
              deviceConfig().http_max_retries, code);
    if (attempt < deviceConfig().http_max_retries) {
      delay(deviceConfig().http_retry_delay_ms);
    }
  }
  return false;
}

static bool finalizeManualCommand(int zone, int32_t id, int executedS, bool clearActiveState) {
  if (zone != 1 && zone != 2) {
    return false;
  }
  if (!ensureWiFi()) {
    logPrintf("Irrigacao manual: WiFi indisponivel; ack do comando %ld adiado.\n", (long)id);
    return false;
  }
  if (!ackManualCommand(id, executedS)) {
    return false;
  }
  persistManualCompletion(zone, id, executedS);
  if (clearActiveState) {
    clearActiveManualPump();
    clearPersistedActiveManualState();
  }
  return true;
}

static void finishManualCommand(const ManualCommand& cmd, int priorElapsedS, int ranS) {
  (void)priorElapsedS;
  if (cmd.zone == 1) {
    rtc_manual_irrig_s_1 += static_cast<uint16_t>(ranS);
  } else {
    rtc_manual_irrig_s_2 += static_cast<uint16_t>(ranS);
  }
  // ack e persistencia ja ocorrem em runPump() antes do flush da fila.
}

// Verifica e executa comandos manuais em todo wake (liga WiFi se preciso).
// A leitura inicial do wake ocorre antes do WiFi; runPump() continua amostrando
// solo e sensores I2C a cada PUMP_SAMPLE_INTERVAL_S com WiFi ativo.
static void processManualIrrigation() {
  if (rtc_active_manual_id > 0) {
    if (!ensureWiFi()) {
      logPrintf("Irrigacao manual: WiFi indisponivel; retomada do comando %ld adiada.\n",
                    (long)rtc_active_manual_id);
      return;
    }
    ManualCommand resumeCmd {};
    resumeCmd.id = rtc_active_manual_id;
    resumeCmd.zone = rtc_active_manual_zone;
    resumeCmd.durationS = rtc_active_manual_total_s;
    const int remainingS = rtc_active_manual_total_s - rtc_active_manual_elapsed_s;
    if (remainingS > 0) {
      logPrintf("Irrigacao manual: retomando comando %ld (zona %d), %d/%d s.\n",
                    (long)resumeCmd.id, resumeCmd.zone, rtc_active_manual_elapsed_s,
                    rtc_active_manual_total_s);
      const int relayPin = (resumeCmd.zone == 1) ? RELAY_PIN_1 : RELAY_PIN_2;
      const int ranS = runPump(relayPin, remainingS, resumeCmd.id, resumeCmd.zone,
                               rtc_active_manual_elapsed_s);
      finishManualCommand(resumeCmd, rtc_active_manual_elapsed_s, ranS);
    } else {
      logPrintf("Irrigacao manual: comando %ld (zona %d) ja esgotou duracao; confirmando ack.\n",
                    (long)resumeCmd.id, resumeCmd.zone);
      if (!finalizeManualCommand(resumeCmd.zone, resumeCmd.id, rtc_active_manual_elapsed_s, true)) {
        logPrintf("Irrigacao manual: ack do comando %ld pendente; mantendo estado para retentativa.\n",
                      (long)resumeCmd.id);
      }
    }
    return;
  }

  if (!ensureWiFi()) {
    logPrintf("Irrigacao manual: WiFi indisponivel; verificacao ignorada.\n");
    return;
  }

  ManualCommand cmds[2];
  const int n = fetchPendingManualCommands(cmds);
  if (n <= 0) {
    return;
  }

  for (int i = 0; i < n; i++) {
    const ManualCommand& cmd = cmds[i];
    const bool isRunning = strcmp(cmd.status, "running") == 0;

    if (isManualCommandAlreadyFinished(cmd.zone, cmd.id)) {
      logPrintf("Irrigacao manual: comando %ld (zona %d) ja concluido; reenviando ack.\n",
                    (long)cmd.id, cmd.zone);
      const int persistedDuration = loadPersistedLastManualDuration(cmd.zone, cmd.id);
      const int ackDuration = (persistedDuration >= 0) ? persistedDuration : cmd.durationS;
      finalizeManualCommand(cmd.zone, cmd.id, ackDuration, false);
      continue;
    }

    if (isRunning) {
      const bool hasResumeState = (rtc_active_manual_id == cmd.id);
      if (!hasResumeState) {
        logPrintf("Irrigacao manual: comando %ld (zona %d) running sem estado RTC; retomando do inicio.\n",
                      (long)cmd.id, cmd.zone);
        const int relayPin = (cmd.zone == 1) ? RELAY_PIN_1 : RELAY_PIN_2;
        const int ranS = runPump(relayPin, cmd.durationS, cmd.id, cmd.zone, 0);
        finishManualCommand(cmd, 0, ranS);
        continue;
      }

      const int prior = rtc_active_manual_elapsed_s;
      const int remaining = cmd.durationS - prior;
      if (remaining <= 0) {
        logPrintf("Irrigacao manual: comando %ld (zona %d) ja esgotou duracao; confirmando ack.\n",
                      (long)cmd.id, cmd.zone);
        if (!finalizeManualCommand(cmd.zone, cmd.id, rtc_active_manual_elapsed_s, true)) {
          logPrintf("Irrigacao manual: ack do comando %ld pendente; mantendo estado para retentativa.\n",
                        (long)cmd.id);
        }
        continue;
      }

      logPrintf("Irrigacao manual: comando %ld (zona %d) em andamento; retomando por %d s.\n",
                    (long)cmd.id, cmd.zone, remaining);
      const int relayPin = (cmd.zone == 1) ? RELAY_PIN_1 : RELAY_PIN_2;
      const int ranS = runPump(relayPin, remaining, cmd.id, cmd.zone, prior);
      finishManualCommand(cmd, prior, ranS);
      continue;
    }

    if (!startManualCommand(cmd.id)) {
      logPrintf("Irrigacao manual: start do comando %ld falhou; bomba nao acionada.\n",
                    (long)cmd.id);
      continue;
    }

    logPrintf("Irrigacao manual: acionando bomba da zona %d por %d s (comando %ld).\n",
                  cmd.zone, cmd.durationS, (long)cmd.id);
    const int relayPin = (cmd.zone == 1) ? RELAY_PIN_1 : RELAY_PIN_2;
    const int ranS = runPump(relayPin, cmd.durationS, cmd.id, cmd.zone, 0);
    finishManualCommand(cmd, 0, ranS);
  }
}

// Unico ponto que energiza uma bomba. Regra: nunca as duas ao mesmo tempo —
// o rele da outra bomba e desligado antes de ligar este (acionamento bloqueante).
static int runPump(int relayPin, int durationS, int32_t manualCmdId, int manualZone,
                   int priorElapsedS) {
  if (durationS <= 0) {
    return 0;
  }
  const bool trackManual = manualCmdId > 0;
  if (trackManual && durationS > deviceConfig().manual_irrigation_max_s) {
    durationS = deviceConfig().manual_irrigation_max_s;
  }

  if (trackManual) {
    rtc_active_manual_id = manualCmdId;
    rtc_active_manual_zone = static_cast<uint8_t>(manualZone);
    rtc_active_manual_total_s = static_cast<uint16_t>(priorElapsedS + durationS);
    rtc_active_manual_elapsed_s = static_cast<uint16_t>(priorElapsedS);
    persistActiveManualState();
  }

  const uint32_t intervalMs = static_cast<uint32_t>(deviceConfig().pump_sample_interval_s) * 1000UL;
  const uint32_t totalRunMs = static_cast<uint32_t>(durationS) * 1000UL;
  uint32_t remainingMs = totalRunMs;
  {
    PumpWdtGuard wdtGuard;
    const int otherPin = (relayPin == RELAY_PIN_1) ? RELAY_PIN_2 : RELAY_PIN_1;
    setRelayState(otherPin, false);
    setRelayState(relayPin, true);

    while (remainingMs > 0) {
      const uint32_t stepMs = min(remainingMs, intervalMs);
      uint32_t stepRemaining = stepMs;
      while (stepRemaining > 0) {
        const uint32_t chunk =
            min(stepRemaining, static_cast<uint32_t>(deviceConfig().pump_delay_chunk_ms));
        delay(chunk);
        stepRemaining -= chunk;
        remainingMs -= chunk;
        if (trackManual) {
          const uint32_t elapsedRunMs = totalRunMs - remainingMs;
          const uint16_t elapsedNow = static_cast<uint16_t>(priorElapsedS + elapsedRunMs / 1000UL);
          if (elapsedNow != rtc_active_manual_elapsed_s) {
            rtc_active_manual_elapsed_s = elapsedNow;
            persistActiveManualState();
          }
        }
      }
      if (remainingMs > 0) {
        const SensorSnapshot snap = captureInstantSnapshot();
        logPrintf("Irrigacao: amostra durante bomba (solo Z1:%.1f%% Z2:%.1f%%, sistema I:%.2f mA)\n",
                      snap.soil1, snap.soil2, snap.inaSistemaReadOk ? snap.iSistema : NAN);
        // Enfileira sem HTTP bloqueante enquanto o rele esta ligado (evita panic no WiFi stack).
        uploadSnapshotNow(snap, true);
      }
    }
  }

  setRelayState(relayPin, false);
  if (trackManual) {
    rtc_active_manual_elapsed_s =
        static_cast<uint16_t>(priorElapsedS + durationS);
    persistActiveManualState();
  }
  const SensorSnapshot postSnap = captureInstantSnapshot();
  logPrintf("Irrigacao: amostra pos-bomba (solo Z1:%.1f%% Z2:%.1f%%)\n", postSnap.soil1, postSnap.soil2);
  uploadSnapshotNow(postSnap, true);

  if (trackManual) {
    const int totalRan = priorElapsedS + durationS;
    if (!finalizeManualCommand(manualZone, manualCmdId, totalRan, true)) {
      logPrintf("Irrigacao manual: ack do comando %ld falhou; retomada sera tentada no proximo wake.\n",
                    (long)manualCmdId);
    }
  }

  if (s_littlefsOk) {
    flushPendingQueueWithFallback("Irrigacao");
  }

  return durationS;
}

static int maybeIrrigateZone(int relayPin, float soilPct, IrrigationZoneConfig cfg, uint8_t& armedFlag) {
  if (!cfg.valid || isnan(soilPct)) {
    return 0;
  }
  if (!cfg.active) {
    logPrintf("Irrigacao: zona desabilitada (GPIO %d); bomba nao acionada.\n", relayPin);
    return 0;
  }

  bool armed = armedFlag != 0;
  const float rearmLevel = cfg.thresholdPct + cfg.hysteresisPct;
  if (soilPct >= rearmLevel) {
    armed = true;
  }

  if (armed && soilPct < cfg.thresholdPct && cfg.pumpDurationS > 0) {
    logPrintf("Irrigacao: acionando rele GPIO %d por %d s (solo=%.1f%%, limiar=%.1f%%).\n",
                  relayPin, cfg.pumpDurationS, soilPct, cfg.thresholdPct);
    const int ranS = runPump(relayPin, cfg.pumpDurationS);
    armed = false;
    armedFlag = armed ? 1 : 0;
    return ranS;
  }

  armedFlag = armed ? 1 : 0;
  return 0;
}

static float roundTo2(float v) {
  return roundf(v * 100.0f) / 100.0f;
}

static SensorSnapshot captureInstantSnapshot() {
  SensorSnapshot snap;
  const DeviceConfig& cfg = deviceConfig();

  readSoilReading(SOIL_ADC_PIN_1, cfg.soil1_dry_mv, cfg.soil1_wet_mv, snap.soilAdc1, snap.soil1);
  readSoilReading(SOIL_ADC_PIN_2, cfg.soil2_dry_mv, cfg.soil2_wet_mv, snap.soilAdc2, snap.soil2);

  if (s_shtOk) {
    snap.temperatura = sht31.readTemperature();
    snap.umidade = sht31.readHumidity();
    if (!isnan(snap.temperatura) && !isnan(snap.umidade)) {
      snap.shtReadOk = true;
    }
  }

  if (s_bmeOk) {
    bme.takeForcedMeasurement();
    const float pLocalBme = bme.readPressure() / 100.0F;
    if (!isnan(pLocalBme)) {
      const float tempRefC = snap.shtReadOk ? snap.temperatura : 25.0f;
      const float tKelvin = tempRefC + 273.15f;
      snap.pSeaBme = pLocalBme * expf((kGravity * static_cast<float>(deviceConfig().altitude_local)) /
                                      (kGasConstantDryAir * tKelvin));
      snap.bmeReadOk = true;
    }
  }

  if (s_inaPainelOk) {
    snap.vPainel = inaPainel.getBusVoltage_V();
    snap.iPainel = inaPainel.getCurrent_mA();
    snap.pPainel = inaPainel.getPower_mW();
    if (cfg.panel_voltage_noise_floor_v > 0.0f && snap.vPainel < cfg.panel_voltage_noise_floor_v) {
      snap.vPainel = 0.0f;
    }
    if (snap.iPainel < 0.0f) snap.iPainel = 0.0f;
    if (snap.pPainel < 0.0f) snap.pPainel = 0.0f;
    if (!isnan(snap.vPainel) && !isnan(snap.iPainel) && !isnan(snap.pPainel)) {
      snap.inaPainelReadOk = true;
    }
  }
  if (s_inaSistemaOk) {
    snap.vSistema = inaSistema.getBusVoltage_V();
    snap.iSistema = inaSistema.getCurrent_mA();
    snap.pSistema = inaSistema.getPower_mW();
    if (snap.iSistema < 0.0f) snap.iSistema = 0.0f;
    if (snap.pSistema < 0.0f) snap.pSistema = 0.0f;
    if (!isnan(snap.vSistema) && !isnan(snap.iSistema) && !isnan(snap.pSistema)) {
      snap.inaSistemaReadOk = true;
    }
  }

  return snap;
}

static SensorSnapshot captureAmbientSnapshot() {
  SensorSnapshot snap;
  const DeviceConfig& cfg = deviceConfig();

  float sumSoil1 = 0.0F;
  float sumSoil2 = 0.0F;
  float sumSoilAdc1 = 0.0F;
  float sumSoilAdc2 = 0.0F;
  uint16_t countSoil1 = 0;
  uint16_t countSoil2 = 0;
  uint16_t countSoilAdc1 = 0;
  uint16_t countSoilAdc2 = 0;
  float sumTemp = 0.0F;
  float sumHum = 0.0F;
  uint16_t countTempHum = 0;
  float sumPress = 0.0F;
  uint16_t countPress = 0;

  for (int round = 0; round < cfg.sensor_average_rounds; round++) {
    float soil1 = NAN;
    float soil2 = NAN;
    float soilAdc1 = NAN;
    float soilAdc2 = NAN;
    readSoilReading(SOIL_ADC_PIN_1, cfg.soil1_dry_mv, cfg.soil1_wet_mv, soilAdc1, soil1);
    readSoilReading(SOIL_ADC_PIN_2, cfg.soil2_dry_mv, cfg.soil2_wet_mv, soilAdc2, soil2);
    if (!isnan(soil1)) {
      sumSoil1 += soil1;
      countSoil1++;
    }
    if (!isnan(soil2)) {
      sumSoil2 += soil2;
      countSoil2++;
    }
    if (!isnan(soilAdc1)) {
      sumSoilAdc1 += soilAdc1;
      countSoilAdc1++;
    }
    if (!isnan(soilAdc2)) {
      sumSoilAdc2 += soilAdc2;
      countSoilAdc2++;
    }

    if (s_shtOk) {
      const float temperatura = sht31.readTemperature();
      const float umidade = sht31.readHumidity();
      if (!isnan(temperatura) && !isnan(umidade)) {
        sumTemp += temperatura;
        sumHum += umidade;
        countTempHum++;
      } else {
        logPrintf("Leitura invalida do SHT31 (NaN).\n");
      }
    }

    if (s_bmeOk) {
      bme.takeForcedMeasurement();
      const float pLocalBme = bme.readPressure() / 100.0F;
      if (!isnan(pLocalBme)) {
        const float tempRefC = countTempHum > 0 ? (sumTemp / countTempHum) : 25.0f;
        const float tKelvin = tempRefC + 273.15f;
        const float pSea = pLocalBme * expf((kGravity * static_cast<float>(deviceConfig().altitude_local)) /
                                            (kGasConstantDryAir * tKelvin));
        sumPress += pSea;
        countPress++;
      } else {
        logPrintf("Leitura invalida do BME280 (NaN).\n");
      }
    }
  }

  if (countSoil1 > 0) {
    snap.soil1 = sumSoil1 / static_cast<float>(countSoil1);
  }
  if (countSoil2 > 0) {
    snap.soil2 = sumSoil2 / static_cast<float>(countSoil2);
  }
  if (countSoilAdc1 > 0) {
    snap.soilAdc1 = sumSoilAdc1 / static_cast<float>(countSoilAdc1);
  }
  if (countSoilAdc2 > 0) {
    snap.soilAdc2 = sumSoilAdc2 / static_cast<float>(countSoilAdc2);
  }
  if (countTempHum > 0) {
    snap.temperatura = sumTemp / static_cast<float>(countTempHum);
    snap.umidade = sumHum / static_cast<float>(countTempHum);
    snap.shtReadOk = true;
  }
  if (countPress > 0) {
    snap.pSeaBme = sumPress / static_cast<float>(countPress);
    snap.bmeReadOk = true;
  }

  return snap;
}

static void capturePowerSnapshot(SensorSnapshot& snap) {
  if (!s_inaPainelOk && !s_inaSistemaOk) {
    return;
  }

  const DeviceConfig& cfg = deviceConfig();
  const float panelNoiseFloor = cfg.panel_voltage_noise_floor_v;

  float sumVpainel = 0.0F;
  float sumIpainel = 0.0F;
  float sumPpainel = 0.0F;
  uint16_t countPainel = 0;
  float sumVsistema = 0.0F;
  float sumIsistema = 0.0F;
  float sumPsistema = 0.0F;
  uint16_t countSistema = 0;

  for (int round = 0; round < cfg.ina_average_rounds; round++) {
    if (s_inaPainelOk) {
      float vPainel = inaPainel.getBusVoltage_V();
      float iPainel = inaPainel.getCurrent_mA();
      float pPainel = inaPainel.getPower_mW();
      if (panelNoiseFloor > 0.0f && vPainel < panelNoiseFloor) vPainel = 0.0f;
      if (iPainel < 0.0f) iPainel = 0.0f;
      if (pPainel < 0.0f) pPainel = 0.0f;
      if (!isnan(vPainel) && !isnan(iPainel) && !isnan(pPainel)) {
        sumVpainel += vPainel;
        sumIpainel += iPainel;
        sumPpainel += pPainel;
        countPainel++;
      } else {
        logPrintf("Leitura invalida do INA219 painel (NaN).\n");
      }
    }

    if (s_inaSistemaOk) {
      float vSistema = inaSistema.getBusVoltage_V();
      float iSistema = inaSistema.getCurrent_mA();
      float pSistema = inaSistema.getPower_mW();
      if (iSistema < 0.0f) iSistema = 0.0f;
      if (pSistema < 0.0f) pSistema = 0.0f;
      if (!isnan(vSistema) && !isnan(iSistema) && !isnan(pSistema)) {
        sumVsistema += vSistema;
        sumIsistema += iSistema;
        sumPsistema += pSistema;
        countSistema++;
      } else {
        logPrintf("Leitura invalida do INA219 sistema (NaN).\n");
      }
    }

    if (round + 1 < cfg.ina_average_rounds) {
      delay(cfg.ina_sample_delay_ms);
    }
  }

  if (countPainel > 0) {
    snap.vPainel = sumVpainel / static_cast<float>(countPainel);
    snap.iPainel = sumIpainel / static_cast<float>(countPainel);
    snap.pPainel = sumPpainel / static_cast<float>(countPainel);
    snap.inaPainelReadOk = true;
  }
  if (countSistema > 0) {
    snap.vSistema = sumVsistema / static_cast<float>(countSistema);
    snap.iSistema = sumIsistema / static_cast<float>(countSistema);
    snap.pSistema = sumPsistema / static_cast<float>(countSistema);
    snap.inaSistemaReadOk = true;
  }
}

static bool appendSnapshotToJson(const SensorSnapshot& snap, JsonDocument& doc) {
  doc.clear();
  if (snap.shtReadOk) {
    doc["temperatura"] = roundTo2(snap.temperatura);
    doc["umidade"] = roundTo2(snap.umidade);
  }
  if (snap.bmeReadOk) {
    doc["pressao"] = roundTo2(snap.pSeaBme);
  }
  if (snap.inaPainelReadOk) {
    doc["tensao_painel"] = roundTo2(snap.vPainel);
    doc["corrente_painel"] = roundTo2(snap.iPainel);
    doc["potencia_painel"] = roundTo2(snap.pPainel);
  }
  if (snap.inaSistemaReadOk) {
    doc["tensao_sistema"] = roundTo2(snap.vSistema);
    doc["corrente_sistema"] = roundTo2(snap.iSistema);
    doc["potencia_sistema"] = roundTo2(snap.pSistema);
  }
  if (!isnan(snap.soil1)) {
    doc["umidade_solo_1"] = roundTo2(snap.soil1);
  }
  if (!isnan(snap.soil2)) {
    doc["umidade_solo_2"] = roundTo2(snap.soil2);
  }
  if (!isnan(snap.soilAdc1)) {
    doc["adc_solo_1"] = static_cast<int>(lroundf(snap.soilAdc1));
  }
  if (!isnan(snap.soilAdc2)) {
    doc["adc_solo_2"] = static_cast<int>(lroundf(snap.soilAdc2));
  }
  return doc.size() > 0;
}

static void enqueueSnapshotPayload(const char* payload) {
  if (s_littlefsOk && !pendingQueueAppend(payload)) {
    logPrintf("Fila local: falha ao gravar (LittleFS cheio ou erro).\n");
  } else if (!s_littlefsOk) {
    logPrintf("Fila local: LittleFS indisponivel; snapshot nao salvo.\n");
  }
}

static bool uploadSnapshotNow(const SensorSnapshot& snap, bool queueOnly, int irrigS1, int irrigS2) {
  JsonDocument doc;
  if (!appendSnapshotToJson(snap, doc)) {
    return false;
  }
  if (irrigS1 >= 0) {
    doc["tempo_irrigacao_s_1"] = irrigS1;
  }
  if (irrigS2 >= 0) {
    doc["tempo_irrigacao_s_2"] = irrigS2;
  }
  appendCreatedAtUtcIfSynced(doc);

  char payload[768];
  const size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0 || n >= sizeof(payload)) {
    logPrintf("Erro: JSON snapshot excede buffer ou serializacao falhou.\n");
    return false;
  }

  if (queueOnly) {
    enqueueSnapshotPayload(payload);
    return s_littlefsOk;
  }

  if (!ensureWiFi()) {
    logPrintf("Irrigacao: WiFi indisponivel; gravando snapshot na fila local.\n");
    enqueueSnapshotPayload(payload);
    return s_littlefsOk;
  }

  if (sendPayloadWithRetries(payload, n) == 201) {
    return true;
  }
  logPrintf("Irrigacao: falha ao enviar snapshot; gravando na fila local.\n");
  enqueueSnapshotPayload(payload);
  return s_littlefsOk;
}

static void flushPendingQueueWithFallback(const char* context) {
  if (!s_littlefsOk || !pendingQueueHasPending()) {
    return;
  }
  if (!ensureWiFi()) {
    return;
  }
  const int flushed = pendingQueueFlushBatch(deviceConfig().pending_batch_max_items,
                                             deviceConfig().pending_batch_max_bytes, sendBatchWithRetries);
  if (flushed > 0) {
    logPrintf("%s: %d registro(s) enviado(s) da fila (lote).\n", context, flushed);
    return;
  }
  if (flushed == -422) {
    logPrintf("%s: lote rejeitado com 422; tentando envio unitario da fila.\n", context);
    const int singleFlushed =
        pendingQueueFlush(deviceConfig().pending_batch_max_items, sendPayloadWithRetriesForQueue);
    if (singleFlushed > 0) {
      logPrintf("%s: %d registro(s) enviado(s) da fila (unitario).\n", context, singleFlushed);
    }
  }
}

static void logSensorSnapshot(const SensorSnapshot& snap) {
  logPrintf("P:%.2f | T:%.2f U:%.2f | Painel V:%.2f I:%.2f P:%.2f | Sistema V:%.2f I:%.2f P:%.2f\n",
                snap.bmeReadOk ? snap.pSeaBme : NAN,
                snap.shtReadOk ? snap.temperatura : NAN,
                snap.shtReadOk ? snap.umidade : NAN,
                snap.inaPainelReadOk ? snap.vPainel : NAN,
                snap.inaPainelReadOk ? snap.iPainel : NAN,
                snap.inaPainelReadOk ? snap.pPainel : NAN,
                snap.inaSistemaReadOk ? snap.vSistema : NAN,
                snap.inaSistemaReadOk ? snap.iSistema : NAN,
                snap.inaSistemaReadOk ? snap.pSistema : NAN);
  logPrintf("Solo Z1:%.1f%% Z2:%.1f%%\n", snap.soil1, snap.soil2);
}

static void runCaptureCycle() {
  SensorSnapshot snap = captureAmbientSnapshot();

  processManualIrrigation();

  if (ensureWiFi()) {
    capturePowerSnapshot(snap);
  } else {
    logPrintf("Power: WiFi indisponivel; INA219 omitido neste wake.\n");
  }

  logSensorSnapshot(snap);

  int irrigationSeconds1 = rtc_manual_irrig_s_1;
  int irrigationSeconds2 = rtc_manual_irrig_s_2;

  if (ensureWiFi()) {
    IrrigationConfig irrigationCfg {};
    if (fetchIrrigationConfig(irrigationCfg)) {
      const DeviceConfig& cfg = deviceConfig();
      float discardRaw = NAN;
      float freshSoil1 = NAN;
      float freshSoil2 = NAN;
      readSoilReading(SOIL_ADC_PIN_1, cfg.soil1_dry_mv, cfg.soil1_wet_mv, discardRaw, freshSoil1);
      readSoilReading(SOIL_ADC_PIN_2, cfg.soil2_dry_mv, cfg.soil2_wet_mv, discardRaw, freshSoil2);
      const float soilDecision1 = !isnan(freshSoil1) ? freshSoil1 : snap.soil1;
      const float soilDecision2 = !isnan(freshSoil2) ? freshSoil2 : snap.soil2;
      irrigationSeconds1 += maybeIrrigateZone(RELAY_PIN_1, soilDecision1, irrigationCfg.zone1,
                                              rtc_irrigation_armed_1);
      irrigationSeconds2 += maybeIrrigateZone(RELAY_PIN_2, soilDecision2, irrigationCfg.zone2,
                                              rtc_irrigation_armed_2);
    } else {
      logPrintf("Irrigacao: mantendo bombas desligadas por falta de configuracao.\n");
    }
    setRelayState(RELAY_PIN_1, false);
    setRelayState(RELAY_PIN_2, false);
  }

  const bool persisted = uploadSnapshotNow(snap, false, irrigationSeconds1, irrigationSeconds2);
  if (persisted) {
    rtc_manual_irrig_s_1 = 0;
    rtc_manual_irrig_s_2 = 0;
  }

  wifiOffIfConnected();
}

void setup() {
  deviceConfigLoadNvs();

  Serial.begin(115200);

  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  const bool coldBoot = (wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED);

  // Boot a frio: espera breve pelo host USB-CDC para nao perder logs iniciais.
  // Wake de timer (campo): sem espera — evita ~10 s de CPU ativa a cada acordar.
  if (coldBoot && deviceConfig().cold_boot_usb_wait_ms > 0) {
    uint32_t waited = 0;
    while (!Serial && waited < static_cast<uint32_t>(deviceConfig().cold_boot_usb_wait_ms)) {
      delay(50);
      waited += 50;
    }
    if (Serial) {
      delay(100);  // buffer do host pronto
    }
  }

  logPrintf("Acordando... (cause=%d)\n", static_cast<int>(wakeCause));

  s_littlefsOk = pendingQueueInit();

  pinMode(RELAY_PIN_1, OUTPUT);
  pinMode(RELAY_PIN_2, OUTPUT);
  setRelayState(RELAY_PIN_1, false);
  setRelayState(RELAY_PIN_2, false);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire1.begin(INA_SDA_PIN, INA_SCL_PIN);

  s_bmeOk = bme.begin(BME280_I2C_ADDR, &Wire);
  if (!s_bmeOk) {
    logPrintf("BME280 nao encontrado no endereco 0x%02X.\n", (unsigned)BME280_I2C_ADDR);
  } else {
    bme.setSampling(Adafruit_BME280::MODE_FORCED,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::SAMPLING_X1,
                    Adafruit_BME280::FILTER_OFF,
                    Adafruit_BME280::STANDBY_MS_0_5);
  }

  s_shtOk = sht31.begin(SHT31_I2C_ADDR);
  if (!s_shtOk) {
    logPrintf("SHT31 nao encontrado no endereco 0x%02X.\n", (unsigned)SHT31_I2C_ADDR);
  }

  s_inaPainelOk = inaPainel.begin(&Wire1);
  if (!s_inaPainelOk) {
    logPrintf("INA219 painel nao encontrado no endereco 0x%02X.\n", (unsigned)INA219_PAINEL_ADDR);
  }
  s_inaSistemaOk = inaSistema.begin(&Wire1);
  if (!s_inaSistemaOk) {
    logPrintf("INA219 sistema nao encontrado no endereco 0x%02X.\n", (unsigned)INA219_SISTEMA_ADDR);
  }

  if (!s_bmeOk && !s_shtOk && !s_inaPainelOk && !s_inaSistemaOk) {
    if (deviceConfig().deep_sleep_enabled) {
      logPrintf("Nenhum sensor I2C disponivel. Voltando a dormir.\n");
      enterDeepSleep();
      return;
    }
    logPrintf("Aviso: nenhum sensor I2C disponivel; leituras de solo ADC continuam no modo ativo.\n");
  }

  if (!initManualPrefs()) {
    logPrintf("Aviso: NVS de irrigacao manual indisponivel.\n");
  }

  if (rtc_magic != RTC_MAGIC) {
    resetRtcState();
    rtc_irrigation_armed_1 = 1;
    rtc_irrigation_armed_2 = 1;
    rtc_last_manual_id_1 = 0;
    rtc_last_manual_id_2 = 0;
    syncManualIdsFromNvsIfNeeded();
    if (!restorePersistedActiveManualState()) {
      clearActiveManualPump();
    }
  } else {
    if (rtc_irrigation_armed_1 > 1) rtc_irrigation_armed_1 = 1;
    if (rtc_irrigation_armed_2 > 1) rtc_irrigation_armed_2 = 1;
    restorePersistedActiveManualState();
  }

  if (!s_littlefsOk) {
    logPrintf("Aviso: fila offline indisponivel (LittleFS).\n");
  } else if (pendingQueueHasPending()) {
    int pending = pendingQueueCount();
    if (pending >= 0) {
      logPrintf("Fila offline: %d item(ns) pendente(s); tentando enviar...\n", pending);
    } else {
      logPrintf("Fila offline: itens pendentes; tentando enviar...\n");
    }
    if (ensureWiFi()) {
      flushPendingQueueWithFallback("Fila");
      int remaining = pendingQueueCount();
      if (remaining < 0) {
        remaining = pendingQueueHasPending() ? -1 : 0;
      }
      if (remaining > 0) {
        logPrintf("Fila: nada enviado; restam %d.\n", remaining);
      }
    } else {
      logPrintf("Fila: WiFi indisponivel; pendencias permanecem na flash.\n");
      wifiOffIfConnected();
    }
  } else {
    logPrintf("Fila offline: vazia.\n");
  }

  // O envio da fila pode ter ligado o WiFi; desliga antes de seguir o ciclo.
  wifiOffIfConnected();

  if (deviceConfig().deep_sleep_enabled) {
    logPrintf("Modo: deep sleep (%d s entre wakes)\n", deviceConfig().deep_sleep_seconds);
    runCaptureCycle();
    enterDeepSleep();
  } else {
    logPrintf("Modo: ativo (captura a cada %d s)\n", deviceConfig().capture_interval_seconds);
    runCaptureCycle();
  }
}

void loop() {
  if (deviceConfig().deep_sleep_enabled) {
    return;
  }
  delay(static_cast<uint32_t>(deviceConfig().capture_interval_seconds) * 1000UL);
  if (s_littlefsOk && pendingQueueHasPending()) {
    if (ensureWiFi()) {
      flushPendingQueueWithFallback("Fila");
    }
    wifiOffIfConnected();
  }
  runCaptureCycle();
}
