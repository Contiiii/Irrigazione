
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
#include "sensori.h"
// Tipi condivisi - spostati negli header dedicati

// ========================= CONFIG / COSTANTI =========================

RTC_DATA_ATTR uint32_t bootCounter = 0; // Contatore boot in RTC memory (persistente).



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

// motori
void accendiMotori(int who, int tempo);
void spegniMotori(int who);
void autoTickZone(AutoZone &az, MotorSel m, uint8_t humPct, bool sensoreOk);
void checkMotori(uint32_t now);




bool requestIrrigation(MotorSel m, uint16_t seconds, const char *source, IrrigationBlockReason &reason, uint8_t &motBlocked, uint16_t &waitMin, bool ignoreMeteo);

// funzione Helper per log
static inline String boolToEmoji(bool v, bool inverted)
{
  if (inverted)
    v = !v;
  return v ? "✅" : "❌";
}



/*
Creare controllo livello acqua

riordinare funzione e variabili

sistemare loop e setup

Utilizzare doppio core

watchdog, freertos

yield();
*/
