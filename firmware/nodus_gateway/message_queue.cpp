#include "message_queue.h"

#include <LittleFS.h>

#include <algorithm>

#include "telemetry.h"

namespace store {

std::atomic<bool> outboundPending{true};

namespace {

SemaphoreHandle_t s_mutex = nullptr;
uint32_t s_seq = 0;

struct Lock {
  Lock() { xSemaphoreTake(s_mutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mutex); }
};

const char *dirOf(Q q) {
  switch (q) {
    case Q::In: return "/in";
    case Q::Out: return "/out";
    default: return "/st";
  }
}

// Sorted list of file names (not paths) in a directory.
std::vector<String> listDir(const char *dir) {
  std::vector<String> names;
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) return names;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    String n = f.name();
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);
    if (!f.isDirectory()) names.push_back(n);
    f.close();
  }
  d.close();
  std::sort(names.begin(), names.end(), [](const String &a, const String &b) { return strcmp(a.c_str(), b.c_str()) < 0; });
  return names;
}

// "0000002a_abc.json" -> "abc"
String idFromName(const String &name) {
  int us = name.indexOf('_');
  String rest = us >= 0 ? name.substring(us + 1) : name;
  if (rest.endsWith(".json")) rest.remove(rest.length() - 5);
  return rest;
}

String findName(const char *dir, const String &id) {
  String suffix = "_" + id + ".json";
  for (const String &n : listDir(dir))
    if (n.endsWith(suffix)) return n;
  return "";
}

bool writeAtomic(const String &path, const String &content) {
  const char *tmp = "/tmp.write";
  File f = LittleFS.open(tmp, "w");
  if (!f) return false;
  size_t n = f.print(content);
  f.close();
  if (n != content.length()) {
    LittleFS.remove(tmp);
    return false;
  }
  LittleFS.remove(path);  // rename does not overwrite on all FS versions
  return LittleFS.rename(tmp, path);
}

String readFile(const String &path) {
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

// Ids come from the backend or from our own hashes; keep file names safe.
bool safeId(const String &id) {
  if (id.isEmpty() || id.length() > 64) return false;
  for (char c : id)
    if (!(isalnum((unsigned char)c) || c == '-')) return false;
  return true;
}

}  // namespace

bool begin() {
  s_mutex = xSemaphoreCreateMutex();
  if (!LittleFS.begin(true)) {  // true = format if mounting fails (first boot)
    logf("STORE", "LittleFS mount FAILED");
    return false;
  }
  for (const char *d : {"/in", "/out", "/st", "/parts"})
    if (!LittleFS.exists(d)) LittleFS.mkdir(d);
  LittleFS.remove("/tmp.write");
  // Continue the sequence after the highest existing file.
  for (const char *d : {"/in", "/out", "/st"})
    for (const String &n : listDir(d)) s_seq = std::max(s_seq, (uint32_t)strtoul(n.c_str(), nullptr, 16) + 1);
  logf("STORE", "LittleFS ok: %u/%u KB used, queued in=%d out=%d status=%d", (unsigned)(LittleFS.usedBytes() / 1024),
       (unsigned)(LittleFS.totalBytes() / 1024), count(Q::In), count(Q::Out), count(Q::Status));
  return true;
}

bool exists(Q q, const String &id) {
  Lock l;
  return findName(dirOf(q), id).length() > 0;
}

bool put(Q q, const String &id, const String &json) {
  if (!safeId(id)) {
    logf("STORE", "refusing unsafe id '%s'", id.c_str());
    return false;
  }
  Lock l;
  if (findName(dirOf(q), id).length()) return true;  // already queued: idempotent
  char name[96];
  snprintf(name, sizeof name, "%s/%08lx_%s.json", dirOf(q), (unsigned long)s_seq++, id.c_str());
  bool ok = writeAtomic(name, json);
  if (ok && q == Q::Out) outboundPending = true;
  if (!ok) logf("STORE", "write failed: %s (flash full?)", name);
  return ok;
}

bool peek(Q q, String &id, String &json) {
  Lock l;
  std::vector<String> names = listDir(dirOf(q));
  if (names.empty()) return false;
  id = idFromName(names[0]);
  json = readFile(String(dirOf(q)) + "/" + names[0]);
  return true;
}

bool remove(Q q, const String &id) {
  Lock l;
  String n = findName(dirOf(q), id);
  return n.length() && LittleFS.remove(String(dirOf(q)) + "/" + n);
}

int count(Q q) {
  Lock l;
  return (int)listDir(dirOf(q)).size();
}

// ------------------------------------------------------------------ parts
// /parts/<key>_<seq 2 digits>.json

bool putPart(const String &key, int seq, const String &json) {
  if (!safeId(key)) return false;
  Lock l;
  char name[96];
  snprintf(name, sizeof name, "/parts/%s_%02d.json", key.c_str(), seq);
  return writeAtomic(name, json);
}

std::vector<String> loadParts(const String &key) {
  Lock l;
  std::vector<String> out;
  for (const String &n : listDir("/parts"))
    if (n.startsWith(key + "_")) out.push_back(readFile("/parts/" + n));
  return out;  // listDir is sorted, and seq is zero-padded, so parts are in order
}

int partCount(const String &key) {
  Lock l;
  int c = 0;
  for (const String &n : listDir("/parts"))
    if (n.startsWith(key + "_")) c++;
  return c;
}

void removeParts(const String &key) {
  Lock l;
  for (const String &n : listDir("/parts"))
    if (n.startsWith(key + "_")) LittleFS.remove("/parts/" + n);
}

std::vector<String> partKeys() {
  Lock l;
  std::vector<String> keys;
  for (const String &n : listDir("/parts")) {
    String k = n.substring(0, n.lastIndexOf('_'));
    if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
  }
  return keys;
}

}  // namespace store
