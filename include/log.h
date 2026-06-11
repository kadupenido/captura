#ifndef LOG_H
#define LOG_H

// Log no Serial com prefixo de hora (RTC local) ou uptime antes do NTP.
void logPrintf(const char* fmt, ...);

#endif
