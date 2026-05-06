#include "telegram.h"
#include "log.h"
#include "config.h"
#include "secrets.h"
#include "health.h"
#include "motori.h"
#include "meteo.h"
#include "utils.h"
#include "sensori.h"
#include <esp_wifi.h>

// Da main.cpp / stats
extern long lastHandledUpdateId;

// ── Istanze ─────────────────────────────────────────────────────────
WiFiClientSecure client;
UniversalTelegramBot bot(SECRET_BOT_TOKEN, client);

// ── Stato bot ───────────────────────────────────────────────────────
BotState botstate = IDLE;
bool motorOperationInProgress = false;

// ── Variabili interne ────────────────────────────────────────────────
static volatile bool tgBusy = false;
static uint32_t tgLastSendMs = 0;
static uint32_t pollBoostUntilMs = 0;
static uint32_t nextPollMs = 0;
static bool wifiPsOn = false;
static uint32_t nextWifiPolicyMs = 0;
static uint32_t stateTimeoutMs = 0;



void handleCallBack(String text, String chatId, String messageId)
{
  const uint32_t now = millis();

  // 1) Scelta tempo: gestiscila SUBITO e qui avvia davvero l'irrigazione
  if (text.startsWith("t_"))
  {
    if (botstate == IDLE || isStateTimeoutExpired())
    {
      resetAskSession();
      return;
    }

    uint16_t seconds = 0;
    if (text == "t_10")
      seconds = 10;
    else if (text == "t_30")
      seconds = 30;
    else if (text == "t_60")
      seconds = 60;
    else
    {
      tgSend("Tempo non valido.");
      resetAskSession();
      botstate = IDLE;
      motorOperationInProgress = false;
      return;
    }

    uint8_t motB = 0;
    uint16_t waitM = 0;
    IrrigationBlockReason rr = IRR_OK;

    const bool ok = requestIrrigation(pendingMotor, seconds, "MANUALE", rr, motB, waitM, true);

    if (!ok)
    {
      if (rr == IRR_TOO_SOON)
        tgSend("⏳ Motore " + motorLabel(motB) + ": attendi ~" + String(waitM) + " min");
      else if (rr == IRR_DAY_LIMIT)
        tgSend("🚫 Motore " + motorLabel(motB) + ": limite 10/giorno raggiunto");
      else if (rr == IRR_RAIN_BLOCK)
        tgSend("🌧️ Irrigazione bloccata (pioggia/blocco)");
      else if (rr == IRR_MOTOR_LOCKED)
        tgSend("🚫 Motore " + motorLabel(motB) + " BLOCCATO SICUREZZA. Usa /sblocca" + motorLabel(motB));
      else
        tgSend("Irrigazione bloccata.");

      resetAskSession();
      return;
    }

    botstate = IDLE;
    motorOperationInProgress = false;
    lastMotorCommandTime = now; // opzionale: evita doppi click subito dopo
    resetAskSession();
    return;
  }

  // 2) Debounce SOLO per la scelta motore
  if ((uint32_t)(now - lastMotorCommandTime) < MOTOR_DEBOUNCE_INTERVAL)
  {
    logLine(WARN, "⏱️ Click ignorato (debounce)", true, false);
    return;
  }

  if (text == "mot1_on")
  {
    if (botstate != IDLE)
      return;
    motorOperationInProgress = true;
    lastMotorCommandTime = now;
    pendingMotor = Motore_1;
    botstate = ASK_TIME_MOT1;
    askTime("motore 1");

    armStateTimeout(STATE_TIMEOUT_WINDOW_MS);
    return;
  }

  if (text == "mot2_on")
  {
    if (botstate != IDLE)
      return;
    motorOperationInProgress = true;
    lastMotorCommandTime = now;
    pendingMotor = Motore_2;
    botstate = ASK_TIME_MOT2;
    askTime("motore 2");

    armStateTimeout(STATE_TIMEOUT_WINDOW_MS);
    return;
  }

  if (text == "mot_all_on")
  {
    if (botstate != IDLE)
      return;
    motorOperationInProgress = true;
    lastMotorCommandTime = now;
    pendingMotor = Entrambi_i_Motori;
    botstate = ASK_TIME_BOTH;
    askTime("entrambi i motori");

    armStateTimeout(STATE_TIMEOUT_WINDOW_MS);
    return;
  }
}

