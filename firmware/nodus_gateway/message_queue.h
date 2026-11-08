// Persistent message queues in LittleFS. Every message is one small JSON file,
// written atomically (temp file + rename), so a power cut never leaves a
// half-written message and nothing in a queue is lost on reboot.
//
//   /in/      received SMS waiting to be uploaded to the backend
//   /out/     replies waiting to be sent by SMS
//   /st/      delivery results waiting to be reported to the backend
//   /parts/   fragments of multipart SMS waiting for their siblings
//
// Files are named <8-hex sequence>_<id>.json so the oldest is handled first
// (keeps a user's messages in order).
#pragma once

#include <Arduino.h>

#include <atomic>
#include <vector>

namespace store {

bool begin();  // mount LittleFS (formats on first use), create directories

enum class Q { In, Out, Status };

bool exists(Q q, const String &id);
bool put(Q q, const String &id, const String &json);  // no-op success if id already queued
bool peek(Q q, String &id, String &json);              // oldest item
bool remove(Q q, const String &id);
int count(Q q);

// Multipart fragments. key identifies one long message (sender + reference).
bool putPart(const String &key, int seq, const String &json);
std::vector<String> loadParts(const String &key);  // JSON of each stored part, ordered by seq
int partCount(const String &key);
void removeParts(const String &key);
std::vector<String> partKeys();

// Set whenever an outbound SMS is queued, so the modem task reacts immediately.
extern std::atomic<bool> outboundPending;

}  // namespace store
