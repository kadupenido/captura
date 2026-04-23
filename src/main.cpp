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

#define ADC_MAX 4095  // ESP32 ADC 12-bit
#define RTC_MAGIC 0x4D455448  // "METH" meteo

// Reducao barometrica isotermica: P_nm = P_local * exp(g*h/(R*T_K)), T_K = temperatura medida.
static constexpr float kGravity = 9.80665f;
static constexpr float kGasConstantDryAir = 287.05f;

RTC_DATA_ATTR uint32_t rtc_magic;
RTC_DATA_ATTR float rtc_sum_temp;
RTC_DATA_ATTR float rtc_sum_hum;
RTC_DATA_ATTR float rtc_sum_press;
RTC_DATA_ATTR float rtc_sum_precip;
RTC_DATA_ATTR uint8_t rtc_sample_count;
RTC_DATA_ATTR int16_t rtc_rain_first_raw;
RTC_DATA_ATTR bool rtc_rain_all_equal;

Adafruit_BME280 bme;

static bool s_littlefsOk = false;

void resetRtcWindow() {
  rtc_magic = RTC_MAGIC;
  rtc_sum_temp = 0.0F;
  rtc_sum_hum = 0.0F;
  rtc_sum_press = 0.0F;
  rtc_sum_precip = 0.0F;
  rtc_sample_count = 0;
  rtc_rain_first_raw = -1;
  rtc_rain_all_equal = true;
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

void runCycle() {
  bme.takeForcedMeasurement();
  float temperature = bme.readTemperature();
  float humidity = bme.readHumidity();
  float pressureLocal = bme.readPressure() / 100.0F;

  if (isnan(temperature) || isnan(humidity) || isnan(pressureLocal)) {
    Serial.printf("Leitura invalida do BME280 (NaN).\n");
    wifiOffIfConnected();
    return;
  }

  float tKelvin = temperature + 273.15f;
  float pressureSeaLevel =
      pressureLocal * expf((kGravity * (float)ALTITUDE_LOCAL) / (kGasConstantDryAir * tKelvin));

  int rainRaw = analogRead(RAIN_SENSOR_PIN);
  float precipSample = (float)(ADC_MAX - rainRaw);  // Invertido: maior = mais chuva

  if (rtc_rain_first_raw < 0) {
    rtc_rain_first_raw = (int16_t)rainRaw;
  } else if (rainRaw != rtc_rain_first_raw) {
    rtc_rain_all_equal = false;
  }

  rtc_sum_temp += temperature;
  rtc_sum_hum += humidity;
  rtc_sum_press += pressureSeaLevel;
  rtc_sum_precip += precipSample;
  rtc_sample_count++;

  Serial.printf("[%d/%d] T:%.2f U:%.2f P:%.2f Chuva:%d\n",
                rtc_sample_count, (int)SAMPLES_PER_API_UPLOAD,
                temperature, humidity, pressureSeaLevel, rainRaw);

  if (rtc_sample_count < SAMPLES_PER_API_UPLOAD) {
    float avgT = rtc_sum_temp / rtc_sample_count;
    Serial.printf("Media parcial T:%.2f U:%.2f P:%.2f\n", avgT,
                  rtc_sum_hum / rtc_sample_count, rtc_sum_press / rtc_sample_count);
    wifiOffIfConnected();
    return;
  }

  float avgTemp = rtc_sum_temp / SAMPLES_PER_API_UPLOAD;
  float avgHum = rtc_sum_hum / SAMPLES_PER_API_UPLOAD;
  float avgPress = rtc_sum_press / SAMPLES_PER_API_UPLOAD;
  float precipitacao = rtc_rain_all_equal ? 0.0F : (rtc_sum_precip / SAMPLES_PER_API_UPLOAD);

  Serial.printf("Medias finais: T:%.2f U:%.2f P:%.2f Precip:%.2f (chuva_igual=%d)\n",
                avgTemp, avgHum, avgPress, precipitacao, rtc_rain_all_equal);

  JsonDocument doc;
  doc["temperatura"] = round(avgTemp * 100) / 100.0;
  doc["umidade"] = round(avgHum * 100) / 100.0;
  doc["pressao"] = round(avgPress * 100) / 100.0;
  doc["precipitacao"] = round(precipitacao * 100) / 100.0;

  char payload[192];
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

  // Faixa ~0–3,3 V no ADC; ajuste se o divisor do sensor de chuva for diferente.
  analogSetPinAttenuation(RAIN_SENSOR_PIN, ADC_11db);

  s_littlefsOk = pendingQueueInit();

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!bme.begin(0x76, &Wire)) {
    Serial.printf("BME280 nao encontrado! Verifique a fiação.\n");
    enterDeepSleep();
    return;
  }

  bme.setSampling(Adafruit_BME280::MODE_FORCED,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::SAMPLING_X1,
                  Adafruit_BME280::FILTER_OFF,
                  Adafruit_BME280::STANDBY_MS_0_5);

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

  runCycle();

  enterDeepSleep();
}

void loop() {
}
