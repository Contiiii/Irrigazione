#include "health.h"
#include "config.h"
#include "log.h"
#include "telegram.h"
#include "motori.h"
#include <WiFi.h>

extern const char* ssid;
extern const char* password;
extern String openWeatherMapApiKey;
extern bool bloccoIrrigazione;
extern unsigned long scadenzaBloccoIrrigazione;
extern bool spiffsOK;
extern bool debug;

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

uint8_t validazioneSensori(int raw1, int raw2)
{
  static bool lastSensor1Error = false;
  static bool lastSensor2Error = false;

  // Sensore 1
  bool sensor1Error = (raw1 < SENSOR_LOW || raw1 > SENSOR_HIGH);

  if (sensor1Error && !lastSensor1Error)
  {
    health.sensore1Disconnesso = true;
    lastSensor1Error = true;
    logLine(WARN, "⚠️ Sensore 1 disconnesso (val: " + String(raw1) + ")", true, true);
    return 1;
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
    lastSensor2Error = true;
    logLine(WARN, "⚠️ Sensore 2 disconnesso (val: " + String(raw2) + ")", true, true);
    return 2;
  }
  else if (!sensor2Error)
  {
    health.sensore2Disconnesso = false;
  }
  lastSensor2Error = sensor2Error;

  // Check umidità critica
  if (!sensor1Error && !sensor2Error)
  {
    int pct1 = map(constrain(raw1, SENSOR_WET_ADC, SENSOR_DRY_ADC), SENSOR_DRY_ADC, SENSOR_WET_ADC, 0, 100);
    int pct2 = map(constrain(raw2, SENSOR_WET_ADC, SENSOR_DRY_ADC), SENSOR_DRY_ADC, SENSOR_WET_ADC, 0, 100);

    bool critica = (pct1 < UMIDITA_CRITICA || pct2 < UMIDITA_CRITICA);

    if (critica && !health.umiditaCritica)
    {
      health.umiditaCritica = true;
      logLine(ERROR_L, "🚨 UMIDITA' CRITICA: Sens1=" + String(pct1) + "% Sens2=" + String(pct2) + "%", true, true);
    }
    else if (!critica)
    {
      health.umiditaCritica = false;
    }
  }

  return 0;
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
      WiFi.disconnect(true); // true: svuota buffer credenziali, non serve delay
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
