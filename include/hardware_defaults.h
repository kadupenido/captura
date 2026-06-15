#ifndef HARDWARE_DEFAULTS_H
#define HARDWARE_DEFAULTS_H

// Pinos e enderecos fixos no firmware (nao editaveis pelo painel web).

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

// --- Irrigacao (solo + bombas) ---
#define SOIL_ADC_PIN_1      1
#define SOIL_ADC_PIN_2      2
#define RELAY_PIN_1         35
#define RELAY_PIN_2         36
// 1 = rele ativo em nivel alto; 0 = ativo em nivel baixo.
#define RELAY_ACTIVE_HIGH   1

#endif
