#pragma once
#include <Arduino.h>

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

bool requestIrrigation(MotorSel m, uint16_t seconds, const char *source,
                       IrrigationBlockReason &reason, uint8_t &motBlocked,
                       uint16_t &waitMin, bool ignoreMeteo = false);
// motorsOnNow() è definita inline in utils.h
String motorLabel(uint8_t m);
void spegniMotori(uint8_t m);
void checkMotori(uint32_t now);