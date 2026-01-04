// impementato polling adattivo che di notte passa da 3s a 20s ma torna attivo (a 2s) se: riceve un messaggio, motori si accendono, telnet si connette, sensori rilevano irrigazione neccessaria e aggiunto wifi sleep mode che segue il polling adattivo
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
#define Pin_SensoreContenitore 34
#define Pin_Sensore1 33
#define Pin_Sensore2 32
#define Pin_Relay1 18
#define Pin_Relay2 19

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
  IRR_DAY_LIMIT
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

const int8_t RSSI_DEBOLE = -75;     // Soglia RSSI “debole” (dBm).
const int8_t RSSI_CRITICO = -85;    // Soglia RSSI “critico” (dBm).
const float TEMP_WARNING = 75.0f;   // Soglia warning temperatura ESP32 (°C).
const float TEMP_CRITICAL = 85.0f;  // Soglia critica temperatura ESP32 (°C).
const uint8_t UMIDITA_CRITICA = 15; // Soglia umidità (%) per allarme “critica”.

const uint16_t MAX_MOTOR_SECONDS = 600;     // Massima durata continua motore (fail-safe).
const uint32_t MIN_IRRIGATION_MS = 30000UL; // Distanza minima tra irrigazioni dello stesso motore (rate-limit).
const uint8_t MAX_IRRIGATIONS_DAY = 10;     // Max irrigazioni/giorno per motore.

const uint16_t MIN_FREE_KB = 50;   // Soglia heap minima (KB) per allarme memoria.
const uint16_t SENSOR_LOW = 500;   // Min ADC plausibile sensore (sotto = errore/disconnesso).
const uint16_t SENSOR_HIGH = 4000; // Max ADC plausibile sensore (sopra = errore/disconnesso).

// Intervalli check (secondi)
const uint8_t CHECK_TEMP = 30UL;     // Ogni quanto controllare temperatura ESP32.
const uint8_t CHECK_WIFI = 10UL;     // Ogni quanto controllare WiFi/RSSI.
const uint16_t CHECK_MEMORY = 300UL; // Ogni quanto controllare heap/SPIFFS.
const uint8_t CHECK_MOTOR = 5UL;     // Ogni quanto controllare durata motori.
const uint8_t CHECK_TELEGRAM = 60UL; // Ogni quanto gestire check Telegram (se usato).
const uint8_t CHECK_SENS = 20UL;     // Ogni quanto leggere sensori umidità.

RTC_DATA_ATTR uint32_t bootCounter = 0; // Contatore boot in RTC memory (persistente).

// Polling Telegram adattivo
static const uint32_t POLL_DAY_MS = 3000;
static const uint32_t POLL_NIGHT_MS = 20000;
static const uint32_t POLL_BOOST_MS = 2500;    // quanto spesso durante boost (reattivo)
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
  unsigned long lastDayReset;               // Marker day-id/ultimo reset giornaliero.
  unsigned long motore1StartTime;           // millis() inizio motore 1 (runtime).
  unsigned long motore2StartTime;           // millis() inizio motore 2 (runtime).
  unsigned long lastSuccessfulTelegramComm; // millis() ultima comm Telegram OK.
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

