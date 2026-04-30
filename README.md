# Firmware Monitor Ambiental (Captura)

Firmware embarcado para ESP32-S3 que coleta dados ambientais (temperatura, umidade, pressão) por dois sensores I2C (BME280 + SHT31), além de monitorar a tensão da bateria e do painel solar via dois divisores resistivos. Envia tudo para a API do Monitor Ambiental.

## Hardware necessário

- **ESP32-S3 DevKitC-1**
- **Sensor BME280** (temperatura, umidade, pressão) — I2C, endereço `0x76`
- **Sensor SHT31** (temperatura, umidade) — I2C, endereço `0x44`
- **Divisor de tensão da bateria** — 2× 100 kΩ ligados entre `+V_bat` e GND, ponto médio na GPIO 13
- **Divisor de tensão do painel solar (6 V)** — 2× 330 kΩ ligados entre `+V_painel` e GND, ponto médio na GPIO 12

## Esquema de fiação (pinos padrão)

| Componente | Pino ESP32-S3 |
|------------|---------------|
| BME280 / SHT31 SDA | GPIO 17 |
| BME280 / SHT31 SCL | GPIO 18 |
| Divisor bateria (100k/100k) → ponto médio | GPIO 13 (ADC2) |
| Divisor painel solar (330k/330k) → ponto médio | GPIO 12 (ADC2) |

> Os dois sensores compartilham o mesmo barramento I2C; cada um responde em seu endereço único (`0x76` BME, `0x44` SHT).
>
> **Atenção:** GPIO 12 e 13 estão no ADC2 do ESP32-S3, que conflita com o Wi-Fi. O firmware lê as tensões **antes** de ligar o Wi-Fi, então não é necessário tratamento adicional do usuário.

Os pinos e endereços são configuráveis em `include/config.h` (`I2C_SDA_PIN`, `I2C_SCL_PIN`, `BME280_I2C_ADDR`, `SHT31_I2C_ADDR`, `BAT_ADC_PIN`, `SOLAR_ADC_PIN`).

## Cálculo das tensões

Com divisores de relação 1:1 (resistores iguais):

```
V_bateria = V_pino_13 × 2.0
V_painel  = V_pino_12 × 2.0
```

A leitura de tensão usa `analogReadMilliVolts()` (ADC já calibrado pela ROM do ESP32-S3) e faz a média de `ADC_SAMPLES` amostras (default 16).

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
- `ALTITUDE_LOCAL` — altitude em metros; a pressão ao nível do mar usa redução barométrica isotérmica com a temperatura medida no ciclo (`P_nm = P_local · exp(g·h/(R·T_K))`)
- `DEEP_SLEEP_SECONDS` — intervalo entre acordes (60 = 1 min)
- `SAMPLES_PER_API_UPLOAD` — amostras por janela antes do envio (10 = envio a cada 10 min)
- `BAT_DIVIDER_RATIO` / `SOLAR_DIVIDER_RATIO` — fator de correção do divisor (2.0 para resistores iguais)

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

**Formato do body JSON (todos os campos são opcionais — graceful degradation):**

```json
{
  "temperatura_bme": 25.5,
  "umidade_bme": 65.2,
  "temperatura_sht": 25.6,
  "umidade_sht": 65.0,
  "pressao": 1013.25,
  "tensao_bateria": 4.05,
  "tensao_painel": 5.83
}
```

Se um sensor falhar durante toda a janela, seus campos correspondentes simplesmente não são incluídos no payload (a API armazena `NULL` para esses campos). Pressão é exclusiva do BME280.

O endpoint e schema esperados estão documentados no projeto da API (Node).

## Comportamento

1. **Acorda** do deep sleep a cada 1 minuto (`DEEP_SLEEP_SECONDS` = 60).
2. **Lê tensões** (bateria e painel solar) **antes** de qualquer Wi-Fi (ADC2).
3. **Lê** temperatura, umidade e pressão do BME280; temperatura e umidade do SHT31. Cada sensor é tratado de forma independente — falha de um não impede a coleta do outro.
4. **Acumula** as leituras na memória RTC (somas e contadores separados por sensor).
5. A cada **10 acordes** (10 min):
   - Calcula médias de cada métrica usando apenas amostras válidas daquele sensor (graceful degradation).
   - Aplica a redução barométrica à pressão usando a temperatura média do BME280.
   - **Conecta** ao Wi-Fi.
   - **Envia** as médias em JSON para a API (até 3 tentativas em caso de falha).
   - Se o envio falhar, persiste o JSON em uma fila NDJSON no LittleFS para retry no próximo wake.
   - Reinicia a janela de acumulação na RTC.
6. **Entra em deep sleep** por 60 s e repete.
