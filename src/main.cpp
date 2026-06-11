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
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_SHT31.h>
#include <Adafruit_INA219.h>

#include "config.h"
#include "log.h"
#include "pending_queue.h"

#ifndef SAMPLES_PER_API_UPLOAD
#define SAMPLES_PER_API_UPLOAD 10
#endif

#ifndef PENDING_FLUSH_MAX_PER_WAKE
#define PENDING_FLUSH_MAX_PER_WAKE 10
#endif

#ifndef COLD_BOOT_USB_WAIT_MS
#define COLD_BOOT_USB_WAIT_MS 2000
#endif

#ifndef ADC_SAMPLES
#define ADC_SAMPLES 16
#endif

#ifndef MANUAL_IRRIGATION_MAX_S
#define MANUAL_IRRIGATION_MAX_S 600
#endif

#ifndef NTP_SERVER_PRIMARY
#define NTP_SERVER_PRIMARY "pool.ntp.org"
#endif
#ifndef NTP_SERVER_SECONDARY
#define NTP_SERVER_SECONDARY "time.google.com"
#endif
#ifndef NTP_SYNC_WAIT_MS
#define NTP_SYNC_WAIT_MS 3500
#endif
#ifndef NTP_MIN_VALID_YEAR
#define NTP_MIN_VALID_YEAR 2024
#endif
#ifndef NTP_GMT_OFFSET_SEC
#define NTP_GMT_OFFSET_SEC (-3 * 3600)
#endif
#ifndef NTP_DAYLIGHT_OFFSET_SEC
#define NTP_DAYLIGHT_OFFSET_SEC 0
#endif

#define RTC_MAGIC 0x4D455449  // "METI" meteo dual

// Reducao barometrica isotermica: P_nm = P_local * exp(g*h/(R*T_K)), T_K = temperatura medida.
static constexpr float kGravity = 9.80665f;
static constexpr float kGasConstantDryAir = 287.05f;

RTC_DATA_ATTR uint32_t rtc_magic;
// BME280: somente pressao
RTC_DATA_ATTR float rtc_sum_press;
RTC_DATA_ATTR uint8_t rtc_count_press;
// SHT31: temperatura, umidade (fonte exclusiva)
RTC_DATA_ATTR float rtc_sum_temp;
RTC_DATA_ATTR float rtc_sum_hum;
RTC_DATA_ATTR uint8_t rtc_count_temp_hum;
// INA219 painel
RTC_DATA_ATTR float rtc_sum_vpainel;
RTC_DATA_ATTR float rtc_sum_ipainel;
RTC_DATA_ATTR float rtc_sum_ppainel;
RTC_DATA_ATTR uint8_t rtc_count_painel;
// INA219 sistema
RTC_DATA_ATTR float rtc_sum_vsistema;
RTC_DATA_ATTR float rtc_sum_isistema;
RTC_DATA_ATTR float rtc_sum_psistema;
RTC_DATA_ATTR uint8_t rtc_count_sistema;
// Umidade do solo por zona
RTC_DATA_ATTR float rtc_sum_soil_1;
RTC_DATA_ATTR float rtc_sum_soil_2;
RTC_DATA_ATTR uint8_t rtc_count_soil_1;
RTC_DATA_ATTR uint8_t rtc_count_soil_2;
// Total de wakes acumulados na janela
RTC_DATA_ATTR uint8_t rtc_total_samples;
// Histerese da irrigacao por zona (persistente entre wakes).
RTC_DATA_ATTR uint8_t rtc_irrigation_armed_1;
RTC_DATA_ATTR uint8_t rtc_irrigation_armed_2;
// Irrigacao manual: segundos acumulados na janela (entram em tempo_irrigacao_s_N).
RTC_DATA_ATTR uint16_t rtc_manual_irrig_s_1;
RTC_DATA_ATTR uint16_t rtc_manual_irrig_s_2;
// Ultimo comando manual executado por zona (evita re-execucao se o ack falhar).
RTC_DATA_ATTR int32_t rtc_last_manual_id_1;
RTC_DATA_ATTR int32_t rtc_last_manual_id_2;

