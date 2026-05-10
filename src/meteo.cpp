#include "meteo.h"
#include "config.h"
#include "log.h"
#include "telegram.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WiFi.h>

extern String openWeatherMapApiKey;
extern bool bloccoIrrigazione;
extern unsigned long scadenzaBloccoIrrigazione;

bool rilevoMeteo()
{
  if (WiFi.status() != WL_CONNECTED)
    return false;

  WiFiClientSecure secureClient;
  secureClient.setInsecure();

  String url;
  url.reserve(256);
  url = "https://api.openweathermap.org/data/2.5/weather?q=";
  url += CITY;
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

  bool piove = (main == "Rain" || main == "Drizzle" || r1h >= SOGLIA_MINIMA_PIOGGIA_MM);

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
  if (ora >= ORA_INIZIO_GIORNO && ora <= ORA_FINE_GIORNO)
    return INTERVALLO_REFRESH_GIORNO_MS;
  return INTERVALLO_REFRESH_NOTTE_MS;
}

bool validitaCashMeteo()
{
  if (!meteo.datiValidi)
    return false;
  unsigned long elapsed = millis() - meteo.ultimoAggiornamento;
  return elapsed <= DURATA_CACHE_METEO_MS;
}

bool aggiornamentoMeteoServe(bool forza)
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
  scadenzaBloccoIrrigazione = millis() + DURATA_BLOCCO_PIOGGIA_MS;
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
  msg += "🌤️ METEO — " + String(CITY) + "\n";
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
    msg += "• Soglia: ≥ " + String(SOGLIA_MINIMA_PIOGGIA_MM, 1) + " mm\n";
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

static inline bool isRainLike(float mm3h, const String &main)
{
  // Soglia principale: mm negli ultimi 3h previsti
  if (mm3h >= SOGLIA_MINIMA_PIOGGIA_MM)
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
  return (millis() - meteo.ultimoAggForecast) < DURATA_CACHE_FORECAST_MS;
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
  url = "http://api.openweathermap.org/data/2.5/forecast?q=" + String(CITY) +
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
