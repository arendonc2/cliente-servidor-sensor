#pragma once
#include <Arduino.h>

namespace coapmin {
  enum Type { CON=0, NON=1, ACK=2, RST=3 };

  inline size_t buildPost(uint8_t* out, const char* path1, const char* path2,
                          const char* json, uint16_t msgId) {
    uint8_t* p = out;

    // Header: ver=1, type=CON, TKL=2, code=POST(0x02)
    *p++ = (1 << 6) | (CON << 4) | 2;
    *p++ = 0x02;
    *p++ = uint8_t(msgId >> 8);
    *p++ = uint8_t(msgId & 0xFF);

    // Token = msgId
    *p++ = uint8_t(msgId >> 8);
    *p++ = uint8_t(msgId & 0xFF);

    // Opciones (delta/len < 13)
    uint16_t last = 0;
    auto addOpt = [&](uint16_t number, const uint8_t* val, uint8_t len) {
      uint16_t delta = number - last;
      *p++ = uint8_t((delta << 4) | (len & 0x0F));
      memcpy(p, val, len); p += len;
      last = number;
    };

    if (path1 && *path1) addOpt(11, (const uint8_t*)path1, strlen(path1)); // Uri-Path
    if (path2 && *path2) addOpt(11, (const uint8_t*)path2, strlen(path2)); // Uri-Path

    uint8_t cf = 50; 
    addOpt(12, &cf, 1);

    // Payload marker + JSON
    *p++ = 0xFF;
    size_t jlen = strlen(json);
    memcpy(p, json, jlen); p += jlen;

    return (size_t)(p - out);
  }

  //Parseo de header
  inline bool parseHeader(const uint8_t* b, size_t n,
                          Type& type, uint8_t& code, uint16_t& msgId) {
    if (n < 4) return false;
    if (((b[0] >> 6) & 0x03) != 1) return false; 
    type = (Type)((b[0] >> 4) & 0x03);
    code = b[1];
    msgId = (uint16_t(b[2]) << 8) | b[3];
    return true;
  }
}
