#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>

#include "config.h"

#define ADC_MAX 4095  // ESP32 ADC 12-bit

Adafruit_BME280 bme;

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

int sendData(const String& payload) {
  HTTPClient http;
  String url = String(API_BASE_URL) + "/dados";
  http.begin(url);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " API_TOKEN);

  int code = http.POST(payload);
  if (code != 201) {
    Serial.printf("Erro HTTP %d: %s\n", code, http.getString().c_str());
  }
  http.end();
  return code;
}

void runCycle() {
  bme.takeForcedMeasurement();
  float temperature = bme.readTemperature();
  float humidity = bme.readHumidity();
  float pressureLocal = bme.readPressure() / 100.0F;

  if (isnan(temperature) || isnan(humidity) || isnan(pressureLocal)) {
    Serial.printf("Leitura invalida do BME280 (NaN).\n");
    return;
  }

  float pressureSeaLevel = pressureLocal / pow(1.0 - (ALTITUDE_LOCAL / 44330.0), 5.255);

  int rainRaw = analogRead(RAIN_SENSOR_PIN);
  float precipitacao = (float)(ADC_MAX - rainRaw);  // Invertido: maior = mais chuva

  Serial.printf("Temperatura: %.2f C | Umidade: %.2f%% | Pressao: %.2f hPa | Chuva: %.1f%%\n",
               temperature, humidity, pressureSeaLevel, precipitacao);

  if (!connectWiFi()) {
    Serial.printf("Falha ao conectar WiFi.\n");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    return;
  }
  Serial.printf("WiFi conectado.\n");

  JsonDocument doc;
  doc["temperatura"] = round(temperature * 100) / 100.0;
  doc["umidade"] = round(humidity * 100) / 100.0;
  doc["pressao"] = round(pressureSeaLevel * 100) / 100.0;
  doc["precipitacao"] = round(precipitacao * 100) / 100.0;

  String payload;
  serializeJson(doc, payload);

  bool success = false;
  for (int attempt = 1; attempt <= HTTP_MAX_RETRIES; attempt++) {
    Serial.printf("Envio tentativa %d/%d...\n", attempt, HTTP_MAX_RETRIES);
    if (sendData(payload) == 201) {
      Serial.printf("Dados enviados com sucesso.\n");
      success = true;
      break;
    }
    if (attempt < HTTP_MAX_RETRIES) {
      delay(2000);
    }
  }

  if (!success) {
    Serial.printf("Falha ao enviar dados apos todas as tentativas.\n");
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  // Aguarda até 10 segundos para o host USB conectar (evita perder primeiras mensagens)
  for (int i = 0; i < 100 && !Serial; i++) {
    delay(100);
  }
  if (Serial) delay(100);  // buffer do host pronto
  Serial.printf("Acordando...\n");

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

  runCycle();

  enterDeepSleep();
}

void loop() {
}
