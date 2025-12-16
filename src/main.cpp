#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoOTA.h>
#include <UniversalTelegramBot.h>

#include "secrets.h"

// Dati WiFi
const char *ssid = SECRET_WIFI_SSID;
const char *password = SECRET_WIFI_PASS;

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

//inizializzo varibili per debug e manutenzione
bool manutenzione = false;
bool debug = false;

// ---- FUNZIONI DI DEBUG (Serial + Telnet) ----
void debugPrint(const String &msg)
{
  Serial.print(msg);
  if (telnetClient && telnetClient.connected())
  {
    telnetClient.print(msg);
  } 
}

void debugPrintln(const String &msg)
{
  Serial.println(msg);
  if (telnetClient && telnetClient.connected())
  {
    telnetClient.println(msg);
  }
}

void setup()
{
  Serial.begin(115200);
  delay(100);

  debugPrintln("Boot ESP32...");

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

void handleMessage(String text){
  text.trim(); // togli spazi / \n
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
    /* handleSensore(); */
  }
  else if (text == "/debug")
  {
    /* handleDebug(); */
  }
  else if (text == "/manutenzione")
  {
    /* handleManutenzione(); */
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
        String text = bot.messages[i].text;
        debugPrint("Messaggio: ");
        debugPrintln(text);
        handleMessage(text);
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
}
