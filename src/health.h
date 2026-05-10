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

struct DailyStats
{
  uint16_t warnCount = 0; // Warning accumulati oggi.
  uint16_t errCount = 0;  // Errori accumulati oggi.

  uint16_t humMin1 = 101, humMax1 = 0; // Min/max umidità % vaso 1.
  uint32_t humSum1 = 0;                // Somma umidità % vaso 1 (media).
  uint16_t humN1 = 0;                  // Numero campioni vaso 1.

  uint16_t humMin2 = 101, humMax2 = 0; // Min/max umidità % vaso 2.
  uint32_t humSum2 = 0;                // Somma umidità % vaso 2.
  uint16_t humN2 = 0;                  // Numero campioni vaso 2.

  uint16_t irrCount1 = 0, irrCount2 = 0; // Numero irrigazioni oggi motore 1/2.
  uint32_t irrSec1 = 0, irrSec2 = 0;     // Secondi irrigati oggi motore 1/2.

  uint16_t blockRain = 0, blockTooSoon = 0, blockDayLimit = 0; // Conteggio blocchi per causa.

  uint32_t lastReportDayId = 0; // DayId ultimo report notturno inviato.
};

extern DailyStats stats;

void handleHealth();
void checkTemperaturaESP32(uint32_t now);
void checkMemory(uint32_t now);
void checkWiFiSignal(uint32_t now);