SystemHealth health = {0}; // Stato salute (flag+valori misurati).

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
void checkTemperaturaESP32();
void handleHealth();
void checkWiFiSignal();
void checkTelegramConnection();
int safeGetUpdates();
void checkMemory();
void checkMotori();

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
static inline uint32_t currentPollDelayMs(int hourNow);
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
  logLine(DEBUG_L, String("Connesso! IP: ") + WiFi.localIP().toString(), true, false);

  // dopo che il WiFi è connesso
  logLine(DEBUG_L, "🕐 Imposto orario NTP...", true, false);
  configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");

  struct tm t;
  timeReady = getLocalTime(&t, 10000);

  // Certificato root per Telegram HTTPS
  client.setHandshakeTimeout(7);
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  client.setTimeout(8000);
  bot.waitForResponse = 5000;

  // Messaggio di avvio
  bot.sendMessage(CHAT_ID, "BOT ATTIVO " + WiFi.macAddress() + " boot#" + String(bootCounter), "");

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
  ArduinoOTA.handle();
  unsigned long now = millis();

  dailyResetTick(now);

  // report notturno
  nightlyReportTick(now);

  // TIMEOUT risposta accensione motori manuale
  if (botstate != IDLE && isStateTimeoutExpired())
  {
    resetAskSession();
    bot.sendMessage(CHAT_ID, "Richiesta scaduta, riprova.", "");
  }

  // Gestione nuove connessioni Telnet
  handleTelnet();

  // controllo spegnimento motori
  if (offTimeMot1 != 0 && (long)(now - offTimeMot1) >= 0)
  {
    spegniMotori(1);
  }

  if (offTimeMot2 != 0 && (long)(now - offTimeMot2) >= 0)
  {
    spegniMotori(2);
  }

  // Healt Cheack
  checkTemperaturaESP32();
  checkWiFiSignal();
  checkMemory();
  checkMotori();

  static uint32_t lastAutoSense = 0;
  if (now - lastAutoSense >= CHECK_SENS * 1000UL)
  {
    lastAutoSense = now;
    handleSensore(false);
  }
  
  // --- polling adattivo ---
  int hourNow = 12;
  if (timeReady)
  {
    struct tm t;
    if (getLocalTime(&t, 50))
      hourNow = t.tm_hour;
  }

  uint32_t delayMs = currentPollDelayMs(hourNow);

  wifiFollowPolling(now, delayMs);

  if ((int32_t)(now - nextPollMs) >= 0)
  {
    int numNewMessages = safeGetUpdates();
    if (numNewMessages > 0)
    boostPolling(BOOST_MSG_MS);
    wifiFollowPolling(now, currentPollDelayMs(hourNow));

    if (numNewMessages > 0)
    {
      for (int i = 0; i < numNewMessages; i++)
      {
        long uid = bot.messages[i].update_id;
        if (uid <= lastHandledUpdateId)
          continue;
        lastHandledUpdateId = uid;

        String type = bot.messages[i].type;
        String text = bot.messages[i].text;
        String chatId = bot.messages[i].chat_id;

        int msgIdiNT = bot.messages[i].message_id;
        String messageId = String(msgIdiNT);

        if (type == "message")
        {
          logLine(DEBUG_L, String("Messaggio: ") + text, true, false);
          handleMessage(text, chatId, messageId);
        }
        else if (type == "callback_query")
        {
          logLine(DEBUG_L, String("Callback: ") + text, true, false);
          handleCallBack(text, chatId, messageId);
        }
      }
    }
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

  //  Verifica spazio disponibile
  if (SPIFFS.totalBytes() - SPIFFS.usedBytes() < 10240)
  { // <10KB
    SPIFFS.remove("/log.old");
    SPIFFS.rename("/log.txt", "/log.old");
    logBytes = 0;
  }

  // Ruota PRIMA di scrivere, basandoti sul contatore in RAM
  if (logBytes + line.length() > MAX_LOG_SIZE)
  {
    SPIFFS.remove("/log.old");             // ok anche se non esiste
    SPIFFS.rename("/log.txt", "/log.old"); // backup dell’ultimo log
    logBytes = 0;                          // riparti con un log nuovo
  }

  File f = SPIFFS.open("/log.txt", FILE_APPEND);
  if (!f)
    return;

  size_t written = f.print(line); // print() ritorna i byte scritti (valore utile per il contatore) [web:430]
  f.close();

  logBytes += written;
}

void logLine(LogLevel lvl, const String &msg, bool newline = true, bool toTelegram = false)
{
  if (!debug && lvl == DEBUG_L)
    return;

  const char *L[] = {"I", "D", "W", "E"};

  String line;
  line.reserve(256); // scegli una stima realistica
  line = getTime();
  line += " | ";
  line += L[lvl];
  line += " | ";
  line += " ";
  line += msg;

  // Log file
  appendLogFile(line + "\r\n");

  // Serial
  Serial.print(line);

  // Telnet
  if (telnetClient && telnetClient.connected())
  {
    telnetClient.print(line);
    if (newline)
      telnetClient.print("\r\n"); // <-- QUESTO sistema la “scaletta” [web:187]
  }

  // statistiche giornaliere
  if (lvl == WARN)
    stats.warnCount++;
  if (lvl == ERROR_L)
    stats.errCount++;

  // Telegram

  if (toTelegram)
  {
    if (millis() - lastTelegramMs >= TELEGRAM_MIN_INTERVAL_MS)
    {
      bot.sendMessage(CHAT_ID, line); // line già include timestamp+livello
      lastTelegramMs = millis();
    }
  }
}

String tailLog(int maxLines)
{
  File f = SPIFFS.open("/log.txt", FILE_READ);
  if (!f)
    return "Nessun log.";

  String lines[80];
  maxLines = min(maxLines, 80);
  int idx = 0;

  while (f.available())
  {
    lines[idx % maxLines] = f.readStringUntil('\n');
    idx++;
  }
  f.close();

  int start = max(0, idx - maxLines);
  String out;
  out.reserve(2528);
  for (int i = start; i < idx; i++)
    out += lines[i % maxLines] + "\n";
  return out;
}

