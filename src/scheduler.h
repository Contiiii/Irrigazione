#pragma once
#include <Arduino.h>

// Modulo scheduler: funzioni di scheduling e tick temporale.
// Gestisce computeDayId(), dailyResetTick(), nightlyReportTick().

uint32_t computeDayId();
void dailyResetTick(uint32_t nowMs);
void nightlyReportTick(uint32_t nowMs);