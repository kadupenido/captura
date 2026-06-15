#include "pending_queue.h"

#include <LittleFS.h>
#include <cstring>
#include <sys/stat.h>

#include "device_config.h"
#include "log.h"

static const char* const kPendingPath = "/pending.ndjson";
static const char* const kPendingTmp = "/pending.tmp";

// Caminhos VFS reais (LittleFS.begin() usa basePath "/littlefs" por defeito).
// NAO usar LittleFS.exists() para ficheiros em falta: no core ESP32, exists()
// faz open("r") e dispara log_e "no permits for creation" (vfs_api.cpp:105).
static const char kPendingVfs[] = "/littlefs/pending.ndjson";
static const char kPendingTmpVfs[] = "/littlefs/pending.tmp";

// Contagem de linhas nao vazias; reidratado uma vez em pendingQueueInit().
static int s_pendingLineCount = 0;
static bool s_lineCountValid = false;

static int mergeTmpIntoPending() {
  File tmp = LittleFS.open(kPendingTmp, "r");
  if (!tmp) {
    return 0;
  }
  File pending = LittleFS.open(kPendingPath, "a", true);
  if (!pending) {
    tmp.close();
    return -1;
  }

  int merged = 0;
  while (tmp.available()) {
    String line = tmp.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    pending.println(line);
    merged++;
  }
  pending.close();
  tmp.close();
  LittleFS.remove(kPendingTmp);
  return merged;
}

static size_t pendingFileSize() {
  struct stat st;
  if (stat(kPendingVfs, &st) != 0) {
    return 0;
  }
  return (size_t)st.st_size;
}

// So para montagem inicial (evita ler o ficheiro inteiro em cada append).
static int countLinesFromFile() {
  if (pendingFileSize() == 0) {
    return 0;
  }
  File f = LittleFS.open(kPendingPath, "r");
  if (!f) {
    return 0;
  }
  int n = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() > 0) {
      n++;
    }
  }
  f.close();
  return n;
}

static bool removeFirstLine() {
  File in = LittleFS.open(kPendingPath, "r");
  if (!in || in.size() == 0) {
    if (in) {
      in.close();
    }
    return false;
  }

  File out = LittleFS.open(kPendingTmp, "w", true);
  if (!out) {
    in.close();
    return false;
  }

  bool skipped = false;
  bool wroteAny = false;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    if (!skipped) {
      skipped = true;
      continue;
    }
    line.trim();
    if (line.length() > 0) {
      out.println(line);
      wroteAny = true;
    }
  }

  in.close();
  out.close();

  LittleFS.remove(kPendingPath);
  if (wroteAny) {
    LittleFS.rename(kPendingTmp, kPendingPath);
  } else {
    LittleFS.remove(kPendingTmp);
  }

  if (s_lineCountValid) {
    if (!wroteAny) {
      s_pendingLineCount = 0;
    } else {
      s_pendingLineCount--;
    }
  }
  return true;
}

static bool pendingQueueAppendBytes(const char* data, size_t lineLen) {
  if (!data || lineLen == 0) {
    return false;
  }
  for (size_t i = 0; i < lineLen; i++) {
    if (data[i] == '\n' || data[i] == '\r') {
      return false;
    }
  }

  while (s_lineCountValid && s_pendingLineCount >= deviceConfig().pending_max_lines) {
    logPrintf("Fila: descartando linha mais antiga (limite %d linhas).\n",
              deviceConfig().pending_max_lines);
    if (!removeFirstLine()) {
      return false;
    }
  }

  while (true) {
    size_t fsz = pendingFileSize();
    size_t add = lineLen + (fsz > 0 ? 1 : 0);
    if (fsz + add <= static_cast<size_t>(deviceConfig().pending_max_bytes)) {
      break;
    }
    logPrintf("Fila: descartando linha mais antiga (limite %d bytes).\n",
              deviceConfig().pending_max_bytes);
    if (!removeFirstLine()) {
      return false;
    }
  }

  File f = LittleFS.open(kPendingPath, "a", true);
  if (!f) {
    return false;
  }
  f.write((const uint8_t*)data, lineLen);
  f.write('\n');
  f.close();

  if (s_lineCountValid) {
    s_pendingLineCount++;
    logPrintf("Fila offline: gravado (total %d).\n", s_pendingLineCount);
  } else {
    logPrintf("Fila offline: gravado.\n");
  }
  return true;
}

