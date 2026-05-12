#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <esp_sleep.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include <Adafruit_SHT31.h>

#include "config.h"
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

#define RTC_MAGIC 0x4D455449  // "METI" meteo dual

// Reducao barometrica isotermica: P_nm = P_local * exp(g*h/(R*T_K)), T_K = temperatura medida.
static constexpr float kGravity = 9.80665f;
static constexpr float kGasConstantDryAir = 287.05f;

RTC_DATA_ATTR uint32_t rtc_magic;
// BME280: temperatura, umidade, pressao
RTC_DATA_ATTR float rtc_sum_temp_bme;
RTC_DATA_ATTR float rtc_sum_hum_bme;
RTC_DATA_ATTR float rtc_sum_press;
RTC_DATA_ATTR uint8_t rtc_count_bme;
// SHT31: temperatura, umidade
RTC_DATA_ATTR float rtc_sum_temp_sht;
RTC_DATA_ATTR float rtc_sum_hum_sht;
RTC_DATA_ATTR uint8_t rtc_count_sht;
// Tensoes (bateria, painel)
RTC_DATA_ATTR float rtc_sum_vbat;
RTC_DATA_ATTR float rtc_sum_vpainel;
RTC_DATA_ATTR uint8_t rtc_count_v;
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

Adafruit_BME280 bme;
Adafruit_SHT31 sht31 = Adafruit_SHT31();

static bool s_littlefsOk = false;
static bool s_bmeOk = false;
static bool s_shtOk = false;

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
  rtc_sum_temp_bme = 0.0F;
  rtc_sum_hum_bme = 0.0F;
  rtc_sum_press = 0.0F;
  rtc_count_bme = 0;
  rtc_sum_temp_sht = 0.0F;
  rtc_sum_hum_sht = 0.0F;
  rtc_count_sht = 0;
  rtc_sum_vbat = 0.0F;
  rtc_sum_vpainel = 0.0F;
  rtc_count_v = 0;
  rtc_sum_soil_1 = 0.0F;
  rtc_sum_soil_2 = 0.0F;
  rtc_count_soil_1 = 0;
  rtc_count_soil_2 = 0;
  rtc_total_samples = 0;
}

void enterDeepSleep() {
  setRelayState(RELAY_PIN_1, false);
  setRelayState(RELAY_PIN_2, false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Wire.end();
  esp_sleep_enable_timer_wakeup((uint64_t)DEEP_SLEEP_SECONDS * 1000000ULL);
  Serial.printf("Entrando em deep sleep por %d segundos...\n", DEEP_SLEEP_SECONDS);
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
  configTime(0, 0, NTP_SERVER_PRIMARY, NTP_SERVER_SECONDARY);
  const uint32_t start = millis();
  while (millis() - start < static_cast<uint32_t>(NTP_SYNC_WAIT_MS) && WiFi.status() == WL_CONNECTED) {
    if (wallClockLooksSynced()) {
      Serial.printf("NTP: sincronizado.\n");
      return;
    }
    delay(100);
  }
  Serial.printf("NTP: timeout (RTC pode estar invalido neste wake).\n");
}

static void appendCreatedAtUtcIfSynced(JsonDocument& doc) {
  if (!wallClockLooksSynced()) {
    return;
  }
  const time_t now = time(nullptr);
  struct tm t {};
  if (!gmtime_r(&now, &t)) {
    return;
  }
  char buf[32];
  if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t) == 0) {
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

// Le tensao em V usando analogReadMilliVolts (calibrado) e fator do divisor.
// Faz ADC_SAMPLES leituras e devolve a media. Importante: WiFi precisa estar
// desligado quando lemos pinos ADC2 (GPIO 11-20 no ESP32-S3).
float readVoltage(int pin, float dividerRatio) {
  float avgMv = readAdcMilliVoltsAvg(pin);
  if (isnan(avgMv)) {
    return NAN;
  }
  return (avgMv * dividerRatio) / 1000.0f;
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
    Serial.printf("Erro HTTP %d: %s\n", code, http.getString().c_str());
  }
  http.end();
  return code;
}

