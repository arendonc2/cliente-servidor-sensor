#pragma once
#include <Arduino.h>
#include <math.h>

#define HCSR04_TRIG_PIN 5
#define HCSR04_ECHO_PIN 18

// Tiempo máx de espera para el pulso ≈ 25 ms
#define HCSR04_TIMEOUT_US 25000UL

// Distancia = (duración_us * 0.0343) / 2 = duración_us * 0.01715
static const float HCSR04_US_TO_CM = 0.01715f;

inline void hcsrBegin() {
  pinMode(HCSR04_TRIG_PIN, OUTPUT);
  pinMode(HCSR04_ECHO_PIN, INPUT);
  digitalWrite(HCSR04_TRIG_PIN, LOW);
  delay(50);
}

inline float hcsrReadDistanceCm(uint8_t samples = 5) {
  float acc = 0.0f;
  uint8_t ok = 0;

  for (uint8_t i = 0; i < samples; i++) {
    // Disparo TRIG
    digitalWrite(HCSR04_TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(HCSR04_TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(HCSR04_TRIG_PIN, LOW);

    // Medir el pulso HIGH en ECHO 
    unsigned long dur = pulseIn(HCSR04_ECHO_PIN, HIGH, HCSR04_TIMEOUT_US);

    if (dur > 0) {
      float d = dur * HCSR04_US_TO_CM; 
      if (d < 2.0f) d = 2.0f;
      if (d > 400.0f) d = 400.0f;
      acc += d;
      ok++;
    }

    delay(20); // pequeña pausa entre muestras
  }

  if (ok == 0) return NAN;      
  return acc / (float)ok;    
}
