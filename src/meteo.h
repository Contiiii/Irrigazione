#pragma once
#include <Arduino.h>

struct DatiMeteo
{
  bool staPiovendo;
  String condizioniMeteo;
  float temperatura;
  float pioggiaUltimaOra;
  int umidita;
  unsigned long ultimoAggiornamento;
  bool datiValidi;
  bool forecastValidi = false;
  unsigned long ultimoAggForecast = 0;
  bool pioggiaPrevista3h = false;
  bool pioggiaPrevista6h = false;
  float mmPrevisti3h = 0.0f;
  float mmPrevisti6h = 0.0f;
};

extern DatiMeteo meteo;

bool rilevoMeteo();
bool rilevoForecastPioggia();
void handleMeteo();
bool irrigazioneConsentita();