#pragma once
#include <Arduino.h>

struct SystemHealth
{
  bool sensore1Disconnesso : 1;
  bool sensore2Disconnesso : 1;
  bool umiditaCritica : 1;
  bool temperaturaElevata : 1;
  bool wifiDebole : 1;
  bool wifiDisconnesso : 1;
  bool telegramIrraggiungibile : 1;
  bool memoriaInsufficiente : 1;
  uint16_t heapFreeKB;
  uint16_t heapLargestKB;
  bool motore1AttivoTroppoTempo : 1;
  bool motore2AttivoTroppoTempo : 1;
  bool motore1BloccatoSicurezza = false;
  bool motore2BloccatoSicurezza = false;
  bool irrigazioniTroppoFrequenti : 1;
  uint8_t irrigazioniOggiMot1;
  uint8_t irrigazioniOggiMot2;
  unsigned long lastIrrMot1;
  unsigned long lastIrrMot2;
  int8_t rssi;
  float temperaturaESP32;
  uint16_t spiffsFreeKB;
  unsigned long lastIrrigationTime;
  uint32_t lastDayReset;
  unsigned long motore1StartTime;
  unsigned long motore2StartTime;
  unsigned long lastSuccessfulTelegramComm;
  uint32_t pollDelayMs;
  bool pollBoostActive;
};

extern SystemHealth health;

void handleHealth();
void checkTemperaturaESP32(uint32_t now);
void checkMemory(uint32_t now);
void checkWiFiSignal(uint32_t now);