// rifatto deleteMessage, correzione messaggi di avvio entrambi i motori
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

#include "secrets.h"

// Pin utilizzati
#define Pin_SensoreContenitore 34
#define Pin_Sensore1 33
#define Pin_Sensore2 32
#define Pin_Relay1 18
#define Pin_Relay2 19

// Variabili utilizzate
struct DatiMeteo
{
  bool staPiovendo;
  String condizioniMeteo;
  float temperatura;
  float pioggiaUltimaOra;
  int umidita;
  unsigned long ultimoAggiornamento;
  bool datiValidi;
};

enum BotState
{
  IDLE,
  ASK_TIME_MOT1,
  ASK_TIME_MOT2,
  ASK_TIME_BOTH
};

enum LogLevel
{
  INFO,
  DEBUG_L,
  WARN,
  ERROR_L
};

enum MotorSel
{
  Motore_1 = 1,
  Motore_2 = 2,
  Entrambi_i_Motori = 3
};

BotState botstate = IDLE;
MotorSel pendingMotor = Motore_1;
bool timeReady = false;
bool spiffsOK = false;
const size_t MAX_LOG_SIZE = 400 * 1024;
size_t logBytes = 0;

// Dati Pioggia
DatiMeteo meteo;                                          // variabile per contenere i dati del meteo
bool bloccoIrrigazione = false;                           // blocca irrigazione quando piove
unsigned long scadenzaBloccoIrrigazione = 0;              // tempo dal blocco
const unsigned long DurataBloccoPioggia = 1000 * 60 * 60; // durata blocco 1 ora

const unsigned long intervallo_Refresh_Giorno = 1000 * 60 * 60;    // di giorno il refresh è ogni ora
const unsigned long intervallo_Refresh_Notte = 1000 * 60 * 60 * 3; // di notte il refresh è ogni 3 ore
const unsigned long durata_Cash_Valida = 1000 * 60 * 30;           // la cash dura 30 minuti
const int soglia_Minima_Pioggia = 0.5;

const int ora_Inizio_Giorno = 6; // indica l'orario di inizio giorno
const int ora_Fine_Giorno = 23;  // indica l'orario di fine giorno

// count Irrigazioni
enum IrrigationBlockReason : uint8_t
{
  IRR_OK = 0,
  IRR_RAIN_BLOCK,
  IRR_TOO_SOON,
  IRR_DAY_LIMIT
};

static uint32_t g_nextDayCheckMs = 0; // rate-limit del check giorno

// variabili Healt
struct SystemHealth
{
  // Sensori
  bool sensore1Disconnesso : 1;
  bool sensore2Disconnesso : 1;
  bool umiditaCritica : 1;

  // Temperatura ESP32
  bool temperaturaElevata : 1;

  // WiFi
  bool wifiDebole : 1;
  bool wifiDisconnesso : 1;

  // Telegram
  bool telegramIrraggiungibile : 1;

  // Memoria
  bool memoriaInsufficiente : 1;
  uint16_t heapFreeKB;
  uint16_t heapLargestKB;

  // Motori
  bool motore1AttivoTroppoTempo : 1;
  bool motore2AttivoTroppoTempo : 1;

  // Irrigazioni
  bool irrigazioniTroppoFrequenti : 1;

  uint8_t irrigazioniOggiMot1;
  uint8_t irrigazioniOggiMot2;

  unsigned long lastIrrMot1;
  unsigned long lastIrrMot2;

  // Valori (solo quelli necessari)
  int8_t rssi; // potenza segnale wifi
  float temperaturaESP32;
  uint16_t spiffsFreeKB;
  uint8_t irrigazioniOggi;

  // Timestamp ottimizzati (usa solo quando serve)
  unsigned long lastSensorCheck;
  unsigned long lastTempCheck;
  unsigned long lastWifiCheck;
  unsigned long lastMemoryCheck;
  unsigned long lastIrrigationTime;
  unsigned long lastDayReset;
  unsigned long motore1StartTime;
  unsigned long motore2StartTime;
  unsigned long lastSuccessfulTelegramComm;
};

SystemHealth health = {0};