Adafruit_BME280 bme;
Adafruit_SHT31 sht31 = Adafruit_SHT31();
Adafruit_INA219 inaPainel(INA219_PAINEL_ADDR);
Adafruit_INA219 inaSistema(INA219_SISTEMA_ADDR);

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
#if RELAY_ACTIVE_HIGH
  digitalWrite(pin, on ? HIGH : LOW);
#else
  digitalWrite(pin, on ? LOW : HIGH);
#endif
}

static float readAdcMilliVoltsAvg(int pin) {
  uint32_t accumMv = 0;
  int valid = 0;
  for (int i = 0; i < ADC_SAMPLES; i++) {
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

static float readSoilPercent(int pin, float dryMv, float wetMv) {
  float mv = readAdcMilliVoltsAvg(pin);
  if (isnan(mv) || fabsf(wetMv - dryMv) < 1.0f) {
    return NAN;
  }
  const float pct = ((mv - dryMv) / (wetMv - dryMv)) * 100.0f;
  return clampPct(pct);
}

void resetRtcWindow() {
  rtc_magic = RTC_MAGIC;
  rtc_sum_press = 0.0F;
  rtc_count_press = 0;
  rtc_sum_temp = 0.0F;
  rtc_sum_hum = 0.0F;
  rtc_count_temp_hum = 0;
  rtc_sum_vpainel = 0.0F;
  rtc_sum_ipainel = 0.0F;
  rtc_sum_ppainel = 0.0F;
  rtc_count_painel = 0;
  rtc_sum_vsistema = 0.0F;
  rtc_sum_isistema = 0.0F;
  rtc_sum_psistema = 0.0F;
  rtc_count_sistema = 0;
  rtc_sum_soil_1 = 0.0F;
  rtc_sum_soil_2 = 0.0F;
  rtc_count_soil_1 = 0;
  rtc_count_soil_2 = 0;
  rtc_manual_irrig_s_1 = 0;
  rtc_manual_irrig_s_2 = 0;
  rtc_total_samples = 0;
}

void enterDeepSleep() {
  setRelayState(RELAY_PIN_1, false);
  setRelayState(RELAY_PIN_2, false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Wire.end();
  Wire1.end();
  esp_sleep_enable_timer_wakeup((uint64_t)DEEP_SLEEP_SECONDS * 1000000ULL);
  logPrintf("Entrando em deep sleep por %d segundos...\n", DEEP_SLEEP_SECONDS);
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
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < WIFI_TIMEOUT_MS) {
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
  return (t.tm_year + 1900) >= NTP_MIN_VALID_YEAR;
}

static void trySyncTimeFromNtp() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  if (wallClockLooksSynced()) {
    return;
  }
  configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
  const uint32_t start = millis();
  while (millis() - start < static_cast<uint32_t>(NTP_SYNC_WAIT_MS) && WiFi.status() == WL_CONNECTED) {
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
  struct tm t {};
  if (!getLocalTime(&t, 0)) {
    return;
  }
  char buf[32];
  if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &t) == 0) {
    return;
  }
  doc["created_at"] = buf;
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    trySyncTimeFromNtp();
    return true;
  }
  if (!connectWiFi()) {
    return false;
  }
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
  http.setTimeout(HTTP_TIMEOUT_MS);
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

int sendPayloadWithRetries(const char* payload, size_t payloadLen) {
  int lastCode = -1;
  for (int attempt = 1; attempt <= HTTP_MAX_RETRIES; attempt++) {
    logPrintf("Envio tentativa %d/%d...\n", attempt, HTTP_MAX_RETRIES);
    lastCode = sendData(payload, payloadLen);
    if (lastCode == 201) {
      logPrintf("Dados enviados com sucesso.\n");
      return 201;
    }
    if (attempt < HTTP_MAX_RETRIES) {
      delay(2000);
    }
  }
  return lastCode;
}

// Ponte para PendingSendFn (linhas lidas como String na fila).
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
  http.setTimeout(HTTP_TIMEOUT_MS);
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

static int runPump(int relayPin, int durationS);

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
  http.setTimeout(HTTP_TIMEOUT_MS);
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

  for (int attempt = 1; attempt <= HTTP_MAX_RETRIES; attempt++) {
    HTTPClient http;
    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
    http.addHeader("Authorization", "Bearer " API_TOKEN);
    const int code = http.POST(nullptr, 0);
    http.end();
    if (code == 200) {
      logPrintf("Irrigacao manual: inicio do comando %ld confirmado.\n", (long)id);
      return true;
    }
    if (code == 404 || code == 409) {
      logPrintf("Irrigacao manual: start do comando %ld respondeu %d; tratado como iniciado.\n",
                    (long)id, code);
      return true;
    }
    logPrintf("Irrigacao manual: start tentativa %d/%d falhou (%d).\n", attempt, HTTP_MAX_RETRIES, code);
    if (attempt < HTTP_MAX_RETRIES) {
      delay(2000);
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

  for (int attempt = 1; attempt <= HTTP_MAX_RETRIES; attempt++) {
    HTTPClient http;
    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_MS);
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
    logPrintf("Irrigacao manual: ack tentativa %d/%d falhou (%d).\n", attempt, HTTP_MAX_RETRIES, code);
    if (attempt < HTTP_MAX_RETRIES) {
      delay(2000);
    }
  }
  return false;
}

// Verifica e executa comandos manuais em todo wake (liga WiFi se preciso).
// Deve rodar APOS as leituras de ADC/sensores (restricao ADC2 x WiFi).
static void processManualIrrigation() {
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
    int32_t& lastId = (cmd.zone == 1) ? rtc_last_manual_id_1 : rtc_last_manual_id_2;
    const bool isRunning = strcmp(cmd.status, "running") == 0;

    if (cmd.id == lastId || isRunning) {
      logPrintf("Irrigacao manual: comando %ld (zona %d) ja executado; reenviando ack.\n",
                    (long)cmd.id, cmd.zone);
      ensureWiFi();
      ackManualCommand(cmd.id, cmd.durationS);
      if (cmd.id != lastId) {
        lastId = cmd.id;
      }
      continue;
    }

    if (!startManualCommand(cmd.id)) {
      logPrintf("Irrigacao manual: start do comando %ld falhou; bomba nao acionada.\n",
                    (long)cmd.id);
      continue;
    }

    const int relayPin = (cmd.zone == 1) ? RELAY_PIN_1 : RELAY_PIN_2;
    logPrintf("Irrigacao manual: acionando bomba da zona %d por %d s (comando %ld).\n",
                  cmd.zone, cmd.durationS, (long)cmd.id);
    const int ranS = runPump(relayPin, cmd.durationS);
    if (cmd.zone == 1) {
      rtc_manual_irrig_s_1 += static_cast<uint16_t>(ranS);
    } else {
      rtc_manual_irrig_s_2 += static_cast<uint16_t>(ranS);
    }
    lastId = cmd.id;
    ensureWiFi();
    ackManualCommand(cmd.id, ranS);
  }
}

