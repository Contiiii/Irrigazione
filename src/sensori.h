#pragma once
#include <Arduino.h>
#include "config.h"
#include "health.h"


String umiditaStatusEmoji(int um);

// Modulo lettura sensori umidità terreno.
// Interfaccia pubblica: leggiSensori(), validazioneSensori(), handleSensore().

// Legge i sensori di umidità e ritorna i valori raw ADC.
// Pre-condizione: array umidita[2] passato dal chiamante.
void leggiSensori(int umidita[2]);

// Valida i valori raw e aggiorna health flags.
// Esegue anche spegniMotori() se un sensore risulta disconnesso.
// Nota: usa extern spegniMotori() definita in main.cpp - da ripulire.
void validazioneSensori(int raw1, int raw2);

// Legge sensori, costruisce messaggio stato, gestisce irrigazione AUTO.
// Se toTelegram=true, invia il report via Telegram.
void handleSensore(bool toTelegram);