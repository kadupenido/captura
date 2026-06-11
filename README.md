# Firmware Monitor Ambiental (Captura)

Firmware embarcado para **ESP32-S3 DevKitC-1** que coleta dados ambientais, monitora energia (INA219), mede umidade do solo e controla duas zonas de irrigação. Envia médias agregadas para a API e consulta comandos manuais de irrigação em todo acordar.

Diagrama de pinos: [`esp32s3_pinout.svg`](esp32s3_pinout.svg).

## Hardware

| Componente | Função | Barramento / pino (padrão em `config.example.h`) |
|------------|--------|-----------------------------------------------------|
| BME280 | Pressão (redução barométrica no dispositivo) | I2C `0x76`, SDA GPIO 8, SCL GPIO 9 |
| SHT31 | Temperatura e umidade | I2C `0x44`, mesmo barramento |
| INA219 painel | Tensão, corrente e potência do painel solar | I2C `0x40`, SDA GPIO 15, SCL GPIO 16 |
| INA219 sistema | Tensão, corrente e potência do barramento | I2C `0x41`, mesmo barramento INA |
| Sensor capacitivo solo 1 | Umidade do solo zona 1 | ADC GPIO 37 |
| Sensor capacitivo solo 2 | Umidade do solo zona 2 | ADC GPIO 38 |
| Relé bomba 1 | Irrigação zona 1 | GPIO 35 |
| Relé bomba 2 | Irrigação zona 2 | GPIO 36 |

> **ADC e Wi-Fi:** leituras de solo e sensores I2C ocorrem no início de cada ciclo, **antes** de ligar o Wi-Fi. Durante irrigação manual com Wi-Fi ativo, o firmware continua amostrando solo e INA a cada `PUMP_SAMPLE_INTERVAL_S`.

> **Bombas:** as duas nunca ficam energizadas ao mesmo tempo — `runPump()` desliga o outro relé antes de acionar.

Pinos, endereços I2C e calibração do solo são configuráveis em `include/config.h` (copiar de `include/config.example.h`).

## Requisitos

- [PlatformIO](https://platformio.org/) — CLI ou extensão VS Code

## Configuração

```bash
cp include/config.example.h include/config.h
```

Edite `include/config.h`:

| Constante | Descrição |
|-----------|-----------|
| `WIFI_SSID`, `WIFI_PASSWORD` | Rede Wi-Fi |
| `API_BASE_URL` | URL base da API (ex.: `https://tempo.exemplo.com/api`) |
| `API_TOKEN` | Deve coincidir com `API_TOKEN` da API |
| `ALTITUDE_LOCAL` | Altitude (m) para pressão ao nível do mar |
| `SOIL*_DRY_MV` / `SOIL*_WET_MV` | Calibração dos sensores de solo (mV → %) |
| `DEEP_SLEEP_SECONDS` | Intervalo entre acordares (60 = 1 min) |
| `SAMPLES_PER_API_UPLOAD` | Amostras por janela antes do envio (10 ≈ 10 min) |
| `MANUAL_IRRIGATION_MAX_S` | Teto de segurança por comando manual (s) |
| `RELAY_ACTIVE_HIGH` | Polaridade do relé (1 = ativo em HIGH) |

Opcional: `WIFI_STATIC_IP`, `WIFI_GATEWAY`, `WIFI_SUBNET` para IP fixo.

## Build e upload

```bash
pio run              # compilar
pio run -t upload    # gravar
pio device monitor -b 115200
```

## Integração com a API

### Medições — `POST {API_BASE_URL}/dados`

Header: `Authorization: Bearer {API_TOKEN}`.

Campos opcionais (graceful degradation — sensores com falha na janela são omitidos):

```json
{
  "temperatura": 25.6,
  "umidade": 65.0,
  "pressao": 1013.25,
  "umidade_solo_1": 42.0,
  "umidade_solo_2": 38.5,
  "tensao_painel": 5.83,
  "corrente_painel": 0.12,
  "potencia_painel": 0.70,
  "tensao_sistema": 4.05,
  "corrente_sistema": 0.05,
  "potencia_sistema": 0.20,
  "tempo_irrigacao_s_1": 10,
  "tempo_irrigacao_s_2": 0
}
```

`temperatura`/`umidade` vêm do SHT31; `pressao` é reduzida ao nível do mar a partir do BME280.

### Irrigação

| Endpoint | Quando |
|----------|--------|
| `GET /irrigation/manual/pending` | Todo acordar (após leituras de sensores) |
| `POST /irrigation/manual/:id/start` | Antes de ligar a bomba |
| `POST /irrigation/manual/:id/ack` | Após execução |
| `GET /irrigation/config` | No fim da janela de upload (irrigação automática) |

Comandos manuais ignoram o flag `active` da zona. Irrigação automática respeita limiar, histerese e `active` da config na API.

## Comportamento do ciclo

1. **Acorda** do deep sleep (`DEEP_SLEEP_SECONDS`, padrão 60 s).
2. **Drena fila offline** (LittleFS NDJSON) se houver pendências e Wi-Fi disponível.
3. **Lê sensores** (solo, BME280, SHT31, INA219) sem Wi-Fi.
4. **Verifica irrigação manual** — liga Wi-Fi, consulta `/irrigation/manual/pending`, executa bombas se necessário, confirma com ack.
5. Se ainda não completou a janela (`SAMPLES_PER_API_UPLOAD`), **volta a dormir**.
6. Na janela completa:
   - Calcula médias por sensor com amostras válidas.
   - Conecta Wi-Fi, aplica irrigação automática conforme config da API.
   - Envia médias via `POST /dados` (até 3 tentativas).
   - Em falha de rede ou upload, persiste JSON na fila LittleFS para retry.
7. **Deep sleep** e repete.

NTP sincroniza relógio para `created_at` em reenvios offline. Ver `pending_queue.cpp` e constantes `PENDING_*` em `config.h`.
