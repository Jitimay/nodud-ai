// SMS PDU encoding/decoding (3GPP TS 23.040 / 23.038).
//
// Plain C++ (no Arduino dependencies) so it can be unit-tested on a PC:
//   g++ -std=c++17 -I firmware/nodus_gateway firmware/test/pdu_host_test.cpp
//       firmware/nodus_gateway/pdu.cpp -o /tmp/pdu_test && /tmp/pdu_test
//
// Why PDU mode instead of text mode: text mode cannot send concatenated
// (multipart) SMS, cannot tell us that an incoming SMS is part of a longer
// message, and mangles characters outside ASCII.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pdu {

struct Deliver {
  std::string sender;     // "+25779123456", local digits, or alphanumeric ("Lumitel")
  std::string text;       // UTF-8
  std::string timestamp;  // ISO 8601 with offset, e.g. "2026-11-08T14:31:00+02:00"
  bool concatenated = false;
  uint16_t concatRef = 0;
  uint8_t concatTotal = 1;
  uint8_t concatSeq = 1;
};

// Decodes an SMS-DELIVER PDU as printed by AT+CMGL / AT+CMGR in PDU mode
// (hex string, starting with the SMSC field). Returns false on malformed input.
bool decodeDeliver(const std::string &hex, Deliver &out, std::string *error = nullptr);

struct Submit {
  std::string hex;  // complete PDU including the "00" (default SMSC) prefix
  int tpduLength;   // the value to pass to AT+CMGS=<n> (octets, SMSC excluded)
};

// Encodes UTF-8 text as one or more SMS-SUBMIT PDUs to `number`.
// GSM-7 when every character fits the GSM-7 alphabet, otherwise UCS-2.
// `ref` is the concatenation reference shared by all parts of one message.
// `validity` is the relative validity period octet (0xA7 = 24 hours).
std::vector<Submit> encodeSubmit(const std::string &number, const std::string &utf8, uint8_t ref,
                                 bool *usedUcs2 = nullptr, uint8_t validity = 0xA7);

// --- helpers exposed for tests and for the gateway -------------------------
bool isGsm7(const std::string &utf8);
std::vector<uint32_t> utf8ToCodepoints(const std::string &utf8);
std::string codepointsToUtf8(const std::vector<uint32_t> &cps);

// Decodes the TP-User-Data field. `ud` holds exactly the UD octets.
bool decodeUserData(uint8_t dcs, bool udhi, uint8_t udl, const std::vector<uint8_t> &ud, Deliver &out);

}  // namespace pdu
