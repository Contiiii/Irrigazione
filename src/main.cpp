
// modificata lettura sensori a motori accesi (3s), aggiunto blocco motori quando sensori disconnessi, modificato report notturno
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include <UniversalTelegramBot.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>
#include <math.h>
#include "esp_heap_caps.h"
#include <esp_system.h>
#include <esp_wifi.h>

#include "secrets.h"

// Pin utilizzati
#define Pin_SensoreContenitore 18 // 18
#define Pin_Sensore1 34         // 21
#define Pin_Sensore2 33       // 19
#define Pin_Relay1 35            // 32
#define Pin_Relay2 32            // 33

// ========================= ENUM (stati/cause) =========================
enum BotState
{
  IDLE,
  ASK_TIME_MOT1,
  ASK_TIME_MOT2,
  ASK_TIME_BOTH
}; // Stato “conversazione” Telegram (idle/attesa durata).

enum LogLevel
{
  INFO,
  DEBUG_L,
  WARN,
  ERROR_L
}; // Livello severità log.

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

// ========================= CONFIG / COSTANTI =========================
const size_t MAX_LOG_SIZE = 400 * 1024;                         // Dimensione max /log.txt prima della rotazione.
const unsigned long DurataBloccoPioggia = 1000UL * 60UL * 60UL; // Durata blocco standard “piove ora” (1h).

const unsigned long intervallo_Refresh_Giorno = 1000UL * 60UL * 60UL;      // Refresh meteo/forecast di giorno (1h).
const unsigned long intervallo_Refresh_Notte = 1000UL * 60UL * 60UL * 3UL; // Refresh meteo/forecast di notte (3h).
const unsigned long durata_Cash_Valida = 1000UL * 60UL * 30UL;             // Validità cache meteo (30 min).
const unsigned long durataCacheForecast = 1000UL * 60UL * 30UL;            // Validità cache forecast (30 min).
const float soglia_Minima_Pioggia = 0.5f;                                  // Soglia mm per considerare “pioggia rilevante”.

const int ora_Inizio_Giorno = 6; // Ora inizio fascia “giorno”.
const int ora_Fine_Giorno = 23;  // Ora fine fascia “giorno”.

const int8_t RSSI_DEBOLE = -78;     // Soglia RSSI “debole” (dBm).
const int8_t RSSI_CRITICO = -85;    // Soglia RSSI “critico” (dBm).
const float TEMP_WARNING = 75.0f;   // Soglia warning temperatura ESP32 (°C).
const float TEMP_CRITICAL = 85.0f;  // Soglia critica temperatura ESP32 (°C).
const uint8_t UMIDITA_CRITICA = 15; // Soglia umidità (%) per allarme “critica”.

const uint16_t MAX_MOTOR_SECONDS = 120;     // Massima durata continua motore (fail-safe).
const uint32_t MIN_IRRIGATION_MS = 30000UL; // Distanza minima tra irrigazioni dello stesso motore (rate-limit).
const uint8_t MAX_IRRIGATIONS_DAY = 10;     // Max irrigazioni/giorno per motore.

const uint16_t MIN_FREE_KB = 50;   // Soglia heap minima (KB) per allarme memoria.
const uint16_t SENSOR_LOW = 500;   // Min ADC plausibile sensore (sotto = errore/disconnesso).
const uint16_t SENSOR_HIGH = 4000; // Max ADC plausibile sensore (sopra = errore/disconnesso).

const int ADC_DRY = 3300; // ADC sensore completamente asciutto
const int ADC_WET = 1050; // ADC sensore completamente bagnato

// Intervalli check (secondi)
const uint8_t CHECK_TEMP = 30UL;       // Ogni quanto controllare temperatura ESP32.
const uint8_t CHECK_WIFI = 10UL;       // Ogni quanto controllare WiFi/RSSI.
const uint16_t CHECK_MEMORY = 300UL;   // Ogni quanto controllare heap/SPIFFS.
const uint8_t CHECK_MOTOR = 5UL;       // Ogni quanto controllare durata motori.
const uint8_t CHECK_TELEGRAM = 60UL;   // Ogni quanto gestire check Telegram (se usato).
const uint32_t SENS_BASE_MS = 20000UL; // Ogni quanto leggere sensori umidità a motori spenti.
const uint32_t SENS_IRR_MS = 3000UL;   // Ogni quanto leggere sensori umidità a motori accesi.

RTC_DATA_ATTR uint32_t bootCounter = 0; // Contatore boot in RTC memory (persistente).

// Polling Telegram adattivo
static const uint32_t POLL_DAY_MS = 5000;
static const uint32_t POLL_NIGHT_MS = 20000;
static const uint32_t POLL_BOOST_MS = 3000;    // quanto spesso durante boost (reattivo)
static const uint32_t BOOST_MSG_MS = 120000;   // 2 min dopo msg Telegram
static const uint32_t BOOST_MOTOR_MS = 300000; // 5 min quando motori ON
static const uint32_t BOOST_TELNET_MS = 60000; // 1 min dopo connessione telnet (poi rinnovi se resta attivo)
static const uint32_t BOOST_IRR_MS = 180000;   // 3 min se irrigazione richiesta/partita

// ========================= STRUTTURE DATI =========================
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

  bool irrigazioniTroppoFrequenti : 1; // Flag “troppo frequente” (se usato nei controlli).

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

struct AutoZone
{
  bool active = false;  // True se AUTO ha avviato irrigazione (zona “attiva”).
  uint8_t startTh = 25; // %: sotto/uguale -> avvia.
  uint8_t stopTh = 30;  // %: sopra/uguale -> ferma.
};

struct DailyStats
{
  uint16_t warnCount = 0; // Warning accumulati oggi.
  uint16_t errCount = 0;  // Errori accumulati oggi.

  uint16_t humMin1 = 101, humMax1 = 0; // Min/max umidità % vaso 1.
  uint32_t humSum1 = 0;                // Somma umidità % vaso 1 (media).
  uint16_t humN1 = 0;                  // Numero campioni vaso 1.

  uint16_t humMin2 = 101, humMax2 = 0; // Min/max umidità % vaso 2.
  uint32_t humSum2 = 0;                // Somma umidità % vaso 2.
  uint16_t humN2 = 0;                  // Numero campioni vaso 2.

  uint16_t irrCount1 = 0, irrCount2 = 0; // Numero irrigazioni oggi motore 1/2.
  uint32_t irrSec1 = 0, irrSec2 = 0;     // Secondi irrigati oggi motore 1/2.

  uint16_t blockRain = 0, blockTooSoon = 0, blockDayLimit = 0; // Conteggio blocchi per causa.

  uint32_t lastReportDayId = 0; // DayId ultimo report notturno inviato.
};

// ========================= ISTANZE / STATO RUNTIME =========================
BotState botstate = IDLE;         // Stato bot (idle/attesa durata).
MotorSel pendingMotor = Motore_1; // Motore selezionato, in attesa durata.

bool timeReady = false; // Ora NTP valida (per timestamp log).
bool spiffsOK = false;  // SPIFFS montato OK.

size_t logBytes = 0; // Byte già scritti nel log corrente (rotazione).

DatiMeteo meteo;                             // Meteo attuale + forecast.
bool bloccoIrrigazione = false;              // True se irrigazione bloccata (pioggia/forecast).
unsigned long scadenzaBloccoIrrigazione = 0; // millis() scadenza blocco irrigazione.

static uint32_t g_nextDayCheckMs = 0; // millis() prossimo check cambio-giorno (rate-limit).

SystemHealth health = {}; // Stato salute (flag+valori misurati).

AutoZone az1, az2;       // Zone auto (vaso 1 / vaso 2).
bool autoEnabled = true; // Abilita/disabilita AUTO globale.

unsigned long offTimeMot1 = 0; // millis() spegnimento programmato motore 1 (0=spento).
unsigned long offTimeMot2 = 0; // millis() spegnimento programmato motore 2 (0=spento).