String tailWarnError(int maxLines, bool includeOld)
{
  if (!spiffsOK)
    return "SPIFFS non disponibile";

  const size_t MAX_OUT = 3500; // limite di caratteri per telegram

  // Ring buffer per le ultime righe matching
  const int CAP = 120;
  String ring[CAP];
  for (int i = 0; i < CAP; i++)
    ring[i].reserve(128);

  int cap = min(maxLines, CAP);
  int idx = 0;

  auto scanFile = [&](const char *path)
  {
    File f = SPIFFS.open(path, FILE_READ);
    if (!f)
      return;

    while (f.available())
    {
      String s = f.readStringUntil('\n');
      s.trim(); // toglie \r e spazi (come fai in telnetSendTail) [file:268]

      // Match sul formato del tuo logLine: " | W | " / " | E | " [file:268]
      if (s.indexOf(" | W | ") >= 0 || s.indexOf(" | E | ") >= 0)
      {
        ring[idx % cap] = s;
        idx++;
      }
    }
    f.close();
  };

  // Prima /log.old (opzionale), poi /log.txt così in uscita hai i più recenti
  if (includeOld)
    scanFile("/log.old");
  scanFile("/log.txt");

  if (idx == 0)
    return "Nessun WARNING/ERROR nel log.";

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
  logBytes = r ? (size_t)r.size() : 0;
  if (r)
    r.close();
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
      telnetClient.println("Telnet OK. Comandi: tail, alert, clear, size");
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
  else if (cmd == "clear")
  {
    SPIFFS.remove("/log.txt");
    telnetClient.println("OK cleared.");
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
    telnetClient.println("Comandi: tail, alert, clear, size");
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

  if (maxLines <= 0)
    maxLines = 50;
  maxLines = min(maxLines, 200); // limite hard

  // Ring buffer statico: non va sullo stack e non rialloca ogni volta
  static String lines[200];
  static bool inited = false;
  if (!inited)
  {
    for (int i = 0; i < 200; i++)
    {
      lines[i].reserve(128); // riduce frammentazione/allocazioni [web:162]
    }
    inited = true;
  }

  int idx = 0;

  while (f.available())
  {
    String s = f.readStringUntil('\n'); // leggi una riga [web:215]
    s.trim();                           // toglie \r e spazi finali
    lines[idx % maxLines] = s;          // ring buffer
    idx++;
  }
  f.close();

  int start = max(0, idx - maxLines);
  for (int i = start; i < idx; i++)
  {
    telnetClient.println(lines[i % maxLines]);
  }
  telnetClient.println("-- EOF (tail) --");
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

  int dryValue = 3300;
  int wetValue = 1050;

  leggiSensori(umidita);

  int umiditaSens1 = map(umidita[0], dryValue, wetValue, 0, 100);
  int umiditaSens2 = map(umidita[1], dryValue, wetValue, 0, 100);

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

  autoTickZone(az1, Motore_1, umiditaSens1, !health.sensore1Disconnesso);
  autoTickZone(az2, Motore_2, umiditaSens2, !health.sensore2Disconnesso);

  // --- anti-spam log umidità ---
  static int lastLoggedPct1 = -1;
  static int lastLoggedPct2 = -1;
  static uint32_t lastHumLogMs = 0;

  const uint32_t now = millis();

  const uint32_t HUM_LOG_INTERVAL_MS = 10UL * 60UL * 1000UL; // 10 minuti
  const int HUM_DELTA_PCT = 5;                               // 5%

  int d1 = (lastLoggedPct1 < 0) ? 999 : abs(umiditaSens1 - lastLoggedPct1);
  int d2 = (lastLoggedPct2 < 0) ? 999 : abs(umiditaSens2 - lastLoggedPct2);

  bool periodic = (now - lastHumLogMs >= HUM_LOG_INTERVAL_MS);
  bool changed = (d1 >= HUM_DELTA_PCT) || (d2 >= HUM_DELTA_PCT); // "> 5%" come hai scritto

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
  }
  if (who == 2)
  {
    offTimeMot2 = 0;
    health.motore2StartTime = 0;
    digitalWrite(Pin_Relay2, HIGH);
  }
  if (who == 3)
  {
    offTimeMot1 = 0;
    health.motore1StartTime = 0;
    digitalWrite(Pin_Relay1, HIGH);

    offTimeMot2 = 0;
    health.motore2StartTime = 0;
    digitalWrite(Pin_Relay2, HIGH);
  }
}

