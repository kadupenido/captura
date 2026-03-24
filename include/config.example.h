#ifndef CONFIG_EXAMPLE_H
#define CONFIG_EXAMPLE_H

// Copie este arquivo para config.h e preencha com suas credenciais.

// --- Rede ---
#define WIFI_SSID      "sua_rede_wifi"
#define WIFI_PASSWORD  "sua_senha_wifi"

// Descomente e preencha para usar IP fixo (economiza ~2-3s de DHCP)
// #define WIFI_STATIC_IP      "192.168.1.50"
// #define WIFI_GATEWAY        "192.168.1.1"
// #define WIFI_SUBNET         "255.255.255.0"

// --- API ---
#define API_BASE_URL   "http://192.168.1.100:8000"
#define API_TOKEN      "seu_token_secreto_aqui"

// --- Sensores ---
#define SDA_PIN            8
#define SCL_PIN            9
#define RAIN_SENSOR_PIN    7
#define ALTITUDE_LOCAL     1000  // Altitude em metros para correcao barometrica

// --- Temporização ---
#define DEEP_SLEEP_SECONDS          60    // Intervalo de acordar (segundos). 60 = 1 min
#define SAMPLES_PER_API_UPLOAD      10   // Amostras por janela antes do envio à API (60s × 10 = 10 min)
#define HTTP_TIMEOUT_MS     10000
#define HTTP_MAX_RETRIES    3
#define WIFI_TIMEOUT_MS     20000

// Descomente para habilitar prints no Serial (debug)
// #define DEBUG_MODE

#endif
