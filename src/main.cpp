
// modificata lettura sensori a motori accesi (3s), aggiunto blocco motori quando sensori disconnessi, modificato report notturno
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>
#include "esp_heap_caps.h"
#include <esp_system.h>
#include <esp_wifi.h>

#include "secrets.h"
#include "config.h"
#include "log.h"
#include "telegram.h"
#include "utils.h"
#include "sensori.h"
#include "health.h"
#include "motori.h"
#include "meteo.h"
#include "telnet.h"

// Tipi condivisi - spostati negli header dedicati
/*
enum MotorSel
{
  Motore_1 = 1,
  Motore_2 = 2,
  Entrambi_i_Motori = 3
}; // Selettore motori/zone (1,2,entrambi).

enum IrrigationBlockReason : uint8_t
{
  IRR_OK = 0,
  IRR_RAIN_BLOCK,
  IRR_TOO_SOON,
  IRR_DAY_LIMIT,
  IRR_MOTOR_LOCKED
}; // Motivo blocco irrigazione.
*/

// ========================= CONFIG / COSTANTI =========================

RTC_DATA_ATTR uint32_t bootCounter = 0; // Contatore boot in RTC memory (persistente).


// Tipi condivisi - spostati negli header dedicati
/*
struct DatiMeteo
{
  bool staPiovendo;                  // True se sta piovendo ora (condizione o mm/h).
  String condizioniMeteo;            // Condizione OWM (es. Rain/Clouds/Clear).
  float temperatura;                 // Temperatura attuale (°C).
  float pioggiaUltimaOra;            // Pioggia ultime 1h (mm).
  int umidita;                       // Umidità attuale (%).
  unsigned long ultimoAggiornamento; // millis() ultimo update meteo.
  bool datiValidi;                   // True se i dati meteo sono validi.

  bool forecastValidi = false;         // True se il forecast è valido.
  unsigned long ultimoAggForecast = 0; // millis() ultimo update forecast.
  bool pioggiaPrevista3h = false;      // True se prevista pioggia entro 3h.
  bool pioggiaPrevista6h = false;      // True se prevista pioggia entro 6h.
  float mmPrevisti3h = 0.0f;           // mm previsione entro 3h (slot considerati).
  float mmPrevisti6h = 0.0f;           // mm previsione entro 6h (slot considerati).
};
*/

// struct SystemHealth - spostata in health.h
/*
struct SystemHealth
{
  bool sensore1Disconnesso : 1; // Sensore 1 fuori range/assente.
  bool sensore2Disconnesso : 1; // Sensore 2 fuori range/assente.
  bool umiditaCritica : 1;      // Almeno un vaso in umidità critica.

  bool temperaturaElevata : 1; // Temperatura ESP sopra soglia.

  bool wifiDebole : 1;      // WiFi debole (RSSI basso).
  bool wifiDisconnesso : 1; // WiFi non connesso.

  bool telegramIrraggiungibile : 1; // Telegram offline/backoff/fallimenti.

  bool memoriaInsufficiente : 1; // Heap sotto soglia.
  uint16_t heapFreeKB;           // Heap libera (KB).
  uint16_t heapLargestKB;        // Largest free block (KB).

  bool motore1AttivoTroppoTempo : 1; // Motore 1 oltre MAX_MOTOR_SECONDS.
  bool motore2AttivoTroppoTempo : 1; // Motore 2 oltre MAX_MOTOR_SECONDS.

  bool motore1BloccatoSicurezza = false; // Motore 1 bloccato
  bool motore2BloccatoSicurezza = false; // Motore 2 bloccato

  bool irrigazioniTroppoFrequenti : 1; // Flag "troppo frequente" (se usato nei controlli).

  uint8_t irrigazioniOggiMot1; // Conteggio irrigazioni oggi (motore 1).
  uint8_t irrigazioniOggiMot2; // Conteggio irrigazioni oggi (motore 2).

  unsigned long lastIrrMot1; // millis() ultima irrigazione motore 1.
  unsigned long lastIrrMot2; // millis() ultima irrigazione motore 2.

  int8_t rssi;             // RSSI WiFi in dBm.
  float temperaturaESP32;  // Temperatura ESP32 (°C).
  uint16_t spiffsFreeKB;   // Spazio libero SPIFFS (KB).
  uint8_t irrigazioniOggi; // Totale/placeholder (se lo usi come aggregato).

  unsigned long lastSensorCheck;            // millis() ultimo check sensori.
  unsigned long lastTempCheck;              // millis() ultimo check temperatura.
  unsigned long lastWifiCheck;              // millis() ultimo check WiFi.
  unsigned long lastMemoryCheck;            // millis() ultimo check memoria.
  unsigned long lastIrrigationTime;         // millis() ultima irrigazione (generale).
  uint32_t lastDayReset;                    // Marker day-id/ultimo reset giornaliero.
  unsigned long motore1StartTime;           // millis() inizio motore 1 (runtime).
  unsigned long motore2StartTime;           // millis() inizio motore 2 (runtime).
  unsigned long lastSuccessfulTelegramComm; // millis() ultima comm Telegram OK.

  uint32_t pollDelayMs; // delay corrente
  bool pollBoostActive; // true se sei in boost
};
*/

