#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include <UniversalTelegramBot.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#include "secrets.h"

// Pin utilizzati
enum BotState
{
  IDLE,
  ASK_TIME_MOT1,
  ASK_TIME_MOT2,
  ASK_TIME_BOTH
};

#define Pin_SensoreContenitore 34
#define Pin_Sensore1 33
#define Pin_Sensore2 32
#define Pin_Relay1 18
#define Pin_Relay2 19

BotState botstate = IDLE;

// Dati WiFi
const char *ssid = SECRET_WIFI_SSID;
const char *password = SECRET_WIFI_PASS;

// Configurazione Meteo
String openWeatherMapApiKey = SECRET_API_OPENWEATHER;
String city = "Vernasca";           // O la tua città
String countryCode = "IT";      // Codice paese

// Token del bot Telegram e chat ID
#define BOTtoken SECRET_BOT_TOKEN
#define CHAT_ID SECRET_CHAT_ID

// avvio bot telegram
WiFiClientSecure client;
UniversalTelegramBot bot(BOTtoken, client);

// Avvio di Telnet
WiFiServer telnetServer(23); // Porta Telnet
WiFiClient telnetClient;

int botRequestDelay = 3000;       // Tempo minimo tra due controlli per nuovi messaggi da Telegram
unsigned long lastTimeBotRan = 0; // Memorizza l’ultima volta in cui il bot ha controllato nuovi messaggi
unsigned long offTimeMot1 = 0;
unsigned long offTimeMot2 = 0;

// inizializzo varibili per debug e manutenzione
bool manutenzione = false;
bool debug = false;

// ---- FUNZIONI DI DEBUG (Serial + Telnet) ----
void logPrint(const String &msg)
{
  Serial.print(msg);
  if (telnetClient && telnetClient.connected())
  {
    telnetClient.print(msg);
  }
  bot.sendMessage(CHAT_ID, msg);
}

void logPrintln(const String &msg)
{
  Serial.println(msg);
  if (telnetClient && telnetClient.connected())
  {
    telnetClient.println(msg);
  }
  bot.sendMessage(CHAT_ID, msg);
}

void debugPrint(const String &msg)
{
  if (!debug)
    return;
  Serial.print(msg);
  if (telnetClient && telnetClient.connected())
  {
    telnetClient.print(msg);
  }
  bot.sendMessage(CHAT_ID, msg);
}

void debugPrintln(const String &msg)
{
  if (!debug)
    return;
  Serial.println(msg);
  if (telnetClient && telnetClient.connected())
  {
    telnetClient.println(msg);
  }
  bot.sendMessage(CHAT_ID, msg);
}

void setup()
{
  Serial.begin(115200);
  delay(100);

  debugPrintln("Boot ESP32...");

  pinMode(Pin_SensoreContenitore, INPUT_PULLUP);
  pinMode(Pin_Sensore1, INPUT);
  pinMode(Pin_Sensore2, INPUT);
  pinMode(Pin_Relay1, OUTPUT);
  pinMode(Pin_Relay2, OUTPUT);

  // Spengo i motori all'accensione
  digitalWrite(Pin_Relay1, HIGH);
  digitalWrite(Pin_Relay2, HIGH);

  // Avvio wifi
  debugPrint("Connessione a ");
  debugPrintln(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(500);
    debugPrint(".");
  }
  debugPrintln("");
  debugPrintln("Connesso a WiFi");
  debugPrint("Connesso! IP: ");
  debugPrintln(WiFi.localIP().toString());

  // Certificato root per Telegram HTTPS
  client.setCACert(TELEGRAM_CERTIFICATE_ROOT);

  // Messaggio di avvio
  bot.sendMessage(CHAT_ID, "BOT ATTIVO!", "");

  // Avvio modalita OTA
  ArduinoOTA.setHostname("esp32-ota");
  ArduinoOTA.begin();

  // Avvio modalita TELNET
  telnetServer.begin(); // Avvia server Telnet
  telnetServer.setNoDelay(true);

  // dopo che il WiFi è connesso
  debugPrintln("Imposto orario NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // UTC [web:106]

  debugPrintln("ArduinoOTA pronto");
}



bool deleteMessage(String chatId, String messageId)
{
  if (WiFi.status() != WL_CONNECTED)
    return false;

  String url = "https://api.telegram.org/bot" + String(BOTtoken) +
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

void leggiSensori(int umidita[2])
{
  umidita[0] = analogRead(Pin_Sensore1);
  umidita[1] = analogRead(Pin_Sensore2);
}

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

void handleSensore()
{
  int umidita[2];
  leggiSensori(umidita);
  debugPrintln("umidità: " + String(umidita[0]) + ", " + String(umidita[1]));
}

void handleDebug()
{
  debug = !debug;
  if (debug)
  {
    debugPrintln("Ho attivato la modalita DEBUG!");
  }
  else
  {
    debugPrintln("Ho disattivato la modalita DEBUG!");
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
    bot.sendMessage(CHAT_ID, "Avvio il motore " + String(botstate) + " per " + String(seconds) + " secondi");
    accendiMotori(int(botstate), seconds);
    botstate = IDLE;
  }
}

void handleMessage(String text, String chatId, String messageId)
{
  text.trim(); // togli spazi / \n
  deleteMessage(chatId, messageId);
  if (text == "/acceso")
  {
    /* handleAcceso(); */
  }
  else if (text == "/spento")
  {
    /* handleSpento(); */
  }
  else if (text == "/stato")
  {
    /* handleStato(); */
  }
  else if (text == "/meteo")
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
  else
  {
    debugPrintln("Comando sconosciuto: " + text);
    bot.sendMessage(CHAT_ID, "Comando non riconosciuto", "");
  }
}

void loop()
{
  ArduinoOTA.handle();

  // Gestione bot Telegram ogni botRequestDelay ms
  unsigned long now = millis();
  if (now - lastTimeBotRan > (unsigned long)botRequestDelay)
  {
    int numNewMessages = bot.getUpdates(bot.last_message_received + 1);

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
          debugPrint("Messaggio: ");
          debugPrintln(text);
          handleMessage(text, chatId, messageId);
        }
        else if (type == "callback_query")
        {
          debugPrint("Callback: ");
          debugPrintln(text);
          handleCallBack(text, chatId, messageId);
        }
      }
      lastTimeBotRan = now;
    }

    // Gestione nuove connessioni Telnet
    if (telnetServer.hasClient())
    {
      if (telnetClient && telnetClient.connected())
      {
        telnetClient.stop();
      }
      telnetClient = telnetServer.available();
      debugPrintln("Client Telnet connesso");
    }

    // controllo spegnimento motori
    if (offTimeMot1 != 0 && (long)(now - offTimeMot1) >= 0)
    {
      spegniMotori(1);
    }

    if (offTimeMot2 != 0 && (long)(now - offTimeMot2) >= 0)
    {
      spegniMotori(2);
    }
  }
}