void askTime(const String &who)
{
  String keyboardJson = F(
      "[["
      "{\"text\":\"10s\",\"callback_data\":\"t_10\"},"
      "{\"text\":\"30s\",\"callback_data\":\"t_30\"}"
      "],"
      "["
      "{\"text\":\"60s\",\"callback_data\":\"t_60\"}"
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
      bot.sendMessage(CHAT_ID, "Tempo non valido.", "");
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
        bot.sendMessage(CHAT_ID, "⏳ Motore " + motorLabel(motB) + ": attendi ~" + String(waitM) + " min", "");
      else if (rr == IRR_DAY_LIMIT)
        bot.sendMessage(CHAT_ID, "🚫 Motore " + motorLabel(motB) + ": limite 10/giorno raggiunto", "");
      else if (rr == IRR_RAIN_BLOCK)
        bot.sendMessage(CHAT_ID, "🌧️ Irrigazione bloccata (pioggia/blocco)", "");
      else
        bot.sendMessage(CHAT_ID, "Irrigazione bloccata.", "");

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
  text.trim(); // togli spazi / \n

  if (text == "/meteo")
  {
    handleMeteo();
  }
  else if (text == "/updatemeteo")
  {
    meteo.datiValidi = false;
    meteo.forecastValidi = false;

    bool ok1 = rilevoMeteo();
    bool ok2 = rilevoForecastPioggia();
    if (ok2)
      applicaBloccoDaForecast();

    bot.sendMessage(CHAT_ID,
                    (ok1 && ok2) ? "Aggiornamento meteo+forecast OK." : (ok1 ? "Meteo OK, forecast FALLITO." : "Aggiornamento meteo FALLITO."),
                    "");
  }
  else if (text == "/sensore")
  {
    handleSensore(true);
  }
  else if (text == "/debug")
  {
    handleDebug();
  }
  else if (text == "/alert")
  {
    bot.sendMessage(CHAT_ID, tailWarnError(40, true));
  }
  else if (text == "/health")
  {
    handleHealth();
  }
  else if (text == "/test")
  {
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
        "", // parseMode
        keyboardJson);
  }
  else if (text == "/log")
  {
    bot.sendMessage(CHAT_ID, tailLog(40), "");
  }
  else if (text == "/clearlog")
  {
    SPIFFS.remove("/log.txt");
    bot.sendMessage(CHAT_ID, "Log cancellato.", "");
  }
  else
  {
    logLine(INFO, String("Comando sconosciuto: ") + text, true, true);
  }
}

