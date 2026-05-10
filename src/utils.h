
#pragma once
#include <Arduino.h>
#include <WiFi.h>

// Variabili esterne definite in main.cpp
extern uint32_t offTimeMot1;
extern uint32_t offTimeMot2;
extern WiFiClient telnetClient;

inline String motorLabel(uint8_t m) {
  if (m == 1) return "1";
  if (m == 2) return "2";
  if (m == 3) return "1+2";
  return "?";
}

inline bool motorsOnNow() {
  return (offTimeMot1 != 0) || (offTimeMot2 != 0);
}

inline bool telnetOnNow() {
  return telnetClient && telnetClient.connected();
}

// utils.h
inline const char* boolToEmoji(bool v) { return v ? "✅" : "❌"; }