bool manutenzione = false; // Modalità manutenzione (se la usi per bypass).
bool debug = false;        // Abilita log DEBUG_L.

static DailyStats stats; // Statistiche giornaliere.

static const uint8_t NIGHT_REPORT_HOUR = 3;     // Ora invio report notturno.
static const uint8_t NIGHT_REPORT_MIN_FROM = 0; // Minuto inizio finestra report.
static const uint8_t NIGHT_REPORT_MIN_TO = 59;  // Minuto fine finestra report.

static uint32_t pollBoostUntilMs = 0; // fino a quando restare in boost
static uint32_t nextPollMs = 0;       // scheduler (al posto di lastTimeBotRan + botRequestDelay)

static bool wifiPsOn = false;
static uint32_t nextWifiPolicyMs = 0;

// ========================= RETE / SERVIZI =========================
const char *ssid = SECRET_WIFI_SSID;     // SSID WiFi (secrets.h).
const char *password = SECRET_WIFI_PASS; // Password WiFi (secrets.h).

String openWeatherMapApiKey = SECRET_API_OPENWEATHER; // API key OpenWeatherMap.
String city = "Vernasca,IT";                          // Città per chiamate meteo.

#define BOTtoken SECRET_BOT_TOKEN // Token bot Telegram.
#define CHAT_ID SECRET_CHAT_ID    // Chat ID autorizzato.

WiFiClientSecure client;                    // Client TLS per Telegram.
UniversalTelegramBot bot(BOTtoken, client); // Istanza bot Telegram.

WiFiServer telnetServer(23); // Server Telnet (porta 23).
WiFiClient telnetClient;     // Client Telnet corrente.
String telnetLine;           // Buffer riga comandi Telnet.

// ========================= SCHEDULER TELEGRAM =========================
unsigned long lastTelegramMs = 0;                    // millis() ultimo invio messaggio (anti-spam).
const unsigned long TELEGRAM_MIN_INTERVAL_MS = 1200; // ms min tra sendMessage.

long lastHandledUpdateId = 0;                       // Ultimo update_id gestito (anti-doppio).
unsigned long lastMotorCommandTime = 0;             // millis() ultimo comando motore (debounce).
const unsigned long MOTOR_DEBOUNCE_INTERVAL = 2000; // ms debounce pulsanti inline.

bool motorOperationInProgress = false;            // True se “sessione” manuale in corso.
static uint32_t stateTimeoutMs = 0;               // millis() scadenza attesa risposta durata.
const uint32_t STATE_TIMEOUT_WINDOW_MS = 30000UL; // ms timeout scelta durata.

RTC_DATA_ATTR long lastHandledUpdateIdRTC = 0; // Persistente!

// da sistemare
static uint32_t tgLastSendMs = 0;            // ultimo invio OK
static String tgLastPayload = "";            // ultimo messaggio inviato
static const uint32_t TG_DEDUPE_MS = 5000UL; // dedupe 5s
static uint8_t tgSendCounter = 0;            // per debug duplicati

// ✅ LOCK per evitare che safeGetUpdates() e tgSend() corrano in parallelo
static volatile bool tgBusy = false;
const uint32_t TG_LOCK_TIMEOUT_MS = 5000UL; // timeout di sicurezza se uno si blocca

// Prototipi di log
String getTime();
void appendLogFile(const String &line);
void logLine(LogLevel lvl, const String &msg, bool newline, bool toTelegram);
String tailLog(int maxLines);
String tailWarnError(int maxLines = 50, bool includeOld = false);
void handleDebug();
void initLogSize();

// Prototipi di telnet
void handleTelnet();
void handleTelnetCommand(const String &cmd);
void telnetSendTail(const char *path, int maxLines);
void telnetPrintWarnErrorFile(const char *path);
void telnetPrintAllWarnError(bool includeOld);
static void telnetWelcome();

// Prototipi dei sensori
void leggiSensori(int umidita[2]);
void handleSensore(bool toTelegram);

// Prototipi dei motori
void accendiMotori(int who, int tempo);
void spegniMotori(int who);
void askTime(const String &who);
void autoTickZone(AutoZone &az, MotorSel m, uint8_t humPct, bool sensoreOk);
static inline void armStateTimeout(uint32_t windowMs);
static inline bool isStateTimeoutExpired();
static inline void resetAskSession();
static inline String motorLabel(uint8_t m);

// Prototipi per messaggi telegram
void handleCallBack(String text, String chatId, String messageId);
void handleMessage(String text, String chatId, String messageId);
bool tgSend(const String &msg);

// Rilevo meteo
bool rilevoMeteo();
unsigned long refreshData(int ora);
bool validitaCashMeteo();
bool aggiornamentoMeteoServe(bool forza);
void attivoBloccoPioggia();
void controlloBloccoPioggia();
bool irrigazioneConsentita();
void handleMeteo();

// previsioni ore successive
bool rilevoForecastPioggia();
bool validitaCacheForecast();
bool aggiornamentoForecastServe(bool forza = false);
void applicaBloccoDaForecast();

// check sistem Health
void validazioneSensori(int raw1, int raw2);
void checkTemperaturaESP32(uint32_t now);
void handleHealth();
void checkWiFiSignal(uint32_t now);
void checkTelegramConnection(uint32_t now);
int safeGetUpdates();
void checkMemory(uint32_t now);
void checkMotori(uint32_t now);

// check irrigazioni
uint32_t computeDayId();
void dailyResetTick(uint32_t nowMs);
bool requestIrrigation(MotorSel m, uint16_t seconds, const char *source, IrrigationBlockReason &reason, uint8_t &motBlocked, uint16_t &waitMin, bool ignoreMeteo = false);

// funzione Helper per log
static inline String boolToEmoji(bool v, bool inverted = false);
static inline String umiditaStatusEmoji(int um);

// statistiche giornaliere
void nightlyReportTick(uint32_t nowMs);
static bool sendNightlyReport();
static void resetDailyStats();

// helper per polling e wifi sleep mode adattivi
static inline bool isNightHour(int h);
static inline void boostPolling(uint32_t ms);
static inline uint32_t currentPollDelayMs(int hourNow, uint32_t nowMs);
static inline void wifiFollowPolling(uint32_t nowMs, uint32_t delayMs);
static inline bool isBoostedNow(uint32_t nowMs);
static inline bool motorsOnNow();
static inline bool telnetOnNow();

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
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    logLine(DEBUG_L, ".", false, false);
  }
  delay(500);

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

  // Messaggio di avvio
  tgSend("BOT ATTIVO " + WiFi.macAddress() + " boot#" + String(bootCounter));

  // Avvio modalita OTA
  ArduinoOTA.setHostname("esp32-ota");
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

// Funzioni di log
String getTime()
{
  if (!timeReady)
    return "BOOT";
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo))
    return "No time!";

  const char *giorni[] = {"DOM", "LUN", "MAR", "MER", "GIO", "VEN", "SAB"};

  char buff[32];

  sprintf(buff, "%s %02d/%02d %02d:%02d:%02d",
          giorni[timeinfo.tm_wday],
          timeinfo.tm_mday, timeinfo.tm_mon + 1,
          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

  return String(buff);
}

void appendLogFile(const String &line)
{
  if (!spiffsOK)
    return;

  static uint16_t writeCount = 0;

  // ✅ Controlla spazio solo ogni 100 scritture
  if (writeCount % 100 == 0)
  {
    if (SPIFFS.totalBytes() - SPIFFS.usedBytes() < 10240)
    {
      SPIFFS.remove("/log.old");
      SPIFFS.rename("/log.txt", "/log.old");
      logBytes = 0;
    }
  }

  writeCount++;

  // Rotazione normale
  if (logBytes + line.length() > MAX_LOG_SIZE)
  {
    SPIFFS.remove("/log.old");
    SPIFFS.rename("/log.txt", "/log.old");
    logBytes = 0;
  }

  File f = SPIFFS.open("/log.txt", FILE_APPEND);
  if (!f)
    return;

  size_t written = f.print(line);
  f.close();

  logBytes += written;
}

