#include "motori.h"

void accendiMotori(int who, int tempo)
{
  boostPolling(BOOST_MOTOR_MS);
  const uint32_t now = millis();
  const bool was1On = (offTimeMot1 != 0);
  const bool was2On = (offTimeMot2 != 0);

  if (who == 1)
  {
    if (health.motore1BloccatoSicurezza) return;

    offTimeMot1 = now + (uint32_t)tempo * 1000UL;
    if (!was1On) health.motore1StartTime = now;
    health.motore1AttivoTroppoTempo = false;
    digitalWrite(Pin_Relay1, LOW);
  }
  else if (who == 2)
  {
    if (health.motore2BloccatoSicurezza) return;

    offTimeMot2 = now + (uint32_t)tempo * 1000UL;
    if (!was2On) health.motore2StartTime = now;
    health.motore2AttivoTroppoTempo = false;
    digitalWrite(Pin_Relay2, LOW);
  }
  else if (who == 3)
  {
    if (health.motore1BloccatoSicurezza || health.motore2BloccatoSicurezza) return;

    offTimeMot1 = now + (uint32_t)tempo * 1000UL;
    offTimeMot2 = now + (uint32_t)tempo * 1000UL;
    if (!was1On) health.motore1StartTime = now;
    if (!was2On) health.motore2StartTime = now;
    health.motore1AttivoTroppoTempo = false;
    health.motore2AttivoTroppoTempo = false;
    digitalWrite(Pin_Relay1, LOW);
    digitalWrite(Pin_Relay2, LOW);
  }
}

void spegniMotori(int who)
{
  if (who == 1)
  {
    offTimeMot1 = 0;
    health.motore1StartTime = 0;
    digitalWrite(Pin_Relay1, HIGH);
    az1.active = false;
  }

  if (who == 2)
  {
    offTimeMot2 = 0;
    health.motore2StartTime = 0;
    digitalWrite(Pin_Relay2, HIGH);
    az2.active = false;
  }

  if (who == 3)
  {
    offTimeMot1 = 0;
    health.motore1StartTime = 0;
    digitalWrite(Pin_Relay1, HIGH);
    az1.active = false;

    offTimeMot2 = 0;
    health.motore2StartTime = 0;
    digitalWrite(Pin_Relay2, HIGH);
    az2.active = false;
  }
}

void autoTickZone(AutoZone &az, MotorSel m, uint8_t humPct, bool sensoreOk)
{
  if (!autoEnabled)
    return;

  // Sensore disconnesso mentre il motore è attivo in AUTO → spegni subito
  if (!sensoreOk)
  {
    if (az.active)
    {
      az.active = false;
      spegniMotori((int)m);
      logLine(WARN, "⚠️ AUTO STOP motore " + motorLabel((int)m) + ": sensore disconnesso", true, true);
    }
    return;
  }

  if (!irrigazioneConsentita())
  {
    // Blocco meteo durante irrigazione AUTO → spegni
    if (az.active)
    {
      az.active = false;
      spegniMotori((int)m);
      logLine(WARN, "🌧️ AUTO STOP motore " + motorLabel((int)m) + ": irrigazione non consentita", true, true);
    }
    return;
  }

  const bool motOn = (m == Motore_1) ? (offTimeMot1 != 0) : (offTimeMot2 != 0);

  // Motore acceso da fonte esterna (manuale) → non interferire
  if (motOn && !az.active)
    return;

  // Motore spento esternamente mentre AUTO era attiva → riallinea
  if (az.active && !motOn)
  {
    az.active = false;
    logLine(WARN, "AUTO: motore " + motorLabel((int)m) + " spento esternamente", true, true);
    return;
  }

  // Umidità sotto soglia → avvia irrigazione
  if (!az.active && humPct <= az.startTh)
  {
    uint8_t motB = 0;
    uint16_t waitM = 0;
    IrrigationBlockReason rr = IRR_OK;

    if (!requestIrrigation(m, MAX_MOTOR_SECONDS, "AUTO", rr, motB, waitM, false))
    {
      logLine(WARN, "⛔ AUTO BLOCCATA mot=" + motorLabel(m) + " reason=" + String((int)rr), true, true);
      return;
    }

    az.active = true;
    return;
  }

  // Umidità sopra soglia stop → ferma irrigazione
  if (az.active && humPct >= az.stopTh)
  {
    az.active = false;
    spegniMotori((int)m);
    logLine(INFO, "⏸️ AUTO STOP motore " + motorLabel(m) + " umid=" + String(humPct) + "%", true, true);
    return;
  }
}

