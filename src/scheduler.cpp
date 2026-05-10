#include <Arduino.h>
#include <time.h>
#include "log.h"
#include "health.h"      // SystemHealth, DailyStats
#include "motori.h"      // MotorSel, offTimeMot1/2, AutoZone
#include "scheduler.h"

// ── forward declarations di tipi definiti in main.cpp ──
struct SystemHealth;
struct DailyStats;

// ── extern delle variabili globali di main.cpp ──
extern bool timeReady;
extern SystemHealth health;
extern DailyStats stats;

// ── forward declaration di funzioni definite in main.cpp ──
bool tgSend(const String &msg);
static bool sendNightlyReport();
static void resetDailyStats();

uint32_t computeDayId()
{
  if (!timeReady)
    return 0;
  struct tm t;
  if (!getLocalTime(&t, 50))
    return 0;
  return (uint32_t)(t.tm_year + 1900) * 400UL + (uint32_t)t.tm_yday;
}

// statistiche giornaliere
void dailyResetTick(uint32_t nowMs)
{
  // Rate-limit: esegui al massimo 1 volta/minuto (overflow-safe)
  if ((int32_t)(nowMs - g_nextDayCheckMs) < 0)
    return;
  g_nextDayCheckMs = nowMs + 60000UL;

  const uint32_t dayId = computeDayId();
  if (dayId == 0)
    return;

  if ((uint32_t)health.lastDayReset == dayId)
    return;

  health.lastDayReset = (unsigned long)dayId;
  health.irrigazioniOggiMot1 = 0;
  health.irrigazioniOggiMot2 = 0;

  logLine(INFO, "🔄🚿 Reset conteggi irrigazioni giornaliere (per motore)", true, false);
}

void nightlyReportTick(uint32_t nowMs)
{
  // Check periodico: normalmente 60s, ma in finestra e dopo fallimento puoi ridurre.
  static uint32_t nextCheckMs = 0;
  static bool lastAttemptFailedInWindow = false;

  if ((int32_t)(nowMs - nextCheckMs) < 0)
    return;

  // Default: un check al minuto
  nextCheckMs = nowMs + 60000UL;

  if (!timeReady)
    return;

  struct tm t;
  if (!getLocalTime(&t, 50))
    return;

  const bool inWindow =
      (t.tm_hour == NIGHT_REPORT_HOUR) &&
      (t.tm_min >= NIGHT_REPORT_MIN_FROM) &&
      (t.tm_min <= NIGHT_REPORT_MIN_TO);

  if (!inWindow)
  {
    lastAttemptFailedInWindow = false; // reset stato retry
    return;
  }

  // Se sono in finestra e l’ultimo tentativo è fallito, riprova più spesso.
  if (lastAttemptFailedInWindow)
  {
    nextCheckMs = nowMs + 15000UL; // retry ogni 15s dentro la finestra
  }

  const uint32_t dayId = computeDayId();
  if (dayId == 0)
    return;

  if (stats.lastReportDayId == dayId)
    return; // già inviato oggi

  // Evita invio mentre irriga: riduce spam e migliora reattività
  if (offTimeMot1 != 0 || offTimeMot2 != 0)
    return;

  const bool ok = sendNightlyReport();
  if (ok)
  {
    resetDailyStats();
    stats.lastReportDayId = dayId; // marca come inviato dopo reset
    lastAttemptFailedInWindow = false;
  }
  else
  {
    lastAttemptFailedInWindow = true;
    logLine(WARN, "⚠️ Report notturno NON inviato (Telegram/WiFi). Riprovo nella finestra.", true, false);
  }
}