void logLine(LogLevel lvl, const String &msg, bool newline = true, bool toTelegram = false)
{
  if (!debug && lvl == DEBUG_L)
    return;

  const char *L[] = {"I", "D", "W", "E"};

  // ✅ Buffer statico (zero allocazioni heap)
  static char line[512];
  int pos = 0;

  // Formatta direttamente in buffer
  pos = snprintf(line, sizeof(line), "%s | %s | %s", getTime().c_str(), L[lvl], msg.c_str()); // Uno c_str() solo per il messaggio

  if (pos < 0 || pos >= (int)sizeof(line))
    pos = sizeof(line) - 1; // Protezione overflow

  // Log file
  appendLogFile(String(line) + "\r\n"); // Una sola String conversione

  // Serial
  Serial.print(line);
  if (newline)
    Serial.println();

  // Telnet
  if (telnetClient && telnetClient.connected())
  {
    telnetClient.print(line);
    if (newline)
      telnetClient.print("\r\n");
  }

  // Statistiche
  if (lvl == WARN)
    stats.warnCount++;
  if (lvl == ERROR_L)
    stats.errCount++;

  // Telegram
  if (toTelegram)
    tgSend(String(line)); // Una conversione sola
}

String tailLog(int maxLines)
{
  File f = SPIFFS.open("/log.txt", FILE_READ);
  if (!f)
    return "Nessun log.";

  if (maxLines <= 0)
    maxLines = 50;
  if (maxLines > 200)
    maxLines = 200;

  // ✅ Alloca dinamicamente
  String *lines = new String[maxLines];
  if (!lines)
    return "Out of memory";

  int idx = 0;
  while (f.available() && idx < maxLines * 2)
  { // Leggi max 2x per safety
    lines[idx % maxLines] = f.readStringUntil('\n');
    idx++;
  }
  f.close();

  int start = max(0, idx - maxLines);
  String out;
  out.reserve(2528);

  for (int i = start; i < idx; i++)
  {
    out += lines[i % maxLines];
    out += "\n";
  }

  delete[] lines; // ✅ Libera
  return out;
}

String tailWarnError(int maxLines, bool includeOld)
{
  if (!spiffsOK)
    return "SPIFFS non disponibile";

  const size_t MAX_OUT = 3500;
  const int CAP = 80; // Ridotto da 120
  int requestedLines = min(maxLines, CAP);

  // ✅ Alloca SOLO quando richiesto
  String *ring = new String[requestedLines];
  if (!ring)
  {
    return "Out of memory";
  }

  // Pre-alloca i buffer
  for (int i = 0; i < requestedLines; i++)
  {
    ring[i].reserve(128);
  }

  int cap = requestedLines;
  int idx = 0;

  auto scanFile = [&](const char *path)
  {
    File f = SPIFFS.open(path, FILE_READ);
    if (!f)
      return;

    while (f.available())
    {
      String s = f.readStringUntil('\n');
      s.trim();

      if (s.indexOf(" | W | ") >= 0 || s.indexOf(" | E | ") >= 0)
      {
        ring[idx % cap] = s;
        idx++;
      }
    }
    f.close();
  };

  if (includeOld)
    scanFile("/log.old");
  scanFile("/log.txt");

  if (idx == 0)
  {
    delete[] ring; // ✅ Libera prima di ritornare
    return "Nessun WARNING/ERROR nel log.";
  }

  int start = max(0, idx - cap);

  String out;
  out.reserve(MAX_OUT);
  out = "Ultimi ";
  out += String(min(idx, cap));
  out += " WARNING/ERROR:\n\n";

  for (int i = start; i < idx; i++)
  {
    const String &line = ring[i % cap];
    if (out.length() + line.length() + 1 > MAX_OUT)
      break;
    out += line;
    out += "\n";
  }

  delete[] ring; // ✅ Libera memoria
  return out;
}

void handleDebug()
{
  debug = !debug;
  logLine(INFO, debug ? "🔧 DEBUG ATTIVO" : "🔧 DEBUG DISATTIVO", true, true);
}

void initLogSize()
{
  File r = SPIFFS.open("/log.txt", FILE_READ);
  if (!r)
  {
    logBytes = 0;
    return;
  }

  size_t sz = r.size();
  r.close();

  // ✅ Protezione: se size > MAX_LOG_SIZE, forza rotazione
  if (sz > MAX_LOG_SIZE)
  {
    SPIFFS.remove("/log.old");
    SPIFFS.rename("/log.txt", "/log.old");
    logBytes = 0;
  }
  else
  {
    logBytes = sz;
  }
}

// telnet

void handleTelnet()
{

  if (telnetServer.hasClient())
  {
    WiFiClient newClient = telnetServer.available();
    if (newClient)
    {
      boostPolling(BOOST_TELNET_MS);
      if (telnetClient && telnetClient.connected())
        telnetClient.stop();
      telnetClient = newClient;
      telnetClient.println("Telnet OK. Comandi: tail, alert, clear, size, tgreset, health");
      telnetWelcome();
    }
  }

  if (!(telnetClient && telnetClient.connected()))
    return;

  if (telnetClient && telnetClient.connected())
  {
    boostPolling(2000); // piccolo rinnovo continuo, leggero
  }

  while (telnetClient.available())
  {
    uint8_t c = (uint8_t)telnetClient.read();

    // Telnet: IAC (255) introduce comandi/negoziazione, non testo [web:494]
    if (c == 0xFF)
    {
      // spesso: IAC + (DO/DONT/WILL/WONT) + option => 3 byte totali [web:482]
      if (telnetClient.available())
        telnetClient.read();
      if (telnetClient.available())
        telnetClient.read();
      continue;
    }

    // Ignora NUL (può arrivare in alcune varianti CR NUL)
    if (c == 0x00)
      continue;

    // Fine riga: accetta CR o LF
    if (c == '\r' || c == '\n')
    {
      telnetLine.trim();
      if (telnetLine.length() > 0)
        handleTelnetCommand(telnetLine);
      telnetLine = "";
      continue;
    }

    // Backspace (utile con PuTTY/iTerminal quando modifichi la riga)
    if (c == 0x08 || c == 0x7F)
    {
      if (telnetLine.length() > 0)
        telnetLine.remove(telnetLine.length() - 1);
      continue;
    }

    telnetLine += (char)c;
  }
}

void handleTelnetCommand(const String &cmd)
{
  if (cmd.startsWith("tail"))
  {
    String arg = cmd.substring(4); // dopo "tail"
    arg.trim();
    arg.replace("[", "");
    arg.replace("]", "");
    int n = arg.toInt();
    if (n <= 0)
      n = 50;
    telnetSendTail("/log.txt", n);
  }
  else if (cmd == "tgreset")
  {
    lastHandledUpdateId = 0;
    lastHandledUpdateIdRTC = 0;
    tgBusy = false;
    client.stop();
    telnetClient.println("OK - Telegram offset reset to 0");
  }
  else if (cmd == "clear")
  {
    SPIFFS.remove("/log.txt");
    telnetClient.println("OK cleared.");
  }
  else if (cmd == "health")
  {
    handleHealth();
    return;
  }
  else if (cmd == "size")
  {
    File f = SPIFFS.open("/log.txt", FILE_READ);
    telnetClient.printf("log.txt = %u bytes\r\n", f ? (unsigned)f.size() : 0);
    if (f)
      f.close();
  }
  else if (cmd.startsWith("alert"))
  {
    bool includeOld = true; // default: include anche log.old
    if (cmd.indexOf(" new") >= 0)
      includeOld = false;

    telnetPrintAllWarnError(includeOld); // funzione che ti ho dato prima
  }
  else
  {
    telnetClient.println("Comandi: tail, alert, clear, size, tgreset, health");
  }
}

