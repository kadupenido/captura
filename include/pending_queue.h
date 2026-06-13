#ifndef PENDING_QUEUE_H
#define PENDING_QUEUE_H

#include <Arduino.h>

// Retorna codigo HTTP (201 = sucesso para esta API).
using PendingSendFn = int (*)(const String& payload);
using PendingBatchSendFn = int (*)(const char* payload, size_t payloadLen);

bool pendingQueueInit();
bool pendingQueueHasPending();
// Numero atual de linhas pendentes; -1 se a contagem ainda nao esta valida.
int pendingQueueCount();
bool pendingQueueAppend(const String& jsonLine);
bool pendingQueueAppend(const char* jsonLine);
// Envia ate maxItems linhas (FIFO). WiFi deve estar conectado pelo chamador.
int pendingQueueFlush(int maxItems, PendingSendFn send);
// Agrupa ate maxItems linhas num unico POST (array JSON). maxBytes limita o corpo HTTP.
int pendingQueueFlushBatch(int maxItems, size_t maxBytes, PendingBatchSendFn send);

#endif
