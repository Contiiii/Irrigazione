#include "sensori.h"
#include "config.h"
#include "log.h"
#include "health.h"
#include "motori.h"
#include "telegram.h"

extern bool debug;
extern SystemHealth health;
extern float umiditaSens1, umiditaSens2;
extern int umidita[2];
extern AutoZone az1, az2;
extern String umiditaStatusEmoji(uint8_t);

String umiditaStatusEmoji(int um)
{
  if (um < 20) return "🔴 CRITICA";
  if (um < 30) return "🌵 BASSA";
  if (um < 70) return "🟢 OTTIMALE";
  return "🟣 SATURA";
}

void leggiSensori(int umidita[2])
{
  long sum1 = 0;
  long sum2 = 0;

  for (int i = 0; i < SENSOR_NUM_SAMPLES; i++)
  {
    sum1 += analogRead(Pin_Sensore1);
    sum2 += analogRead(Pin_Sensore2);
    delay(SENSOR_SAMPLE_DELAY_MS); // Piccolo ritardo tra letture
  }

  // ✅ Calcola media
  umidita[0] = sum1 / SENSOR_NUM_SAMPLES;
  umidita[1] = sum2 / SENSOR_NUM_SAMPLES;

  validazioneSensori(umidita[0], umidita[1]);
}

void handleSensore(bool toTelegram)
{
  int umidita[2];

  leggiSensori(umidita);

  int umiditaSens1 = map(umidita[0], SENSOR_DRY_ADC, SENSOR_WET_ADC, 0, 100);
  int umiditaSens2 = map(umidita[1], SENSOR_DRY_ADC, SENSOR_WET_ADC, 0, 100);

  umiditaSens1 = constrain(umiditaSens1, 0, 100);
  umiditaSens2 = constrain(umiditaSens2, 0, 100);

  String msg;
  String p1 = health.sensore1Disconnesso ? "💧 Pianta 1: N/D (sensore off)" : "💧 Pianta 1: " + String(umiditaSens1) + "% " + umiditaStatusEmoji(umiditaSens1) + " (" + String(umidita[0]) + ")";

  String p2 = health.sensore2Disconnesso ? "💧 Pianta 2: N/D (sensore off)" : "💧 Pianta 2: " + String(umiditaSens2) + "% " + umiditaStatusEmoji(umiditaSens2) + " (" + String(umidita[1]) + ")";

  msg = "🌱 SENSORI\n" + p1 + "\n" + p2;

  if (health.sensore1Disconnesso)
    msg += "\n⚠️ Sensore 1 disconnesso";
  if (health.sensore2Disconnesso)
    msg += "\n⚠️ Sensore 2 disconnesso";
  if (health.umiditaCritica)
    msg += "\n🚨 Umidità critica";

  // Mostra blocco sicurezza nel messaggio /sensore
  if (toTelegram)
  {
    if (health.motore1BloccatoSicurezza)
      msg += "\n🚫 Motore 1 BLOCCATO SICUREZZA";
    if (health.motore2BloccatoSicurezza)
      msg += "\n🚫 Motore 2 BLOCCATO SICUREZZA";
  }

  if (!health.sensore1Disconnesso)
  {
    // minima
    if (umiditaSens1 < stats.humMin1)
      stats.humMin1 = umiditaSens1;
    // massima
    if (umiditaSens1 > stats.humMax1)
      stats.humMax1 = umiditaSens1;
    // somma
    stats.humSum1 += umiditaSens1;
    // totali
    stats.humN1 += 1;
  }

  if (!health.sensore2Disconnesso)
  {
    // minima
    if (umiditaSens2 < stats.humMin2)
      stats.humMin2 = umiditaSens2;
    // massima
    if (umiditaSens2 > stats.humMax2)
      stats.humMax2 = umiditaSens2;
    // somma
    stats.humSum2 += umiditaSens2;
    // totali
    stats.humN2 += 1;
  }

  // ====== AUTO con blocco sicurezza + anti-spam ======
  const uint32_t now = millis();

  // cooldown log "richiesta ma bloccato"
  static uint32_t lastBlockedLog1 = 0;
  static uint32_t lastBlockedLog2 = 0;

  const bool mot1On = (offTimeMot1 != 0);
  const bool mot2On = (offTimeMot2 != 0);

  // --- Zona 1 ---
  if (health.motore1BloccatoSicurezza)
  {
    // riallinea stato AUTO se il motore è stato spento dal fail-safe
    if (az1.active && !mot1On)
      az1.active = false;

    // se sarebbe da START, logga ma con cooldown
    const bool wouldStart1 = (!az1.active && !health.sensore1Disconnesso && umiditaSens1 <= az1.startTh);

    if (wouldStart1 && (now - lastBlockedLog1 >= BLOCK_LOG_COOLDOWN_MS))
    {
      logLine(WARN, "🚫 AUTO: Motore 1 bloccato sicurezza (umid=" + String(umiditaSens1) + "%) -> SKIP", true, true);
      lastBlockedLog1 = now;
    }
  }
  else
  {
    // normale auto
    autoTickZone(az1, Motore_1, (uint8_t)umiditaSens1, !health.sensore1Disconnesso);
  }

  // --- Zona 2 ---
  if (health.motore2BloccatoSicurezza)
  {
    if (az2.active && !mot2On)
      az2.active = false;

    const bool wouldStart2 = (!az2.active && !health.sensore2Disconnesso && umiditaSens2 <= az2.startTh);

    if (wouldStart2 && (now - lastBlockedLog2 >= BLOCK_LOG_COOLDOWN_MS))
    {
      logLine(WARN, "🚫 AUTO: Motore 2 bloccato sicurezza (umid=" + String(umiditaSens2) + "%) -> SKIP", true, true);
      lastBlockedLog2 = now;
    }
  }
  else
  {
    autoTickZone(az2, Motore_2, (uint8_t)umiditaSens2, !health.sensore2Disconnesso);
  }

  // ====== Anti-spam log umidità (come tuo) ======
  static int lastLoggedPct1 = -1;
  static int lastLoggedPct2 = -1;
  static uint32_t lastHumLogMs = 0;

  int d1 = (lastLoggedPct1 < 0) ? 999 : abs(umiditaSens1 - lastLoggedPct1);
  int d2 = (lastLoggedPct2 < 0) ? 999 : abs(umiditaSens2 - lastLoggedPct2);

  bool periodic = (now - lastHumLogMs >= HUM_LOG_INTERVAL_MS);
  bool changed = (d1 >= HUM_DELTA_PCT) || (d2 >= HUM_DELTA_PCT);

  bool shouldLog = debug || toTelegram || periodic || changed;

  if (shouldLog)
  {
    logLine(INFO, msg, true, toTelegram);
    lastLoggedPct1 = umiditaSens1;
    lastLoggedPct2 = umiditaSens2;
    lastHumLogMs = now;
  }
}