void telnetSendTail(const char *path, int maxLines)
{
  File f = SPIFFS.open(path, FILE_READ);
  if (!f)
  {
    telnetClient.println("No file.");
    return;
  }

  maxLines = min(maxLines, 100); // Limite hard ridotto

  // ✅ Alloca dinamicamente (liberato a fine funzione)
  String *lines = new String[maxLines];
  if (!lines)
  {
    telnetClient.println("Out of memory");
    f.close();
    return;
  }

  int idx = 0;
  while (f.available())
  {
    lines[idx % maxLines] = f.readStringUntil('\n');
    idx++;
  }
  f.close();

  int start = max(0, idx - maxLines);
  for (int i = start; i < idx; i++)
  {
    telnetClient.println(lines[i % maxLines]);
  }

  delete[] lines; // ✅ Libera memoria
  telnetClient.println("-- EOF --");
}

void telnetPrintWarnErrorFile(const char *path)
{
  if (!(telnetClient && telnetClient.connected()))
    return;

  File f = SPIFFS.open(path, FILE_READ);
  if (!f)
  {
    telnetClient.println("No file.");
    return;
  }

  char line[512];
  while (f.available())
  {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';

    // pulizia CR finale (log su file spesso ha \r\n)
    if (n && line[n - 1] == '\r')
      line[n - 1] = '\0';

    if (strstr(line, " | W | ") || strstr(line, " | E | "))
    {
      telnetClient.println(line);
    }
  }
  f.close();
}

void telnetPrintAllWarnError(bool includeOld = true)
{
  if (!(telnetClient && telnetClient.connected()))
    return;

  telnetClient.println("\n-- WARN/ERROR --");
  if (includeOld)
    telnetPrintWarnErrorFile("/log.old");
  telnetPrintWarnErrorFile("/log.txt");
  // telnetClient.println("-- EOF --\n");
}

static void telnetWelcome()
{
  if (!telnetClient || !telnetClient.connected())
    return;

  telnetClient.println();
  telnetClient.println("=== ESP32 TELNET ===");
  telnetClient.println("IP: " + WiFi.localIP().toString());
  telnetClient.println("Uptime(ms): " + String(millis()));
  telnetClient.println("Comandi: tail [n], alert, clear, size");

  if (!spiffsOK)
  {
    telnetClient.println("SPIFFS non montato, niente log.");
    telnetClient.println("=== END ===");
    return;
  }

  telnetPrintAllWarnError(true);

  telnetClient.println("=== END ===");
}

// sensori
void leggiSensori(int umidita[2])
{
  const int NUM_SAMPLES = 5;           // Numero di campioni per media
  const int DELAY_BETWEEN_SAMPLES = 5; // 5ms tra letture

  long sum1 = 0;
  long sum2 = 0;

  for (int i = 0; i < NUM_SAMPLES; i++)
  {
    sum1 += analogRead(Pin_Sensore1);
    sum2 += analogRead(Pin_Sensore2);
    delay(DELAY_BETWEEN_SAMPLES); // Piccolo ritardo tra letture
  }

  // ✅ Calcola media
  umidita[0] = sum1 / NUM_SAMPLES;
  umidita[1] = sum2 / NUM_SAMPLES;

  validazioneSensori(umidita[0], umidita[1]);
}

