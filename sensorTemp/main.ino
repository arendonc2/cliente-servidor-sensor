#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "ntc_sensor.h"
#include "coap_min.h"

const char* WIFI_SSID   = "Wokwi-GUEST";
const char* WIFI_PASS   = "";              
const char* SERVER_IP   = "100.27.228.1";   
const uint16_t SERVER_PORT = 5683;       
const uint32_t PERIOD_MS   = 3000;     

WiFiUDP udp;

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  ntcBegin();
  connectWiFi();
  udp.begin(0); 
  Serial.print("IP local: "); Serial.println(WiFi.localIP());
}

void loop() {
  // 1) Leer temperatura
  float tC = ntcReadCelsius(12);

  // 2) Armar JSON
  char json[64];
  snprintf(json, sizeof(json), "{\"t\":%.2f,\"unit\":\"C\"}", tC);

  // 3) Armar paquete CoAP POST 
  uint8_t pkt[256];
  uint16_t msgId = (uint16_t)millis();
  size_t len = coapmin::buildPost(pkt, "sensor", nullptr, json, msgId);

  // 4) Enviar por UDP
  IPAddress ip; ip.fromString(SERVER_IP);
  bool sent = false;
  if (udp.beginPacket(ip, SERVER_PORT) == 1) {
    udp.write(pkt, len);
    sent = (udp.endPacket() == 1);
  }
  Serial.print("[CoAP] POST "); Serial.print(sent ? "OK " : "FALLO ");
  Serial.print("/sensor payload="); Serial.println(json);

  // 5) Esperar ACK (timeout 1.5s)
  uint8_t rx[256];
  int n = 0;
  uint32_t start = millis();
  while ((millis() - start) < 1500) {
    int psize = udp.parsePacket();
    if (psize > 0) { n = udp.read(rx, sizeof(rx)); break; }
    delay(5);
  }

  if (n > 0) {
    coapmin::Type t; uint8_t code; uint16_t mid;
    if (coapmin::parseHeader(rx, n, t, code, mid)) {
      Serial.print("[CoAP] RX type="); Serial.print((int)t);
      Serial.print(" code=0x"); Serial.print(code, HEX);
      Serial.print(" msgId="); Serial.println(mid);
    } else {
      Serial.println("[CoAP] RX no válido");
    }
  } else {
    Serial.println("[CoAP] Sin ACK (timeout)");
  }

  delay(PERIOD_MS);
}
