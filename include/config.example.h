#ifndef CONFIG_EXAMPLE_H
#define CONFIG_EXAMPLE_H

// Copie este arquivo para config.h e preencha com suas credenciais.
// Nunca commite config.h: o repositorio inclui include/config.h no .gitignore.

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

// --- Sensores I2C (BME280 + SHT31) ---
#define I2C_SDA_PIN        17
#define I2C_SCL_PIN        18
#define BME280_I2C_ADDR    0x76
#define SHT31_I2C_ADDR     0x44

// --- Medições de tensão (ADC2 - ler ANTES de ligar o WiFi) ---
// Bateria: divisor 100k + 100k → fator 2.0 (V_real = V_pin * 2)
#define BAT_ADC_PIN        13
#define BAT_DIVIDER_RATIO  2.0f
// Painel solar 6V: divisor 330k + 330k → fator 2.0
#define SOLAR_ADC_PIN      12
#define SOLAR_DIVIDER_RATIO 2.0f
// Amostras de ADC por wake (média) para reduzir ruído
#define ADC_SAMPLES        16

// --- Pressão atmosférica ---
#define ALTITUDE_LOCAL     1000  // Altitude em metros para correcao barometrica

// --- Temporização ---
// Intervalo entre acordares (deep sleep). Ex.: 60 = 1 min por ciclo de sono.
#define DEEP_SLEEP_SECONDS          60
// Amostras RTC agregadas antes de enviar à API (tempo total ~ DEEP_SLEEP_SECONDS * este valor).
#define SAMPLES_PER_API_UPLOAD      10
#define HTTP_TIMEOUT_MS     10000
#define HTTP_MAX_RETRIES    3
#define WIFI_TIMEOUT_MS     20000
// Tempo maximo de espera pelo host USB-CDC em boot a frio (ms). 0 desabilita.
#define COLD_BOOT_USB_WAIT_MS       2000

// --- Fila offline (LittleFS) ---
// Maximo de POSTs por acordar ao drenar a fila (evita WiFi ligado demais).
#define PENDING_FLUSH_MAX_PER_WAKE  10
// Teto de tamanho do arquivo NDJSON; ao estourar, remove linhas mais antigas.
#define PENDING_MAX_BYTES           65536
// Limite extra por numero de linhas (cada janela = 1 linha).
#define PENDING_MAX_LINES           500

// Descomente para habilitar prints no Serial (debug)
// #define DEBUG_MODE

#endif
