#pragma once
#include <Arduino.h>
#include <math.h>

#define NTC_ADC_PIN 34
#define ADC_SAMPLES 8

// Calibración del senosor
static const float CAL_T1 = 0.0f;   
static const float CAL_T2 = 80.0f;  
static const float adcT1  = 3149.0f; 
static const float adcT2  = 462.0f;  

inline void ntcBegin() { pinMode(NTC_ADC_PIN, INPUT); }

inline float ntcReadCelsius(uint8_t samples = ADC_SAMPLES) {
  // 1) Promediar lectura de valores
  uint32_t acc = 0;
  for (uint8_t i = 0; i < samples; i++) {
    acc += analogRead(NTC_ADC_PIN);
    delay(2);
  }
  float adc = acc / (float)samples;
  float spanAdc = (adcT2 - adcT1);       
  if (fabsf(spanAdc) < 1e-6f) return NAN; // evita división por cero

  float slope = (CAL_T2 - CAL_T1) / spanAdc; 
  float t = (adc - adcT1) * slope + CAL_T1;

  // 3) Limitar al rango del sensor (0 - 80) °C
  if (t < CAL_T1) t = CAL_T1;
  if (t > CAL_T2) t = CAL_T2;

  return t;
}
