#pragma once
#include <Arduino.h>
#include <SPIFFS.h>
#include "config.h"

// Livelli di log
enum LogLevel { INFO, DEBUG_L, WARN, ERROR_L };

// Stato globale (extern: definiti in log.cpp)
extern bool spiffsOK;
extern bool debug;
extern bool timeReady;

// Inizializzazione
void initLogSize();

// Scrittura
void logLine(LogLevel lvl, const String &msg, bool newline = true, bool toTelegram = false);
void appendLogFile(const String &line);

// Lettura
String getTime();
String tailLog(int maxLines = 50);
String tailWarnError(int maxLines = 50, bool includeOld = false);

// Toggle debug
void handleDebug();