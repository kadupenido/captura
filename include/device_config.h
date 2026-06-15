#ifndef DEVICE_CONFIG_H
#define DEVICE_CONFIG_H

#include <Arduino.h>

struct DeviceConfig {
  float soil1_dry_mv = 2600.0f;
  float soil1_wet_mv = 1200.0f;
  float soil2_dry_mv = 2600.0f;
  float soil2_wet_mv = 1200.0f;
  int altitude_local = 1000;
  int manual_irrigation_max_s = 600;
  int pump_sample_interval_s = 10;
  bool deep_sleep_enabled = true;
  int capture_interval_seconds = 30;
  int deep_sleep_seconds = 60;
  int http_timeout_ms = 10000;
  int http_max_retries = 3;
  int wifi_timeout_ms = 20000;
  int cold_boot_usb_wait_ms = 2000;
  char ntp_server_primary[48] = "pool.ntp.org";
  char ntp_server_secondary[48] = "time.google.com";
  int ntp_sync_wait_ms = 3500;
  int ntp_min_valid_year = 2024;
  int ntp_gmt_offset_sec = -10800;
  int ntp_daylight_offset_sec = 0;
  int pending_batch_max_items = 20;
  int pending_batch_max_bytes = 16384;
  int pending_max_bytes = 262144;
  int pending_max_lines = 800;
  float panel_voltage_noise_floor_v = 1.0f;
  int sensor_average_rounds = 3;
  int adc_samples = 16;
  int ina_average_rounds = 5;
  int ina_sample_delay_ms = 50;
  int pump_delay_chunk_ms = 500;
  int http_retry_delay_ms = 2000;
  bool relay_active_high = true;
  char updated_at[32] = "";
};

const DeviceConfig& deviceConfig();
void deviceConfigApplyDefaults();
bool deviceConfigLoadNvs();
bool deviceConfigSyncFromApi();

#endif