void checkMotori(uint32_t now)
{
  static uint32_t lastCheck = 0;
  if (now - lastCheck < (uint32_t)CHECK_MOTOR * 1000UL)
    return;
  lastCheck = now;

  const bool mot1On = (offTimeMot1 != 0);
  const bool mot2On = (offTimeMot2 != 0);

  if (mot1On && health.motore1StartTime == 0)
    health.motore1StartTime = now;
  if (mot2On && health.motore2StartTime == 0)
    health.motore2StartTime = now;

  if (mot1On && health.motore1StartTime != 0)
  {
    const uint32_t runS = (now - health.motore1StartTime) / 1000UL;
    if (runS > MAX_MOTOR_SECONDS)
    {
      if (!health.motore1BloccatoSicurezza)
      {
        health.motore1BloccatoSicurezza = true;
        health.motore1AttivoTroppoTempo = true;
        spegniMotori(1);
        logLine(ERROR_L, "🚨🚫 MOTORE 1 BLOCCATO (>300s) - MANUAL /sblocca1 TO RESET", true, true);
      }
      return;
    }
  }

  if (mot2On && health.motore2StartTime != 0)
  {
    const uint32_t runS = (now - health.motore2StartTime) / 1000UL;
    if (runS > MAX_MOTOR_SECONDS)
    {
      if (!health.motore2BloccatoSicurezza)
      {
        health.motore2BloccatoSicurezza = true;
        health.motore2AttivoTroppoTempo = true;
        spegniMotori(2);
        logLine(ERROR_L, "🚨🚫 MOTORE 2 BLOCCATO (>300s) - MANUAL /sblocca2 TO RESET", true, true);
      }
      return;
    }
  }

  if (!mot1On && health.motore1StartTime != 0)
  {
    health.motore1StartTime = 0;
    if (!health.motore1BloccatoSicurezza)
      health.motore1AttivoTroppoTempo = false;
  }

  if (!mot2On && health.motore2StartTime != 0)
  {
    health.motore2StartTime = 0;
    if (!health.motore2BloccatoSicurezza)
      health.motore2AttivoTroppoTempo = false;
  }
}

static inline bool motorIsOn(uint8_t mot)
{
  if (mot == 1)
    return offTimeMot1 != 0;
  if (mot == 2)
    return offTimeMot2 != 0;
  return false;
}

static inline uint8_t *todayCountPtr(uint8_t mot)
{
  if (mot == 1)
    return &health.irrigazioniOggiMot1;
  if (mot == 2)
    return &health.irrigazioniOggiMot2;
  return nullptr;
}

static inline unsigned long *lastIrrPtr(uint8_t mot)
{
  if (mot == 1)
    return &health.lastIrrMot1;
  if (mot == 2)
    return &health.lastIrrMot2;
  return nullptr;
}