const int8_t RSSI_DEBOLE = -75;
const int8_t RSSI_CRITICO = -85;
const float TEMP_WARNING = 75.0;
const float TEMP_CRITICAL = 85.0;
const uint8_t UMIDITA_CRITICA = 15;
const uint16_t MAX_MOTOR_SECONDS = 600;   // 10 min in secondi
const uint32_t MIN_IRRIGATION_MS = 30000; // 1 ore 3600000UL
const uint8_t MAX_IRRIGATIONS_DAY = 10;
const uint16_t MIN_FREE_KB = 50;
const uint16_t SENSOR_LOW = 500;
const uint16_t SENSOR_HIGH = 4000;

// Intervalli check (ottimizzati)
const uint8_t CHECK_TEMP = 30UL;
const uint8_t CHECK_WIFI = 10UL;
const uint16_t CHECK_MEMORY = 300UL;
const uint8_t CHECK_MOTOR = 5UL;
const uint8_t CHECK_TELEGRAM = 60UL;
const uint8_t CHECK_SENS = 20UL;

// delete Message
WiFiClientSecure deleteClient;                       //  Client dedicato SOLO alle delete (non usare lo stesso "client" del bot)
static const uint32_t DELETE_MIN_INTERVAL_MS = 1200; // allineato al tuo TELEGRAM_MIN_INTERVAL_MS

enum DeleteKind : uint8_t
{
  DEL_CALLBACK = 1,
  DEL_USERMSG = 2,
  DEL_BOTMSG = 3
};

struct DeleteReq
{
  char chatId[24];
  uint32_t msgId;
  uint8_t retries;
  DeleteKind kind;
  uint32_t enqMs; // millis() quando lo accodi
  uint32_t ttlMs; // durata massima di retry per questo messaggio
};

static DeleteReq delQ[12];
static uint8_t delHead = 0, delTail = 0, delCount = 0;
static uint32_t delNextMs = 0;

static const uint32_t TTL_CALLBACK_MS = 30UL * 60UL * 1000UL;      // 30 min per eliminare messaggi di callback
static const uint32_t TTL_USERMSG_MS = 6UL * 60UL * 60UL * 1000UL; // 6 ore per eliminare messaggi dell'utente
static const uint32_t TTL_BOTMSG_MS = 2UL * 60UL * 60UL * 1000UL;  // 2 ore per eliminare messaggi del bot temporanei

enum DelResult : uint8_t
{
  DEL_OK,
  DEL_RETRY,
  DEL_DROP
};

struct DelOutcome
{
  DelResult res;
  uint32_t retryAfterMs; // 0 se non presente
  int httpCode;          // per log/debug
  int apiErrorCode;      // 0 se non presente
};

// variabili per irrigazione automatica
struct AutoZone
{
  bool active = false;  // true = sto irrigando questo vaso in AUTO
  uint8_t startTh = 25; // start: sotto a questo -> accendo
  uint8_t stopTh = 30;  // stop: sopra a questo -> spengo
};

AutoZone az1, az2;
bool autoEnabled = true;

// Dati WiFi
const char *ssid = SECRET_WIFI_SSID;
const char *password = SECRET_WIFI_PASS;

// Configurazione Meteo
String openWeatherMapApiKey = SECRET_API_OPENWEATHER;
String city = "Vernasca,IT"; // Città

// Token del bot Telegram e chat ID
#define BOTtoken SECRET_BOT_TOKEN
#define CHAT_ID SECRET_CHAT_ID

// avvio bot telegram
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// Avvio di Telnet
WiFiServer telnetServer(23); // Porta Telnet
WiFiClient telnetClient;
String telnetLine; // buffer comando telnet

// Variabili per bot telegram
int botRequestDelay = 3000;       // Tempo minimo tra due controlli per nuovi messaggi da Telegram
unsigned long lastTimeBotRan = 0; // Memorizza l’ultima volta in cui il bot ha controllato nuovi messaggi
unsigned long lastTelegramMs = 0;
const unsigned long TELEGRAM_MIN_INTERVAL_MS = 1200; // ~1 msg/sec prudente
long lastHandledUpdateId = 0;
unsigned long lastMotorCommandTime = 0;
const unsigned long MOTOR_DEBOUNCE_INTERVAL = 2000; // 2 secondi tra comandi
bool motorOperationInProgress = false;
static uint32_t stateTimeoutMs = 0;
const uint32_t STATE_TIMEOUT_WINDOW_MS = 30000UL; // tempo di timeout per mancata risposta nel accensione motori manualmente

// Variabili motori
unsigned long offTimeMot1 = 0;
unsigned long offTimeMot2 = 0;

// inizializzo varibili per debug e manutenzione
bool manutenzione = false;
bool debug = false;

