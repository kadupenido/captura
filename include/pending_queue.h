#ifndef PENDING_QUEUE_H
#define PENDING_QUEUE_H

#include <Arduino.h>

// Retorna codigo HTTP (201 = sucesso para esta API).
using PendingSendFn = int (*)(const String& payload);

bool pendingQueueInit();
bool pendingQueueHasPending();
bool pendingQueueAppend(const String& jsonLine);
bool pendingQueueAppend(const char* jsonLine);
// Envia ate maxItems linhas (FIFO). WiFi deve estar conectado pelo chamador.
int pendingQueueFlush(int maxItems, PendingSendFn send);

#endif
