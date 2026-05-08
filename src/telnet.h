#pragma once
#include <Arduino.h>
#include "utils.h"
#include "log.h"
#include "config.h"
#include "health.h"
#include "telegram.h"

// telnetOnNow() è definita inline in utils.h
// telnetClient è in utils.h

// Variabili globali usate da telnet
extern WiFiServer telnetServer;
extern String telnetLine;
extern long lastHandledUpdateId;
extern long lastHandledUpdateIdRTC;
extern volatile bool tgBusy;

// Gestione Telnet
void handleTelnet();
void handleTelnetCommand(const String &cmd);
void telnetSendTail(const char *path, int maxLines);
void telnetPrintWarnErrorFile(const char *path);
void telnetPrintAllWarnError(bool includeOld = true);
void telnetWelcome();