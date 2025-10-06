#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "hcsr04_sensor.h"
#include "coap_min.h"

const char* WIFI_SSID   = "Wokwi-GUEST";
const char* WIFI_PASS   = "";              
const char* SERVER_IP   = "100.27.228.1";    
const uint16_t SERVER_PORT = 5683;
const uint32_t PERIOD_MS   = 2000;       

WiFiUDP udp;

static void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(300);
}

static void printCoapPayload(const uint8_t* rx, int n) {
  int idx = -1;
  for (int i = 0; i < n; i++) { if (rx[i] == 0xFF) { idx = i + 1; break; } }
  if (idx > 0 && idx < n) {
    Serial.print("[CoAP] Payload: ");
    for (int i = idx; i < n; i++) Serial.print((char)rx[i]);
    Serial.println();
  } else {
    Serial.println("[CoAP] Sin payload");
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  hcsrBegin();
  connectWiFi();
  udp.begin(0); 
  Serial.print("IP local: "); Serial.println(WiFi.localIP());
}

void loop() {
  // 1) Leer distancia (cm)
  float dcm = hcsrReadDistanceCm(5);
  if (isnan(dcm)) dcm = -1.0f; 

  // 2) Armar JSON
  char json[64];
  snprintf(json, sizeof(json), "{\"d\":%.2f,\"unit\":\"cm\"}", dcm);

  // 3) Construir CoAP POST 
  uint8_t pkt[256];
  uint16_t msgId = (uint16_t)millis();
  size_t len = coapmin::buildPost(pkt, "sensor", nullptr, json, msgId);

  // 4) Enviar por UDP
  bool sent = false;
  if (udp.beginPacket(SERVER_IP, SERVER_PORT) == 1) {
    udp.write(pkt, len);
    sent = (udp.endPacket() == 1);
  }
  Serial.print("[CoAP] POST "); Serial.print(sent ? "OK " : "FALLO ");
  Serial.print("/sensor payload="); Serial.println(json);

  // 5) Esperar ACK + payload
  uint8_t rx[256]; int n = 0;
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
    printCoapPayload(rx, n);
  } else {
    Serial.println("[CoAP] Sin ACK (timeout)");
  }

  delay(PERIOD_MS);
}