static bool checkOneMotorGate(uint8_t mot, uint32_t nowMs, IrrigationBlockReason &reason, uint16_t &waitMin)
{
  waitMin = 0;

  uint8_t *todayCnt = todayCountPtr(mot);
  unsigned long *lastIrr = lastIrrPtr(mot);
  if (!todayCnt || !lastIrr)
  {
    reason = IRR_MOTOR_LOCKED;
    return false;
  }

  if (*todayCnt >= MAX_IRRIGATIONS_DAY)
  {
    reason = IRR_DAY_LIMIT;
    return false;
  }

  const uint32_t last = (uint32_t)(*lastIrr);
  if (last != 0 && (uint32_t)(nowMs - last) < MIN_IRRIGATION_MS)
  {
    reason = IRR_TOO_SOON;
    const uint32_t elapsed = (uint32_t)(nowMs - last);
    const uint32_t remMs = MIN_IRRIGATION_MS - elapsed;
    waitMin = (uint16_t)((remMs + 60000UL - 1) / 60000UL);
    return false;
  }

  return true;
}

bool requestIrrigation(MotorSel m, uint16_t seconds, const char *source, IrrigationBlockReason &reason, uint8_t &motBlocked, uint16_t &waitMin, bool ignoreMeteo)
{
  reason = IRR_OK;
  motBlocked = 0;
  waitMin = 0;

  if (seconds == 0)
    return false;
  if (seconds > MAX_MOTOR_SECONDS)
    seconds = MAX_MOTOR_SECONDS;

  if ((m == Motore_1 || m == Entrambi_i_Motori) && health.motore1BloccatoSicurezza)
  {
    reason = IRR_MOTOR_LOCKED;
    motBlocked = 1;
    return false;
  }
  if ((m == Motore_2 || m == Entrambi_i_Motori) && health.motore2BloccatoSicurezza)
  {
    reason = IRR_MOTOR_LOCKED;
    motBlocked = 2;
    return false;
  }

  boostPolling(BOOST_IRR_MS);
  const uint32_t now = millis();
  dailyResetTick(now);

  if (!irrigazioneConsentita() && !ignoreMeteo)
  {
    reason = IRR_RAIN_BLOCK;
    stats.blockRain++;
    return false;
  }

  const bool start1 = (m == Motore_1 || m == Entrambi_i_Motori) && !motorIsOn(1);
  const bool start2 = (m == Motore_2 || m == Entrambi_i_Motori) && !motorIsOn(2);

  if (start1)
  {
    if (!checkOneMotorGate(1, now, reason, waitMin))
    {
      motBlocked = 1;
      if (reason == IRR_TOO_SOON)
        stats.blockTooSoon++;
      else if (reason == IRR_DAY_LIMIT)
        stats.blockDayLimit++;
      return false;
    }
  }
  if (start2)
  {
    if (!checkOneMotorGate(2, now, reason, waitMin))
    {
      motBlocked = 2;
      if (reason == IRR_TOO_SOON)
        stats.blockTooSoon++;
      else if (reason == IRR_DAY_LIMIT)
        stats.blockDayLimit++;
      return false;
    }
  }

  accendiMotori((int)m, (int)seconds);

  if (start1)
  {
    uint8_t *todayCnt1 = todayCountPtr(1);
    unsigned long *lastIrr1 = lastIrrPtr(1);
    if (todayCnt1 && lastIrr1)
    {
      (*todayCnt1)++;
      (*lastIrr1) = now;
    }
    stats.irrCount1++;
    stats.irrSec1 += (uint32_t)seconds;
  }
  if (start2)
  {
    uint8_t *todayCnt2 = todayCountPtr(2);
    unsigned long *lastIrr2 = lastIrrPtr(2);
    if (todayCnt2 && lastIrr2)
    {
      (*todayCnt2)++;
      (*lastIrr2) = now;
    }
    stats.irrCount2++;
    stats.irrSec2 += (uint32_t)seconds;
  }

  char line[220];
  snprintf(line, sizeof(line),
           "🚿▶️ IRR START %s m=%s c1=%u c2=%u",
           source ? source : "?", motorLabel((int)m).c_str(),
           (unsigned)health.irrigazioniOggiMot1,
           (unsigned)health.irrigazioniOggiMot2);
  logLine(INFO, String(line), true, true);

  return true;
}