#pragma once
#include <Arduino.h>
#include "secrets.h"

// =========================
// Hardware
// =========================
constexpr uint8_t Pin_SensoreContenitore = 18;
constexpr uint8_t Pin_Sensore1 = 34;
constexpr uint8_t Pin_Sensore2 = 33;
constexpr uint8_t Pin_Relay1 = 19;
constexpr uint8_t Pin_Relay2 = 32;

// =========================
// File / storage
// =========================
constexpr size_t MAX_LOG_SIZE = 400 * 1024;
constexpr const char* LOG_FILE = "/log.txt";
constexpr const char* LOG_OLD_FILE = "/log.old";

// =========================
// Sensori umidità
// =========================
constexpr uint16_t SENSOR_LOW = 500;
constexpr uint16_t SENSOR_HIGH = 4000;
constexpr uint16_t SENSOR_WET_ADC = 1050;
constexpr uint16_t SENSOR_DRY_ADC = 3300;
constexpr uint8_t SENSOR_NUM_SAMPLES = 5;
constexpr uint8_t SENSOR_SAMPLE_DELAY_MS = 5;
constexpr uint8_t UMIDITA_CRITICA = 15;

// =========================
// Irrigazione
// =========================
constexpr uint16_t MAX_MOTOR_SECONDS = 120;
constexpr uint32_t MIN_IRRIGATION_MS = 30000UL;
constexpr uint8_t MAX_IRRIGATIONS_DAY = 10;

// =========================
// Meteo
// =========================
constexpr uint32_t DURATA_BLOCCO_PIOGGIA_MS = 1000UL * 60UL * 60UL;
constexpr uint32_t INTERVALLO_REFRESH_GIORNO_MS = 1000UL * 60UL * 60UL;
constexpr uint32_t INTERVALLO_REFRESH_NOTTE_MS = 1000UL * 60UL * 60UL * 3UL;
constexpr uint32_t DURATA_CACHE_METEO_MS = 1000UL * 60UL * 30UL;
constexpr uint32_t DURATA_CACHE_FORECAST_MS = 1000UL * 60UL * 30UL;
constexpr float SOGLIA_MINIMA_PIOGGIA_MM = 0.5f;
constexpr int ORA_INIZIO_GIORNO = 6;
constexpr int ORA_FINE_GIORNO = 23;
constexpr const char* CITY = "Vernasca,IT";

// =========================
// Wi‑Fi / rete
// =========================
constexpr int8_t RSSI_DEBOLE = -78;
constexpr int8_t RSSI_CRITICO = -85;
constexpr uint16_t TELNET_PORT = 23;
constexpr uint32_t WIFI_BOOT_TIMEOUT_MS = 20000UL; // 20s timeout boot WiFi
constexpr const char* OTA_HOSTNAME = "esp32-ota";

// =========================
// Temperature / memoria
// =========================
constexpr float TEMP_WARNING = 75.0f;
constexpr float TEMP_CRITICAL = 85.0f;
constexpr uint16_t MIN_FREE_KB = 50;

// =========================
// Intervalli check
// =========================
constexpr uint8_t CHECK_TEMP = 30;
constexpr uint8_t CHECK_WIFI = 10;
constexpr uint16_t CHECK_MEMORY = 300;
constexpr uint8_t CHECK_MOTOR = 5;
constexpr uint8_t CHECK_TELEGRAM = 60;

constexpr uint32_t SENS_BASE_MS = 20000UL;
constexpr uint32_t SENS_IRR_MS = 3000UL;

// =========================
// Telegram polling
// =========================
constexpr uint32_t POLL_DAY_MS = 5000UL;
constexpr uint32_t POLL_NIGHT_MS = 20000UL;
constexpr uint32_t POLL_BOOST_MS = 3000UL;
constexpr uint32_t BOOST_MSG_MS = 120000UL;
constexpr uint32_t BOOST_MOTOR_MS = 300000UL;
constexpr uint32_t BOOST_TELNET_MS = 60000UL;
constexpr uint32_t BOOST_IRR_MS = 180000UL;

constexpr uint32_t TELEGRAM_MIN_INTERVAL_MS = 1200UL;
constexpr uint32_t MOTOR_DEBOUNCE_INTERVAL = 2000UL;
constexpr uint32_t STATE_TIMEOUT_WINDOW_MS = 30000UL;
constexpr uint32_t TG_DEDUPE_MS = 5000UL;
constexpr uint32_t TG_LOCK_TIMEOUT_MS = 5000UL;

// =========================
// Report notturno
// =========================
constexpr uint8_t NIGHT_REPORT_HOUR = 3;
constexpr uint8_t NIGHT_REPORT_MIN_FROM = 0;
constexpr uint8_t NIGHT_REPORT_MIN_TO = 59;

// =========================
// Logging sensori / auto
// =========================
constexpr uint32_t BLOCK_LOG_COOLDOWN_MS = 20UL * 60UL * 1000UL;
constexpr uint32_t HUM_LOG_INTERVAL_MS = 20UL * 60UL * 1000UL;
constexpr uint8_t HUM_DELTA_PCT = 5;