void handleSensore(bool toTelegram)
{
  int umidita[2];

  leggiSensori(umidita);

  int umiditaSens1 = map(umidita[0], ADC_DRY, ADC_WET, 0, 100);
  int umiditaSens2 = map(umidita[1], ADC_DRY, ADC_WET, 0, 100);

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
  const uint32_t BLOCK_LOG_COOLDOWN_MS = 20UL * 60UL * 1000UL; // 10 min

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

  const uint32_t HUM_LOG_INTERVAL_MS = 20UL * 60UL * 1000UL; // 20 min
  const int HUM_DELTA_PCT = 5;

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
void accendiMotori(int who, int tempo)
{
  boostPolling(BOOST_MOTOR_MS);
  const uint32_t now = millis();
  const bool was1On = (offTimeMot1 != 0);
  const bool was2On = (offTimeMot2 != 0);

  if (who == 1)
  {
    if ((who == 1 || who == 3) && health.motore1BloccatoSicurezza)
      return;
    if ((who == 2 || who == 3) && health.motore2BloccatoSicurezza)
      return;

    offTimeMot1 = now + (uint32_t)tempo * 1000UL;
    if (!was1On)
      health.motore1StartTime = now;
    health.motore1AttivoTroppoTempo = false;
    digitalWrite(Pin_Relay1, LOW);
  }
  else if (who == 2)
  {
    offTimeMot2 = now + (uint32_t)tempo * 1000UL;
    if (!was2On)
      health.motore2StartTime = now;
    health.motore2AttivoTroppoTempo = false;
    digitalWrite(Pin_Relay2, LOW);
  }
  else if (who == 3)
  {
    offTimeMot1 = now + (uint32_t)tempo * 1000UL;
    offTimeMot2 = now + (uint32_t)tempo * 1000UL;
    if (!was1On)
      health.motore1StartTime = now;
    if (!was2On)
      health.motore2StartTime = now;
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

    // ✅ riallinea AUTO: se era AUTO attivo, non è "spento esternamente"
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
      CHAT_ID,
      "Quanto tempo per " + who + "?",
      "",
      keyboardJson);
}

void autoTickZone(AutoZone &az, MotorSel m, uint8_t humPct, bool sensoreOk)
{
  if (!autoEnabled)
    return;
  if (!sensoreOk)
    return;

  if (!irrigazioneConsentita())
    return;

  // Stato fisico reale del motore
  const bool motOn = (m == Motore_1) ? (offTimeMot1 != 0) : (offTimeMot2 != 0);

  // Se il motore è ON ma AUTO non lo ha “avviato”, consideralo MANUALE e non interferire
  if (motOn && !az.active)
    return;

  // Se AUTO credeva di essere attivo ma il motore è stato spento da fuori, riallinea
  if (az.active && !motOn)
  {
    az.active = false;
    logLine(WARN, "AUTO: motore " + motorLabel((int)m) + " spento esternamente", true, true);
    return;
  }

  // START: vaso secco
  if (!az.active && humPct <= az.startTh)
  {
    uint8_t motB = 0;
    uint16_t waitM = 0;
    IrrigationBlockReason rr = IRR_OK;

    if (!requestIrrigation(m, MAX_MOTOR_SECONDS, "AUTO", rr, motB, waitM))
    {
      logLine(WARN, "⛔ AUTO BLOCCATA mot=" + motorLabel(m) + " reason=" + String((int)rr), true, true);
      return;
    }

    az.active = true; // set solo se è partita davvero
    return;
  }

  // STOP: vaso ok (solo se AUTO aveva avviato)
  if (az.active && humPct >= az.stopTh)
  {
    az.active = false;
    spegniMotori((int)m);
    logLine(INFO, "⏸️ AUTO STOP motore " + motorLabel(m) + " umid=" + String(humPct) + "%", true, true);
    return;
  }
}

static inline void armStateTimeout(uint32_t windowMs)
{
  stateTimeoutMs = millis() + windowMs;
}

static inline bool isStateTimeoutExpired()
{
  return stateTimeoutMs != 0 && (int32_t)(millis() - stateTimeoutMs) >= 0; // overflow-safe
}

static inline void resetAskSession()
{
  botstate = IDLE;
  motorOperationInProgress = false;
  stateTimeoutMs = 0;
}

static inline String motorLabel(uint8_t m)
{
  if (m == 1)
    return "1";
  if (m == 2)
    return "2";
  if (m == 3)
    return "1+2"; // oppure "Entrambi"
  return "?";
}

// Messaggi telegram
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
  if (chatId != String(CHAT_ID))
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
        CHAT_ID,
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
    SPIFFS.remove("/log.txt");
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
  {
    tgBusy = false;
    return false;
  }

  tgBusy = true;

  // ✅ NON chiudere se già configurato
  static bool clientConfigured = false;
  if (!clientConfigured)
  {
    client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
    clientConfigured = true;
  }

  bot.waitForResponse = 3000;
  bool success = bot.sendMessage(CHAT_ID, msg, "");

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

bool rilevoMeteo()
{
  if (WiFi.status() != WL_CONNECTED)
    return false;

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  String url;
  url.reserve(256);
  url = "https://api.openweathermap.org/data/2.5/weather?q=";
  url += city;
  url += "&appid=";
  url += openWeatherMapApiKey;
  url += "&units=metric&lang=it";

  HTTPClient http;
  http.setTimeout(8000);
  http.begin(secureClient, url);

  int httpCode = http.GET();
  if (httpCode != 200)
  {
    http.end();
    logLine(ERROR_L, "☁️❌ Errore HTTP: " + String(httpCode), true, false);
    return false;
  }

  // CONTROLLO NULLPTR CRITICO
  WiFiClient *stream = http.getStreamPtr();
  if (!stream)
  {
    http.end();
    logLine(ERROR_L, "☁️❌ Stream non disponibile", true, false);
    return false;
  }

  // FILTRO JSON ottimizzato
  JsonDocument filter;
  filter["weather"][0]["main"] = true;
  filter["main"]["temp"] = true;
  filter["main"]["humidity"] = true;
  filter["rain"]["1h"] = true;
  filter["rain"]["3h"] = true;

  JsonDocument doc;

  DeserializationError error = deserializeJson(doc, *stream, DeserializationOption::Filter(filter));
  http.end();

  if (error)
  {
    logLine(ERROR_L, "🧩❌ Errore JSON: " + String(error.c_str()), true, false);
    return false;
  }

  // Operatore | per gestione sicura valori opzionali
  String main = doc["weather"][0]["main"] | "Unknown";
  float temp = doc["main"]["temp"] | 0.0f;
  int umidita = doc["main"]["humidity"] | 0;
  float r1h = doc["rain"]["1h"] | 0.0f;
  float r3h = doc["rain"]["3h"] | 0.0f;

  bool piove = (main == "Rain" || main == "Drizzle" || r1h >= soglia_Minima_Pioggia);

  meteo.condizioniMeteo = main;
  meteo.temperatura = temp;
  meteo.umidita = umidita;
  meteo.pioggiaUltimaOra = r1h;
  meteo.staPiovendo = piove;
  meteo.ultimoAggiornamento = millis();
  meteo.datiValidi = true;

  return true;
}

unsigned long refreshData(int ora)
{ // restituisce l'intervallo di refresh appropiato
  if (ora >= ora_Inizio_Giorno && ora <= ora_Fine_Giorno)
    return intervallo_Refresh_Giorno;
  return intervallo_Refresh_Notte;
}

bool validitaCashMeteo()
{
  if (!meteo.datiValidi)
    return false;
  unsigned long elapsed = millis() - meteo.ultimoAggiornamento;
  return elapsed <= durata_Cash_Valida;
}

bool aggiornamentoMeteoServe(bool forza = false)
{
  if (forza || !validitaCashMeteo())
  {
    return rilevoMeteo();
  }
  return true;
}

void attivoBloccoPioggia()
{
  bloccoIrrigazione = true;
  scadenzaBloccoIrrigazione = millis() + DurataBloccoPioggia;
}

void controlloBloccoPioggia()
{
  if (!bloccoIrrigazione)
    return;
  if (scadenzaBloccoIrrigazione != 0 &&
      (int32_t)(millis() - scadenzaBloccoIrrigazione) >= 0)
  {
    bloccoIrrigazione = false;
    scadenzaBloccoIrrigazione = 0;
  }
}

bool irrigazioneConsentita()
{
  controlloBloccoPioggia();
  if (bloccoIrrigazione)
    return false;
  if (!meteo.datiValidi)
    return true; // se WiFi assente irriga comunque
  if (meteo.staPiovendo)
    return false;
  return true;
}

void handleMeteo()
{
  aggiornamentoMeteoServe();
  aggiornamentoForecastServe(false);
  applicaBloccoDaForecast();

  if (!meteo.datiValidi)
  {
    logLine(ERROR_L, "☁️❌ Meteo non disponibile", true, true);
    return;
  }

  const uint32_t nowMs = millis();
  const uint32_t etaMin = (nowMs - meteo.ultimoAggiornamento) / 60000UL;

  String msg;
  msg.reserve(650);

  // Header
  msg += "🌤️ METEO — " + city + "\n";
  msg += "━━━━━━━━━━━━━━\n";

  // Dati attuali
  msg += "📌 Ora\n";
  msg += "• Condizioni: " + meteo.condizioniMeteo + "\n";
  msg += "• 🌡️ Temp: " + String(meteo.temperatura, 1) + " °C\n";
  msg += "• 💧 Umidità: " + String(meteo.umidita) + "%\n";
  msg += "• 🌧️ Pioggia (1h): " + String(meteo.pioggiaUltimaOra, 2) + " mm\n";
  msg += "• 🕒 Aggiornato: " + String(etaMin) + " min fa\n";

  // Forecast
  msg += "\n🔮 Previsioni\n";
  if (meteo.forecastValidi)
  {
    msg += "• Entro 3h: " + String(meteo.pioggiaPrevista3h ? "🌧️ SI" : "✅ NO");
    msg += " (" + String(meteo.mmPrevisti3h, 2) + " mm)\n";

    msg += "• Entro 6h: " + String(meteo.pioggiaPrevista6h ? "🌧️ SI" : "✅ NO");
    msg += " (" + String(meteo.mmPrevisti6h, 2) + " mm)\n";

    // Nota soglia (usa il nome reale della tua costante: sogliaMinimaPioggia nel file)
    msg += "• Soglia: ≥ " + String(soglia_Minima_Pioggia, 1) + " mm\n";
  }
  else
  {
    msg += "• Previsione 3h/6h: ND\n";
  }

  // Blocco irrigazione
  msg += "\n🚿 Irrigazione\n";
  if (bloccoIrrigazione)
  {
    const int32_t remMs = (int32_t)(scadenzaBloccoIrrigazione - nowMs);
    if (remMs > 0)
    {
      msg += "• ⛔ Blocco attivo: " + String((uint32_t)remMs / 60000UL) + " min\n";
    }
    else
    {
      msg += "• ✅ Blocco scaduto\n";
    }
  }
  else
  {
    msg += "• ✅ Nessun blocco attivo\n";
  }

  tgSend(msg);
}

// previsioni ore successive

static inline bool isRainLike(float mm3h, const String &main)
{
  // Soglia principale: mm negli ultimi 3h previsti
  if (mm3h >= soglia_Minima_Pioggia)
    return true;
  // Fallback (utile quando "rain.3h" non c'è ma la condizione è Rain/Drizzle)
  if (main == "Rain" || main == "Drizzle")
    return true;
  return false;
}

bool validitaCacheForecast()
{
  if (!meteo.forecastValidi)
    return false;
  return (millis() - meteo.ultimoAggForecast) < durataCacheForecast;
}

bool aggiornamentoForecastServe(bool forza)
{
  if (forza || !validitaCacheForecast())
  {
    return rilevoForecastPioggia();
  }
  return true;
}

bool rilevoForecastPioggia()
{
  if (WiFi.status() != WL_CONNECTED)
    return false;

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  // Richiesta forecast: prendo pochi timestamp (cnt=3) per coprire fino a ~6-9 ore
  // OpenWeatherMap supporta cnt per limitare il numero di elementi in "list". [page:0]
  String url;
  url.reserve(256);
  url = "http://api.openweathermap.org/data/2.5/forecast?q=" + city +
        "&appid=" + openWeatherMapApiKey + "&units=metric&lang=it&cnt=3";

  HTTPClient http;
  http.setTimeout(8000);
  http.begin(secureClient, url);
  int httpCode = http.GET();
  if (httpCode != 200)
  {
    http.end();
    logLine(ERROR_L, "Forecast HTTP error " + String(httpCode), true, false);
    return false;
  }

  WiFiClient *stream = http.getStreamPtr();
  if (!stream)
  {
    http.end();
    logLine(ERROR_L, "Forecast stream non disponibile", true, false);
    return false;
  }

  // Filtro ArduinoJson: estrai solo ciò che serve (dt, main, rain.3h) sui primi 3 slot
  JsonDocument filter;
  for (int i = 0; i < 3; i++)
  {
    filter["list"][i]["dt"] = true;
    filter["list"][i]["weather"][0]["main"] = true;
    filter["list"][i]["rain"]["3h"] = true;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, *stream, DeserializationOption::Filter(filter));
  http.end();
  if (err)
  {
    logLine(ERROR_L, "Forecast JSON error " + String(err.c_str()), true, false);
    return false;
  }

  // Reset valori forecast
  meteo.pioggiaPrevista3h = false;
  meteo.pioggiaPrevista6h = false;
  meteo.mmPrevisti3h = 0.0f;
  meteo.mmPrevisti6h = 0.0f;

  const time_t nowUtc = time(nullptr); // hai già NTP in setup() [file:603]
  const long T3 = 3L * 3600L;
  const long T6 = 6L * 3600L;

  JsonArray list = doc["list"].as<JsonArray>();
  for (JsonObject item : list)
  {
    const long dt = item["dt"] | 0;
    const long delta = dt - (long)nowUtc;
    if (delta <= 0)
      continue; // slot già passato

    const String main = item["weather"][0]["main"] | "";
    const float r3h = item["rain"]["3h"] | 0.0f;

    const bool rainLike = isRainLike(r3h, main);

    if (delta <= T3)
    {
      meteo.pioggiaPrevista3h = meteo.pioggiaPrevista3h || rainLike;
      meteo.mmPrevisti3h += r3h;
    }
    if (delta <= T6)
    {
      meteo.pioggiaPrevista6h = meteo.pioggiaPrevista6h || rainLike;
      meteo.mmPrevisti6h += r3h;
    }
  }

  meteo.forecastValidi = true;
  meteo.ultimoAggForecast = millis();
  return true;
}

void applicaBloccoDaForecast()
{
  controlloBloccoPioggia(); // se era scaduto lo pulisce [file:603]
  if (!meteo.forecastValidi)
    return;

  // Se pioggia prevista entro 3h o 6h, attiva blocco fino a fine finestra
  unsigned long durataMs = 0;
  if (meteo.pioggiaPrevista3h)
    durataMs = 3UL * 60UL * 60UL * 1000UL;
  else if (meteo.pioggiaPrevista6h)
    durataMs = 6UL * 60UL * 60UL * 1000UL;

  if (durataMs > 0)
  {
    bloccoIrrigazione = true;
    scadenzaBloccoIrrigazione = millis() + durataMs;
  }
}

// check sistem Health
void handleHealth()
{
  // Buffer statico: niente allocazioni dinamiche
  static char buf[800];
  int len = 0;
  const int maxLen = sizeof(buf);

  auto safePrintf = [&](const char *fmt, ...)
  {
    if (len >= maxLen - 1)
      return; // già pieno
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(buf + len, maxLen - len, fmt, args);
    va_end(args);
    if (written > 0)
    {
      if (written > maxLen - len - 1)
        written = maxLen - len - 1;
      len += written;
    }
  };

  // Titolo
  safePrintf("🩺 SYSTEM HEALTH\n");

  // Temperatura
  safePrintf("\n🌡️ ESP32: %.1fC", health.temperaturaESP32);
  if (health.temperaturaElevata)
    safePrintf(" (⚠️)");

  // WiFi
  safePrintf("\n📡 WiFi: ");
  if (health.wifiDisconnesso)
  {
    safePrintf("OFF");
  }
  else
  {
    safePrintf("%d dBm", (int)health.rssi);
    if (health.wifiDebole)
      safePrintf(" (⚠️)");
  }

  // Heap / SPIFFS
  safePrintf("\n💾 HEAP: %u KB (largest %u KB)",
             (unsigned)health.heapFreeKB,
             (unsigned)health.heapLargestKB);
  if (health.memoriaInsufficiente)
    safePrintf(" (🚨)");
  safePrintf("\n📁 SPIFFS free: %u KB", (unsigned)health.spiffsFreeKB);

  // Telegram
  safePrintf("\n🤖 Telegram: ");
  if (health.wifiDisconnesso)
  {
    safePrintf("WiFi off");
  }
  else if (health.telegramIrraggiungibile)
  {
    safePrintf("OFFLINE");
  }
  else
  {
    uint32_t ageS = (millis() - health.lastSuccessfulTelegramComm) / 1000UL;
    safePrintf("OK (");
    if (ageS < 60)
    {
      safePrintf("%lus", (unsigned long)ageS);
    }
    else if (ageS < 3600)
    {
      safePrintf("%lum", (unsigned long)(ageS / 60UL));
    }
    else
    {
      safePrintf("%luh", (unsigned long)(ageS / 3600UL));
    }
    safePrintf(" fa)");
  }

  // polling telegram
  safePrintf("\n⏱️ Polling TG: %lu ms | %s",
             (unsigned long)health.pollDelayMs,
             health.pollBoostActive ? "⚡ BOOST" : "🐌 BASE");

  // Blocco pioggia / AUTO
  safePrintf("\n\n🌧️ Blocco irrigazione: ");
  if (!bloccoIrrigazione)
  {
    safePrintf("NO");
  }
  else
  {
    safePrintf("SI");
    int32_t remMs = (int32_t)(scadenzaBloccoIrrigazione - millis());
    if (remMs > 0)
    {
      safePrintf(" (restano ~%lu min)", (unsigned long)(remMs / 60000UL));
    }
  }
  safePrintf("\n🤖 AUTO: %s", autoEnabled ? "ON" : "OFF");

  // Motori
  safePrintf("\n\n🔌 MOTORI\n");

  auto addMotorLine = [&](uint8_t m, unsigned long offTime, bool blocked)
  {
    bool on = (offTime != 0);
    safePrintf("Mot%u: ", (unsigned)m);

    if (blocked)
    {
      safePrintf("🚫 BLOCCATO SICUREZZA");
    }
    else
    {
      safePrintf("%s", on ? "✅ ON" : "⏹️ OFF");
    }

    if (on)
    {
      int32_t remMs = (int32_t)(offTime - millis());
      if (remMs > 0)
      {
        safePrintf(" (restano %lus)", (unsigned long)(remMs / 1000UL));
      }
    }
    safePrintf("\n");
  };

  addMotorLine(1, offTimeMot1, health.motore1BloccatoSicurezza);
  addMotorLine(2, offTimeMot2, health.motore2BloccatoSicurezza);

  // Contatori irrigazioni
  safePrintf("\n💧 Irrigazioni oggi: M1 %u | M2 %u",
             (unsigned)health.irrigazioniOggiMot1,
             (unsigned)health.irrigazioniOggiMot2);

  // Avvisi attivi
  safePrintf("\n\n⚠️ AVVISI:\n");
  if (health.sensore1Disconnesso)
    safePrintf("- Sensore 1 disconnesso\n");
  if (health.sensore2Disconnesso)
    safePrintf("- Sensore 2 disconnesso\n");
  if (health.umiditaCritica)
    safePrintf("- Umidità critica\n");
  if (health.memoriaInsufficiente)
    safePrintf("- Memoria insufficiente\n");
  if (health.motore1BloccatoSicurezza)
    safePrintf("- Motore 1 bloccato sicurezza\n");
  if (health.motore2BloccatoSicurezza)
    safePrintf("- Motore 2 bloccato sicurezza\n");

  // Invio Telegram
  buf[len] = '\0';
  tgSend(String(buf));
}

void validazioneSensori(int raw1, int raw2)
{
  static bool lastSensor1Error = false;
  static bool lastSensor2Error = false;

  // Sensore 1
  bool sensor1Error = (raw1 < SENSOR_LOW || raw1 > SENSOR_HIGH);

  if (sensor1Error && !lastSensor1Error)
  {
    health.sensore1Disconnesso = true;
    spegniMotori(1);
    logLine(WARN, "⚠️ Sensore 1 disconnesso (val: " + String(raw1) + ")", true, true);
  }

  else if (!sensor1Error)
  {
    health.sensore1Disconnesso = false;
  }

  lastSensor1Error = sensor1Error;

  // Sensore 2
  bool sensor2Error = (raw2 < SENSOR_LOW || raw2 > SENSOR_HIGH);

  if (sensor2Error && !lastSensor2Error)
  {
    health.sensore2Disconnesso = true;
    spegniMotori(2);
    logLine(WARN, "⚠️ Sensore 2 disconnesso (val: " + String(raw2) + ")", true, true);
  }
  else if (!sensor2Error)
  {
    health.sensore2Disconnesso = false;
  }

  lastSensor2Error = sensor2Error;

  // check umidita critica
  if (!sensor1Error && !sensor2Error)
  {
    int pct1 = map(constrain(raw1, ADC_WET, ADC_DRY), ADC_DRY, ADC_WET, 0, 100);
    int pct2 = map(constrain(raw2, ADC_WET, ADC_DRY), ADC_DRY, ADC_WET, 0, 100);

    // Umidità sotto 15% su ALMENO UN sensore? → CRITICA
    bool critica = (pct1 < UMIDITA_CRITICA || pct2 < UMIDITA_CRITICA);

    // Se NUOVA condizione critica → Logga allarme
    if (critica && !health.umiditaCritica)
    {
      health.umiditaCritica = true;
      logLine(ERROR_L, "🚨 UMIDITA' CRITICA: Sens1=" + String(pct1) + "% Sens2=" + String(pct2) + "%", true, true);
    }
    // Se umidità torna OK → Resetta flag
    else if (!critica)
    {
      health.umiditaCritica = false;
    }
  }
}

void checkTemperaturaESP32(uint32_t now)
{
  static uint32_t lastCheck = 0;

  if ((now - lastCheck) < (CHECK_TEMP * 1000UL))
    return;

  lastCheck = now;

  health.temperaturaESP32 = temperatureRead();

  if (health.temperaturaESP32 > TEMP_CRITICAL)
  {
    // Prima volta che supera 85°C? → Logga allarme
    if (!health.temperaturaElevata)
    {
      health.temperaturaElevata = true;
      logLine(ERROR_L, "🔥 TEMP ESP32 CRITICA: " + String(health.temperaturaESP32, 1) + "°C", true, true);
    }
    // 🛡️ PROTEZIONE: Riduci frequenza CPU per raffreddare
    setCpuFrequencyMhz(80); // Da 240MHz → 80MHz (riduce consumo/calore 67%)
    logLine(WARN, "⚙️ CPU ridotta a 80MHz per raffreddamento", true, false);
  }
  else if (health.temperaturaESP32 > TEMP_WARNING)
  {
    // Prima volta che supera 75°C? → Logga avviso
    if (!health.temperaturaElevata)
    {
      health.temperaturaElevata = true;
      logLine(WARN, "🌡️⚠️ Temp ESP32 elevata: " + String(health.temperaturaESP32, 1) + "°C", true, false);
    }
  }
  else if (health.temperaturaESP32 < (TEMP_WARNING - 5.0))
  {
    // Se temperatura scende sotto 70°C → Resetta flag
    if (health.temperaturaElevata)
    {
      health.temperaturaElevata = false;
      logLine(INFO, "🌡️✅ Temp ESP32 OK: " + String(health.temperaturaESP32, 1) + "°C", true, false);

      // Ripristina CPU a velocità normale se era stata ridotta
      if (getCpuFrequencyMhz() < 240)
      {
        setCpuFrequencyMhz(240);
        logLine(INFO, "⚙️ CPU ripristinata a 240MHz", true, false);
      }
    }
  }
}

void checkWiFiSignal(uint32_t now)
{
  static uint32_t lastCheck = 0;
  static uint32_t reconnectStartMs = 0;
  static bool reconnecting = false;

  if ((now - lastCheck) < (CHECK_WIFI * 1000UL))
    return;
  lastCheck = now;

  // CHECK DISCONNESSIONE WiFi
  if (WiFi.status() != WL_CONNECTED)
  {
    if (!health.wifiDisconnesso)
    {
      health.wifiDisconnesso = true;
      logLine(ERROR_L, "📡❌ WiFi disconnesso!", true, true);
    }

    // Avvia reconnessione SOLO se non già in corso
    if (!reconnecting)
    {
      reconnecting = true;
      reconnectStartMs = now;
      WiFi.disconnect();
      delay(10);
      WiFi.begin(ssid, password);
      logLine(WARN, "🔄📡 Reconnessione WiFi avviata...", true, false);
    }
    else if (now - reconnectStartMs > 30000UL)
    {
      // Dopo 30s senza successo, riprova
      reconnecting = false;
      logLine(WARN, "⏱️ Reconnessione timeout, nuovo tentativo...", true, false);
    }
    return;
  }

  // WiFi tornato online
  reconnecting = false;
  if (health.wifiDisconnesso)
  {
    health.wifiDisconnesso = false;
    health.rssi = WiFi.RSSI();
    logLine(INFO, "📡✅ WiFi tornato online. IP: " + WiFi.localIP().toString() + " (" + String(health.rssi) + " dBm)", true, true);
  }
  else
  {
    health.rssi = WiFi.RSSI();
  }

  // CHECK SEGNALE DEBOLE
  if (health.rssi < RSSI_CRITICO)
  {
    if (!health.wifiDebole)
    {
      health.wifiDebole = true;
      logLine(ERROR_L, "📶🚨 WiFi CRITICO: " + String(health.rssi) + " dBm", true, true);
    }
  }
  else if (health.rssi < RSSI_DEBOLE)
  {
    if (!health.wifiDebole)
    {
      health.wifiDebole = true;
      logLine(WARN, "📶⚠️ WiFi debole: " + String(health.rssi) + " dBm", true, false);
    }
  }
  else if (health.rssi > (RSSI_DEBOLE + 5))
  {
    if (health.wifiDebole)
    {
      health.wifiDebole = false;
      logLine(INFO, "📶✅ WiFi OK: " + String(health.rssi) + " dBm", true, false);
    }
  }
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

void checkMemory(uint32_t now)
{
  static uint32_t lastCheck = 0;
  if ((now - lastCheck) < (CHECK_MEMORY * 1000UL))
    return; // 300s = CHECK_MEMORY default
  lastCheck = now;

  // HEAP interno: totale, largest block, minimo storico
  const size_t freeB = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t largestB = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  const size_t minEverB = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

  // Flag “low heap”
  const bool low = (freeB < (MIN_FREE_KB * 1024UL));
  health.memoriaInsufficiente = low;

  // SPIFFS free (flash): total-used
  if (spiffsOK)
  {
    const uint32_t freeFlash = SPIFFS.totalBytes() - SPIFFS.usedBytes();
    health.spiffsFreeKB = freeFlash / 1024;
  }
  else
  {
    health.spiffsFreeKB = 0;
  }

  // Log solo su cambio stato (no spam)
  static bool lastLow = false;
  if (low != lastLow)
  {
    if (low)
    {
      logLine(ERROR_L, "💾🚨 HEAP LOW", true, true);
    }
    else
    {
      logLine(INFO, "💾✅ HEAP OK", true, false);
    }
    lastLow = low;
  }

  health.heapFreeKB = freeB / 1024;
  health.heapLargestKB = largestB / 1024;

  // se debug: log più dettagliato ma senza concatenazioni pesanti
  if (debug)
  {
    char buf[120];
    snprintf(buf, sizeof(buf),
             "heapKB=%u largestKB=%u minKB=%u spiffsFreeKB=%u",
             (unsigned)(freeB / 1024), (unsigned)(largestB / 1024),
             (unsigned)(minEverB / 1024), (unsigned)health.spiffsFreeKB);
    logLine(DEBUG_L, String(buf), true, false);
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

  // ✅ Se vedo ON ma startTime=0, inizializzo
  if (mot1On && health.motore1StartTime == 0)
    health.motore1StartTime = now;
  if (mot2On && health.motore2StartTime == 0)
    health.motore2StartTime = now;

  // ============ MOTORE 1 ============
  if (mot1On && health.motore1StartTime != 0)
  {
    const uint32_t runS = (now - health.motore1StartTime) / 1000UL;

    // ✅ NON controllare il flag: rileva SEMPRE il timeout
    if (runS > MAX_MOTOR_SECONDS)
    {
      // ✅ Se PRIMA non era bloccato → è la PRIMA volta che supero il limite
      if (!health.motore1BloccatoSicurezza)
      {
        health.motore1BloccatoSicurezza = true;
        health.motore1AttivoTroppoTempo = true;
        spegniMotori(1);
        logLine(ERROR_L, "🚨🚫 MOTORE 1 BLOCCATO (>300s) - MANUAL /sblocca1 TO RESET", true, true);
      }
      // Se è già bloccato, non loggare di nuovo (spam prevention)
      return;
    }
  }

  // ============ MOTORE 2 ============
  if (mot2On && health.motore2StartTime != 0)
  {
    const uint32_t runS = (now - health.motore2StartTime) / 1000UL;

    // ✅ NON controllare il flag: rileva SEMPRE il timeout
    if (runS > MAX_MOTOR_SECONDS)
    {
      // ✅ Se PRIMA non era bloccato → è la PRIMA volta
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

  // ============ RESET QUANDO SPENTO ============
  // ✅ Reset SOLO quando motore è OFF E RIMANE OFF (non durante spegnimento)
  if (!mot1On)
  {
    if (health.motore1StartTime != 0)
    {
      health.motore1StartTime = 0;
      // ⚠️ NON resettare il blocco se `bloccatoSicurezza` è true
      // Il blocco rimane finché l'utente non fa /sblocca1
      if (!health.motore1BloccatoSicurezza)
      {
        health.motore1AttivoTroppoTempo = false;
      }
    }
  }

  if (!mot2On)
  {
    if (health.motore2StartTime != 0)
    {
      health.motore2StartTime = 0;
      if (!health.motore2BloccatoSicurezza)
      {
        health.motore2AttivoTroppoTempo = false;
      }
    }
  }
}

// check irrigazioni
uint32_t computeDayId()
{
  if (!timeReady)
    return 0;
  struct tm t;
  if (!getLocalTime(&t, 50))
    return 0;
  return (uint32_t)(t.tm_year + 1900) * 400UL + (uint32_t)t.tm_yday;
}

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
    reason = IRR_MOTOR_LOCKED; // oppure aggiungi un reason tipo IRR_INVALID
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

    // Arrotonda per eccesso: 1..60000ms => 1 minuto
    waitMin = (uint16_t)((remMs + 60000UL - 1) / 60000UL);
    return false;
  }

  return true;
}

bool requestIrrigation(MotorSel m, uint16_t seconds, const char *source, IrrigationBlockReason &reason, uint8_t &motBlocked, uint16_t &waitMin, bool ignoreMeteo)
{
  // Normalizza parametri di uscita
  reason = IRR_OK;
  motBlocked = 0;
  waitMin = 0;

  // Validazione base
  if (seconds == 0)
  {
    return false; // Nessuna irrigazione richiesta
  }
  if (seconds > MAX_MOTOR_SECONDS)
  {
    seconds = MAX_MOTOR_SECONDS; // Fail-safe
  }

  // Blocchi sicurezza motori (persistenti finché non fai /sbloccaX)
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

  // Boost reattività polling/wifi durante richiesta irrigazione
  boostPolling(BOOST_IRR_MS);

  const uint32_t now = millis();
  dailyResetTick(now);

  // Rispetta pioggia/blocco anche in manuale (se vuoi bypass manuale usa ignoreMeteo=true)
  if (!irrigazioneConsentita() && !ignoreMeteo)
  {
    reason = IRR_RAIN_BLOCK;
    stats.blockRain++;
    return false;
  }

  // Quali motori sto davvero avviando ORA? (se già ON non conto e non applico "gap")
  const bool start1 = (m == Motore_1 || m == Entrambi_i_Motori) && !motorIsOn(1);
  const bool start2 = (m == Motore_2 || m == Entrambi_i_Motori) && !motorIsOn(2);

  // Se devo avviarli entrambi, devono passare entrambi i check (altrimenti blocco tutto)
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

  // OK -> accendo
  accendiMotori((int)m, (int)seconds);

  // Aggiorno contatori solo per i motori realmente partiti da OFF
  // Usa i tuoi helper con POINTER (*todayCnt)
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

  // Log compatto con buffer statico
  {
    char line[220];
    snprintf(line, sizeof(line),
             "🚿▶️ IRR START %s m=%s c1=%u c2=%u",
             source ? source : "?", motorLabel((int)m).c_str(),
             (unsigned)health.irrigazioniOggiMot1,
             (unsigned)health.irrigazioniOggiMot2);
    logLine(INFO, String(line), true, true);
  }

  return true;
}

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
    nextCheckMs = nowMs + 60000UL; // retry ogni 15s dentro la finestra
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

// helper per polling e wifi sleep mode adattivi
static inline bool isNightHour(int h) // Ritorna true se l'ora è nella fascia NOTTE.
{
  // Valori non validi => considera "notte"
  if (h < 0 || h > 23)
    return true;

  const int start = ora_Inizio_Giorno;
  const int end = ora_Fine_Giorno;

  // Caso normale: giorno è [start..end]
  if (start <= end)
  {
    return (h < start) || (h > end);
  }

  return (h > end) && (h < start);
}

static inline void boostPolling(uint32_t ms) // Attiva un periodo di polling "boost" (più frequente) per ms millisecondi.
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

static inline uint32_t currentPollDelayMs(int hourNow, uint32_t nowMs) // Restituisce il delay di polling in base all'ora e allo stato boost.
{
  const uint32_t base = isNightHour(hourNow) ? POLL_NIGHT_MS : POLL_DAY_MS;

  // Se siamo ancora entro la finestra boost => usa delay più aggressivo.
  if ((int32_t)(nowMs - pollBoostUntilMs) < 0)
  {
    return POLL_BOOST_MS;
  }
  return base;
}

static inline bool isBoostedNow(uint32_t nowMs) // True se adesso (nowMs) siamo in periodo boost.
{
  return (int32_t)(nowMs - pollBoostUntilMs) < 0;
}

static inline bool motorsOnNow() // Motori ON se c'è un offTime programmato (vuol dire che sono in esecuzione).
{
  return (offTimeMot1 != 0) || (offTimeMot2 != 0);
}

static inline bool telnetOnNow() // Telnet "attivo" se c'è un client e la connessione è aperta.
{
  return (telnetClient && telnetClient.connected());
}

static inline void wifiFollowPolling(uint32_t nowMs, uint32_t delayMs) // Allinea la policy di WiFi power-save con la frequenza di polling.
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

/*
Creare controllo livello acqua

riordinare funzione e variabili

sistemare loop e setup

Utilizzare doppio core

watchdog, freertos

yield();
*/