void handleMessage(String text, String chatId, String messageId)
{
  text.trim(); // come già fai [file:188]

  // Sicurezza: rispondi solo alla chat autorizzata
  if (chatId != String(SECRET_CHAT_ID))
  {
    // opzionale: logLine(WARN, "Chat non autorizzata: " + chatId, true, false);
    return;
  }

  // --- NUOVI COMANDI MOTORI ---
  if (text == "/motori")
  {
    String msg;
    msg.reserve(300);

    const bool mot1On = (offTimeMot1 != 0);
    const bool mot2On = (offTimeMot2 != 0);

    msg += "🔧 MOTORI\n";
    msg += "Mot1: ";
    msg += health.motore1BloccatoSicurezza ? "🚫 BLOCCATO" : (mot1On ? "✅ ON" : "⏹️ OFF");
    if (mot1On)
    {
      int32_t remMs = (int32_t)(offTimeMot1 - millis());
      if (remMs > 0)
        msg += " (restano " + String((uint32_t)remMs / 1000UL) + "s)";
    }
    msg += "\n";

    msg += "Mot2: ";
    msg += health.motore2BloccatoSicurezza ? "🚫 BLOCCATO" : (mot2On ? "✅ ON" : "⏹️ OFF");
    if (mot2On)
    {
      int32_t remMs = (int32_t)(offTimeMot2 - millis());
      if (remMs > 0)
        msg += " (restano " + String((uint32_t)remMs / 1000UL) + "s)";
    }
    tgSend(msg);
    return;
  }
  else if (text == "/accendimotori")
  {
    // Se c'è già una "sessione" in corso, evita di creare altre richieste
    if (botstate != IDLE && !isStateTimeoutExpired())
    {
      tgSend("⏳ Operazione già in corso, attendi o riprova.");
      return;
    }

    // Se la vecchia sessione è scaduta, pulisci
    if (botstate != IDLE && isStateTimeoutExpired())
    {
      resetAskSession();
    }

    // Facoltativo: se i motori sono bloccati, avvisa (ma lascia comunque scegliere)
    if (health.motore1BloccatoSicurezza || health.motore2BloccatoSicurezza)
    {
      tgSend("⚠️ Nota: uno o più motori sono BLOCCATI. Usa /sblocca1 /sblocca2.");
    }

    // Inline keyboard: callback_data compatibili con handleCallBack() [file:34]
    String keyboardJson = F(
        "[["
        "{\"text\":\"Motore 1\",\"callback_data\":\"mot1_on\"},"
        "{\"text\":\"Motore 2\",\"callback_data\":\"mot2_on\"}"
        "],"
        "["
        "{\"text\":\"Entrambi\",\"callback_data\":\"mot_all_on\"}"
        "]]");

    bot.sendMessageWithInlineKeyboard(
        SECRET_CHAT_ID,
        "Scegli cosa accendere:",
        "",
        keyboardJson);

    return;
  }
  else if (text == "/sblocca1")
  {
    health.motore1BloccatoSicurezza = false;
    health.motore1AttivoTroppoTempo = false; // reset allarme runtime
    az1.active = false;                      // riallinea auto
    tgSend("🔓 Motore 1 sbloccato.");
    logLine(INFO, "🔓 Motore 1 sbloccato manualmente", true, true);
    return;
  }
  else if (text == "/sblocca2")
  {
    health.motore2BloccatoSicurezza = false;
    health.motore2AttivoTroppoTempo = false; // reset allarme runtime
    az2.active = false;                      // riallinea auto
    tgSend("🔓 Motore 2 sbloccato.");
    logLine(INFO, "🔓 Motore 2 sbloccato manualmente", true, true);
    return;
  }
  else if (text == "/sblocca")
  {
    health.motore1BloccatoSicurezza = false;
    health.motore2BloccatoSicurezza = false;
    health.motore1AttivoTroppoTempo = false;
    health.motore2AttivoTroppoTempo = false;
    az1.active = false;
    az2.active = false;
    tgSend("🔓 Motori sbloccati.");
    logLine(INFO, "🔓 Motori sbloccati manualmente", true, true);
    return;
  }

  // --- I TUOI COMANDI ESISTENTI (uguali) ---
  if (text == "/meteo")
  {
    handleMeteo();
    return;
  }
  if (text == "/updatemeteo")
  {
    meteo.datiValidi = false;
    bool ok = rilevoMeteo();
    tgSend(ok ? "Aggiornamento meteo OK." : "Aggiornamento meteo FALLITO.");
    return;
  }
  if (text == "/sensore")
  {
    handleSensore(true);
    return;
  }
  if (text == "/debug")
  {
    handleDebug();
    return;
  }
  if (text == "/alert")
  {
    tgSend(tailWarnError(20, true));
    return;
  }
  if (text == "/health")
  {
    handleHealth();
    return;
  }
  if (text == "/log")
  {
    tgSend(tailLog(20));
    return;
  }
  if (text == "/clearlog")
  {
    SPIFFS.remove(LOG_FILE);
    tgSend("Log cancellato.");
    return;
  }
  if (text == "/start" || text == "/help")
  {
    String h = "🌿 *Comandi disponibili:*\n\n";
    h += "📊 *Stato*\n";
    h += "/sensore — umidità piante\n";
    h += "/meteo — meteo attuale\n";
    h += "/motori — stato motori\n";
    h += "/health — stato sistema\n";
    h += "/status — tutto insieme\n\n";
    h += "💧 *Irrigazione*\n";
    h += "/accendimotori — avvia irrigazione\n";
    h += "/sblocca1 /sblocca2 /sblocca\n\n";
    h += "⚙️ *Configurazione*\n";
    h += "/auto on|off — abilita/disabilita AUTO\n";
    h += "/soglie1 [start%] [stop%] — soglie zona 1\n";
    h += "/soglie2 [start%] [stop%] — soglie zona 2\n\n";
    h += "🔧 *Sistema*\n";
    h += "/log — ultimi log\n";
    h += "/alert — ultimi warning/error\n";
    h += "/debug — toggle debug\n";
    h += "/updatemeteo — forza aggiornamento meteo\n";
    h += "/clearlog — cancella log";
    tgSend(h);
    return;
  }

  logLine(INFO, String("Comando sconosciuto: ") + text, true, true);
}

