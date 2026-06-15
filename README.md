# Firmware Monitor Ambiental (Captura)

Firmware embarcado para **ESP32-S3 DevKitC-1** que coleta dados ambientais, monitora energia (INA219), mede umidade do solo e controla duas zonas de irrigação. Opera em modo deep sleep ou modo sempre ativo; em ambos envia **uma medição por ciclo** à API. Calibração, temporização, amostragem, NTP, fila offline e polaridade do relé são configuráveis no painel web (`GET/PUT /device/config`).

Diagrama de pinos: [`esp32s3_pinout.svg`](esp32s3_pinout.svg).

## Hardware

| Componente | Função | Barramento / pino (padrão em `hardware_defaults.h`) |
|------------|--------|-----------------------------------------------------|
| BME280 | Pressão (redução barométrica no dispositivo) | I2C `0x76`, SDA GPIO 8, SCL GPIO 9 |
| SHT31 | Temperatura e umidade | I2C `0x44`, mesmo barramento |
| INA219 painel | Tensão, corrente e potência do painel solar | I2C `0x40`, SDA GPIO 15, SCL GPIO 16 |
| INA219 sistema | Tensão, corrente e potência do barramento | I2C `0x41`, mesmo barramento INA |
| Sensor capacitivo solo 1 | Umidade do solo zona 1 | ADC (ver `hardware_defaults.h`) |
| Sensor capacitivo solo 2 | Umidade do solo zona 2 | ADC (ver `hardware_defaults.h`) |
| Relé bomba 1 | Irrigação zona 1 | GPIO 35 |
| Relé bomba 2 | Irrigação zona 2 | GPIO 36 |

> **ADC e Wi-Fi:** leituras de solo ocorrem **antes** de ligar o Wi-Fi. INA219 (painel e sistema) é lido **com Wi-Fi ativo**, para que `corrente_sistema`/`potencia_sistema` incluam o consumo do rádio. Durante irrigação manual com Wi-Fi ativo, o firmware continua amostrando a cada intervalo configurável no painel.

> **Bombas:** as duas nunca ficam energizadas ao mesmo tempo — `runPump()` desliga o outro relé antes de acionar.

Pinos e endereços I2C ficam em `include/hardware_defaults.h`. Wi-Fi e API em `include/config.h`.

## Requisitos

- [PlatformIO](https://platformio.org/) — CLI ou extensão VS Code

## Configuração

```bash
cp include/config.example.h include/config.h
```

Edite `include/config.h` (somente rede e API):

| Constante | Descrição |
|-----------|-----------|
| `WIFI_SSID`, `WIFI_PASSWORD` | Rede Wi-Fi |
| `API_BASE_URL` | URL base da API (ex.: `https://tempo.exemplo.com/api`) |
| `API_TOKEN` | Deve coincidir com `API_TOKEN` da API |

Demais parâmetros operacionais são geridos no painel web em **Dispositivo → Configurações** e sincronizados via `GET /device/config`:

| Campo | Descrição |
|-------|-----------|
| `soil*_dry_mv` / `soil*_wet_mv` | Calibração ADC do solo por zona |
| `altitude_local` | Altitude para correção barométrica |
| `manual_irrigation_max_s` | Teto de duração da irrigação manual |
| `pump_sample_interval_s` / `pump_delay_chunk_ms` | Amostragem durante irrigação |
| `deep_sleep_enabled` / `deep_sleep_seconds` / `capture_interval_seconds` | Modo e intervalos de ciclo |
| `http_*` / `wifi_timeout_ms` / `cold_boot_usb_wait_ms` | Timeouts de rede |
| `ntp_*` | Servidores e offsets NTP |
| `pending_*` | Limites da fila offline (LittleFS) |
| `panel_voltage_noise_floor_v` | Limiar de ruído do painel solar (V); `0` desativa |
| `sensor_average_rounds` / `adc_samples` | Média ambiental e amostras ADC do solo |
| `ina_average_rounds` / `ina_sample_delay_ms` | Média INA219 por ciclo |
| `relay_active_high` | Relé ativo em nível alto (`true`) ou baixo (`false`) |

Pinos de hardware: edite `include/hardware_defaults.h` se necessário.

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

Campos opcionais (graceful degradation — sensores com falha no ciclo são omitidos):

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
| `GET /irrigation/manual/pending` | Todo ciclo (após leituras ambientais) |
| `POST /irrigation/manual/:id/start` | Antes de ligar a bomba |
| `POST /irrigation/manual/:id/ack` | Após execução |
| `GET /irrigation/config` | A cada captura (antes da irrigação automática) |

Comandos manuais ignoram o flag `active` da zona. Irrigação automática respeita limiar, histerese e `active` da config na API.

## Comportamento do ciclo

O firmware opera em um de dois modos, definidos por `deep_sleep_enabled` no painel web:

### Modo deep sleep (`deep_sleep_enabled=true`, padrão)

Indicado para operação em campo com bateria/painel solar — economiza energia dormindo entre ciclos.

1. **Acorda** do deep sleep (`deep_sleep_seconds`, padrão 60 s).
2. **Drena fila offline** (LittleFS NDJSON) se houver pendências e Wi-Fi disponível.
3. **Lê sensores ambientais** (solo, BME280, SHT31) sem Wi-Fi — média de `sensor_average_rounds` rodadas por wake (solo usa `adc_samples` amostras ADC por rodada).
4. **Verifica irrigação manual** — liga Wi-Fi, consulta `/irrigation/manual/pending`, executa bombas se necessário, confirma com ack.
   - **Durante a irrigação**: a cada `pump_sample_interval_s` e na amostra pós-bomba, enfileira leitura instantânea via `POST /dados`. Falhas vão para a fila LittleFS.
5. **Lê INA219** (painel e sistema) com Wi-Fi ativo — média de `ina_average_rounds` amostras.
6. **Aplica irrigação automática** e **envia** o snapshot via `POST /dados` (até 3 tentativas). Em falha, persiste na fila LittleFS.
7. **Deep sleep** e repete.

### Modo ativo (`deep_sleep_enabled=false`)

Indicado para bancada, debug ou alimentação contínua (USB/rede). O ESP32 permanece ligado e consome muito mais energia.

1. **Boot único** — inicializa sensores, fila offline e NVS.
2. A cada `capture_interval_seconds`, executa o **mesmo fluxo de captura** do modo deep sleep (passos 3–6 acima).
3. Tenta drenar a fila offline antes de cada captura, se houver pendências.

NTP sincroniza relógio; `created_at` nos payloads é enviado em **UTC com sufixo `Z`** (ex.: `2025-06-10T17:30:00Z`). Reenvios offline preservam o timestamp original na fila.
