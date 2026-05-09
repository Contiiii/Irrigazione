#pragma once
#include <Arduino.h>
#include "config.h"
#include "health.h"
#include "log.h"
#include "sensori.h"
#include "utils.h"
#include "telegram.h"
#include "meteo.h"
#include "scheduler.h"

enum MotorSel
{
  Motore_1 = 1,
  Motore_2 = 2,
  Entrambi_i_Motori = 3
};

enum IrrigationBlockReason : uint8_t
{
  IRR_OK = 0,
  IRR_RAIN_BLOCK,
  IRR_TOO_SOON,
  IRR_DAY_LIMIT,
  IRR_MOTOR_LOCKED
};

struct AutoZone
{
  bool active = false;
  uint8_t startTh = 25;
  uint8_t stopTh = 30;
};

extern uint32_t offTimeMot1;
extern uint32_t offTimeMot2;
extern uint32_t lastMotorCommandTime;
extern MotorSel pendingMotor;
extern AutoZone az1, az2;
extern bool autoEnabled;
extern bool bloccoIrrigazione;
extern bool manutenzione;

bool requestIrrigation(MotorSel m, uint16_t seconds, const char *source,
                       IrrigationBlockReason &reason, uint8_t &motBlocked,
                       uint16_t &waitMin, bool ignoreMeteo = false);
// motorsOnNow() e' definita inline in utils.h
String motorLabel(uint8_t m);
void accendiMotori(int who, int tempo);
void spegniMotori(int who);
void autoTickZone(AutoZone &az, MotorSel m, uint8_t humPct, bool sensoreOk);
// armStateTimeout() appartiene a telegram.h, non a questo modulo
void checkMotori(uint32_t now);