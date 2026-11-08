// Host-side tests for the PDU codec. Run on a PC (no ESP32 needed):
//   g++ -std=c++17 -Wall -Wextra -I firmware/nodus_gateway firmware/test/pdu_host_test.cpp
//       firmware/nodus_gateway/pdu.cpp -o /tmp/pdu_test && /tmp/pdu_test
#include <cstdio>
#include <string>
#include <vector>

#include "pdu.h"

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (cond) g_pass++;                                                \
    else { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
  } while (0)
#define CHECK_EQ(a, b)                                                                     \
  do {                                                                                     \
    auto _a = (a); auto _b = (b);                                                          \
    if (_a == _b) g_pass++;                                                                \
    else { g_fail++; printf("FAIL %s:%d  %s\n   got:      [%s]\n   expected: [%s]\n", __FILE__, __LINE__, #a, \
                            std::string(_a).c_str(), std::string(_b).c_str()); }           \
  } while (0)

// Parses one of our SUBMIT PDUs back (test-only) and decodes its user data.
static pdu::Deliver parseSubmit(const std::string &hex, std::string *number = nullptr) {
  std::vector<uint8_t> b;
  for (size_t i = 0; i < hex.size(); i += 2) b.push_back((uint8_t)std::stoi(hex.substr(i, 2), nullptr, 16));
  size_t p = 1 + b[0];
  uint8_t first = b[p++];
  p++;  // MR
  uint8_t digits = b[p++];
  uint8_t toa = b[p++];
  std::string num = (toa == 0x91) ? "+" : "";
  for (int i = 0; i < digits; i++) {
    uint8_t n = (i % 2 == 0) ? (b[p + i / 2] & 0x0F) : (b[p + i / 2] >> 4);
    num.push_back((char)('0' + n));
  }
  if (number) *number = num;
  p += (digits + 1) / 2;
  p++;  // PID
  uint8_t dcs = b[p++];
  p++;  // VP (relative)
  uint8_t udl = b[p++];
  pdu::Deliver d;
  std::vector<uint8_t> ud(b.begin() + (long)p, b.end());
  pdu::decodeUserData(dcs, first & 0x40, udl, ud, d);
  return d;
}

