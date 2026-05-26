#include "telnet.h"

// telnetOnNow() è definita inline in utils.h

// Funzioni Telnet - estratte da main.cpp (Fase 4A)

void telnetSendTail(const char *path, int maxLines)
{
  File f = SPIFFS.open(path, FILE_READ);
  if (!f)
  {
    telnetClient.println("No file.");
    return;
  }

  maxLines = min(maxLines, 100);

  String *lines = new String[maxLines];
  if (!lines)
  {
    telnetClient.println("Out of memory");
    f.close();
    return;
  }

  int idx = 0;
  while (f.available())
  {
    lines[idx % maxLines] = f.readStringUntil('\n');
    idx++;
  }
  f.close();

  int start = max(0, idx - maxLines);
  for (int i = start; i < idx; i++)
  {
    telnetClient.println(lines[i % maxLines]);
  }

  delete[] lines;
  telnetClient.println("-- EOF --");
}

void telnetWelcome()
{
  if (!telnetClient || !telnetClient.connected())
    return;

  telnetClient.println();
  telnetClient.println("=== ESP32 TELNET ===");
  telnetClient.println("IP: " + WiFi.localIP().toString());
  telnetClient.println("Uptime(ms): " + String(millis()));
  telnetClient.println("Comandi: tail [n], alert, clear, size");

  if (!spiffsOK)
  {
    telnetClient.println("SPIFFS non montato, niente log.");
    telnetClient.println("=== END ===");
    return;
  }

  telnetPrintAllWarnError(true);

  telnetClient.println("=== END ===");
}

void telnetPrintWarnErrorFile(const char *path)
{
  if (!(telnetClient && telnetClient.connected()))
    return;

  File f = SPIFFS.open(path, FILE_READ);
  if (!f)
  {
    telnetClient.println("No file.");
    return;
  }

  char line[512];
  while (f.available())
  {
    size_t n = f.readBytesUntil('\n', line, sizeof(line) - 1);
    line[n] = '\0';

    if (n && line[n - 1] == '\r')
      line[n - 1] = '\0';

    if (strstr(line, " | W | ") || strstr(line, " | E | "))
    {
      telnetClient.println(line);
    }
  }
  f.close();
}

void telnetPrintAllWarnError(bool includeOld)
{
  if (!(telnetClient && telnetClient.connected()))
    return;

  telnetClient.println("\n-- WARN/ERROR --");
  if (includeOld)
    telnetPrintWarnErrorFile(LOG_OLD_FILE);
  telnetPrintWarnErrorFile(LOG_FILE);
}

void handleTelnetCommand(const String &cmd)
{
  if (cmd.startsWith("tail"))
  {
    String arg = cmd.substring(4);
    arg.trim();
    arg.replace("[", "");
    arg.replace("]", "");
    int n = arg.toInt();
    if (n <= 0)
      n = 50;
    telnetSendTail(LOG_FILE, n);
  }
  // FIX 2 — health su canale telnet invece di Telegram
else if (cmd == "health")
{
  // Stampa health direttamente sul client Telnet
  telnetClient.println("\n-- HEALTH --");
  telnetClient.println("Mot1 bloccato: " + String(health.motore1BloccatoSicurezza));
  telnetClient.println("Mot2 bloccato: " + String(health.motore2BloccatoSicurezza));
  telnetClient.println("Mot1 troppo tempo: " + String(health.motore1AttivoTroppoTempo));
  telnetClient.println("Mot2 troppo tempo: " + String(health.motore2AttivoTroppoTempo));
  telnetClient.println("Irr oggi mot1: " + String(health.irrigazioniOggiMot1));
  telnetClient.println("Irr oggi mot2: " + String(health.irrigazioniOggiMot2));
  telnetClient.println("SPIFFS OK: " + String(spiffsOK));
  telnetClient.println("Meteo valido: " + String(meteo.datiValidi));
  telnetClient.println("Sta piovendo: " + String(meteo.staPiovendo));
  telnetClient.println("Blocco irr: " + String(bloccoIrrigazione));
  telnetClient.println("-- END HEALTH --");
}
  else if (cmd == "clear")
  {
    SPIFFS.remove(LOG_FILE);
    telnetClient.println("OK cleared.");
  }
  else if (cmd == "health")
  {
    handleHealth();
    return;
  }
  else if (cmd == "size")
  {
    File f = SPIFFS.open(LOG_FILE, FILE_READ);
    telnetClient.printf("log.txt = %u bytes\r\n", f ? (unsigned)f.size() : 0);
    if (f)
      f.close();
  }
  else if (cmd.startsWith("alert"))
  {
    bool includeOld = true;
    if (cmd.indexOf(" new") >= 0)
      includeOld = false;

    telnetPrintAllWarnError(includeOld);
  }
  else
  {
    telnetClient.println("Comandi: tail, alert, clear, size, tgreset, health");
  }
}

void handleTelnet()
{
  if (telnetServer.hasClient())
  {
    WiFiClient newClient = telnetServer.available();
    if (newClient)
    {
      boostPolling(BOOST_TELNET_MS);  // boost solo alla connessione
      if (telnetClient && telnetClient.connected())
        telnetClient.stop();
      telnetClient = newClient;
      telnetClient.println("Telnet OK. Comandi: tail, alert, clear, size, tgreset, health");
      telnetWelcome();
    }
  }

  // FIX 3 — un solo controllo
  if (!(telnetClient && telnetClient.connected()))
    return;

  // FIX 1 — boost rimosso da qui (era chiamato ogni loop)

  while (telnetClient.available())
  {
    uint8_t c = (uint8_t)telnetClient.read();

    if (c == 0xFF)
    {
      if (telnetClient.available()) telnetClient.read();
      if (telnetClient.available()) telnetClient.read();
      continue;
    }
    if (c == 0x00) continue;

    if (c == '\r' || c == '\n')
    {
      telnetLine.trim();
      if (telnetLine.length() > 0)
        handleTelnetCommand(telnetLine);
      telnetLine = "";
      continue;
    }

    if (c == 0x08 || c == 0x7F)
    {
      if (telnetLine.length() > 0)
        telnetLine.remove(telnetLine.length() - 1);
      continue;
    }

    telnetLine += (char)c;
  }
}