int sendPayloadWithRetries(const char* payload, size_t payloadLen) {
  int lastCode = -1;
  for (int attempt = 1; attempt <= HTTP_MAX_RETRIES; attempt++) {
    Serial.printf("Envio tentativa %d/%d...\n", attempt, HTTP_MAX_RETRIES);
    lastCode = sendData(payload, payloadLen);
    if (lastCode == 201) {
      Serial.printf("Dados enviados com sucesso.\n");
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
    Serial.printf("Irrigacao: GET /irrigation/config falhou (%d).\n", code);
    http.end();
    return false;
  }

  const String body = http.getString();
  http.end();

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    Serial.printf("Irrigacao: JSON invalido em /irrigation/config (%s).\n", err.c_str());
    return false;
  }

  bool ok1 = parseZoneConfig(doc["zone_1"], cfg.zone1);
  bool ok2 = parseZoneConfig(doc["zone_2"], cfg.zone2);
  cfg.valid = ok1 && ok2;
  if (!cfg.valid) {
    Serial.printf("Irrigacao: configuracao incompleta.\n");
  }
  return cfg.valid;
}

static int maybeIrrigateZone(int relayPin, float soilPct, IrrigationZoneConfig cfg, uint8_t& armedFlag) {
  if (!cfg.valid || isnan(soilPct)) {
    return 0;
  }
  if (!cfg.active) {
    Serial.printf("Irrigacao: zona desabilitada (GPIO %d); bomba nao acionada.\n", relayPin);
    return 0;
  }

  bool armed = armedFlag != 0;
  const float rearmLevel = cfg.thresholdPct + cfg.hysteresisPct;
  if (soilPct >= rearmLevel) {
    armed = true;
  }

  if (armed && soilPct < cfg.thresholdPct && cfg.pumpDurationS > 0) {
    Serial.printf("Irrigacao: acionando rele GPIO %d por %d s (solo=%.1f%%, limiar=%.1f%%).\n",
                  relayPin, cfg.pumpDurationS, soilPct, cfg.thresholdPct);
    setRelayState(relayPin, true);
    delay(cfg.pumpDurationS * 1000);
    setRelayState(relayPin, false);
    armed = false;
    armedFlag = armed ? 1 : 0;
    return cfg.pumpDurationS;
  }

  armedFlag = armed ? 1 : 0;
  return 0;
}

static float roundTo2(float v) {
  return roundf(v * 100.0f) / 100.0f;
}