// struct AutoZone - spostata in motori.h
/*
struct AutoZone
{
  bool active = false;  // True se AUTO ha avviato irrigazione (zona "attiva").
  uint8_t startTh = 25; // %: sotto/uguale -> avvia.
  uint8_t stopTh = 30;  // %: sopra/uguale -> ferma.
};
*/


// ========================= ISTANZE / STATO RUNTIME =========================
MotorSel pendingMotor = Motore_1; // Motore selezionato, in attesa durata.

DatiMeteo meteo;                             // Meteo attuale + forecast.
bool bloccoIrrigazione = false;              // True se irrigazione bloccata (pioggia/forecast).
unsigned long scadenzaBloccoIrrigazione = 0; // millis() scadenza blocco irrigazione.

uint32_t g_nextDayCheckMs = 0; // millis() prossimo check cambio-giorno (rate-limit).

SystemHealth health = {}; // Stato salute (flag+valori misurati).

AutoZone az1, az2;       // Zone auto (vaso 1 / vaso 2).
bool autoEnabled = true; // Abilita/disabilita AUTO globale.

uint32_t offTimeMot1 = 0;
uint32_t offTimeMot2 = 0;

bool manutenzione = false; // Modalità manutenzione (se la usi per bypass).

DailyStats stats; // Statistiche giornaliere. // Statistiche giornaliere.

static uint32_t pollBoostUntilMs = 0; // fino a quando restare in boost
static uint32_t nextPollMs = 0;       // scheduler (al posto di lastTimeBotRan + botRequestDelay)

static bool wifiPsOn = false;
static uint32_t nextWifiPolicyMs = 0;

// ========================= RETE / SERVIZI =========================
const char *ssid = SECRET_WIFI_SSID;     // SSID WiFi (secrets.h).
const char *password = SECRET_WIFI_PASS; // Password WiFi (secrets.h).

String openWeatherMapApiKey = SECRET_API_OPENWEATHER; // API key OpenWeatherMap.

#define BOTtoken SECRET_BOT_TOKEN // Token bot Telegram.
#define CHAT_ID SECRET_CHAT_ID    // Chat ID autorizzato.

WiFiServer telnetServer(TELNET_PORT); // Server Telnet (porta 23).
WiFiClient telnetClient;     // Client Telnet corrente.
String telnetLine;           // Buffer riga comandi Telnet.

// ========================= SCHEDULER TELEGRAM =========================
unsigned long lastTelegramMs = 0;                    // millis() ultimo invio messaggio (anti-spam).

long lastHandledUpdateId = 0;                       // Ultimo update_id gestito (anti-doppio).
uint32_t lastMotorCommandTime = 0;             // millis() ultimo comando motore (debounce).

static uint32_t stateTimeoutMs = 0;               // millis() scadenza attesa risposta durata.

RTC_DATA_ATTR long lastHandledUpdateIdRTC = 0; // Persistente!

// da sistemare
static uint32_t tgLastSendMs = 0;            // ultimo invio OK
static String tgLastPayload = "";            // ultimo messaggio inviato
static uint8_t tgSendCounter = 0;            // per debug duplicati

// ✅ LOCK per evitare che safeGetUpdates() e tgSend() corrano in parallelo (definito in telegram.cpp)

// Prototipi di telnet - già in telnet.h, ma telnetWelcome è static
void handleTelnet();
void handleTelnetCommand(const String &cmd);
void telnetSendTail(const char *path, int maxLines);
void telnetPrintWarnErrorFile(const char *path);
void telnetPrintAllWarnError(bool includeOld);

// Prototipi dei sensori - già in sensori.h
/*
void leggiSensori(int umidita[2]);
void handleSensore(bool toTelegram);
*/

// Prototipi dei motori - NON in header, tenuti qui
void accendiMotori(int who, int tempo);
void spegniMotori(int who);
void autoTickZone(AutoZone &az, MotorSel m, uint8_t humPct, bool sensoreOk);
void armStateTimeout(uint32_t windowMs);


