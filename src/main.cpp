
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
             // millis() scadenza attesa risposta durata.

RTC_DATA_ATTR long lastHandledUpdateIdRTC = 0; // Persistente!

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
  uint32_t now = millis();

  ArduinoOTA.handle();

  static uint32_t lastSensors = 0;
  static uint32_t lastHour    = 0;
  static int      hourNow     = 12;

  // Spegnimento motori + blocco sicurezza
  if (offTimeMot1 && (int32_t)(now - offTimeMot1) >= 0)
  {
    uint32_t runS = 0;
    if (health.motore1StartTime != 0)
      runS = (now - health.motore1StartTime) / 1000UL;
    spegniMotori(1);
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

  handleTelnet();

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

  // Sensori umidità
  uint32_t sensInterval = motorsOnNow() ? SENS_IRR_MS : SENS_BASE_MS;
  if (debug)
    logLine(DEBUG_L, "DBG off1=" + String(offTimeMot1) + " off2=" + String(offTimeMot2) + " " + String(sensInterval), true, false);
  if (now - lastSensors >= sensInterval)
  {
    lastSensors = now;
    handleSensore(false);
  }

  // Ora locale (aggiornata ogni 60s)
  if (timeReady && (now - lastHour >= 60000))
  {
    lastHour = now;
    struct tm t;
    if (getLocalTime(&t, 50))
      hourNow = t.tm_hour;
  }

  // Polling Telegram (tutto dentro telegram.cpp)
  handleTelegramPolling(now, hourNow);
}