// Unico ponto que energiza uma bomba. Regra: nunca as duas ao mesmo tempo —
// o rele da outra bomba e desligado antes de ligar este (acionamento bloqueante).
static int runPump(int relayPin, int durationS) {
  if (durationS <= 0) {
    return 0;
  }
  if (durationS > MANUAL_IRRIGATION_MAX_S) {
    durationS = MANUAL_IRRIGATION_MAX_S;
  }
  const int otherPin = (relayPin == RELAY_PIN_1) ? RELAY_PIN_2 : RELAY_PIN_1;
  setRelayState(otherPin, false);
  setRelayState(relayPin, true);
  delay(static_cast<uint32_t>(durationS) * 1000UL);
  setRelayState(relayPin, false);
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

void runCycle() {
  // 1) Le umidade do solo.
  float soil1 = readSoilPercent(SOIL_ADC_PIN_1, SOIL1_DRY_MV, SOIL1_WET_MV);
  float soil2 = readSoilPercent(SOIL_ADC_PIN_2, SOIL2_DRY_MV, SOIL2_WET_MV);
  if (!isnan(soil1)) {
    rtc_sum_soil_1 += soil1;
    rtc_count_soil_1++;
  }
  if (!isnan(soil2)) {
    rtc_sum_soil_2 += soil2;
    rtc_count_soil_2++;
  }

  // 2) BME280: somente pressao (corrigida para nivel do mar com temperatura do SHT31).
  bool bmeReadOk = false;
  float pLocalBme = NAN, pSeaBme = NAN;
  if (s_bmeOk) {
    bme.takeForcedMeasurement();
    pLocalBme = bme.readPressure() / 100.0F;
    if (!isnan(pLocalBme)) {
      const float tempRefC = (rtc_count_temp_hum > 0) ? (rtc_sum_temp / rtc_count_temp_hum) : 25.0f;
      float tKelvin = tempRefC + 273.15f;
      pSeaBme = pLocalBme * expf((kGravity * (float)ALTITUDE_LOCAL) / (kGasConstantDryAir * tKelvin));
      rtc_sum_press += pSeaBme;
      rtc_count_press++;
      bmeReadOk = true;
    } else {
      logPrintf("Leitura invalida do BME280 (NaN).\n");
    }
  }

  // 3) SHT31: temperatura e umidade (fonte unica).
  bool shtReadOk = false;
  float temperatura = NAN, umidade = NAN;
  if (s_shtOk) {
    temperatura = sht31.readTemperature();
    umidade = sht31.readHumidity();
    if (!isnan(temperatura) && !isnan(umidade)) {
      rtc_sum_temp += temperatura;
      rtc_sum_hum += umidade;
      rtc_count_temp_hum++;
      shtReadOk = true;
    } else {
      logPrintf("Leitura invalida do SHT31 (NaN).\n");
    }
  }

  // 4) INA219: painel (0x40) e sistema (0x41)
  bool inaPainelReadOk = false;
  bool inaSistemaReadOk = false;
  float vPainel = NAN, iPainel = NAN, pPainel = NAN;
  float vSistema = NAN, iSistema = NAN, pSistema = NAN;
  if (s_inaPainelOk) {
    vPainel = inaPainel.getBusVoltage_V();
    iPainel = inaPainel.getCurrent_mA();
    pPainel = inaPainel.getPower_mW();
    if (iPainel < 0.0f) iPainel = 0.0f;
    if (pPainel < 0.0f) pPainel = 0.0f;
    if (!isnan(vPainel) && !isnan(iPainel) && !isnan(pPainel)) {
      rtc_sum_vpainel += vPainel;
      rtc_sum_ipainel += iPainel;
      rtc_sum_ppainel += pPainel;
      rtc_count_painel++;
      inaPainelReadOk = true;
    } else {
      logPrintf("Leitura invalida do INA219 painel (NaN).\n");
    }
  }
  if (s_inaSistemaOk) {
    vSistema = inaSistema.getBusVoltage_V();
    iSistema = inaSistema.getCurrent_mA();
    pSistema = inaSistema.getPower_mW();
    if (!isnan(vSistema) && !isnan(iSistema) && !isnan(pSistema)) {
      rtc_sum_vsistema += vSistema;
      rtc_sum_isistema += iSistema;
      rtc_sum_psistema += pSistema;
      rtc_count_sistema++;
      inaSistemaReadOk = true;
    } else {
      logPrintf("Leitura invalida do INA219 sistema (NaN).\n");
    }
  }

  rtc_total_samples++;

  logPrintf("[%d/%d] P:%.2f | T:%.2f U:%.2f | Painel V:%.2f I:%.2f P:%.2f | Sistema V:%.2f I:%.2f P:%.2f\n",
                rtc_total_samples, (int)SAMPLES_PER_API_UPLOAD,
                bmeReadOk ? pSeaBme : NAN,
                shtReadOk ? temperatura : NAN,
                shtReadOk ? umidade : NAN,
                inaPainelReadOk ? vPainel : NAN,
                inaPainelReadOk ? iPainel : NAN,
                inaPainelReadOk ? pPainel : NAN,
                inaSistemaReadOk ? vSistema : NAN,
                inaSistemaReadOk ? iSistema : NAN,
                inaSistemaReadOk ? pSistema : NAN);
  logPrintf("Solo Z1:%.1f%% Z2:%.1f%%\n", soil1, soil2);

  // Comandos manuais sao verificados em todo wake (apos leituras de ADC/sensores).
  processManualIrrigation();

  if (rtc_total_samples < SAMPLES_PER_API_UPLOAD) {
    wifiOffIfConnected();
    return;
  }

  // 4) Janela completa: monta JSON apenas com campos cujo contador > 0.
  JsonDocument doc;

  if (rtc_count_temp_hum > 0) {
    doc["temperatura"] = roundTo2(rtc_sum_temp / rtc_count_temp_hum);
    doc["umidade"] = roundTo2(rtc_sum_hum / rtc_count_temp_hum);
  }
  if (rtc_count_press > 0) {
    doc["pressao"] = roundTo2(rtc_sum_press / rtc_count_press);
  }
  if (rtc_count_painel > 0) {
    doc["tensao_painel"] = roundTo2(rtc_sum_vpainel / rtc_count_painel);
    doc["corrente_painel"] = roundTo2(rtc_sum_ipainel / rtc_count_painel);
    doc["potencia_painel"] = roundTo2(rtc_sum_ppainel / rtc_count_painel);
  }
  if (rtc_count_sistema > 0) {
    doc["tensao_sistema"] = roundTo2(rtc_sum_vsistema / rtc_count_sistema);
    doc["corrente_sistema"] = roundTo2(rtc_sum_isistema / rtc_count_sistema);
    doc["potencia_sistema"] = roundTo2(rtc_sum_psistema / rtc_count_sistema);
  }
  if (rtc_count_soil_1 > 0) {
    doc["umidade_solo_1"] = roundTo2(rtc_sum_soil_1 / rtc_count_soil_1);
  }
  if (rtc_count_soil_2 > 0) {
    doc["umidade_solo_2"] = roundTo2(rtc_sum_soil_2 / rtc_count_soil_2);
  }

  if (doc.size() == 0) {
    logPrintf("Janela sem nenhuma medicao valida; descartando.\n");
    wifiOffIfConnected();
    resetRtcWindow();
    return;
  }

  logPrintf("Janela completa (TH=%d Press=%d Painel=%d Sistema=%d amostras validas em %d wakes).\n",
                rtc_count_temp_hum, rtc_count_press, rtc_count_painel, rtc_count_sistema, rtc_total_samples);

  const bool wifiUp = ensureWiFi();

  if (!wifiUp) {
    logPrintf("Falha ao conectar WiFi; gravando na fila local.\n");
    doc["tempo_irrigacao_s_1"] = rtc_manual_irrig_s_1;
    doc["tempo_irrigacao_s_2"] = rtc_manual_irrig_s_2;
    appendCreatedAtUtcIfSynced(doc);
    char payloadOffline[768];
    size_t nOffline = serializeJson(doc, payloadOffline, sizeof(payloadOffline));
    if (nOffline == 0 || nOffline >= sizeof(payloadOffline)) {
      logPrintf("Erro: JSON excede buffer ou serializacao falhou.\n");
      wifiOffIfConnected();
      resetRtcWindow();
      return;
    }
    if (s_littlefsOk && !pendingQueueAppend(payloadOffline)) {
      logPrintf("Fila local: falha ao gravar (LittleFS cheio ou erro).\n");
    } else if (!s_littlefsOk) {
      logPrintf("Fila local: LittleFS indisponivel; dados desta janela nao salvos.\n");
    }
    wifiOffIfConnected();
    resetRtcWindow();
    return;
  }
  logPrintf("WiFi conectado.\n");

  int irrigationSeconds1 = 0;
  int irrigationSeconds2 = 0;
  IrrigationConfig irrigationCfg {};
  if (fetchIrrigationConfig(irrigationCfg)) {
    const float soilDecision1 = !isnan(soil1)
        ? soil1
        : (rtc_count_soil_1 > 0 ? (rtc_sum_soil_1 / rtc_count_soil_1) : NAN);
    const float soilDecision2 = !isnan(soil2)
        ? soil2
        : (rtc_count_soil_2 > 0 ? (rtc_sum_soil_2 / rtc_count_soil_2) : NAN);
    irrigationSeconds1 = maybeIrrigateZone(RELAY_PIN_1, soilDecision1, irrigationCfg.zone1, rtc_irrigation_armed_1);
    irrigationSeconds2 = maybeIrrigateZone(RELAY_PIN_2, soilDecision2, irrigationCfg.zone2, rtc_irrigation_armed_2);
  } else {
    logPrintf("Irrigacao: mantendo bombas desligadas por falta de configuracao.\n");
  }
  setRelayState(RELAY_PIN_1, false);
  setRelayState(RELAY_PIN_2, false);

  doc["tempo_irrigacao_s_1"] = irrigationSeconds1 + rtc_manual_irrig_s_1;
  doc["tempo_irrigacao_s_2"] = irrigationSeconds2 + rtc_manual_irrig_s_2;
  appendCreatedAtUtcIfSynced(doc);

  char payload[768];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0 || n >= sizeof(payload)) {
    logPrintf("Erro: JSON excede buffer ou serializacao falhou.\n");
    wifiOffIfConnected();
    resetRtcWindow();
    return;
  }

  if (sendPayloadWithRetries(payload, n) != 201) {
    logPrintf("Falha ao enviar dados apos todas as tentativas; gravando na fila local.\n");
    if (s_littlefsOk && !pendingQueueAppend(payload)) {
      logPrintf("Fila local: falha ao gravar (LittleFS cheio ou erro).\n");
    } else if (!s_littlefsOk) {
      logPrintf("Fila local: LittleFS indisponivel; dados desta janela nao salvos.\n");
    }
  }

  wifiOffIfConnected();
  resetRtcWindow();
}

