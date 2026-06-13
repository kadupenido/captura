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
#define I2C_SDA_PIN        8
#define I2C_SCL_PIN        9
#define BME280_I2C_ADDR    0x76
#define SHT31_I2C_ADDR     0x44

// --- INA219 (tensao/corrente/potencia) ---
#define INA_SDA_PIN           15
#define INA_SCL_PIN           16
#define INA219_PAINEL_ADDR    0x40
#define INA219_SISTEMA_ADDR   0x41

// --- Irrigação (solo + bombas) ---
#define SOIL_ADC_PIN_1      37
#define SOIL_ADC_PIN_2      38
#define RELAY_PIN_1         35
#define RELAY_PIN_2         36
// 1 = relé ativo em nível alto; 0 = ativo em nível baixo.
#define RELAY_ACTIVE_HIGH   1
// Calibração da umidade do solo (mV). Ajuste conforme os sensores reais.
// Fórmula usa interpolação linear: 0% em DRY_MV e 100% em WET_MV.
#define SOIL1_DRY_MV        2600.0f
#define SOIL1_WET_MV        1200.0f
#define SOIL2_DRY_MV        2600.0f
#define SOIL2_WET_MV        1200.0f
// Teto de seguranca por comando de irrigacao manual (s).
#define MANUAL_IRRIGATION_MAX_S   600
// Intervalo de leitura dos sensores enquanto a bomba esta ligada (s).
#define PUMP_SAMPLE_INTERVAL_S    10

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

// --- NTP (horario local para logs e created_at / fila offline) ---
#define NTP_SERVER_PRIMARY        "pool.ntp.org"
#define NTP_SERVER_SECONDARY      "time.google.com"
#define NTP_SYNC_WAIT_MS          3500
#define NTP_MIN_VALID_YEAR        2024
// Offset NTP (Brasilia = -10800 s; sem horario de verao).
#define NTP_GMT_OFFSET_SEC        (-3 * 3600)
#define NTP_DAYLIGHT_OFFSET_SEC   0

// --- Fila offline (LittleFS) ---
// Maximo de registros por lote ao drenar a fila (1 POST /dados/lote por acordar).
#define PENDING_BATCH_MAX_ITEMS     20
// Teto do corpo HTTP do lote (~20 x 768 B por registro).
#define PENDING_BATCH_MAX_BYTES     16384
// Teto de tamanho do arquivo NDJSON; ao estourar, remove linhas mais antigas.
// Dimensionado para ~5 dias de backlog com DEEP_SLEEP_SECONDS=120 e
// SAMPLES_PER_API_UPLOAD=5 (1 linha a cada 10 min => 720 linhas em 5 dias).
#define PENDING_MAX_BYTES           262144
// Limite extra por numero de linhas (cada janela = 1 linha).
#define PENDING_MAX_LINES           800

// Descomente para habilitar prints no Serial (debug)
// #define DEBUG_MODE

#endif
