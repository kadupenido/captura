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
- `DEEP_SLEEP_SECONDS` — intervalo entre acordes (60 = 1 min)
- `SAMPLES_PER_API_UPLOAD` — amostras por janela antes do envio (10 = envio a cada 10 min)

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

1. **Acorda** do deep sleep a cada 1 minuto (`DEEP_SLEEP_SECONDS` = 60)
2. **Lê** temperatura, umidade, pressão (BME280) e precipitação (sensor analógico)
3. **Acumula** as leituras na memória RTC (somando e contando amostras)
4. A cada **10 acordes** (10 min):
   - Calcula médias de temperatura, umidade e pressão
   - Para precipitação: se todas as 10 leituras do sensor de chuva forem iguais, envia 0 (evita baseline estável em tempo seco); senão envia a média
   - **Conecta** ao Wi-Fi
   - **Envia** as médias em JSON para a API (até 3 tentativas em caso de falha)
   - Reinicia a janela de acumulação na RTC
5. **Entra em deep sleep** por 60 s e repete