// check sistem Health - già in health.h
void checkTelegramConnection(uint32_t now);
void checkMotori(uint32_t now);

// check irrigazioni - NON in header
uint32_t computeDayId();
void dailyResetTick(uint32_t nowMs);
bool requestIrrigation(MotorSel m, uint16_t seconds, const char *source, IrrigationBlockReason &reason, uint8_t &motBlocked, uint16_t &waitMin, bool ignoreMeteo);

// funzione Helper per log
static inline String boolToEmoji(bool v, bool inverted = false);
static inline String umiditaStatusEmoji(int um);

// statistiche giornaliere
void nightlyReportTick(uint32_t nowMs);
static bool sendNightlyReport();
static void resetDailyStats();


void setup()
{
  bootCounter++;
  Serial.begin(115200);
  delay(200);

  Serial.printf("boot #%u reason = %d\n", bootCounter, (int)esp_reset_reason());

  // logLine(DEBUG_L, "🚀 Boot ESP32...", true, false);

  pinMode(Pin_SensoreContenitore, INPUT_PULLUP);
  pinMode(Pin_Sensore1, INPUT);
  pinMode(Pin_Sensore2, INPUT);
  pinMode(Pin_Relay1, OUTPUT);
  pinMode(Pin_Relay2, OUTPUT);

  // Spengo i motori all'accensione
  digitalWrite(Pin_Relay1, HIGH);
  digitalWrite(Pin_Relay2, HIGH);

  // Avvio wifi
  logLine(DEBUG_L, String("📡 Connessione a ") + ssid, true, false);

  client.setCACert(TELEGRAM_CERTIFICATE_ROOT); // Più robusto del CA

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

WiFi.begin(ssid, password);
uint32_t wifiStart = millis();
while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_BOOT_TIMEOUT_MS)
{
    delay(500);
    logLine(DEBUG_L, ".", false, false);
}
if (WiFi.status() != WL_CONNECTED)
    logLine(WARN, "⚠️ WiFi non disponibile al boot, continuo offline", true, false);

  logLine(DEBUG_L, String("Connesso! IP: ") + WiFi.localIP().toString(), true, false);

  // dopo che il WiFi è connesso
  logLine(DEBUG_L, "🕐 Imposto orario NTP...", true, false);
  configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");

  struct tm t;
  timeReady = getLocalTime(&t, 10000);

  // client.setHandshakeTimeout(7);
  // client.setTimeout(7000);
  bot.waitForResponse = 5000;
  lastHandledUpdateId = lastHandledUpdateIdRTC;

  // Avvio modalita OTA
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.begin();

  // Avvio modalita TELNET
  telnetServer.begin(); // Avvia server Telnet
  telnetServer.setNoDelay(true);
  telnetLine.reserve(128);

  // Log su file
  spiffsOK = SPIFFS.begin(true); // true = formatta se non montabile [web:61]
  if (spiffsOK)
    initLogSize();
  logLine(INFO, String("💾 SPIFFS: ") + boolToEmoji(spiffsOK), true, false);

  logLine(DEBUG_L, "🔌 OTA pronto", true, false);
}