// satistiche giornaliere
struct DailyStats
{
  // log
  uint16_t warnCount = 0;
  uint16_t errCount = 0;

  // umidità (sensore 1 e 2)
  uint16_t humMin1 = 101, humMax1 = 0;
  uint32_t humSum1 = 0;
  uint16_t humN1 = 0;

  uint16_t humMin2 = 101, humMax2 = 0;
  uint32_t humSum2 = 0;
  uint16_t humN2 = 0;

  // irrigazioni
  uint16_t irrCount1 = 0, irrCount2 = 0;
  uint32_t irrSec1 = 0, irrSec2 = 0;

  // blocchi irrigazione
  uint16_t blockRain = 0, blockTooSoon = 0, blockDayLimit = 0;

  // scheduler
  uint32_t lastReportDayId = 0;
};

static DailyStats stats;

static const uint8_t NIGHT_REPORT_HOUR = 3; // orario per generare il report
static const uint8_t NIGHT_REPORT_MIN_FROM = 0;
static const uint8_t NIGHT_REPORT_MIN_TO = 60; // intervallo di 1 ora per generarlo

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
bool requestIrrigation(MotorSel m, uint16_t seconds, const char *source, IrrigationBlockReason &reason, uint8_t &motBlocked, uint16_t &waitMin);

// delete Message
static bool enqueueDelete(const String &chatId, uint32_t msgId, DeleteKind kind);
static DelOutcome deleteNow(const char *chatId, uint32_t msgId);
static void processDeleteQueue();
static inline void popDeleteHead();

// funzione Helper per log
static inline String boolToEmoji(bool v, bool inverted = false);
static inline String umiditaStatusEmoji(int um);

// statistiche giornaliere
void nightlyReportTick(uint32_t nowMs);
static void sendNightlyReport();
static void resetDailyStats();

void setup()
{
  Serial.begin(115200);
  delay(200);

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
  client.setTimeout(2000);
  bot.waitForResponse = 3000;

  // delete Message
  deleteClient.setHandshakeTimeout(7);
  deleteClient.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  deleteClient.setTimeout(2000);

  // Messaggio di avvio
  bot.sendMessage(CHAT_ID, "BOT ATTIVO!", "");

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

  dailyResetTick(millis());

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

  // Gestione bot Telegram ogni botRequestDelay ms
  if (now - lastTimeBotRan > (unsigned long)botRequestDelay)
  {
    int numNewMessages = safeGetUpdates();
    lastTimeBotRan = now;

    if (numNewMessages > 0)
    {
      for (int i = 0; i < numNewMessages; i++)
      {
        // evita messaggi doppi
        long uid = bot.messages[i].update_id;
        if (uid <= lastHandledUpdateId)
          continue;
        lastHandledUpdateId = uid;

        // salva tutte le informazioni
        String type = bot.messages[i].type;
        String text = bot.messages[i].text;
        String chatId = bot.messages[i].chat_id;
        int msgIdiNT = bot.messages[i].message_id;
        String messageId = String(msgIdiNT); // ID per cancellare

        if (type == "message")
        {
          logLine(DEBUG_L, String("Messaggio:") + text, true, false);
          handleMessage(text, chatId, messageId);
        }
        else if (type == "callback_query")
        {
          logLine(DEBUG_L, String("messaggio ") + text, true, false);
          handleCallBack(text, chatId, messageId);
        }
      }
    }
  }

  // elimina i messaggi
  processDeleteQueue();
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
    if (telnetClient && telnetClient.connected())
      telnetClient.stop();
    telnetClient = telnetServer.available();
    telnetClient.println("Telnet OK. Comandi: tail, alert, clear, size");
  }

  if (!(telnetClient && telnetClient.connected()))
    return;

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
  telnetClient.println("-- EOF --\n");
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

  // (Opzionale) se vuoi che sia requestIrrigation a dirti IRR_RAIN_BLOCK, rimuovi questo check
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

  // delete message
  enqueueDelete(chatId, (uint32_t)messageId.toInt(), DEL_CALLBACK);

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

    const bool ok = requestIrrigation(pendingMotor, seconds, "MANUALE", rr, motB, waitM);

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

  enqueueDelete(chatId, (uint32_t)messageId.toInt(), DEL_USERMSG);

  if (text == "/meteo")
  {
    handleMeteo();
  }
  else if (text == "/updatemeteo")
  {
    meteo.datiValidi = false;
    bool ok = rilevoMeteo();
    bot.sendMessage(CHAT_ID, ok ? "Aggiornamento meteo OK." : "Aggiornamento meteo FALLITO.", "");
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
  if ((uint32_t)(millis() - scadenzaBloccoIrrigazione) >= 0)
  {
    bloccoIrrigazione = false;
    scadenzaBloccoIrrigazione = 0;
  }
}

