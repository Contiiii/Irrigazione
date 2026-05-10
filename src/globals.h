#pragma once
#include <Arduino.h>

// ========================= ENUM =========================

enum BotState { IDLE, ASK_TIME_MOT1, ASK_TIME_MOT2, ASK_TIME_BOTH };
enum LogLevel  { INFO, DEBUG_L, WARN, ERROR_L };
enum MotorSel  { Motore_1 = 1, Motore_2 = 2, Entrambi_i_Motori = 3 };

enum IrrigationBlockReason : uint8_t {
  IRR_OK = 0,
  IRR_RAIN_BLOCK,
  IRR_TOO_SOON,
  IRR_DAY_LIMIT,
  IRR_MOTOR_LOCKED
};

// ========================= COSTANTI =========================

extern const uint16_t MAX_MOTOR_SECONDS;
extern const uint32_t MIN_IRRIGATION_MS;
extern const uint8_t  MAX_IRRIGATIONS_DAY;
extern const uint8_t  UMIDITA_CRITICA;
extern const uint16_t SENSOR_LOW;
extern const uint16_t SENSOR_HIGH;
extern const float    soglia_Minima_Pioggia;
extern const int      ora_Inizio_Giorno;
extern const int      ora_Fine_Giorno;

// ========================= STRUTTURE =========================

struct DatiMeteo {
  bool staPiovendo;
  String condizioniMeteo;
  float temperatura;
  float pioggiaUltimaOra;
  int umidita;
  unsigned long ultimoAggiornamento;
  bool datiValidi;

  bool forecastValidi        = false;
  unsigned long ultimoAggForecast = 0;
  bool pioggiaPrevista3h     = false;
  bool pioggiaPrevista6h     = false;
  float mmPrevisti3h         = 0.0f;
  float mmPrevisti6h         = 0.0f;
};

struct SystemHealth {
  bool sensore1Disconnesso : 1;
  bool sensore2Disconnesso : 1;
  bool umiditaCritica      : 1;
  bool temperaturaElevata  : 1;
  bool wifiDebole          : 1;
  bool wifiDisconnesso     : 1;
  bool telegramIrraggiungibile : 1;
  bool memoriaInsufficiente    : 1;

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

  int8_t  rssi;
  float   temperaturaESP32;
  uint16_t spiffsFreeKB;
  uint8_t irrigazioniOggi;

  unsigned long lastSensorCheck;
  unsigned long lastTempCheck;
  unsigned long lastWifiCheck;
  unsigned long lastMemoryCheck;
  unsigned long lastIrrigationTime;
  uint32_t      lastDayReset;
  unsigned long motore1StartTime;
  unsigned long motore2StartTime;
  unsigned long lastSuccessfulTelegramComm;

  uint32_t pollDelayMs;
  bool     pollBoostActive;
};

struct AutoZone {
  bool    active   = false;
  uint8_t startTh  = 25;
  uint8_t stopTh   = 30;
};

struct DailyStats {
  uint16_t warnCount = 0;
  uint16_t errCount  = 0;

  uint16_t humMin1 = 101, humMax1 = 0;
  uint32_t humSum1 = 0;
  uint16_t humN1   = 0;

  uint16_t humMin2 = 101, humMax2 = 0;
  uint32_t humSum2 = 0;
  uint16_t humN2   = 0;

  uint16_t irrCount1 = 0, irrCount2 = 0;
  uint32_t irrSec1   = 0, irrSec2   = 0;

  uint16_t blockRain = 0, blockTooSoon = 0, blockDayLimit = 0;

  uint32_t lastReportDayId = 0;
};

// ========================= VARIABILI GLOBALI (extern) =========================

extern bool          timeReady;
extern bool          spiffsOK;
extern bool          debug;

extern SystemHealth  health;
extern DailyStats    stats;
extern DatiMeteo     meteo;

extern AutoZone      az1, az2;
extern bool          autoEnabled;

extern unsigned long offTimeMot1;
extern unsigned long offTimeMot2;

extern bool          bloccoIrrigazione;
extern unsigned long scadenzaBloccoIrrigazione;

extern BotState      botstate;
extern MotorSel      pendingMotor;

// ========================= FUNZIONI (forward decl) =========================

// log.h copre: logLine, tailLog, tailWarnError
// Qui solo quelle necessarie ai moduli secondari:

bool   tgSend(const String &msg);
void   spegniMotori(int who);
void   accendiMotori(int who, int tempo);
bool   irrigazioneConsentita();
void   boostPolling(uint32_t ms);  // definita in main.cpp come static inline — se serve extern, rimuovi static inline
String motorLabel(uint8_t m);
