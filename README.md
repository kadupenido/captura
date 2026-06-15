# Firmware Monitor Ambiental (Captura)

Firmware embarcado para **ESP32-S3 DevKitC-1** que coleta dados ambientais, monitora energia (INA219), mede umidade do solo e controla duas zonas de irrigação. Opera em modo deep sleep (médias agregadas) ou modo sempre ativo (envio imediato a cada captura). Calibração, temporização, NTP e fila offline são configuráveis no painel web (`GET/PUT /device/config`).

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

> **ADC e Wi-Fi:** leituras de solo e sensores I2C ocorrem no início de cada ciclo, **antes** de ligar o Wi-Fi. Durante irrigação manual com Wi-Fi ativo, o firmware continua amostrando solo e INA a cada intervalo configurável no painel.

> **Bombas:** as duas nunca ficam energizadas ao mesmo tempo — `runPump()` desliga o outro relé antes de acionar.

Pinos e endereços I2C ficam em `include/hardware_defaults.h` (fixos no firmware). Wi-Fi e API em `include/config.h`.

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

Demais parâmetros (calibração solo, temporização, NTP, fila offline) são geridos no painel web em **Dispositivo → Configurações** e sincronizados via `GET /device/config`. Pinos de hardware: edite `include/hardware_defaults.h` se necessário.

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
| `GET /irrigation/config` | Modo deep sleep: no fim da janela de upload; modo ativo: a cada captura |

Comandos manuais ignoram o flag `active` da zona. Irrigação automática respeita limiar, histerese e `active` da config na API.

## Comportamento do ciclo

O firmware opera em um de dois modos, definidos por `DEEP_SLEEP_ENABLED` em `config.h`:

### Modo deep sleep (`DEEP_SLEEP_ENABLED=1`, padrão)

Indicado para operação em campo com bateria/painel solar — economiza energia dormindo entre ciclos.

1. **Acorda** do deep sleep (`DEEP_SLEEP_SECONDS`, padrão 60 s).
2. **Drena fila offline** (LittleFS NDJSON) se houver pendências e Wi-Fi disponível — até `PENDING_BATCH_MAX_ITEMS` registros por acordar num único `POST /dados/lote`.
3. **Lê sensores** (solo, BME280, SHT31, INA219) sem Wi-Fi.
4. **Verifica irrigação manual** — liga Wi-Fi, consulta `/irrigation/manual/pending`, executa bombas se necessário, confirma com ack.
   - **Durante a irrigação** (manual ou automática): a cada `PUMP_SAMPLE_INTERVAL_S` e na amostra pós-bomba, envia leitura instantânea via `POST /dados` (sem esperar a janela de upload). Falhas vão para a fila LittleFS.
5. Se ainda não completou a janela (`SAMPLES_PER_API_UPLOAD`), **volta a dormir**.
6. Na janela completa:
   - Calcula médias por sensor com amostras válidas.
   - Conecta Wi-Fi, aplica irrigação automática conforme config da API.
   - Envia médias via `POST /dados` (até 3 tentativas).
   - Em falha de rede ou upload, persiste JSON na fila LittleFS para retry.
7. **Deep sleep** e repete.

### Modo ativo (`DEEP_SLEEP_ENABLED=0`)

Indicado para bancada, debug ou alimentação contínua (USB/rede). O ESP32 permanece ligado e consome muito mais energia.

1. **Boot único** — inicializa sensores, fila offline e NVS.
2. A cada `CAPTURE_INTERVAL_SECONDS`:
   - **Lê sensores** sem Wi-Fi.
   - **Verifica irrigação manual** (mesmo fluxo do modo deep sleep).
   - **Aplica irrigação automática** e **envia cada leitura imediatamente** via `POST /dados` (sem agregação por janela).
   - Tenta drenar a fila offline antes de cada captura, se houver pendências.
3. **Aguarda** `CAPTURE_INTERVAL_SECONDS` e repete.

NTP sincroniza relógio; `created_at` nos payloads é enviado em **UTC com sufixo `Z`** (ex.: `2025-06-10T17:30:00Z`). Reenvios offline preservam o timestamp original na fila. Ver `pending_queue.cpp` e constantes `PENDING_*` em `config.h`.