void loop()
{
  uint32_t now = millis(); // 1 sola lettura per giro (coerenza + micro ottimizzazione) [web:21]

  ArduinoOTA.handle(); // se ti serve sempre reattivo, lascialo “sempre” [web:8]

  static uint32_t lastHealth = 0;
  static uint32_t lastSensors = 0;
  static uint32_t lastHour = 0;

  static int hourNow = 12;

  // Spegnimenti motori (evento urgente) + BLOCCO SICUREZZA al timeout
  if (offTimeMot1 && (int32_t)(now - offTimeMot1) >= 0)
  {
    uint32_t runS = 0;
    if (health.motore1StartTime != 0)
      runS = (now - health.motore1StartTime) / 1000UL;

    spegniMotori(1);

    // Se è arrivato al limite, blocca finché non fai sblocca1
    if (runS >= MAX_MOTOR_SECONDS && !health.motore1BloccatoSicurezza)
    {
      health.motore1BloccatoSicurezza = true;
      health.motore1AttivoTroppoTempo = true;
      logLine(ERROR_L, "🚨🚫 MOTORE 1 BLOCCATO (timeout) - Usa sblocca1", true, true);
    }
  }

  if (offTimeMot2 && (int32_t)(now - offTimeMot2) >= 0)
  {
    uint32_t runS = 0;
    if (health.motore2StartTime != 0)
      runS = (now - health.motore2StartTime) / 1000UL;

    spegniMotori(2);

    if (runS >= MAX_MOTOR_SECONDS && !health.motore2BloccatoSicurezza)
    {
      health.motore2BloccatoSicurezza = true;
      health.motore2AttivoTroppoTempo = true;
      logLine(ERROR_L, "🚨🚫 MOTORE 2 BLOCCATO (timeout) - Usa sblocca2", true, true);
    }
  }

  // Telnet: puoi farlo ogni giro o ogni 10–20ms se vuoi alleggerire
  handleTelnet();

  // Healt Cheack
  checkTemperaturaESP32(now);
  checkWiFiSignal(now);
  checkMemory(now);
  checkMotori(now);

  dailyResetTick(now);
  nightlyReportTick(now);

  if (botstate != IDLE && isStateTimeoutExpired())
  {
    resetAskSession();
    tgSend("Richiesta scaduta");
  }

  // Lettura sensori umidità
  uint32_t sensInterval = motorsOnNow() ? SENS_IRR_MS : SENS_BASE_MS;

  if (debug)
    logLine(DEBUG_L, "DBG off1=" + String(offTimeMot1) + " off2=" + String(offTimeMot2) + " " + String(sensInterval), true, false);

  if (now - lastSensors >= sensInterval)
  {
    lastSensors = now;
    handleSensore(false);
  }

  // Ora (hourNow) aggiornata ogni 60s: non serve chiamare getLocalTime “sempre”
  if (timeReady && (now - lastHour >= 60000))
  {
    lastHour = now;
    struct tm t;
    if (getLocalTime(&t, 50))
      hourNow = t.tm_hour;
  }

  // --- Polling Telegram ---
  uint32_t delayMs = currentPollDelayMs(hourNow, now);
  wifiFollowPolling(now, delayMs);

  health.pollDelayMs = delayMs;
  health.pollBoostActive = isBoostedNow(now);

  if ((int32_t)(now - nextPollMs) >= 0)
  {
    int numNewMessages = safeGetUpdates();

    if (numNewMessages > 0)
    {
      boostPolling(BOOST_MSG_MS);

      long maxUid = lastHandledUpdateId;

      for (int i = 0; i < numNewMessages; i++)
      {
        long uid = bot.messages[i].update_id;
        if (uid > maxUid)
          maxUid = uid;

        String type = bot.messages[i].type;
        String text = bot.messages[i].text;
        String chatId = bot.messages[i].chat_id;

        if (type == "message")
        {
          handleMessage(text, chatId, String()); // messageId vuoto
        }
        else if (type == "callback_query")
        {
          handleCallBack(text, chatId, String()); // messageId vuoto
        }
      }

      // ACK di tutti gli update ricevuti (offset = max+1 al giro dopo)
      lastHandledUpdateId = maxUid;
    }

    // Scheduler: prossimo polling
    nextPollMs = now + delayMs;
  }
}

// telnet

// ESTRATTA in telnet.cpp — Fase 4C
void handleTelnet();

// ESTRATTA in telnet.cpp — Fase 4C
void handleTelnetCommand(const String &cmd);

// ESTRATTA in telnet.cpp — Fase 4A
void telnetSendTail(const char *path, int maxLines);

// ESTRATTA in telnet.cpp — Fase 4A
void telnetPrintWarnErrorFile(const char *path);

// ESTRATTA in telnet.cpp — Fase 4B
void telnetPrintAllWarnError(bool includeOld);

// sensori
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

// motori
void accendiMotori(int who, int tempo);
void spegniMotori(int who);
void autoTickZone(AutoZone &az, MotorSel m, uint8_t humPct, bool sensoreOk);
void checkMotori(uint32_t now);


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

bool requestIrrigation(MotorSel m, uint16_t seconds, const char *source, IrrigationBlockReason &reason, uint8_t &motBlocked, uint16_t &waitMin, bool ignoreMeteo);

// funzione Helper per log
static inline String boolToEmoji(bool v, bool inverted)
{
  if (inverted)
    v = !v;
  return v ? "✅" : "❌";
}

static inline String umiditaStatusEmoji(int um)
{
  if (um < 20)
    return "🔴 CRITICA";
  if (um < 30)
    return "🌵 BASSA";
  if (um < 70)
    return "🟢 OTTIMALE";
  return "🟣 SATURA";
}

// statistiche giornaliere
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


/*
Creare controllo livello acqua

riordinare funzione e variabili

sistemare loop e setup

Utilizzare doppio core

watchdog, freertos

yield();
*/