bool irrigazioneConsentita()
{
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

  if (!meteo.datiValidi)
  {
    logLine(ERROR_L, "☁️❌ Meteo non disponibile", true, true);
    return;
  }

  unsigned long etaMin = (millis() - meteo.ultimoAggiornamento) / 60000;

  String msg;
  msg.reserve(384);
  msg += "METEO " + city + "\n";
  msg += "Condizioni: " + meteo.condizioniMeteo + "\n";
  msg += "Temp: " + String(meteo.temperatura, 2) + " °C\n";
  msg += "Umidita: " + String(meteo.umidita) + "%\n";
  msg += "Pioggia 1h: " + String(meteo.pioggiaUltimaOra, 2) + " mm\n";
  msg += "Aggiornato: " + String(etaMin) + " min fa\n";

  if (bloccoIrrigazione)
  {
    uint32_t elapsed = (uint32_t)(millis() - scadenzaBloccoIrrigazione);
    if (elapsed < 0)
    {
      uint32_t remaining = scadenzaBloccoIrrigazione - millis();
      uint32_t remMin = remaining / 60000UL;
      msg += "\n⛔ Blocco irrigazione: " + String(remMin) + " min";
    }
    else
    {
      msg += "\n✅ Blocco irrigazione scaduto";
    }
  }

  bot.sendMessage(CHAT_ID, msg, "");
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
  bot.waitForResponse = 3000;
  client.setHandshakeTimeout(30);

  int n = bot.getUpdates(lastHandledUpdateId + 1);

  bot.waitForResponse = old;

  if (n >= 0)
  {
    health.telegramIrraggiungibile = false;
    health.lastSuccessfulTelegramComm = now;

    if (wasOffline)
    {
      logLine(INFO, "✅🤖 Telegram tornato online", true, false);
      wasOffline = false;
    }

    backoffMs = 3000;
    lastLoggedBackoffMs = 0; // reset: così al prossimo errore rilogghe
    nextTryMs = now + (uint32_t)botRequestDelay;
    return n;
  }

  // Errore
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
    logLine(WARN, "⚠️🤖 Telegram getUpdates fallito, backoff " + String(newBackoff) + "ms", true, false);
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

  // minimo distacco per-motore (overflow-safe con now-last) [web:1][web:28]
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

bool requestIrrigation(MotorSel m, uint16_t seconds, const char *source, IrrigationBlockReason &reason, uint8_t &motBlocked, uint16_t &waitMin)
{
  const uint32_t now = millis();
  dailyResetTick(now);

  reason = IRR_OK;
  motBlocked = 0;
  waitMin = 0;

  // Rispetta pioggia/blocco anche in manuale (se vuoi bypass manuale dimmelo)
  if (!irrigazioneConsentita())
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

// delete Message
static bool enqueueDelete(const String &chatId, uint32_t msgId, DeleteKind kind)
{
  if (delCount >= (sizeof(delQ) / sizeof(delQ[0])))
    return false;
  DeleteReq &r = delQ[delTail];
  memset(&r, 0, sizeof(r));
  chatId.toCharArray(r.chatId, sizeof(r.chatId));
  r.msgId = msgId;
  r.retries = 0;
  r.kind = kind;
  r.enqMs = millis();
  r.ttlMs = (kind == DEL_CALLBACK) ? TTL_CALLBACK_MS : (kind == DEL_USERMSG) ? TTL_USERMSG_MS
                                                                             : TTL_BOTMSG_MS;
  delTail = (uint8_t)((delTail + 1) % (sizeof(delQ) / sizeof(delQ[0])));
  delCount++;
  return true;
}

static DelOutcome deleteNow(const char *chatId, uint32_t msgId)
{
  DelOutcome out{DEL_RETRY, 0, 0, 0};

  if (WiFi.status() != WL_CONNECTED)
    return out;

  char url[256];
  snprintf(url, sizeof(url),
           "https://api.telegram.org/bot%s/deleteMessage?chat_id=%s&message_id=%u",
           BOTtoken, chatId, (unsigned)msgId);

  HTTPClient http;
  http.setTimeout(3000); // un po' più alto del tuo 1000ms
  if (!http.begin(deleteClient, url))
  {
    http.end();
    return out;
  }

  const int httpCode = http.GET();
  out.httpCode = httpCode;

  String body;
  if (httpCode > 0)
    body = http.getString();
  http.end();

  // Errori di rete/SSL: ritenta
  if (httpCode <= 0)
  {
    out.res = DEL_RETRY;
    return out;
  }

  // Prova a capire l'esito applicativo dal JSON
  StaticJsonDocument<384> doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err)
  {
    out.res = (httpCode == 200) ? DEL_OK : DEL_RETRY; // fallback prudente
    return out;
  }

  const bool ok = doc["ok"] | false;
  if (ok)
  {
    out.res = DEL_OK;
    return out;
  }

  const int apiCode = doc["error_code"] | 0;
  out.apiErrorCode = apiCode;

  // 429 Too Many Requests: usa retry_after se presente
  if (apiCode == 429)
  {
    const uint32_t retryAfterS = doc["parameters"]["retry_after"] | 0;
    out.retryAfterMs = retryAfterS * 1000UL;
    out.res = DEL_RETRY;
    return out;
  }

  // Errori tipicamente permanenti per delete: scarta e vai avanti
  // (es. "message can't be deleted..." / "message to delete not found")
  if (apiCode == 400)
  {
    out.res = DEL_DROP;
    return out;
  }

  // Altri errori: ritenta qualche volta
  out.res = DEL_RETRY;
  return out;
}

static inline void popDeleteHead()
{
  delHead = (uint8_t)((delHead + 1) % (sizeof(delQ) / sizeof(delQ[0])));
  delCount--;
}

static void processDeleteQueue()
{
  if (delCount == 0)
    return;
  if (WiFi.status() != WL_CONNECTED)
    return;

  const uint32_t now = millis();
  if ((int32_t)(now - delNextMs) < 0)
    return;

  DeleteReq &r = delQ[delHead];

  // TTL scaduto -> scarta
  if ((uint32_t)(now - r.enqMs) > r.ttlMs)
  {
    popDeleteHead();
    delNextMs = now + DELETE_MIN_INTERVAL_MS;
    return;
  }

  DelOutcome o = deleteNow(r.chatId, r.msgId);

  if (o.res == DEL_OK || o.res == DEL_DROP)
  {
    popDeleteHead();
    delNextMs = now + DELETE_MIN_INTERVAL_MS;
    return;
  }

  // RETRY
  r.retries++;
  if (r.retries >= 6)
  {
    popDeleteHead();
    delNextMs = now + DELETE_MIN_INTERVAL_MS;
    return;
  }

  // Backoff
  uint32_t waitMs = DELETE_MIN_INTERVAL_MS;
  if (o.apiErrorCode == 429)
  {
    waitMs = (o.retryAfterMs > 0) ? o.retryAfterMs : 5000UL;
  }
  else
  {
    uint8_t sh = r.retries;
    if (sh > 3)
      sh = 3;
    waitMs = DELETE_MIN_INTERVAL_MS * (1UL << sh);
    if (waitMs > 15000UL)
      waitMs = 15000UL;
  }

  delNextMs = now + waitMs;
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
  nextCheckMs = nowMs + 60000UL; // ogni 30s, leggero

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

  sendNightlyReport();
  resetDailyStats();
  stats.lastReportDayId = dayId; // set dopo il reset
}

static void sendNightlyReport()
{
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

  // ultimi warning ed errori
  msg += "\n";
  msg += tailWarnError(20, false);

  bot.sendMessage(CHAT_ID, msg, "");
}

static inline void resetDailyStats()
{
  stats = DailyStats();            // reset totale (richiede costruttori/valori di default)
}
/*
crear ciclo di controllo

Creare controllo livello acqua

wifi Sleep mode solo di notte

check notturno che manda statistiche, qunait warning e error, umidità minima massima e media, irrigazioni totali

sistemare boot con messaggi su telnet

implementazione nella ricerca meteo di controllo se piovera nelle prossime 3 ore

POLLING ADATTIVO (di notte alto e quanod messaggio veloce per 5 minuti) (modifica con messaggio telegram, motori accesi, telnet connesso, sensori rilevano irrigazione)

Utilizzare doppio core

yield();
*/
