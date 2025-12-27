// Migliorato sistema di accensione motori (messaggi telegram)
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include <UniversalTelegramBot.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <SPIFFS.h>

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

// Variabili motori
unsigned long offTimeMot1 = 0;
unsigned long offTimeMot2 = 0;

// inizializzo varibili per debug e manutenzione
bool manutenzione = false;
bool debug = false;

// Prototipi di log
String getTime();
void appendLogFile(const String &line);
void logLine(LogLevel lvl, const String &msg, bool newline, bool toTelegram);
bool deleteMessage(String chatId, String messageId);
String tailLog(int maxLines);
void handleDebug();
void initLogSize();

// Prototipi di telnet
void handleTelnet();
void handleTelnetCommand(const String &cmd);
void telnetSendTail(const char *path, int maxLines);

// Prototipi dei sensori
void leggiSensori(int umidita[2]);
void handleSensore();

// Prototipi dei motori
void accendiMotori(int who, int tempo);
void spegniMotori(int who);
void askTime(const String &who);

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

// ---- FUNZIONI DI DEBUG (Serial + Telnet) ----
void setup()
{
  Serial.begin(115200);
  delay(200);

  // logLine(DEBUG_L, "Boot ESP32...", true, false);

  pinMode(Pin_SensoreContenitore, INPUT_PULLUP);
  pinMode(Pin_Sensore1, INPUT);
  pinMode(Pin_Sensore2, INPUT);
  pinMode(Pin_Relay1, OUTPUT);
  pinMode(Pin_Relay2, OUTPUT);

  // Spengo i motori all'accensione
  digitalWrite(Pin_Relay1, HIGH);
  digitalWrite(Pin_Relay2, HIGH);

  // Avvio wifi
  logLine(DEBUG_L, String("Connessione a ") + ssid, true, false);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    logLine(DEBUG_L, ".", false, false);
  }
  logLine(DEBUG_L, String("Connesso! IP: ") + WiFi.localIP().toString(), true, false);

  // dopo che il WiFi è connesso
  logLine(DEBUG_L, "Imposto orario NTP...", true, false);
  configTime(3600, 3600, "pool.ntp.org", "time.nist.gov");

  struct tm t;
  timeReady = getLocalTime(&t, 10000);

  // Certificato root per Telegram HTTPS
  client.setHandshakeTimeout(15);
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  bot.waitForResponse = 5000;

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
  logLine(INFO, String("SPIFFS: ") + (spiffsOK ? "OK" : "FAIL"), true, false);

  logLine(DEBUG_L, "ArduinoOTA pronto", true, false);
}