static bool sendNightlyReport()
{
  static const size_t TG_MAX = 3000; // margine sotto 4096

  // Buffer statico: niente realloc/fragmentation durante la costruzione del testo.
  static char buf[TG_MAX + 1];
  size_t len = 0;
  buf[0] = '\0';

  auto safePrintf = [&](const char *fmt, ...)
  {
    if (len >= TG_MAX)
      return;

    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + len, (TG_MAX - len) + 1, fmt, args);
    va_end(args);

    if (written <= 0)
      return;

    // vsnprintf ritorna "quanti avrebbe scritto" -> clamp a spazio disponibile
    size_t w = (size_t)written;
    if (w > (TG_MAX - len))
      w = (TG_MAX - len);
    len += w;
    buf[len] = '\0';
  };

  // Medie safe
  const double avg1 = (stats.humN1 > 0) ? ((double)stats.humSum1 / (double)stats.humN1) : 0.0;
  const double avg2 = (stats.humN2 > 0) ? ((double)stats.humSum2 / (double)stats.humN2) : 0.0;

  safePrintf("🌙 REPORT NOTTURNO\n");
  safePrintf("📊 Log: WARN %u | ERROR %u\n", (unsigned)stats.warnCount, (unsigned)stats.errCount);

  // Stato generale (facoltativo ma utile)
  if (health.sensore1Disconnesso || health.sensore2Disconnesso ||
      health.motore1BloccatoSicurezza || health.motore2BloccatoSicurezza)
  {
    safePrintf("\n🧩 Stato:\n");
    if (health.sensore1Disconnesso)
      safePrintf("- Sensore 1: DISCONNESSO\n");
    if (health.sensore2Disconnesso)
      safePrintf("- Sensore 2: DISCONNESSO\n");
    if (health.motore1BloccatoSicurezza)
      safePrintf("- Motore 1: BLOCCATO SICUREZZA\n");
    if (health.motore2BloccatoSicurezza)
      safePrintf("- Motore 2: BLOCCATO SICUREZZA\n");
  }

  safePrintf("\n🌱 Umidità (giorno):\n");
  if (stats.humN1 == 0)
    safePrintf("- P1: ND\n");
  else
    safePrintf("- P1: min %u | avg %.1f | max %u\n",
               (unsigned)stats.humMin1, avg1, (unsigned)stats.humMax1);

  if (stats.humN2 == 0)
    safePrintf("- P2: ND\n");
  else
    safePrintf("- P2: min %u | avg %.1f | max %u\n",
               (unsigned)stats.humMin2, avg2, (unsigned)stats.humMax2);

  safePrintf("\n🚿 Irrigazioni:\n");
  safePrintf("- M1: %u volte (%lus)\n", (unsigned)stats.irrCount1, (unsigned long)stats.irrSec1);
  safePrintf("- M2: %u volte (%lus)\n", (unsigned)stats.irrCount2, (unsigned long)stats.irrSec2);

  safePrintf("\n⛔ Blocchi:\n");
  safePrintf("- Pioggia: %u\n", (unsigned)stats.blockRain);
  safePrintf("- Troppo presto: %u\n", (unsigned)stats.blockTooSoon);
  safePrintf("- Limite giorno: %u\n", (unsigned)stats.blockDayLimit);

  // Aggiungo ultimi warning/error con truncation pulita e nota finale.
  const String tail = tailWarnError(5, false);
  if (tail.length() > 0 && tail != "Nessun WARNING/ERROR nel log.")
  {
    safePrintf("\n⚠️ Ultimi warning/error:\n");

    const char *t = tail.c_str();
    const size_t tlen = tail.length();
    const char *note = "\n...(troncato)";
    const size_t noteLen = strlen(note);

    size_t room = (len < TG_MAX) ? (TG_MAX - len) : 0;
    if (room > 0)
    {
      bool truncated = false;
      size_t take = tlen;
      if (take > room)
      {
        truncated = true;
        take = (room > noteLen) ? (room - noteLen) : room;
      }

      const size_t start = len;
      memcpy(buf + len, t, take);
      len += take;
      buf[len] = '\0';

      if (truncated)
      {
        // taglia a fine riga se possibile
        size_t cut = len;
        for (size_t i = len; i > start; --i)
        {
          if (buf[i - 1] == '\n')
          {
            cut = i - 1;
            break;
          }
        }
        if (cut > start)
        {
          len = cut;
          buf[len] = '\0';
        }

        if ((TG_MAX - len) >= noteLen)
        {
          memcpy(buf + len, note, noteLen);
          len += noteLen;
          buf[len] = '\0';
        }
      }
    }
  }

  // tgSend accetta String: qui fai UNA sola allocazione, a fine costruzione.
  String out;
  out.reserve(len + 1);
  out = buf;
  return tgSend(out);
}

static inline void resetDailyStats()
{
  stats = DailyStats(); // reset totale (richiede costruttori/valori di default)
}
