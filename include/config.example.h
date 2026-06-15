#ifndef CONFIG_EXAMPLE_H
#define CONFIG_EXAMPLE_H

// Copie este arquivo para config.h e preencha com suas credenciais.
// Nunca commite config.h: o repositorio inclui include/config.h no .gitignore.
//
// Demais configuracoes (calibracao, temporizacao, NTP, fila offline) sao
// geridas pelo painel web e sincronizadas pelo firmware via GET /device/config.
// Pinos de hardware ficam em hardware_defaults.h.

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

// Leituras de tensao do painel abaixo deste valor (V) sao tratadas como ruido e enviadas como 0.
#define PAINEL_VOLTAGE_NOISE_FLOOR_V   1.0f

// Descomente para habilitar prints no Serial (debug)
// #define DEBUG_MODE

#endif