bool tgSend(const String &msg)
{
  if (msg.length() == 0 || WiFi.status() != WL_CONNECTED)
    return false;

uint32_t now = millis();
if (now - tgLastSendMs < TELEGRAM_MIN_INTERVAL_MS)
    return false;

tgBusy = true;

  // ✅ NON chiudere se già configurato
  static bool clientConfigured = false;
  if (!clientConfigured)
  {
    client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
    clientConfigured = true;
  }

  bot.waitForResponse = 3000;
  bool success = bot.sendMessage(SECRET_CHAT_ID, msg, "");

  // ✅ Chiudi SOLO se fallisce (per forzare riconnessione)
  if (!success)
  {
    client.stop();
    clientConfigured = false;
  }

  tgBusy = false;

  if (success)
  {
    tgLastSendMs = now;
    health.lastSuccessfulTelegramComm = now;
    health.telegramIrraggiungibile = false;
  }
  else
  {
    health.telegramIrraggiungibile = true;
    logLine(ERROR_L, "tgSend FAILED", true, false);
  }

  return success;
}

void askTime(const String &who)
{
  String keyboardJson = F(
      "[["
      "{\"text\":\"⏱️10s\",\"callback_data\":\"t_10\"},"
      "{\"text\":\"⏱️30s\",\"callback_data\":\"t_30\"}"
      "],"
      "["
      "{\"text\":\"⏱️60s\",\"callback_data\":\"t_60\"}"
      "]]");

  bot.sendMessageWithInlineKeyboard(
      SECRET_CHAT_ID,
      "Quanto tempo per " + who + "?",
      "",
      keyboardJson);
}

bool isNightHour(int h) // Ritorna true se l'ora è nella fascia NOTTE.
{
  // Valori non validi => considera "notte"
  if (h < 0 || h > 23)
    return true;

  const int start = ORA_INIZIO_GIORNO;
  const int end = ORA_FINE_GIORNO;

  // Caso normale: giorno è [start..end]
  if (start <= end)
  {
    return (h < start) || (h > end);
  }

  return (h > end) && (h < start);
}

