#pragma once
#include <Arduino.h>
#include "log.h"
#include "scheduler.h"

// Funzioni scheduler - estratte da main.cpp (Fase 5)

uint32_t computeDayId()
{
  if (!timeReady)
    return 0;
  struct tm t;
  if (!getLocalTime(&t, 50))
    return 0;
  return (uint32_t)(t.tm_year + 1900) * 400UL + (uint32_t)t.tm_yday;
}