void loop()
{
  ArduinoOTA.handle();
  unsigned long now = millis();

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

  // Gestione bot Telegram ogni botRequestDelay ms

  if (now - lastTimeBotRan > (unsigned long)botRequestDelay)
  {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);
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
          logLine(DEBUG_L, String("messaggio ") + text, true, false);
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
  line.reserve(200); // scegli una stima realistica
  line = getTime();
  line += " | ";
  line += L[lvl];
  line += " | ";
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

bool deleteMessage(String chatId, String messageId)
{
  if (WiFi.status() != WL_CONNECTED)
    return false;

  String url;
  url.reserve(220);
  url = "https://api.telegram.org/bot";
  url += BOTtoken;
  url += "/deleteMessage?chat_id=";
  url += chatId;
  url += "&message_id=";
  url += messageId;

  HTTPClient http;
  http.begin(client, url); // Usa lo stesso client sicuro del bot
  int httpCode = http.GET();
  http.end();

  if (httpCode == 200)
  {
    return true;
  }
  return false;
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
  out.reserve(2500);
  for (int i = start; i < idx; i++)
    out += lines[i % maxLines] + "\n";
  return out;
}

void handleDebug()
{
  debug = !debug;
  logLine(INFO, debug ? "DEBUG ATTIVO" : "DEBUG DISATTIVO", true, false);
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
    telnetClient.println("Telnet OK. Comandi: tail, clear, size");
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
  else
  {
    telnetClient.println("Comandi: tail [N], clear, size");
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

// sensori

void leggiSensori(int umidita[2])
{
  umidita[0] = analogRead(Pin_Sensore1);
  umidita[1] = analogRead(Pin_Sensore2);
}

void handleSensore()
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
  msg.reserve(160);
  msg = "umidità: ";
  msg += umiditaSens1;
  msg += "% (";
  msg += umidita[0];
  msg += "), ";
  msg += umiditaSens2;
  msg += "% (";
  msg += umidita[1];
  msg += ")";
  logLine(INFO, msg, true, true);
}

// motori
void accendiMotori(int who, int tempo)
{
  if (who == 1)
  {
    offTimeMot1 = tempo * 1000UL + millis();
    digitalWrite(Pin_Relay1, LOW);
  }
  if (who == 2)
  {
    offTimeMot2 = tempo * 1000UL + millis();
    digitalWrite(Pin_Relay2, LOW);
  }
  if (who == 3)
  {
    offTimeMot1 = tempo * 1000UL + millis();
    digitalWrite(Pin_Relay1, LOW);
    offTimeMot2 = tempo * 1000UL + millis();
    digitalWrite(Pin_Relay2, LOW);
  }
}

void spegniMotori(int who)
{
  if (who == 1)
  {
    offTimeMot1 = 0;
    digitalWrite(Pin_Relay1, HIGH);
  }
  if (who == 2)
  {
    offTimeMot2 = 0;
    digitalWrite(Pin_Relay2, HIGH);
  }
  if (who == 3)
  {
    offTimeMot1 = 0;
    digitalWrite(Pin_Relay1, HIGH);
    offTimeMot2 = 0;
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

// Messaggi telegram
void handleCallBack(String text, String chatId, String messageId)
{
  deleteMessage(chatId, messageId);

  if (text == "mot1_on")
  {
    if (botstate != IDLE)
      return; // se sto aspettando un tempo ignora i messaggi del motore
    pendingMotor = Motore_1;
    botstate = ASK_TIME_MOT1;
    askTime("motore 1");
  }
  else if (text == "mot2_on")
  {
    if (botstate != IDLE)
      return;
    pendingMotor = Motore_2;
    botstate = ASK_TIME_MOT2;
    askTime("motore 2");
  }
  else if (text == "mot_all_on")
  {
    if (botstate != IDLE)
      return;
    pendingMotor = Entrambi_i_Motori;
    botstate = ASK_TIME_BOTH;
    askTime("entrambi i motori?");
  }

  else if (text.startsWith("t_"))
  {
    if (botstate == IDLE)
      return; // se non ho scelto un motore ignora i tempi
    int seconds = 0;
    if (text == "t_10")
      seconds = 10;
    else if (text == "t_30")
      seconds = 30;
    else if (text == "t_60")
      seconds = 60;
    logLine(INFO, "Avvio il motore " + String(pendingMotor) + " per " + String(seconds) + " secondi", true, true);

    accendiMotori((int)pendingMotor, seconds);
    botstate = IDLE;
  }
}

void handleMessage(String text, String chatId, String messageId)
{
  text.trim(); // togli spazi / \n
  deleteMessage(chatId, messageId);
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
    handleSensore();
  }
  else if (text == "/debug")
  {
    handleDebug();
  }
  else if (text == "/manutenzione")
  {
    /* handleManutenzione(); */
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
  url.reserve(160);
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
    logLine(ERROR_L, "Errore HTTP: " + String(httpCode), true, false);
    return false;
  }

  // CONTROLLO NULLPTR CRITICO
  WiFiClient *stream = http.getStreamPtr();
  if (!stream)
  {
    http.end();
    logLine(ERROR_L, "Stream non disponibile", true, false);
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
    logLine(ERROR_L, "Errore JSON: " + String(error.c_str()), true, false);
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
  if (bloccoIrrigazione)
  {
    unsigned long elapsed = millis() - (scadenzaBloccoIrrigazione - DurataBloccoPioggia);
    if (elapsed >= DurataBloccoPioggia)
    {
      bloccoIrrigazione = false;
      scadenzaBloccoIrrigazione = 0;
    }
  }
}

bool irrigazioneConsentita()
{
  if (bloccoIrrigazione)
    return false;
  if (!meteo.datiValidi)
    return false;
  if (meteo.staPiovendo)
    return false;
  return true;
}

void handleMeteo()
{
  aggiornamentoMeteoServe();

  if (!meteo.datiValidi)
  {
    logLine(ERROR_L, "Meteo non disponibile", true, true);
    return;
  }

  unsigned long etaMin = (millis() - meteo.ultimoAggiornamento) / 60000;

  String msg;
  msg.reserve(300);
  msg += "METEO " + city + "\n";
  msg += "Condizioni: " + meteo.condizioniMeteo + "\n";
  msg += "Temp: " + String(meteo.temperatura, 2) + " °C\n";
  msg += "Umidita: " + String(meteo.umidita) + "%\n";
  msg += "Pioggia 1h: " + String(meteo.pioggiaUltimaOra, 2) + " mm\n";
  msg += "Aggiornato: " + String(etaMin) + " min fa\n";

  if (bloccoIrrigazione)
  {
    unsigned long elapsed = millis() - (scadenzaBloccoIrrigazione - DurataBloccoPioggia);
    
    if (elapsed < DurataBloccoPioggia)
    {
      unsigned long remaining = DurataBloccoPioggia - elapsed;
      unsigned long remMin = remaining / 60000;
      msg += "Blocco irrigazione: " + String(remMin) + " min\n";
    }
    else
    {
      msg += "Blocco irrigazione: scaduto\n";
    }
  }

  bot.sendMessage(CHAT_ID, msg, "");
}

/*
yield();

in caso di connessione che salta
void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(5000);
  }
}

in caso di mancata risposta dei secondi del motore
unsigned long stateTimeout = 0;
if (botstate != IDLE && millis() > stateTimeout) {
  botstate = IDLE;
}

Impatto: MEDIO - Previene doppi click accidentali che possono avviare motori due volte:
​
static unsigned long lastCallback = 0;
if (millis() - lastCallback < 500) return;
lastCallback = millis();


POLLING ADATTIVO (di notte alto e quanod messaggio veloce per 5 minuti) (modifica con messaggio telegram, motori accesi, telnet connesso, sensori rilevano irrigazione)

Aggiungere emoji nei log e nei messaggi

Aggiungere lettore di warning o error nei log

crear ciclo di controllo

Creare lettura corretta sensori (leggerli 10 volte e farne una media)

Creare controllo livello acqua

wifi Sleep mode solo di notte

Utilizzare doppio core


aggiungere emoji per log piu belli

correggere bug di avvio motori

check notturno che manda statistiche, qunait warning e error, umidità minima massima e media, irrigazioni totali

comando per leggere solo  log con  errori o warning

sistemare boot con messaggi su telnet

warning sensore disconnesso o valori anomali

umidità critica

temperatura esp32 alta

wifi debole o disconnesso o impossibile connettersi

telegram irraggiungibile o non risponde

spazio memoria piccolo

motore attivo per troppo tempo

irrigazioni troppo frequenti

usare il core 0
*/