void setup() {
  Serial.begin(115200);

  const esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  const bool coldBoot = (wakeCause == ESP_SLEEP_WAKEUP_UNDEFINED);

  // Boot a frio: espera breve pelo host USB-CDC para nao perder logs iniciais.
  // Wake de timer (campo): sem espera — evita ~10 s de CPU ativa a cada acordar.
  if (coldBoot && COLD_BOOT_USB_WAIT_MS > 0) {
    uint32_t waited = 0;
    while (!Serial && waited < static_cast<uint32_t>(COLD_BOOT_USB_WAIT_MS)) {
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
    logPrintf("Nenhum sensor I2C disponivel. Voltando a dormir.\n");
    enterDeepSleep();
    return;
  }

  if (rtc_magic != RTC_MAGIC) {
    resetRtcWindow();
    rtc_irrigation_armed_1 = 1;
    rtc_irrigation_armed_2 = 1;
    rtc_last_manual_id_1 = 0;
    rtc_last_manual_id_2 = 0;
  } else {
    if (rtc_irrigation_armed_1 > 1) rtc_irrigation_armed_1 = 1;
    if (rtc_irrigation_armed_2 > 1) rtc_irrigation_armed_2 = 1;
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
      int n = pendingQueueFlush(PENDING_FLUSH_MAX_PER_WAKE, sendPayloadWithRetriesForQueue);
      int remaining = pendingQueueCount();
      if (remaining < 0) {
        remaining = pendingQueueHasPending() ? -1 : 0;
      }
      if (n > 0) {
        if (remaining >= 0) {
          logPrintf("Fila: enviados %d; restam %d.\n", n, remaining);
        } else {
          logPrintf("Fila: enviados %d registro(s) pendente(s).\n", n);
        }
      } else if (remaining > 0) {
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

  runCycle();

  enterDeepSleep();
}

void loop() {
}