int main() {
  // 1. Classic reference SMS-DELIVER (from the GSM 03.40 tutorial at dreamfabric.com).
  {
    pdu::Deliver d;
    std::string err;
    CHECK(pdu::decodeDeliver("07911326040000F0040B911346610089F60000208062917314080CC8F71D14969741F977FD07", d, &err));
    CHECK_EQ(d.sender, std::string("+31641600986"));
    CHECK_EQ(d.text, std::string("How are you?"));
    // This textbook PDU encodes its timezone as 0x08, which per TS 23.040 (digits are
    // swapped) means "-0 quarters"; +2h is 0x80. Test 8 covers a spec-correct +02:00.
    CHECK_EQ(d.timestamp.substr(0, 19), std::string("2002-08-26T19:37:41"));
    CHECK(!d.concatenated);
  }

  // 2. Reference SMS-SUBMIT "hellohello" to +46708251358, VP 0xAA (same tutorial).
  {
    auto parts = pdu::encodeSubmit("+46708251358", "hellohello", 0, nullptr, 0xAA);
    CHECK(parts.size() == 1);
    CHECK_EQ(parts[0].hex, std::string("0011000B916407281553F80000AA0AE8329BFD4697D9EC37"));
    CHECK(parts[0].tpduLength == 23);
  }

  // 3. French with GSM-7 accents (é è à ù are GSM-7): stays GSM-7, round-trips.
  {
    bool ucs2 = true;
    std::string t = "Récolte à l'école, où ? 5€ [ok]";
    auto parts = pdu::encodeSubmit("+25779123456", t, 1, &ucs2);
    CHECK(!ucs2);
    CHECK(parts.size() == 1);
    std::string num;
    CHECK_EQ(parseSubmit(parts[0].hex, &num).text, t);
    CHECK_EQ(num, std::string("+25779123456"));
  }

  // 4. Non-GSM character forces UCS-2; emoji (surrogate pair) round-trips.
  {
    bool ucs2 = false;
    std::string t = "Fenêtre 🌽 Murakoze!";
    auto parts = pdu::encodeSubmit("+25779123456", t, 2, &ucs2);
    CHECK(ucs2);
    CHECK(parts.size() == 1);
    CHECK_EQ(parseSubmit(parts[0].hex).text, t);
  }

  // 5. Exactly 160 GSM-7 chars = 1 part; 161 = 2 parts (153 + 8) with UDH.
  {
    CHECK(pdu::encodeSubmit("+25779123456", std::string(160, 'a'), 3).size() == 1);
    auto parts = pdu::encodeSubmit("+25779123456", std::string(161, 'a'), 3);
    CHECK(parts.size() == 2);
    pdu::Deliver p1 = parseSubmit(parts[0].hex), p2 = parseSubmit(parts[1].hex);
    CHECK(p1.concatenated && p1.concatRef == 3 && p1.concatTotal == 2 && p1.concatSeq == 1);
    CHECK(p2.concatSeq == 2);
    CHECK(p1.text.size() == 153 && p2.text.size() == 8);
    CHECK_EQ(p1.text + p2.text, std::string(161, 'a'));
  }

  // 6. Long multipart GSM-7 text with extension chars: never split ESC from its char.
  {
    std::string t;
    for (int i = 0; i < 60; i++) t += "Ama{ho}ro ";  // braces are 2 septets each
    auto parts = pdu::encodeSubmit("+25779123456", t, 9);
    CHECK(parts.size() > 1);
    std::string joined;
    for (auto &p : parts) {
      CHECK(p.tpduLength <= 140 + 15);  // header + 140 octets max UD
      joined += parseSubmit(p.hex).text;
    }
    CHECK_EQ(joined, t);
  }

  // 7. Long multipart UCS-2 with emoji at boundaries round-trips.
  {
    std::string t;
    for (int i = 0; i < 40; i++) t += "ê🌽";
    auto parts = pdu::encodeSubmit("+25779123456", t, 10);
    CHECK(parts.size() > 1);
    std::string joined;
    for (auto &p : parts) joined += parseSubmit(p.hex).text;
    CHECK_EQ(joined, t);
  }

  // 8. Incoming multipart SMS-DELIVER with UDH (built by hand: part 2/3, ref 0x42, "World").
  {
    // SMSC 00 | first 0x44 (DELIVER, UDHI) | OA 0B 91 527919325496 (+25979123456? digits only)
    // PID 00 | DCS 00 | SCTS | UDL | UD = UDH(050003420302) + fill + "World"
    // Build the UD with our own packer via a SUBMIT, then transplant it into a DELIVER.
    auto sub = pdu::encodeSubmit("+1", std::string(153, 'x') + std::string(153, 'y') + "World", 0x42);
    CHECK(sub.size() == 3);
    std::string subHex = sub[2].hex;
    // SUBMIT layout: 00 | 51 | 00 | 01 91 F1 | 00 | 00 | A7 | UDL | UD
    std::string udlAndUd = subHex.substr(2 + 2 + 2 + 6 + 2 + 2 + 2);
    std::string deliver = "00" "44" "0B915297193254F6" "00" "00" "62118041130080" + udlAndUd;
    pdu::Deliver d;
    std::string err;
    CHECK(pdu::decodeDeliver(deliver, d, &err));
    CHECK_EQ(d.text, std::string("World"));
    CHECK(d.concatenated && d.concatRef == 0x42 && d.concatTotal == 3 && d.concatSeq == 3);
    CHECK_EQ(d.sender, std::string("+25799123456"));
    CHECK_EQ(d.timestamp, std::string("2026-11-08T14:31:00+02:00"));
  }

  // 9. Alphanumeric sender (operator promo) decodes as text.
  {
    // OA: 0D (13 semi-octets) D0 (alphanumeric) + "Lumitel" packed in GSM-7: CC7A3B4D2FB301
    pdu::Deliver d;
    std::string err;
    CHECK(pdu::decodeDeliver("00040DD0CC7A3B4D2FB301000062118041130080" "02C834", d, &err));
    CHECK_EQ(d.sender, std::string("Lumitel"));
    CHECK_EQ(d.text, std::string("Hi"));
  }

  // 10. Garbage input is rejected, not crashed on.
  {
    pdu::Deliver d;
    CHECK(!pdu::decodeDeliver("ZZ", d));
    CHECK(!pdu::decodeDeliver("", d));
    CHECK(!pdu::decodeDeliver("0791", d));
  }

  printf("%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