bool pendingQueueInit() {
  s_lineCountValid = false;
  s_pendingLineCount = 0;

  bool mounted = LittleFS.begin(false);
  if (!mounted) {
    delay(50);
    mounted = LittleFS.begin(false);
  }
  if (!mounted) {
    logPrintf("LittleFS: montagem falhou; tentando formatar...\n");
    if (!LittleFS.begin(true)) {
      logPrintf("LittleFS: formatacao falhou.\n");
      return false;
    }
  }

  struct stat st;
  if (stat(kPendingTmpVfs, &st) == 0) {
    const int merged = mergeTmpIntoPending();
    if (merged > 0) {
      logPrintf("Fila offline: recuperados %d item(ns) de pending.tmp.\n", merged);
    } else if (merged == 0) {
      LittleFS.remove(kPendingTmp);
    } else {
      logPrintf("Fila offline: falha ao recuperar pending.tmp.\n");
    }
  }

  // Garante ficheiro da fila sem usar exists() (evita log de erro no VFS).
  File touch = LittleFS.open(kPendingPath, "a", true);
  if (!touch) {
    logPrintf("LittleFS: nao foi possivel criar pending.ndjson.\n");
    return false;
  }
  touch.close();

  s_pendingLineCount = countLinesFromFile();
  s_lineCountValid = true;
  if (s_pendingLineCount > 0) {
    logPrintf("Fila offline: %d item(ns) pendente(s) na flash.\n", s_pendingLineCount);
  } else {
    logPrintf("Fila offline: vazia.\n");
  }
  return true;
}

bool pendingQueueHasPending() { return pendingFileSize() > 0; }

int pendingQueueCount() { return s_lineCountValid ? s_pendingLineCount : -1; }

bool pendingQueueAppend(const String& jsonLine) {
  if (jsonLine.isEmpty()) {
    return false;
  }
  return pendingQueueAppendBytes(jsonLine.c_str(), jsonLine.length());
}

bool pendingQueueAppend(const char* jsonLine) {
  if (!jsonLine || !jsonLine[0]) {
    return false;
  }
  return pendingQueueAppendBytes(jsonLine, strlen(jsonLine));
}

int pendingQueueFlush(int maxItems, PendingSendFn send) {
  struct stat st;
  if (!send || maxItems <= 0 || stat(kPendingVfs, &st) != 0 || st.st_size == 0) {
    return 0;
  }

  File in = LittleFS.open(kPendingPath, "r");
  if (!in || in.size() == 0) {
    if (in) {
      in.close();
    }
    return 0;
  }

  File out = LittleFS.open(kPendingTmp, "w", true);
  if (!out) {
    in.close();
    return 0;
  }

  int sent = 0;
  int kept = 0;
  bool keepAny = false;
  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }

    if (sent < maxItems && send(line) == 201) {
      sent++;
    } else {
      out.println(line);
      keepAny = true;
      kept++;
    }
  }

  in.close();
  out.close();

  LittleFS.remove(kPendingPath);
  if (keepAny) {
    LittleFS.rename(kPendingTmp, kPendingPath);
  } else {
    LittleFS.remove(kPendingTmp);
  }

  if (s_lineCountValid) {
    s_pendingLineCount = kept;
  }

  return sent;
}

int pendingQueueFlushBatch(int maxItems, size_t maxBytes, PendingBatchSendFn send) {
  struct stat st;
  if (!send || maxItems <= 0 || maxBytes < 4 || stat(kPendingVfs, &st) != 0 || st.st_size == 0) {
    return 0;
  }

  if (maxItems > PENDING_BATCH_MAX_ITEMS) {
    maxItems = PENDING_BATCH_MAX_ITEMS;
  }

  File in = LittleFS.open(kPendingPath, "r");
  if (!in || in.size() == 0) {
    if (in) {
      in.close();
    }
    return 0;
  }

  String batchLines[PENDING_BATCH_MAX_ITEMS];
  int batchCount = 0;
  size_t batchBytes = 2;  // "[]"
  String holdBack;

  while (in.available() && batchCount < maxItems) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }

    const size_t add = line.length() + (batchCount > 0 ? 1 : 0);
    if (batchBytes + add > maxBytes) {
      if (batchCount == 0) {
        if (line.length() + 2 > maxBytes) {
          in.close();
          logPrintf("Fila: registro excede tamanho maximo do lote (%u bytes); descartando.\n",
                    (unsigned)maxBytes);
          if (!removeFirstLine()) {
            return 0;
          }
          return pendingQueueFlushBatch(maxItems, maxBytes, send);
        }
        batchLines[batchCount++] = line;
        batchBytes += add;
      } else {
        holdBack = line;
      }
      break;
    }

    batchLines[batchCount++] = line;
    batchBytes += add;
  }

  if (batchCount == 0) {
    in.close();
    return 0;
  }

  String payload;
  payload.reserve(batchBytes);
  payload = "[";
  for (int i = 0; i < batchCount; i++) {
    if (i > 0) {
      payload += ",";
    }
    payload += batchLines[i];
  }
  payload += "]";

  const int code = send(payload.c_str(), payload.length());
  if (code != 201) {
    in.close();
    if (code == 422) {
      return -422;
    }
    return 0;
  }

  File out = LittleFS.open(kPendingTmp, "w", true);
  if (!out) {
    in.close();
    return 0;
  }

  int kept = 0;
  if (holdBack.length() > 0) {
    out.println(holdBack);
    kept++;
  }
  while (in.available()) {
    String line = in.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }
    out.println(line);
    kept++;
  }

  in.close();
  out.close();

  LittleFS.remove(kPendingPath);
  if (kept > 0) {
    LittleFS.rename(kPendingTmp, kPendingPath);
  } else {
    LittleFS.remove(kPendingTmp);
  }

  if (s_lineCountValid) {
    s_pendingLineCount = kept;
  }

  return batchCount;
}