void runCycle() {
  // 1) Le tensoes e umidade do solo ANTES do WiFi (ADC2 conflita no ESP32-S3).
  float vbat = readVoltage(BAT_ADC_PIN, BAT_DIVIDER_RATIO);
  float vpainel = readVoltage(SOLAR_ADC_PIN, SOLAR_DIVIDER_RATIO);
  bool vOk = !isnan(vbat) && !isnan(vpainel);
  if (vOk) {
    rtc_sum_vbat += vbat;
    rtc_sum_vpainel += vpainel;
    rtc_count_v++;
  }
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

  // 2) BME280: temperatura, umidade, pressao
  bool bmeReadOk = false;
  float tBme = NAN, hBme = NAN, pLocalBme = NAN, pSeaBme = NAN;
  if (s_bmeOk) {
    bme.takeForcedMeasurement();
    tBme = bme.readTemperature();
    hBme = bme.readHumidity();
    pLocalBme = bme.readPressure() / 100.0F;
    if (!isnan(tBme) && !isnan(hBme) && !isnan(pLocalBme)) {
      float tKelvin = tBme + 273.15f;
      pSeaBme = pLocalBme * expf((kGravity * (float)ALTITUDE_LOCAL) / (kGasConstantDryAir * tKelvin));
      rtc_sum_temp_bme += tBme;
      rtc_sum_hum_bme += hBme;
      rtc_sum_press += pSeaBme;
      rtc_count_bme++;
      bmeReadOk = true;
    } else {
      Serial.printf("Leitura invalida do BME280 (NaN).\n");
    }
  }

  // 3) SHT31: temperatura, umidade
  bool shtReadOk = false;
  float tSht = NAN, hSht = NAN;
  if (s_shtOk) {
    tSht = sht31.readTemperature();
    hSht = sht31.readHumidity();
    if (!isnan(tSht) && !isnan(hSht)) {
      rtc_sum_temp_sht += tSht;
      rtc_sum_hum_sht += hSht;
      rtc_count_sht++;
      shtReadOk = true;
    } else {
      Serial.printf("Leitura invalida do SHT31 (NaN).\n");
    }
  }

  rtc_total_samples++;

  Serial.printf("[%d/%d] BME T:%.2f U:%.2f P:%.2f | SHT T:%.2f U:%.2f | Vbat:%.2f Vpain:%.2f\n",
                rtc_total_samples, (int)SAMPLES_PER_API_UPLOAD,
                bmeReadOk ? tBme : NAN,
                bmeReadOk ? hBme : NAN,
                bmeReadOk ? pSeaBme : NAN,
                shtReadOk ? tSht : NAN,
                shtReadOk ? hSht : NAN,
                vOk ? vbat : NAN,
                vOk ? vpainel : NAN);
  Serial.printf("Solo Z1:%.1f%% Z2:%.1f%%\n", soil1, soil2);

  if (rtc_total_samples < SAMPLES_PER_API_UPLOAD) {
    wifiOffIfConnected();
    return;
  }

  // 4) Janela completa: monta JSON apenas com campos cujo contador > 0.
  JsonDocument doc;

  if (rtc_count_bme > 0) {
    doc["temperatura_bme"] = roundTo2(rtc_sum_temp_bme / rtc_count_bme);
    doc["umidade_bme"] = roundTo2(rtc_sum_hum_bme / rtc_count_bme);
    doc["pressao"] = roundTo2(rtc_sum_press / rtc_count_bme);
  }
  if (rtc_count_sht > 0) {
    doc["temperatura_sht"] = roundTo2(rtc_sum_temp_sht / rtc_count_sht);
    doc["umidade_sht"] = roundTo2(rtc_sum_hum_sht / rtc_count_sht);
  }
  if (rtc_count_v > 0) {
    doc["tensao_bateria"] = roundTo2(rtc_sum_vbat / rtc_count_v);
    doc["tensao_painel"] = roundTo2(rtc_sum_vpainel / rtc_count_v);
  }
  if (rtc_count_soil_1 > 0) {
    doc["umidade_solo_1"] = roundTo2(rtc_sum_soil_1 / rtc_count_soil_1);
  }
  if (rtc_count_soil_2 > 0) {
    doc["umidade_solo_2"] = roundTo2(rtc_sum_soil_2 / rtc_count_soil_2);
  }

  if (doc.size() == 0) {
    Serial.printf("Janela sem nenhuma medicao valida; descartando.\n");
    wifiOffIfConnected();
    resetRtcWindow();
    return;
  }

  Serial.printf("Janela completa (BME=%d SHT=%d V=%d amostras validas em %d wakes).\n",
                rtc_count_bme, rtc_count_sht, rtc_count_v, rtc_total_samples);

  const bool wifiUp = ensureWiFi();

  if (!wifiUp) {
    Serial.printf("Falha ao conectar WiFi; gravando na fila local.\n");
    doc["tempo_irrigacao_s_1"] = 0;
    doc["tempo_irrigacao_s_2"] = 0;
    appendCreatedAtUtcIfSynced(doc);
    char payloadOffline[768];
    size_t nOffline = serializeJson(doc, payloadOffline, sizeof(payloadOffline));
    if (nOffline == 0 || nOffline >= sizeof(payloadOffline)) {
      Serial.printf("Erro: JSON excede buffer ou serializacao falhou.\n");
      wifiOffIfConnected();
      resetRtcWindow();
      return;
    }
    if (s_littlefsOk && !pendingQueueAppend(payloadOffline)) {
      Serial.printf("Fila local: falha ao gravar (LittleFS cheio ou erro).\n");
    } else if (!s_littlefsOk) {
      Serial.printf("Fila local: LittleFS indisponivel; dados desta janela nao salvos.\n");
    }
    wifiOffIfConnected();
    resetRtcWindow();
    return;
  }
  Serial.printf("WiFi conectado.\n");

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
    Serial.printf("Irrigacao: mantendo bombas desligadas por falta de configuracao.\n");
  }
  setRelayState(RELAY_PIN_1, false);
  setRelayState(RELAY_PIN_2, false);

  doc["tempo_irrigacao_s_1"] = irrigationSeconds1;
  doc["tempo_irrigacao_s_2"] = irrigationSeconds2;
  appendCreatedAtUtcIfSynced(doc);

  char payload[768];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0 || n >= sizeof(payload)) {
    Serial.printf("Erro: JSON excede buffer ou serializacao falhou.\n");
    wifiOffIfConnected();
    resetRtcWindow();
    return;
  }

  if (sendPayloadWithRetries(payload, n) != 201) {
    Serial.printf("Falha ao enviar dados apos todas as tentativas; gravando na fila local.\n");
    if (s_littlefsOk && !pendingQueueAppend(payload)) {
      Serial.printf("Fila local: falha ao gravar (LittleFS cheio ou erro).\n");
    } else if (!s_littlefsOk) {
      Serial.printf("Fila local: LittleFS indisponivel; dados desta janela nao salvos.\n");
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

  Serial.printf("Acordando... (cause=%d)\n", static_cast<int>(wakeCause));

  s_littlefsOk = pendingQueueInit();

  pinMode(RELAY_PIN_1, OUTPUT);
  pinMode(RELAY_PIN_2, OUTPUT);
  setRelayState(RELAY_PIN_1, false);
  setRelayState(RELAY_PIN_2, false);

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  s_bmeOk = bme.begin(BME280_I2C_ADDR, &Wire);
  if (!s_bmeOk) {
    Serial.printf("BME280 nao encontrado no endereco 0x%02X.\n", (unsigned)BME280_I2C_ADDR);
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
    Serial.printf("SHT31 nao encontrado no endereco 0x%02X.\n", (unsigned)SHT31_I2C_ADDR);
  }

  if (!s_bmeOk && !s_shtOk) {
    Serial.printf("Nenhum sensor I2C disponivel. Voltando a dormir.\n");
    enterDeepSleep();
    return;
  }

  if (rtc_magic != RTC_MAGIC) {
    resetRtcWindow();
    rtc_irrigation_armed_1 = 1;
    rtc_irrigation_armed_2 = 1;
  } else {
    if (rtc_irrigation_armed_1 > 1) rtc_irrigation_armed_1 = 1;
    if (rtc_irrigation_armed_2 > 1) rtc_irrigation_armed_2 = 1;
  }

  if (!s_littlefsOk) {
    Serial.printf("Aviso: fila offline indisponivel (LittleFS).\n");
  } else if (pendingQueueHasPending()) {
    int pending = pendingQueueCount();
    if (pending >= 0) {
      Serial.printf("Fila offline: %d item(ns) pendente(s); tentando enviar...\n", pending);
    } else {
      Serial.printf("Fila offline: itens pendentes; tentando enviar...\n");
    }
    if (ensureWiFi()) {
      int n = pendingQueueFlush(PENDING_FLUSH_MAX_PER_WAKE, sendPayloadWithRetriesForQueue);
      int remaining = pendingQueueCount();
      if (remaining < 0) {
        remaining = pendingQueueHasPending() ? -1 : 0;
      }
      if (n > 0) {
        if (remaining >= 0) {
          Serial.printf("Fila: enviados %d; restam %d.\n", n, remaining);
        } else {
          Serial.printf("Fila: enviados %d registro(s) pendente(s).\n", n);
        }
      } else if (remaining > 0) {
        Serial.printf("Fila: nada enviado; restam %d.\n", remaining);
      }
    } else {
      Serial.printf("Fila: WiFi indisponivel; pendencias permanecem na flash.\n");
      wifiOffIfConnected();
    }
  } else {
    Serial.printf("Fila offline: vazia.\n");
  }

  // ATENCAO: drain da fila acima pode ter ligado o WiFi. Como GPIO 12/13 estao
  // no ADC2, precisamos garantir que o WiFi esteja desligado antes do runCycle
  // ler as tensoes.
  wifiOffIfConnected();

  runCycle();

  enterDeepSleep();
}

void loop() {
}
