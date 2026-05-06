#include "sensori.h"
#include "log.h"
#include "motori.h"
#include "utils.h"

// Legge i sensori di umidità (molte letture per media)
void leggiSensori(int umidita[2])
{
  long sum1 = 0;
  long sum2 = 0;

  for (int i = 0; i < SENSOR_NUM_SAMPLES; i++)
  {
    sum1 += analogRead(Pin_Sensore1);
    sum2 += analogRead(Pin_Sensore2);
    delay(SENSOR_SAMPLE_DELAY_MS);
  }

  umidita[0] = sum1 / SENSOR_NUM_SAMPLES;
  umidita[1] = sum2 / SENSOR_NUM_SAMPLES;

  validazioneSensori(umidita[0], umidita[1]);
}

// Stub: validazioneSensori da spostare
void validazioneSensori(int raw1, int raw2) {}

// Stub: handleSensore da spostare
void handleSensore(bool toTelegram) {}