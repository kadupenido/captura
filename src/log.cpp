#include "log.h"

#include <Arduino.h>
#include <cstdarg>
#include <cstdio>
#include <ctime>

#include "config.h"

#ifndef NTP_MIN_VALID_YEAR
#define NTP_MIN_VALID_YEAR 2024
#endif

static bool logClockSynced() {
  struct tm t {};
  if (!getLocalTime(&t, 50)) {
    return false;
  }
  return (t.tm_year + 1900) >= NTP_MIN_VALID_YEAR;
}

static void formatLogPrefix(char* buf, size_t len) {
  if (logClockSynced()) {
    struct tm t {};
    if (getLocalTime(&t, 0) && strftime(buf, len, "[%Y-%m-%d %H:%M:%S] ", &t) > 0) {
      return;
    }
  }
  snprintf(buf, len, "[+%lus] ", static_cast<unsigned long>(millis() / 1000UL));
}

void logPrintf(const char* fmt, ...) {
  char prefix[24];
  formatLogPrefix(prefix, sizeof(prefix));
  Serial.print(prefix);

  char body[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(body, sizeof(body), fmt, args);
  va_end(args);
  Serial.print(body);
}
