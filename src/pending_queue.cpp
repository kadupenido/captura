#include "pending_queue.h"

#include <LittleFS.h>
#include <cstring>
#include <sys/stat.h>

#include "config.h"

#ifndef PENDING_MAX_BYTES
#define PENDING_MAX_BYTES 65536
#endif
#ifndef PENDING_MAX_LINES
#define PENDING_MAX_LINES 500
#endif

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

  while (s_lineCountValid && s_pendingLineCount >= PENDING_MAX_LINES) {
    Serial.printf("Fila: descartando linha mais antiga (limite %d linhas).\n", (int)PENDING_MAX_LINES);
    if (!removeFirstLine()) {
      return false;
    }
  }

  while (true) {
    size_t fsz = pendingFileSize();
    size_t add = lineLen + (fsz > 0 ? 1 : 0);
    if (fsz + add <= (size_t)PENDING_MAX_BYTES) {
      break;
    }
    Serial.printf("Fila: descartando linha mais antiga (limite %d bytes).\n", (int)PENDING_MAX_BYTES);
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
  }
  return true;
}

bool pendingQueueInit() {
  s_lineCountValid = false;
  s_pendingLineCount = 0;

  if (!LittleFS.begin(false)) {
    Serial.printf("LittleFS: montagem falhou, formatando...\n");
    if (!LittleFS.begin(true)) {
      Serial.printf("LittleFS: formatacao falhou.\n");
      return false;
    }
  }

  struct stat st;
  if (stat(kPendingTmpVfs, &st) == 0) {
    LittleFS.remove(kPendingTmp);
  }

  // Garante ficheiro da fila sem usar exists() (evita log de erro no VFS).
  File touch = LittleFS.open(kPendingPath, "a", true);
  if (!touch) {
    Serial.printf("LittleFS: nao foi possivel criar pending.ndjson.\n");
    return false;
  }
  touch.close();

  s_pendingLineCount = countLinesFromFile();
  s_lineCountValid = true;
  return true;
}

bool pendingQueueHasPending() { return pendingFileSize() > 0; }

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
