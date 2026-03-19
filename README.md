# Firmware Estação Meteorológica (Captura)

Firmware embarcado para ESP32-S3 que coleta dados meteorológicos (temperatura, umidade, pressão, precipitação) e envia para a API da estação meteorológica.

## Hardware necessário

- **ESP32-S3 DevKitC-1**
- **Sensor BME280** (temperatura, umidade, pressão) — comunicação I2C
- **Sensor de chuva analógico** (resistivo)

## Esquema de fiação (pinos padrão)

| Componente | Pino ESP32-S3 |
|------------|---------------|
| BME280 SDA | GPIO 8 |
| BME280 SCL | GPIO 9 |
| Sensor de chuva | GPIO 7 (analógico ADC) |

Os pinos são configuráveis em `include/config.h` (`SDA_PIN`, `SCL_PIN`, `RAIN_SENSOR_PIN`).

## Requisitos

- [PlatformIO](https://platformio.org/) — CLI ou extensão para VS Code

## Configuração

1. Copie o template de configuração:

```bash
cp include/config.example.h include/config.h
```

2. Edite `include/config.h` com suas credenciais:

- `WIFI_SSID` e `WIFI_PASSWORD` — rede Wi-Fi
- `API_BASE_URL` — URL base da API (ex: `https://tempo.exemplo.com/api`)
- `API_TOKEN` — token de autenticação Bearer (deve corresponder ao token configurado na API)
- `ALTITUDE_LOCAL` — altitude em metros para correção barométrica da pressão

Opcional: descomente e preencha `WIFI_STATIC_IP`, `WIFI_GATEWAY`, `WIFI_SUBNET` para usar IP fixo (reduz ~2–3 s de DHCP).

## Build e upload

```bash
pio run -t upload
```

## Monitor serial

```bash
pio device monitor -b 115200
```

## Integração com a API

O firmware envia dados via `POST {API_BASE_URL}/dados` com o header `Authorization: Bearer {API_TOKEN}`.

**Formato do body JSON:**

```json
{
  "temperatura": 25.5,
  "umidade": 65.2,
  "pressao": 1013.25,
  "precipitacao": 0.0
}
```

O endpoint e schema esperados estão documentados no projeto da API (FastAPI).

## Comportamento

1. **Acorda** do deep sleep
2. **Lê** temperatura, umidade, pressão (BME280) e precipitação (sensor analógico)
3. **Conecta** ao Wi-Fi
4. **Envia** os dados em JSON para a API (até 3 tentativas em caso de falha)
5. **Entra em deep sleep** por `DEEP_SLEEP_SECONDS` (padrão: 600 s = 10 min)
6. Repete o ciclo ao acordar
