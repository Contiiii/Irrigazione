#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include "config.h"

// Istanze rete (extern: definite in telegram.cpp)
extern WiFiClientSecure client;
extern UniversalTelegramBot bot;

// Stato sessione bot
enum BotState { IDLE, ASK_TIME_MOT1, ASK_TIME_MOT2, ASK_TIME_BOTH };
extern BotState botstate;
extern bool motorOperationInProgress;

// Funzioni pubbliche
bool tgSend(const String &msg);
int  safeGetUpdates();
void handleMessage(String text, String chatId, String messageId);
void handleCallBack(String text, String chatId, String messageId);
void askTime(const String &who);

// Polling adattivo
void boostPolling(uint32_t ms);
bool isBoostedNow(uint32_t nowMs);
uint32_t currentPollDelayMs(int hourNow, uint32_t nowMs);
void wifiFollowPolling(uint32_t nowMs, uint32_t delayMs);
bool isNightHour(int h);

bool isStateTimeoutExpired();
void resetAskSession();
void armStateTimeout(uint32_t ms);