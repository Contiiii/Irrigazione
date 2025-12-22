// Inserire check livello acqua, adattare percentuale acqua al 100, meteo
// dimensiono file, ciclo di controllo, lettura corretta sensori
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

BotState botstate = IDLE;
bool timeReady = false;
bool spiffsOK = false;
const size_t MAX_LOG_SIZE = 400 * 1024;

// Dati WiFi
const char *ssid = SECRET_WIFI_SSID;
const char *password = SECRET_WIFI_PASS;

// Configurazione Meteo
String openWeatherMapApiKey = SECRET_API_OPENWEATHER;
String city = "Vernasca";  // O la tua città
String countryCode = "IT"; // Codice paese

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
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  // Messaggio di avvio
  bot.waitForResponse = 9000;
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

  File r = SPIFFS.open("/log.txt", FILE_READ);
  size_t sz = r ? (size_t)r.size() : 0;
  if (r)
    r.close();

  if (sz > MAX_LOG_SIZE)
  {
    SPIFFS.remove("/log.old"); // ok anche se non esiste
    bool ok = SPIFFS.rename("/log.txt", "/log.old");
    // opzionale: Serial.printf("rotate=%d\r\n", ok);
  }

  File f = SPIFFS.open("/log.txt", FILE_APPEND);
  if (!f)
    return;
  f.print(line);
  f.close();
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
  url = "https://api.telegram.org/bot" + String(BOTtoken) +
        "/deleteMessage?chat_id=" + chatId +
        "&message_id=" + messageId;

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

// telnet

void handleTelnet()
{
  // Accetta nuovo client
  if (telnetServer.hasClient())
  {
    if (telnetClient && telnetClient.connected())
      telnetClient.stop();
    telnetClient = telnetServer.available();
    telnetClient.println("Telnet OK. Comandi: tail, clear, size");
  }
  if (telnetClient && telnetClient.connected() && telnetClient.available())
  {
    char c = telnetClient.read();
    if (c == '\r')
      return;
    if (c == '\n')
    {
      telnetLine.trim();
      handleTelnetCommand(telnetLine);
      telnetLine = "";
    }
    else
    {
      telnetLine += c;
    }
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
    botstate = ASK_TIME_MOT1;
    askTime("motore 1");
  }
  else if (text == "mot2_on")
  {
    if (botstate != IDLE)
      return;
    botstate = ASK_TIME_MOT2;
    askTime("motore 2");
  }
  else if (text == "mot_all_on")
  {
    if (botstate != IDLE)
      return;
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
    logLine(INFO, "Avvio il motore " + String(botstate) + " per " + String(seconds) + " secondi", true, true);

    accendiMotori(int(botstate), seconds);
    botstate = IDLE;
  }
}

void handleMessage(String text, String chatId, String messageId)
{
  text.trim(); // togli spazi / \n
  deleteMessage(chatId, messageId);
  if (text == "/meteo")
  {
    /* handleMeteo(); */
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
