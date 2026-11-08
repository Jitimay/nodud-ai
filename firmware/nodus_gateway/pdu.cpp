#include "pdu.h"

#include <cstdio>

namespace pdu {
namespace {

// GSM 03.38 basic table: septet value -> Unicode code point. 0x1B is ESC.
const uint16_t kGsmBasic[128] = {
    0x0040, 0x00A3, 0x0024, 0x00A5, 0x00E8, 0x00E9, 0x00F9, 0x00EC, 0x00F2, 0x00C7, 0x000A, 0x00D8, 0x00F8, 0x000D, 0x00C5, 0x00E5,
    0x0394, 0x005F, 0x03A6, 0x0393, 0x039B, 0x03A9, 0x03A0, 0x03A8, 0x03A3, 0x0398, 0x039E, 0x001B, 0x00C6, 0x00E6, 0x00DF, 0x00C9,
    0x0020, 0x0021, 0x0022, 0x0023, 0x00A4, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002A, 0x002B, 0x002C, 0x002D, 0x002E, 0x002F,
    0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003A, 0x003B, 0x003C, 0x003D, 0x003E, 0x003F,
    0x00A1, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004A, 0x004B, 0x004C, 0x004D, 0x004E, 0x004F,
    0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005A, 0x00C4, 0x00D6, 0x00D1, 0x00DC, 0x00A7,
    0x00BF, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069, 0x006A, 0x006B, 0x006C, 0x006D, 0x006E, 0x006F,
    0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007A, 0x00E4, 0x00F6, 0x00F1, 0x00FC, 0x00E0,
};

struct ExtEntry {
  uint8_t septet;
  uint16_t cp;
};
// Extension table (preceded by ESC 0x1B).
const ExtEntry kGsmExt[] = {
    {0x0A, 0x000C}, {0x14, 0x005E}, {0x28, 0x007B}, {0x29, 0x007D}, {0x2F, 0x005C},
    {0x3C, 0x005B}, {0x3D, 0x007E}, {0x3E, 0x005D}, {0x40, 0x007C}, {0x65, 0x20AC},
};

// Code point -> septets (1 or 2 incl. ESC). Returns 0 if not representable.
int gsmEncodeCp(uint32_t cp, uint8_t outSeptets[2]) {
  for (int i = 0; i < 128; i++) {
    if (i != 0x1B && kGsmBasic[i] == cp) {
      outSeptets[0] = (uint8_t)i;
      return 1;
    }
  }
  for (const auto &e : kGsmExt) {
    if (e.cp == cp) {
      outSeptets[0] = 0x1B;
      outSeptets[1] = e.septet;
      return 2;
    }
  }
  return 0;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

bool hexToBytes(const std::string &hex, std::vector<uint8_t> &out) {
  if (hex.size() % 2) return false;
  out.clear();
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    int hi = hexNibble(hex[i]), lo = hexNibble(hex[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.push_back((uint8_t)((hi << 4) | lo));
  }
  return true;
}

void appendHex(std::string &s, uint8_t b) {
  static const char *d = "0123456789ABCDEF";
  s.push_back(d[b >> 4]);
  s.push_back(d[b & 0x0F]);
}

// Unpack `count` septets from packed GSM-7 data.
std::vector<uint8_t> unpackSeptets(const uint8_t *data, size_t dataLen, size_t count) {
  std::vector<uint8_t> out;
  out.reserve(count);
  for (size_t i = 0; i < count; i++) {
    size_t bit = i * 7;
    size_t byte = bit / 8;
    int shift = (int)(bit % 8);
    if (byte >= dataLen) break;
    uint16_t v = data[byte] >> shift;
    if (shift > 1 && byte + 1 < dataLen) v |= (uint16_t)data[byte + 1] << (8 - shift);
    out.push_back((uint8_t)(v & 0x7F));
  }
  return out;
}

std::string septetsToUtf8(const std::vector<uint8_t> &septets) {
  std::vector<uint32_t> cps;
  for (size_t i = 0; i < septets.size(); i++) {
    uint8_t s = septets[i];
    if (s == 0x1B && i + 1 < septets.size()) {
      uint8_t e = septets[++i];
      uint32_t cp = '?';
      for (const auto &x : kGsmExt)
        if (x.septet == e) cp = x.cp;
      cps.push_back(cp);
    } else {
      cps.push_back(kGsmBasic[s & 0x7F]);
    }
  }
  return codepointsToUtf8(cps);
}

std::string ucs2ToUtf8(const uint8_t *d, size_t len) {
  std::vector<uint32_t> cps;
  for (size_t i = 0; i + 1 < len; i += 2) {
    uint32_t u = ((uint32_t)d[i] << 8) | d[i + 1];
    if (u >= 0xD800 && u <= 0xDBFF && i + 3 < len) {
      uint32_t lo = ((uint32_t)d[i + 2] << 8) | d[i + 3];
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        cps.push_back(0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
        i += 2;
        continue;
      }
    }
    cps.push_back(u);
  }
  return codepointsToUtf8(cps);
}

std::string decodeSemiOctets(const uint8_t *d, size_t digits) {
  std::string s;
  for (size_t i = 0; i < digits; i++) {
    uint8_t b = d[i / 2];
    uint8_t n = (i % 2 == 0) ? (b & 0x0F) : (b >> 4);
    if (n == 0x0F) break;
    s.push_back(n < 10 ? (char)('0' + n) : (n == 0x0A ? '*' : n == 0x0B ? '#' : '?'));
  }
  return s;
}

int bcdSwapped(uint8_t b) { return (b & 0x0F) * 10 + (b >> 4); }

enum class Alphabet { Gsm7, Data8, Ucs2 };

Alphabet alphabetFromDcs(uint8_t dcs) {
  uint8_t group = dcs & 0xF0;
  if ((dcs & 0xC0) == 0x00 || (dcs & 0xC0) == 0x40) {  // general data coding (incl. auto-delete)
    switch ((dcs >> 2) & 0x03) {
      case 1: return Alphabet::Data8;
      case 2: return Alphabet::Ucs2;
      default: return Alphabet::Gsm7;
    }
  }
  if (group == 0xE0) return Alphabet::Ucs2;                                  // MWI, UCS2
  if (group == 0xF0) return (dcs & 0x04) ? Alphabet::Data8 : Alphabet::Gsm7;  // data coding/class
  return Alphabet::Gsm7;                                                       // MWI groups C0/D0
}

}  // namespace

// ------------------------------------------------------------------ UTF-8

std::vector<uint32_t> utf8ToCodepoints(const std::string &s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    uint8_t c = (uint8_t)s[i];
    uint32_t cp;
    int extra;
    if (c < 0x80) { cp = c; extra = 0; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else { out.push_back(0xFFFD); i++; continue; }
    bool ok = true;
    for (int k = 1; k <= extra; k++) {
      if (i + k >= s.size() || (((uint8_t)s[i + k]) & 0xC0) != 0x80) { ok = false; break; }
      cp = (cp << 6) | (((uint8_t)s[i + k]) & 0x3F);
    }
    if (!ok) { out.push_back(0xFFFD); i++; continue; }
    out.push_back(cp);
    i += extra + 1;
  }
  return out;
}

std::string codepointsToUtf8(const std::vector<uint32_t> &cps) {
  std::string s;
  for (uint32_t cp : cps) {
    if (cp < 0x80) s.push_back((char)cp);
    else if (cp < 0x800) { s.push_back((char)(0xC0 | (cp >> 6))); s.push_back((char)(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) {
      s.push_back((char)(0xE0 | (cp >> 12)));
      s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
      s.push_back((char)(0xF0 | (cp >> 18)));
      s.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
      s.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
      s.push_back((char)(0x80 | (cp & 0x3F)));
    }
  }
  return s;
}

bool isGsm7(const std::string &utf8) {
  uint8_t tmp[2];
  for (uint32_t cp : utf8ToCodepoints(utf8))
    if (!gsmEncodeCp(cp, tmp)) return false;
  return true;
}

// ------------------------------------------------------------------ decode

bool decodeUserData(uint8_t dcs, bool udhi, uint8_t udl, const std::vector<uint8_t> &ud, Deliver &out) {
  Alphabet alpha = alphabetFromDcs(dcs);
  size_t headerOctets = 0;
  if (udhi) {
    if (ud.empty()) return false;
    uint8_t udhl = ud[0];
    headerOctets = (size_t)udhl + 1;
    if (headerOctets > ud.size()) return false;
    // Walk information elements looking for concatenation info.
    size_t p = 1;
    while (p + 1 < headerOctets) {
      uint8_t iei = ud[p], len = ud[p + 1];
      if (p + 2 + len > headerOctets) break;
      if (iei == 0x00 && len == 3) {
        out.concatenated = true;
        out.concatRef = ud[p + 2];
        out.concatTotal = ud[p + 3];
        out.concatSeq = ud[p + 4];
      } else if (iei == 0x08 && len == 4) {
        out.concatenated = true;
        out.concatRef = (uint16_t)((ud[p + 2] << 8) | ud[p + 3]);
        out.concatTotal = ud[p + 4];
        out.concatSeq = ud[p + 5];
      }
      p += 2 + len;
    }
  }

  if (alpha == Alphabet::Gsm7) {
    std::vector<uint8_t> septets = unpackSeptets(ud.data(), ud.size(), udl);
    size_t skip = (headerOctets * 8 + 6) / 7;  // header septets incl. fill bits
    if (skip > septets.size()) return false;
    septets.erase(septets.begin(), septets.begin() + (long)skip);
    out.text = septetsToUtf8(septets);
  } else {
    size_t n = udl < ud.size() ? udl : ud.size();
    if (headerOctets > n) return false;
    const uint8_t *body = ud.data() + headerOctets;
    size_t bodyLen = n - headerOctets;
    if (alpha == Alphabet::Ucs2) {
      out.text = ucs2ToUtf8(body, bodyLen);
    } else {  // 8-bit data: treat as Latin-1
      std::vector<uint32_t> cps(body, body + bodyLen);
      out.text = codepointsToUtf8(cps);
    }
  }
  return true;
}

bool decodeDeliver(const std::string &hex, Deliver &out, std::string *error) {
  auto fail = [&](const char *why) {
    if (error) *error = why;
    return false;
  };
  std::vector<uint8_t> b;
  if (!hexToBytes(hex, b)) return fail("not hex");
  size_t p = 0;
  auto need = [&](size_t n) { return p + n <= b.size(); };

  if (!need(1)) return fail("empty");
  p += 1 + b[0];  // skip SMSC
  if (!need(1)) return fail("truncated after SMSC");
  uint8_t first = b[p++];
  if ((first & 0x03) != 0x00) return fail("not SMS-DELIVER");
  bool udhi = first & 0x40;

  if (!need(2)) return fail("truncated OA");
  uint8_t oaDigits = b[p++];
  uint8_t toa = b[p++];
  size_t oaOctets = (oaDigits + 1) / 2;
  if (!need(oaOctets)) return fail("truncated OA digits");
  if ((toa & 0x70) == 0x50) {  // alphanumeric sender
    size_t chars = (oaDigits * 4) / 7;
    out.sender = septetsToUtf8(unpackSeptets(&b[p], oaOctets, chars));
    while (!out.sender.empty() && (out.sender.back() == '@' || out.sender.back() == ' ')) out.sender.pop_back();
  } else {
    std::string digits = decodeSemiOctets(&b[p], oaDigits);
    out.sender = ((toa & 0x70) == 0x10) ? "+" + digits : digits;
  }
  p += oaOctets;

  if (!need(2 + 7 + 1)) return fail("truncated header");
  p++;  // PID
  uint8_t dcs = b[p++];
  const uint8_t *ts = &b[p];
  p += 7;
  {
    int tzRaw = ts[6];
    int quarters = (tzRaw & 0x07) * 10 + (tzRaw >> 4);
    bool neg = tzRaw & 0x08;
    char buf[40];
    snprintf(buf, sizeof buf, "20%02d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", bcdSwapped(ts[0]), bcdSwapped(ts[1]),
             bcdSwapped(ts[2]), bcdSwapped(ts[3]), bcdSwapped(ts[4]), bcdSwapped(ts[5]), neg ? '-' : '+',
             quarters / 4, (quarters % 4) * 15);
    out.timestamp = buf;
  }
  uint8_t udl = b[p++];
  std::vector<uint8_t> ud(b.begin() + (long)p, b.end());
  if (!decodeUserData(dcs, udhi, udl, ud, out)) return fail("bad user data");
  return true;
}

// ------------------------------------------------------------------ encode

namespace {

void appendAddress(std::string &hex, const std::string &number) {
  std::string digits;
  bool intl = !number.empty() && number[0] == '+';
  for (char c : number)
    if (c >= '0' && c <= '9') digits.push_back(c);
  appendHex(hex, (uint8_t)digits.size());
  appendHex(hex, intl ? 0x91 : 0x81);
  for (size_t i = 0; i < digits.size(); i += 2) {
    uint8_t lo = (uint8_t)(digits[i] - '0');
    uint8_t hi = (i + 1 < digits.size()) ? (uint8_t)(digits[i + 1] - '0') : 0x0F;
    appendHex(hex, (uint8_t)((hi << 4) | lo));
  }
}

Submit buildSubmit(const std::string &number, bool ucs2, uint8_t validity, const std::vector<uint8_t> &udh,
                   const std::vector<uint8_t> &payload /* septets (GSM7) or octets (UCS2) */) {
  std::string hex = "00";  // use SMSC stored on the SIM
  appendHex(hex, (uint8_t)(0x11 | (udh.empty() ? 0x00 : 0x40)));  // SUBMIT, VPF relative, UDHI
  appendHex(hex, 0x00);                                            // TP-MR (modem assigns)
  appendAddress(hex, number);
  appendHex(hex, 0x00);                  // PID
  appendHex(hex, ucs2 ? 0x08 : 0x00);    // DCS
  appendHex(hex, validity);              // VP

  std::vector<uint8_t> ud;
  uint8_t udl;
  if (ucs2) {
    ud = udh;
    ud.insert(ud.end(), payload.begin(), payload.end());
    udl = (uint8_t)ud.size();
  } else {
    size_t headerSeptets = (udh.size() * 8 + 6) / 7;
    size_t totalSeptets = headerSeptets + payload.size();
    ud.assign((totalSeptets * 7 + 7) / 8, 0);
    for (size_t i = 0; i < udh.size(); i++) ud[i] = udh[i];
    for (size_t i = 0; i < payload.size(); i++) {
      size_t bit = (headerSeptets + i) * 7;
      size_t byte = bit / 8;
      int shift = (int)(bit % 8);
      ud[byte] |= (uint8_t)(payload[i] << shift);
      if (shift > 1) ud[byte + 1] |= (uint8_t)(payload[i] >> (8 - shift));
    }
    udl = (uint8_t)totalSeptets;
  }
  appendHex(hex, udl);
  for (uint8_t x : ud) appendHex(hex, x);
  return Submit{hex, (int)(hex.size() / 2) - 1};
}

}  // namespace

std::vector<Submit> encodeSubmit(const std::string &number, const std::string &utf8, uint8_t ref, bool *usedUcs2,
                                 uint8_t validity) {
  std::vector<uint32_t> cps = utf8ToCodepoints(utf8);
  std::vector<Submit> parts;

  // Try GSM-7.
  std::vector<uint8_t> septets;
  bool gsm = true;
  for (uint32_t cp : cps) {
    uint8_t s[2];
    int n = gsmEncodeCp(cp, s);
    if (!n) { gsm = false; break; }
    for (int k = 0; k < n; k++) septets.push_back(s[k]);
  }
  if (usedUcs2) *usedUcs2 = !gsm;

  // Split into chunks, never inside an escape sequence / surrogate pair.
  std::vector<std::vector<uint8_t>> chunks;
  if (gsm) {
    if (septets.size() <= 160) {
      chunks.push_back(septets);
    } else {
      for (size_t i = 0; i < septets.size();) {
        size_t end = i + 153 < septets.size() ? i + 153 : septets.size();
        if (end < septets.size() && septets[end - 1] == 0x1B) end--;
        chunks.emplace_back(septets.begin() + (long)i, septets.begin() + (long)end);
        i = end;
      }
    }
  } else {
    std::vector<uint8_t> octets;  // UTF-16BE
    for (uint32_t cp : cps) {
      if (cp >= 0x10000) {
        uint32_t v = cp - 0x10000;
        uint16_t hi = (uint16_t)(0xD800 + (v >> 10)), lo = (uint16_t)(0xDC00 + (v & 0x3FF));
        octets.push_back(hi >> 8); octets.push_back(hi & 0xFF);
        octets.push_back(lo >> 8); octets.push_back(lo & 0xFF);
      } else {
        octets.push_back((uint8_t)(cp >> 8));
        octets.push_back((uint8_t)(cp & 0xFF));
      }
    }
    if (octets.size() <= 140) {
      chunks.push_back(octets);
    } else {
      for (size_t i = 0; i < octets.size();) {
        size_t end = i + 134 < octets.size() ? i + 134 : octets.size();
        if (end < octets.size()) {
          uint16_t last = (uint16_t)((octets[end - 2] << 8) | octets[end - 1]);
          if (last >= 0xD800 && last <= 0xDBFF) end -= 2;  // keep surrogate pair together
        }
        chunks.emplace_back(octets.begin() + (long)i, octets.begin() + (long)end);
        i = end;
      }
    }
  }

  uint8_t total = (uint8_t)chunks.size();
  for (uint8_t i = 0; i < total; i++) {
    std::vector<uint8_t> udh;
    if (total > 1) udh = {0x05, 0x00, 0x03, ref, total, (uint8_t)(i + 1)};
    parts.push_back(buildSubmit(number, !gsm, validity, udh, chunks[i]));
  }
  return parts;
}

}  // namespace pdu
