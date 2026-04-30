#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
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
// Total de wakes acumulados na janela
RTC_DATA_ATTR uint8_t rtc_total_samples;

Adafruit_BME280 bme;
Adafruit_SHT31 sht31 = Adafruit_SHT31();

static bool s_littlefsOk = false;
static bool s_bmeOk = false;
static bool s_shtOk = false;

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
  rtc_total_samples = 0;
}

void enterDeepSleep() {
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

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }
  return connectWiFi();
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
  float avgMv = static_cast<float>(accumMv) / static_cast<float>(valid);
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

static float roundTo2(float v) {
  return roundf(v * 100.0f) / 100.0f;
}

void runCycle() {
  // 1) Le tensoes ANTES do WiFi (ADC2 conflita com WiFi no ESP32-S3).
  float vbat = readVoltage(BAT_ADC_PIN, BAT_DIVIDER_RATIO);
  float vpainel = readVoltage(SOLAR_ADC_PIN, SOLAR_DIVIDER_RATIO);
  bool vOk = !isnan(vbat) && !isnan(vpainel);
  if (vOk) {
    rtc_sum_vbat += vbat;
    rtc_sum_vpainel += vpainel;
    rtc_count_v++;
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

  if (doc.size() == 0) {
    Serial.printf("Janela sem nenhuma medicao valida; descartando.\n");
    wifiOffIfConnected();
    resetRtcWindow();
    return;
  }

  Serial.printf("Janela completa (BME=%d SHT=%d V=%d amostras validas em %d wakes).\n",
                rtc_count_bme, rtc_count_sht, rtc_count_v, rtc_total_samples);

  char payload[256];
  size_t n = serializeJson(doc, payload, sizeof(payload));
  if (n == 0 || n >= sizeof(payload)) {
    Serial.printf("Erro: JSON excede buffer ou serializacao falhou.\n");
    wifiOffIfConnected();
    resetRtcWindow();
    return;
  }

  if (!ensureWiFi()) {
    Serial.printf("Falha ao conectar WiFi; gravando na fila local.\n");
    if (s_littlefsOk && !pendingQueueAppend(payload)) {
      Serial.printf("Fila local: falha ao gravar (LittleFS cheio ou erro).\n");
    } else if (!s_littlefsOk) {
      Serial.printf("Fila local: LittleFS indisponivel; dados desta janela nao salvos.\n");
    }
    wifiOffIfConnected();
    resetRtcWindow();
    return;
  }
  Serial.printf("WiFi conectado.\n");

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
  }

  if (!s_littlefsOk) {
    Serial.printf("Aviso: fila offline indisponivel (LittleFS).\n");
  } else if (pendingQueueHasPending()) {
    if (ensureWiFi()) {
      int n = pendingQueueFlush(PENDING_FLUSH_MAX_PER_WAKE, sendPayloadWithRetriesForQueue);
      if (n > 0) {
        Serial.printf("Fila: enviados %d registro(s) pendente(s).\n", n);
      }
    } else {
      Serial.printf("Fila: WiFi indisponivel; pendencias permanecem na flash.\n");
      wifiOffIfConnected();
    }
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
