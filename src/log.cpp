#include "log.h"
#include "telegram.h" 
#include "utils.h"  // solo per tgSend() nei logLine con toTelegram=true

// ── Variabili globali di questo modulo ──────────────────────────────
bool   spiffsOK  = false;
size_t logBytes  = 0;
bool   debug     = false;
bool   timeReady = false;

// ── getTime ─────────────────────────────────────────────────────────
String getTime()
{
    if (!timeReady) return "BOOT";
    struct tm t;
    if (!getLocalTime(&t)) return "No time!";
    const char *giorni[] = {"DOM","LUN","MAR","MER","GIO","VEN","SAB"};
    char buf[32];
    snprintf(buf, sizeof(buf), "%s %02d/%02d %02d:%02d:%02d",
             giorni[t.tm_wday], t.tm_mday, t.tm_mon + 1,
             t.tm_hour, t.tm_min, t.tm_sec);
    return String(buf);
}

// ── appendLogFile ────────────────────────────────────────────────────
void appendLogFile(const String &line)
{
    if (!spiffsOK) return;

    static uint16_t writeCount = 0;
    if (writeCount++ % 100 == 0) {
        if (SPIFFS.totalBytes() - SPIFFS.usedBytes() < 10240) {
            SPIFFS.remove(LOG_OLD_FILE);
            SPIFFS.rename(LOG_FILE, LOG_OLD_FILE);
            logBytes = 0;
        }
    }

    if (logBytes + line.length() > MAX_LOG_SIZE) {
        SPIFFS.remove(LOG_OLD_FILE);
        SPIFFS.rename(LOG_FILE, LOG_OLD_FILE);
        logBytes = 0;
    }

    File f = SPIFFS.open(LOG_FILE, FILE_APPEND);
    if (!f) return;
    logBytes += f.print(line);
    f.close();
}

// ── logLine ──────────────────────────────────────────────────────────
void logLine(LogLevel lvl, const String &msg, bool newline, bool toTelegram)
{
    if (!debug && lvl == DEBUG_L) return;

    const char *L[] = {"I", "D", "W", "E"};
    static char line[512];

    int pos = snprintf(line, sizeof(line), "%s | %s | %s",
                       getTime().c_str(), L[lvl], msg.c_str());
    if (pos < 0 || pos >= (int)sizeof(line))
        pos = sizeof(line) - 1;

    // File
    appendLogFile(String(line) + "\r\n");

    // Serial
    Serial.print(line);
    if (newline) Serial.print("\r\n");

    // Telnet (dichiarato extern per evitare include circolare)
    extern WiFiClient telnetClient;
    if (telnetClient && telnetClient.connected()) {
        telnetClient.print(line);
        if (newline) telnetClient.print("\r\n");
    }

    // Telegram
    if (toTelegram) tgSend(String(line));
}

// ── initLogSize ──────────────────────────────────────────────────────
void initLogSize()
{
    File f = SPIFFS.open(LOG_FILE, FILE_READ);
    if (!f) { logBytes = 0; return; }
    size_t sz = f.size();
    f.close();

    if (sz > MAX_LOG_SIZE) {
        SPIFFS.remove(LOG_OLD_FILE);
        SPIFFS.rename(LOG_FILE, LOG_OLD_FILE);
        logBytes = 0;
    } else {
        logBytes = sz;
    }
}

// ── tailLog ──────────────────────────────────────────────────────────
String tailLog(int maxLines)
{
    File f = SPIFFS.open(LOG_FILE, FILE_READ);
    if (!f) return "Nessun log.";

    maxLines = constrain(maxLines, 1, 200);
    String *lines = new String[maxLines];
    if (!lines) return "Out of memory";

    int idx = 0;
    while (f.available() && idx < maxLines * 2)
        lines[idx++ % maxLines] = f.readStringUntil('\n');
    f.close();

    int start = max(0, idx - maxLines);
    String out;
    out.reserve(2528);
    for (int i = start; i < idx; i++) {
        out += lines[i % maxLines];
        out += "\n";
    }
    delete[] lines;
    return out;
}

// ── tailWarnError ────────────────────────────────────────────────────
String tailWarnError(int maxLines, bool includeOld)
{
    if (!spiffsOK) return "SPIFFS non disponibile";

    const size_t MAX_OUT = 3500;
    const int CAP = constrain(maxLines, 1, 80);

    String *ring = new String[CAP];
    if (!ring) return "Out of memory";
    for (int i = 0; i < CAP; i++) ring[i].reserve(128);

    int idx = 0;
    auto scanFile = [&](const char *path) {
        File f = SPIFFS.open(path, FILE_READ);
        if (!f) return;
        while (f.available()) {
            String s = f.readStringUntil('\n');
            s.trim();
            if (s.indexOf(" | W | ") >= 0 || s.indexOf(" | E | ") >= 0)
                ring[idx++ % CAP] = s;
        }
        f.close();
    };

    if (includeOld) scanFile(LOG_OLD_FILE);
    scanFile(LOG_FILE);

    if (idx == 0) { delete[] ring; return "Nessun WARNING/ERROR nel log."; }

    int start = max(0, idx - CAP);
    String out;
    out.reserve(MAX_OUT);
    out = "Ultimi " + String(min(idx, CAP)) + " WARNING/ERROR:\n\n";
    for (int i = start; i < idx; i++) {
        if (out.length() + ring[i % CAP].length() + 1 > MAX_OUT) break;
        out += ring[i % CAP];
        out += "\n";
    }
    delete[] ring;
    return out;
}

// ── handleDebug ──────────────────────────────────────────────────────
void handleDebug()
{
    debug = !debug;
    logLine(INFO, debug ? "🔧 DEBUG ATTIVO" : "🔧 DEBUG DISATTIVO", true, true);
}