// Gestione Meteo
bool rilevoMeteo()
{ // aggiorna la variabile meteo
  if (WiFi.status() != WL_CONNECTED)
    return false;

  String url;
  url.reserve(256);
  url = "http://api.openweathermap.org/data/2.5/weather?q=";
  url += city;
  url += "&appid=";
  url += openWeatherMapApiKey;
  url += "&units=metric&lang=it";

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(url);

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

  unsigned long etaMin = (millis() - meteo.ultimoAggiornamento) / 60000UL;

  String msg;
  msg.reserve(450);
  msg += "METEO " + city + "\n";
  msg += "Condizioni: " + meteo.condizioniMeteo + "\n";
  msg += "Temp: " + String(meteo.temperatura, 2) + " °C\n";
  msg += "Umidita: " + String(meteo.umidita) + "%\n";
  msg += "Pioggia 1h: " + String(meteo.pioggiaUltimaOra, 2) + " mm\n";
  msg += "Aggiornato: " + String(etaMin) + " min fa\n";

  if (meteo.forecastValidi)
  {
    msg += "Prev 3h (>= " + String(soglia_Minima_Pioggia, 1) + "mm): ";
    msg += (meteo.pioggiaPrevista3h ? "SI" : "NO");
    msg += " (" + String(meteo.mmPrevisti3h, 2) + "mm)\n";

    msg += "Prev 6h (>= " + String(soglia_Minima_Pioggia, 1) + "mm): ";
    msg += (meteo.pioggiaPrevista6h ? "SI" : "NO");
    msg += " (" + String(meteo.mmPrevisti6h, 2) + "mm)\n";
  }
  else
  {
    msg += "Previsione 3h/6h: ND\n";
  }

  if (bloccoIrrigazione)
  {
    if ((int32_t)(millis() - scadenzaBloccoIrrigazione) < 0)
    {
      uint32_t remaining = scadenzaBloccoIrrigazione - millis();
      msg += "\n⛔ Blocco irrigazione: " + String(remaining / 60000UL) + " min";
    }
    else
    {
      msg += "\n✅ Blocco irrigazione scaduto";
    }
  }

  bot.sendMessage(CHAT_ID, msg, "");
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

  // Richiesta forecast: prendo pochi timestamp (cnt=3) per coprire fino a ~6-9 ore
  // OpenWeatherMap supporta cnt per limitare il numero di elementi in "list". [page:0]
  String url;
  url.reserve(256);
  url = "http://api.openweathermap.org/data/2.5/forecast?q=" + city +
        "&appid=" + openWeatherMapApiKey + "&units=metric&lang=it&cnt=3";

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(url);
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
  String msg;
  msg.reserve(512);

  msg = "=== SYSTEM HEALTH ===\n\n";

  // 🌡️ TEMPERATURA ESP32
  msg += "🌡️ ESP32: ";
  if (!isfinite(health.temperaturaESP32) || health.temperaturaESP32 == 0.0f)
  {
    msg += "--";
  }
  else
  {
    msg += String(health.temperaturaESP32, 1) + "°C";
    if (health.temperaturaElevata)
      msg += " 🔥";
  }
  msg += "\n";

  // 📶 WiFi
  msg += "📶 WiFi: " + String(health.rssi) + " dBm";
  if (health.wifiDebole)
    msg += " ⚠️";
  msg += "\n";

  msg += "💾 HEAP: " + String(health.heapFreeKB) + " KB (largest " + String(health.heapLargestKB) + " KB)\n";
  msg += "🗄️ SPIFFS: " + String(health.spiffsFreeKB) + " KB liberi\n";

  // 📱 Telegram
  msg += "📱 Telegram: ";
  if (health.wifiDisconnesso)
    msg += "— (WiFi off)";
  else if (health.telegramIrraggiungibile)
    msg += "❌ OFFLINE";
  else
  {
    unsigned long ageS = (millis() - health.lastSuccessfulTelegramComm) / 1000UL;
    msg += "✅ OK (" + String(ageS < 60 ? ageS : (ageS < 3600 ? ageS / 60 : ageS / 3600));
    msg += (ageS < 60 ? "s" : (ageS < 3600 ? "m" : "h"));
    msg += ")";
  }
  msg += "\n";

  // 🚿 Irrigazioni
  msg += "🚿 Irrigazioni oggi: M1=" + String(health.irrigazioniOggiMot1) + " M2=" + String(health.irrigazioniOggiMot2) + "\n";

  // ⚠️ AVVISI ATTIVI
  if (health.sensore1Disconnesso)
    msg += "\n❌🌱 Sensore 1 disconnesso";
  if (health.sensore2Disconnesso)
    msg += "\n❌🌱 Sensore 2 disconnesso";
  if (health.umiditaCritica)
    msg += "\n🚨🌵 Umidità critica";
  if (health.memoriaInsufficiente)
    msg += "\n💾 Memoria insufficiente";

  bot.sendMessage(CHAT_ID, msg, ""); // chat_id, text, parse_mode [web:369]
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
    int pct1 = map(constrain(raw1, 1050, 3300), 3300, 1050, 0, 100);
    int pct2 = map(constrain(raw2, 1050, 3300), 3300, 1050, 0, 100);

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

void checkTemperaturaESP32()
{
  static uint32_t lastCheck = 0;
  uint32_t now = millis();

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

void checkWiFiSignal()
{
  static uint32_t lastCheck = 0;
  uint32_t now = millis();

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

    logLine(WARN, "🔄📡 Tentativo riconnessione WiFi (non bloccante)...", true, false);

    WiFi.disconnect();
    delay(10);
    ArduinoOTA.handle(); // OTA-safe
    yield();

    WiFi.begin(ssid, password); // avvia reconnessione, ma NON aspettare qui

    // Se vuoi: dopo begin, prova a leggere status e loggare “in corso”
    logLine(WARN, "📡⏳ WiFi: reconnessione avviata, riprovo al prossimo check", true, false);
    return;
  }

  // WiFi CONNESSO
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
  static uint32_t backoffMs = 3000;

  static bool wasOffline = false;
  static uint32_t lastLoggedBackoffMs = 0;
  static uint32_t lastFailLogMs = 0;

  if (WiFi.status() != WL_CONNECTED)
  {
    health.telegramIrraggiungibile = true;
    wasOffline = true;
    return 0;
  }

  const uint32_t now = millis();

  // Ancora in backoff: non loggare (zero spam)
  if ((int32_t)(now - nextTryMs) < 0)
  {
    return 0;
  }

  uint16_t old = bot.waitForResponse;
  client.setHandshakeTimeout(10);

  bot.waitForResponse = old;

  int n = bot.getUpdates(lastHandledUpdateId + 1);

  health.lastSuccessfulTelegramComm = now;

  if (n > 0)  // n>0: messaggi ricevuti → reset completo
  {
    health.telegramIrraggiungibile = false;

    if (wasOffline)
    {
      logLine(INFO, "✅🤖 Telegram tornato online", true, false);
      wasOffline = false;
    }

    backoffMs = 3000;
    lastLoggedBackoffMs = 0;
    nextTryMs = 0;  // Reset backoff, usa polling adattivo del loop
    return n;
  }
  else if (n >= 0)  // n==0: OK ma vuoto → no backoff
  {
    // Vuoto ma connessione OK → polling normale continua
    backoffMs = 3000;
    nextTryMs = 0;  // No backoff forzato
    return 0;
  }

  // ❌ ERRORE VERO: solo se n < 0
  health.telegramIrraggiungibile = true;
  wasOffline = true;

  // Calcola il prossimo backoff
  uint32_t newBackoff = backoffMs * 2;
  if (newBackoff > 15000UL)
    newBackoff = 15000UL;

  bool backoffChanged = (newBackoff != lastLoggedBackoffMs);

  // Logga solo se il backoff è cambiato (o se è passato molto tempo)
  if (backoffChanged || (now - lastFailLogMs > 60000UL))
  {
    logLine(WARN, "⚠️🤖 Telegram getUpdates fallito (n=" + String(n) + "), backoff " + String(newBackoff) + "ms", true, false);
    lastFailLogMs = now;
    lastLoggedBackoffMs = newBackoff;
  }

  backoffMs = newBackoff;
  nextTryMs = now + backoffMs;

  client.stop();
  return 0;
}


void checkMemory()
{
  static uint32_t lastCheck = 0;
  uint32_t now = millis();
  if ((now - lastCheck) < 300000UL)
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

void checkMotori()
{
  static uint32_t lastCheck = 0;
  const uint32_t now = millis();
  if (now - lastCheck < (uint32_t)CHECK_MOTOR * 1000UL)
    return;
  lastCheck = now;

  // Stato motori = offTime != 0 (coerente col tuo loop)
  const bool mot1On = (offTimeMot1 != 0);
  const bool mot2On = (offTimeMot2 != 0);

  // Se vedo ON ma startTime=0, inizializzo (copre stati incoerenti)
  if (mot1On && health.motore1StartTime == 0)
    health.motore1StartTime = now;
  if (mot2On && health.motore2StartTime == 0)
    health.motore2StartTime = now;

  // Motore 1
  if (mot1On && health.motore1StartTime != 0)
  {
    const uint32_t runS = (now - health.motore1StartTime) / 1000UL;
    if (runS > MAX_MOTOR_SECONDS && !health.motore1AttivoTroppoTempo)
    {
      health.motore1AttivoTroppoTempo = true;
      logLine(ERROR_L, "🚨 Motore 1 attivo da " + String(runS) + "s -> STOP di sicurezza", true, true);
      spegniMotori(1);
    }
  }

  // Motore 2
  if (mot2On && health.motore2StartTime != 0)
  {
    const uint32_t runS = (now - health.motore2StartTime) / 1000UL;
    if (runS > MAX_MOTOR_SECONDS && !health.motore2AttivoTroppoTempo)
    {
      health.motore2AttivoTroppoTempo = true;
      logLine(ERROR_L, "🚨 Motore 2 attivo da " + String(runS) + "s -> STOP di sicurezza", true, true);
      spegniMotori(2);
    }
  }

  // Se entrambi spenti, reset flag (così un evento futuro viene riloggato)
  if (!mot1On)
  {
    health.motore1AttivoTroppoTempo = false;
  }
  if (!mot2On)
  {
    health.motore2AttivoTroppoTempo = false;
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
  if ((int32_t)(nowMs - g_nextDayCheckMs) < 0)
    return;
  g_nextDayCheckMs = nowMs + 60000UL; // 1 volta/minuto

  const uint32_t dayId = computeDayId();
  if (dayId == 0)
    return;

  if ((uint32_t)health.lastDayReset != dayId)
  {
    health.lastDayReset = (unsigned long)dayId;
    health.irrigazioniOggiMot1 = 0;
    health.irrigazioniOggiMot2 = 0;
    logLine(INFO, "🔄🚿 Reset conteggi irrigazioni giornaliere (per motore)", true, false);
  }
}

static inline bool motorIsOn(uint8_t mot)
{
  return (mot == 1) ? (offTimeMot1 != 0) : (offTimeMot2 != 0);
}

static inline uint8_t &todayCountRef(uint8_t mot)
{
  return (mot == 1) ? health.irrigazioniOggiMot1 : health.irrigazioniOggiMot2;
}

static inline unsigned long &lastIrrRef(uint8_t mot)
{
  return (mot == 1) ? health.lastIrrMot1 : health.lastIrrMot2;
}

static bool checkOneMotorGate(uint8_t mot, uint32_t nowMs, IrrigationBlockReason &reason, uint16_t &waitMin)
{
  waitMin = 0;

  // limite per-motore al giorno
  if (todayCountRef(mot) >= MAX_IRRIGATIONS_DAY)
  {
    reason = IRR_DAY_LIMIT;
    return false;
  }

  // minimo distacco per-motore (overflow-safe con now-last)
  const unsigned long last = lastIrrRef(mot);
  if (last != 0 && (uint32_t)(nowMs - (uint32_t)last) < MIN_IRRIGATION_MS)
  {
    reason = IRR_TOO_SOON;
    uint32_t remMs = MIN_IRRIGATION_MS - (uint32_t)(nowMs - (uint32_t)last);
    waitMin = (uint16_t)(remMs / 60000UL);
    return false;
  }

  return true;
}

bool requestIrrigation(MotorSel m, uint16_t seconds, const char *source, IrrigationBlockReason &reason, uint8_t &motBlocked, uint16_t &waitMin, bool ignoreMeteo)
{
  boostPolling(BOOST_IRR_MS);
  const uint32_t now = millis();
  dailyResetTick(now);

  reason = IRR_OK;
  motBlocked = 0;
  waitMin = 0;

  // Rispetta pioggia/blocco anche in manuale (se vuoi bypass manuale dimmelo)
  if (!irrigazioneConsentita() && !ignoreMeteo)
  {
    reason = IRR_RAIN_BLOCK;
    stats.blockRain++;
    return false;
  }

  // Quali motori sto davvero avviando ORA? (se già ON non conto e non applico “gap”)
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
  if (start1)
  {
    todayCountRef(1)++;
    lastIrrRef(1) = now;
  }
  if (start2)
  {
    todayCountRef(2)++;
    lastIrrRef(2) = now;
  }

  // contatori per report notturno
  if (start1)
  {
    stats.irrCount1++;
    stats.irrSec1 += (uint32_t)seconds;
  }
  if (start2)
  {
    stats.irrCount2++;
    stats.irrSec2 += (uint32_t)seconds;
  }

  logLine(INFO, String("🚿▶️ IRR START ") + source + " m=" + motorLabel((int)m) + " c1=" + String(health.irrigazioniOggiMot1) + " c2=" + String(health.irrigazioniOggiMot2),
          true, true);

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
    return "🟠 BASSA";
  if (um < 60)
    return "🟢 OTTIMALE";
  if (um < 80)
    return "🔵 ALTA";
  return "🟣 SATURA";
}

// statistiche giornaliere
void nightlyReportTick(uint32_t nowMs)
{
  static uint32_t nextCheckMs = 0;
  if ((int32_t)(nowMs - nextCheckMs) < 0)
    return;
  nextCheckMs = nowMs + 60000UL; // ogni 60s, leggero

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
    return;

  const uint32_t dayId = computeDayId();
  if (dayId == 0)
    return;

  if (stats.lastReportDayId == dayId)
    return; // già inviato oggi

  // opzionale: evita invio mentre irriga
  if (offTimeMot1 != 0 || offTimeMot2 != 0)
    return;

  const bool ok = sendNightlyReport();
  if (ok)
  {
    resetDailyStats();
    stats.lastReportDayId = dayId; // set dopo il reset
  }
  else
  {
    logLine(WARN, "⚠️ Report notturno NON inviato (Telegram/WiFi). Riprovo nella finestra.", true, false);
  }
}

static bool sendNightlyReport()
{
  static const size_t TG_MAX = 3900; // margine sotto 4096

  String msg;
  msg.reserve(1200);

  // media safe
  const float avg1 = (stats.humN1 > 0) ? (float)stats.humSum1 / (float)stats.humN1 : -1.0f;
  const float avg2 = (stats.humN2 > 0) ? (float)stats.humSum2 / (float)stats.humN2 : -1.0f;

  msg += "REPORT NOTTURNO\n";

  msg += "Log: WARN ";
  msg += String(stats.warnCount);
  msg += " / ERROR ";
  msg += String(stats.errCount);
  msg += "\n";

  msg += "Umidita P1: ";
  if (stats.humN1 == 0)
    msg += "ND\n";
  else
  {
    msg += "min ";
    msg += String(stats.humMin1);
    msg += " avg ";
    msg += String(avg1, 1);
    msg += " max ";
    msg += String(stats.humMax1);
    msg += "\n";
  }

  msg += "Umidita P2: ";
  if (stats.humN2 == 0)
    msg += "ND\n";
  else
  {
    msg += "min ";
    msg += String(stats.humMin2);
    msg += " avg ";
    msg += String(avg2, 1);
    msg += " max ";
    msg += String(stats.humMax2);
    msg += "\n";
  }

  msg += "Irrigazioni: M1 ";
  msg += String(stats.irrCount1);
  msg += " (";
  msg += String(stats.irrSec1);
  msg += "s), M2 ";
  msg += String(stats.irrCount2);
  msg += " (";
  msg += String(stats.irrSec2);
  msg += "s)\n";

  msg += "Blocchi: pioggia ";
  msg += String(stats.blockRain);
  msg += ", troppo presto ";
  msg += String(stats.blockTooSoon);
  msg += ", limite giorno ";
  msg += String(stats.blockDayLimit);
  msg += "\n";

  // ultimi warning ed errori (troncati se troppo lunghi)
  msg += "\n";
  String tail = tailWarnError(20, false);
  size_t room = (msg.length() < TG_MAX) ? (TG_MAX - msg.length()) : 0;
  if (tail.length() > room)
  {
    tail = tail.substring(0, room);
    // opzionale: piccola nota finale (se c'è spazio)
    if (tail.length() >= 15)
    {
      tail.remove(tail.length() - 15);
      tail += "\n...(troncato)";
    }
  }
  msg += tail;

  // ACK: torna true/false
  return bot.sendMessage(CHAT_ID, msg, "");
}

static inline void resetDailyStats()
{
  stats = DailyStats(); // reset totale (richiede costruttori/valori di default)
}

// helper per polling e wifi sleep mode adattivi
static inline bool isNightHour(int h)
{
  return (h < ora_Inizio_Giorno || h > ora_Fine_Giorno);
}

static inline void boostPolling(uint32_t ms)
{
  uint32_t now = millis();
  uint32_t until = now + ms;
  if ((int32_t)(until - pollBoostUntilMs) > 0)
    pollBoostUntilMs = until;
  if ((int32_t)(now - nextPollMs) < 0)
    nextPollMs = now;
  nextWifiPolicyMs = 0;
}

static inline uint32_t currentPollDelayMs(int hourNow)
{
  uint32_t base = isNightHour(hourNow) ? POLL_NIGHT_MS : POLL_DAY_MS;
  if ((int32_t)(millis() - pollBoostUntilMs) < 0)
    return min(base, POLL_BOOST_MS);
  return base;
}

static inline bool isBoostedNow(uint32_t nowMs)
{
  return (int32_t)(nowMs - pollBoostUntilMs) < 0;
}

static inline bool motorsOnNow()
{
  return (offTimeMot1 != 0) || (offTimeMot2 != 0);
}

static inline bool telnetOnNow()
{
  return (telnetClient && telnetClient.connected());
}

static inline void wifiFollowPolling(uint32_t nowMs, uint32_t delayMs)
{
  // Evita toggle continuo
  if ((int32_t)(nowMs - nextWifiPolicyMs) < 0)
    return;
  nextWifiPolicyMs = nowMs + 5000UL;

  // Vincoli richiesti:
  // - finché non ho l’ora: full
  // - telnet connesso: full
  // - motori ON: full
  // - in boost: full
  const bool forceFull =
      (!timeReady) || telnetOnNow() || motorsOnNow() || isBoostedNow(nowMs);

  // “polling rallentato” = stai andando in modalità notte (delay grande)
  const bool wantPs =
      (!forceFull) &&
      (WiFi.status() == WL_CONNECTED) &&
      (delayMs >= POLL_NIGHT_MS);

  if (wantPs == wifiPsOn)
    return;

  if (wantPs)
  {
    WiFi.setSleep(true);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM); // power-save moderato
  }
  else
  {
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE); // full-power / bassa latenza
  }

  wifiPsOn = wantPs;
}

/*
quando motori accesi per troppo tempo mandare warnin e bloccare l'irrigazione per tempo finche non si controlla

con comando /log o /alert continua a spammare messaggi su telegra

riorganizzare loop e setup

Creare controllo livello acqua

Utilizzare doppio core

yield();
*/