void boostPolling(uint32_t ms) // Attiva un periodo di polling "boost" (più frequente) per ms millisecondi.
{
  const uint32_t now = millis();
  const uint32_t until = now + ms;

  // Overflow-safe: aggiorna solo se 'until' è dopo l'attuale pollBoostUntilMs
  if ((int32_t)(until - pollBoostUntilMs) > 0)
  {
    pollBoostUntilMs = until;
  }

  nextPollMs = 0;

  nextWifiPolicyMs = 0;
}

uint32_t currentPollDelayMs(int hourNow, uint32_t nowMs) // Restituisce il delay di polling in base all'ora e allo stato boost.
{
  const uint32_t base = isNightHour(hourNow) ? POLL_NIGHT_MS : POLL_DAY_MS;

  // Se siamo ancora entro la finestra boost => usa delay più aggressivo.
  if ((int32_t)(nowMs - pollBoostUntilMs) < 0)
  {
    return POLL_BOOST_MS;
  }
  return base;
}

bool isBoostedNow(uint32_t nowMs) // True se adesso (nowMs) siamo in periodo boost.
{
  return (int32_t)(nowMs - pollBoostUntilMs) < 0;
}

void wifiFollowPolling(uint32_t nowMs, uint32_t delayMs) // Allinea la policy di WiFi power-save con la frequenza di polling
{
  // Rate limit: evita toggle continui (ogni 5s massimo)
  if ((int32_t)(nowMs - nextWifiPolicyMs) < 0)
    return;
  nextWifiPolicyMs = nowMs + 10000UL;

  // Condizioni in cui NON vogliamo mai power-save
  const bool forceFull =
      (!timeReady) || telnetOnNow() || motorsOnNow() || isBoostedNow(nowMs);

  // Power-save solo se connesso e se "stai davvero in modalità notte"
  const bool wantPs =
      (!forceFull) &&
      (WiFi.status() == WL_CONNECTED) &&
      (delayMs >= POLL_NIGHT_MS);

  // Se già nello stato desiderato, non fare nulla
  if (wantPs == wifiPsOn)
    return;

  // Applica policy. (Ideale: controllare ritorno di esp_wifi_set_ps)
  if (wantPs)
  {
    WiFi.setSleep(true);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
  }
  else
  {
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
  }

  wifiPsOn = wantPs;
}

void armStateTimeout(uint32_t windowMs)
{
  stateTimeoutMs = millis() + windowMs;
}

void resetAskSession()
{
  botstate = IDLE;
  motorOperationInProgress = false;
  stateTimeoutMs = 0;
}

bool isStateTimeoutExpired()
{
  return stateTimeoutMs != 0 && (int32_t)(millis() - stateTimeoutMs) >= 0; // overflow-safe
}

int safeGetUpdates()
{
  static uint32_t nextTryMs = 0;
  static uint32_t backoffMs = 1000;
  uint32_t now = millis();

  if (WiFi.status() != WL_CONNECTED)
    return 0;
  if ((int32_t)(now - nextTryMs) < 0)
    return 0;

  // ✅ ASPETTA il lock
  uint32_t lockStart = millis();
  while (tgBusy && (millis() - lockStart < TG_LOCK_TIMEOUT_MS))
  {
    delay(5); // cedi CPU
  }
  if (tgBusy)
  {
    logLine(WARN, "safeGetUpdates: lock timeout, skipping", true, false);
    nextTryMs = now + 1000; // riprova tra 1 secondo
    return 0;
  }

  tgBusy = true; // ← LOCK ACQUISITO

  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  bot.waitForResponse = 3000;
  bot.longPoll = 0;

  int n = bot.getUpdates(lastHandledUpdateId + 1);

  if (n < 0)
  {
    client.stop();
  }

  tgBusy = false; // ← LOCK RILASCIATO

  if (n >= 0)
  {
    health.telegramIrraggiungibile = false;
    backoffMs = 1000;
    nextTryMs = 0;
    return n;
  }

  uint32_t newBackoff = (backoffMs * 2 > 5000UL) ? 5000UL : backoffMs * 2;
  backoffMs = newBackoff;
  nextTryMs = now + newBackoff;
  health.telegramIrraggiungibile = true;
  return 0;
}