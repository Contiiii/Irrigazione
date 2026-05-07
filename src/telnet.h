#pragma once
#include <Arduino.h>

// telnetOnNow() è definita inline in utils.h

// Gestione Telnet
void handleTelnet();
void handleTelnetCommand(const String &cmd);
void telnetSendTail(const char *path, int maxLines);
void telnetPrintWarnErrorFile(const char *path);
void telnetPrintAllWarnError(bool includeOld = true);