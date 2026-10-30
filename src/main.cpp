// =====================================================================
//  SML Meter Gateway – ESP32-C3 · IEC 62056-21 / SML · OLED · MQTT
// =====================================================================

#include <Arduino.h>
#include <algorithm>
#include <WiFi.h>
#include <MQTT.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// Erscheint in HA als Firmware-Version des Geraets (Discovery "sw").
#ifndef FW_VERSION
#define FW_VERSION "1.2.3"
#endif
#define HA_MODEL "SML Meter Gateway (ESP32-C3)"

// ── Pins (ESP32-C3 optimiert) ───────────────────────────────────
#ifndef CFG_CH1_RX
#define CFG_CH1_RX  4
#endif
#ifndef CFG_CH1_TX
#define CFG_CH1_TX  7
#endif
#ifndef CFG_I2C_SDA
#define CFG_I2C_SDA 6
#endif
#ifndef CFG_I2C_SCL
#define CFG_I2C_SCL 5
#endif
#ifndef CFG_TOUCH_PIN
#define CFG_TOUCH_PIN 3
#endif

// ── Konfiguration (NVS) ─────────────────────────────────────────
struct AppConfig {
  char deviceName[33] = "";   // leer = Geraete-ID aus MAC, sonst "sgm_<name>"
  char ssid[33]      = "changeme";
  char pass[65]      = "";
  char mqttHost[33]  = "192.168.1.10";
  int  mqttPort      = 1883;
  char mqttUser[33]  = "";
  char mqttPass[33]  = "";
  char baseTopic[64] = "smarthome/sgm";
  bool haDiscovery   = true;
  bool jsonTopic     = true;
  bool oledEnabled   = true;
  bool extraSensors  = false;  // Spannung/Strom/Frequenz melden (falls Zaehler das sendet)
  int  uartBaud      = 9600;
  int  pollMs        = 5000;
  bool pushMode      = true;    // Default: SML-Push (DSS), kein Handshake noetig
  int  touchTimeoutS = 10;   // Sekunden bis Display nach letztem Touch wieder ausgeht
};

static AppConfig cfg;
static Preferences prefs;
static constexpr const char* NS = "sgmcfg";

// Frames brauchen intern bis zu 4s (Push-Modus-Timeout) — kürzere Poll-
// Intervalle hätten ohnehin keine Wirkung.
static constexpr int MIN_POLL_MS = 4000;

static void configSave() {
  prefs.begin(NS, false);
  prefs.putString("dn",   cfg.deviceName);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("pass", cfg.pass);
  prefs.putString("mh",   cfg.mqttHost);
  prefs.putInt  ("mp",   cfg.mqttPort);
  prefs.putString("mu",   cfg.mqttUser);
  prefs.putString("mpw",  cfg.mqttPass);
  prefs.putString("bt",   cfg.baseTopic);
  prefs.putInt  ("ha",   cfg.haDiscovery ? 1 : 0);
  prefs.putInt  ("js",   cfg.jsonTopic   ? 1 : 0);
  prefs.putInt  ("ol",   cfg.oledEnabled ? 1 : 0);
  prefs.putInt  ("vcf",  cfg.extraSensors ? 1 : 0);
  prefs.putInt  ("ub",   cfg.uartBaud);
  prefs.putInt  ("pm",   cfg.pollMs);
  prefs.putInt  ("pm2",  cfg.pushMode    ? 1 : 0);
  prefs.putInt  ("tts",  cfg.touchTimeoutS);
  prefs.end();
  Serial.println("[CFG] saved");
}

// Einmal-Flag: per MQTT-Befehl "cmd/config" gesetzt, erzwingt beim naechsten
// Boot das AP-Config-Portal (unabhaengig vom WLAN-Status) und wird danach
// sofort wieder geloescht.
static bool gForceAp = false;

static void requestConfigPortal() {
  prefs.begin(NS, false);
  prefs.putInt("fap", 1);
  prefs.end();
  Serial.println("[CFG] Config-Portal angefordert, starte neu...");
  delay(200);
  ESP.restart();
}

static void configLoad() {
  prefs.begin(NS, true);
  if (!prefs.isKey("ssid")) {
    prefs.end();
    configSave();
    prefs.begin(NS, true);
  }

  gForceAp = prefs.getInt("fap", 0) != 0;
  prefs.getString("dn",   cfg.deviceName, sizeof(cfg.deviceName));
  prefs.getString("ssid", cfg.ssid, sizeof(cfg.ssid));
  prefs.getString("pass", cfg.pass, sizeof(cfg.pass));
  prefs.getString("mh",   cfg.mqttHost, sizeof(cfg.mqttHost));
  cfg.mqttPort = prefs.getInt("mp", 1883);
  prefs.getString("mu",   cfg.mqttUser, sizeof(cfg.mqttUser));
  prefs.getString("mpw",  cfg.mqttPass, sizeof(cfg.mqttPass));
  prefs.getString("bt",   cfg.baseTopic, sizeof(cfg.baseTopic));
  cfg.haDiscovery = prefs.getInt("ha", 1) != 0;
  cfg.jsonTopic   = prefs.getInt("js", 1) != 0;
  cfg.oledEnabled = prefs.getInt("ol", 1) != 0;
  cfg.extraSensors = prefs.getInt("vcf", 0) != 0;
  cfg.uartBaud    = prefs.getInt("ub", 9600);
  cfg.pollMs      = prefs.getInt("pm", 5000);
  cfg.pushMode    = prefs.getInt("pm2", 1) != 0;
  cfg.touchTimeoutS = prefs.getInt("tts", 10);
  prefs.end();

  if (gForceAp) {
    prefs.begin(NS, false);
    prefs.putInt("fap", 0);
    prefs.end();
  }

  if (cfg.ssid[0] == 0)       strcpy(cfg.ssid, "changeme");
  if (cfg.mqttHost[0] == 0)   strcpy(cfg.mqttHost, "192.168.1.10");
  if (cfg.baseTopic[0] == 0)  strcpy(cfg.baseTopic, "smarthome/sgm");
  if (cfg.pollMs < MIN_POLL_MS) cfg.pollMs = MIN_POLL_MS;
  if (cfg.touchTimeoutS < 1) cfg.touchTimeoutS = 10;
}

// ── OLED: SSD1315 (vom User am Chip-Aufdruck identifiziert) ──────
// Konstruktor-Signatur ist (rotation, reset, clock, data) — reset bleibt
// U8X8_PIN_NONE, sonst faellt u8g2 intern auf Wire.begin() ohne Argumente
// (= ESP32-Default-I2C-Pins statt CFG_I2C_SDA/SCL) zurueck.
static U8G2_SSD1315_128X64_NONAME_F_HW_I2C u8g2(
  U8G2_R0, U8X8_PIN_NONE, CFG_I2C_SCL, CFG_I2C_SDA);

static constexpr uint8_t OLED_I2C_ADDR = 0x3C;
static bool oledActive = false;

// WICHTIG: kein eigener Wire.begin() vor u8g2.begin()! u8g2 ruft intern
// selbst Wire.begin(CFG_I2C_SDA, CFG_I2C_SCL) auf (via u8x8_byte_arduino_hw_i2c,
// dank korrekter Pin-Reihenfolge im Konstruktor oben) — ein zusaetzlicher
// eigener Wire.begin()-Aufruf VOR u8g2.begin() lässt den I2C-Treiber auf
// diesem ESP32-C3-Board zuverlaessig haengen (reproduziert, kein Timeout).
// Die Praesenz-Pruefung passiert deshalb NACH u8g2.begin(), nicht davor;
// u8g2.begin() liefert unabhaengig davon immer true, daher der eigene Check.
static bool dispInit() {
  u8g2.begin();
  Wire.beginTransmission(OLED_I2C_ADDR);
  if (Wire.endTransmission() != 0) {
    Serial.printf("[OLED] Kein Display an I2C 0x%02X gefunden - Anzeige deaktiviert\n",
                  OLED_I2C_ADDR);
    return false;
  }
  u8g2.setContrast(255);
  return true;
}

// true, solange ein Status-Bildschirm (Boot/MQTT/Keine Daten) statt einer
// Werteseite zu sehen ist — dann darf der naechste Poll einmalig auf die
// Werteseite wechseln. Werteseiten selbst werden nur per Touch neu gezeichnet.
static bool dispShowsStatus = false;

static void dispStatus(const char* l1, const char* l2, const char* l3) {
  dispShowsStatus = true;
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13_tr);
  u8g2.drawStr(0, 14, l1);
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(4, 32, l2);
  u8g2.drawStr(4, 46, l3);
  u8g2.drawHLine(0, 22, u8g2.getWidth());
  u8g2.drawHLine(0, 38, u8g2.getWidth());
  u8g2.sendBuffer();
}

// AP-Config-Portal-Anzeige: keine Ueberschrift, stattdessen die volle
// Hoehe fuer SSID/PW/IP nutzen — die stehen sonst nirgends am Geraet.
static void dispApInfo(const char* ssid, const char* pw, const char* ip) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tr);
  char buf[40];
  snprintf(buf, sizeof(buf), "SSID: %s", ssid);
  u8g2.drawStr(0, 16, buf);
  snprintf(buf, sizeof(buf), "PW:   %s", pw);
  u8g2.drawStr(0, 36, buf);
  snprintf(buf, sizeof(buf), "IP:   %s", ip);
  u8g2.drawStr(0, 56, buf);
  u8g2.sendBuffer();
}

// ── Touch-Steuerung (TTP223 o.ae., aktiv-HIGH) ────────────────────
// Display ist im Normalbetrieb per Power-Save aus; ein Touch weckt es und
// zeigt Werte gross einzeln an (durchschalten per weiterem Touch), nach
// touchTimeoutS ohne Eingabe geht es wieder aus. Direkt nach dem Booten
// gilt grosszuegig BOOT_DISPLAY_HOLD_MS, damit Status-Meldungen lesbar
// bleiben, ohne dass man dafuer touchen muss.
static constexpr uint32_t BOOT_DISPLAY_HOLD_MS = 60000;
static bool     oledOn         = false;
static bool     oledBootHold   = true;
static uint32_t oledOnSince    = 0;
static int      dispPage       = 0;
static bool     lastTouchRaw   = false;
static uint32_t lastTouchEdgeMs = 0;

// Vorwaerts-deklariert: wird auch aus smlReadFrame() heraus aufgerufen,
// damit ein Touch waehrend des bis zu 4s blockierenden SML-Reads nicht
// verloren geht (siehe pollTouch()-Definition weiter unten).
static void pollTouch();

// Vorwaerts-deklariert: fuettern das Live-Terminal (/terminal-Seite) mit
// Rohbytes bzw. decodierten Werten, werden aber schon in smlListenPush()/
// smlPollD0() aufgerufen, lange bevor der WebSocket weiter unten definiert ist.
static void wsSendRaw(const uint8_t* buf, size_t len);
static void wsSendDecoded();

// ── SML / IEC 62056-21 Parser ───────────────────────────────────
struct ParsedValue {
  char  obis[24];
  float value;
  char  unit[16];
  bool  valid;
};

static HardwareSerial meterSerial(1);
static ParsedValue gvals[24];
static int         gvalCount = 0;
static char        gIdent[64] = {};

// Liest ab buf[*pos] ein SML-TLV-Element (Octet String/Integer/Unsigned/Liste,
// binaere BER-aehnliche Kodierung: oberes Halbbyte des ersten Bytes = Typ,
// unteres Halbbyte = Laenge inkl. TL-Byte(s) bzw. bei Listen = Elementanzahl;
// Fortsetzungsbit 0x80 kettet weitere Laengen-Halbbytes an). Bewegt *pos
// hinter das komplette Element (bei Listen rekursiv ueber alle Kindelemente).
// Numerische Werte (Integer/Unsigned, <=8 Byte) werden vorzeichenrichtig in
// *outVal geschrieben, falls outVal != nullptr. Gibt bei Parse-/Bounds-Fehlern
// false zurueck.
static bool smlSkipOrRead(const uint8_t* buf, size_t len, size_t* pos, int64_t* outVal) {
  if (*pos >= len) return false;
  uint8_t tl = buf[*pos];
  uint8_t type = (tl >> 4) & 0x07;
  size_t nbits = tl & 0x0F;
  size_t tlBytes = 1;
  while (buf[*pos + tlBytes - 1] & 0x80) {
    if (tlBytes > 3 || *pos + tlBytes >= len) return false;
    nbits = (nbits << 4) | (buf[*pos + tlBytes] & 0x0F);
    tlBytes++;
  }

  if (type == 0x07) {
    // Liste: nbits ist hier die Elementanzahl, keine Byte-Laenge.
    size_t p = *pos + tlBytes;
    for (size_t i = 0; i < nbits; i++) {
      if (!smlSkipOrRead(buf, len, &p, nullptr)) return false;
    }
    *pos = p;
    return true;
  }

  if (nbits < tlBytes) return false;
  size_t dataLen = nbits - tlBytes;
  size_t dataStart = *pos + tlBytes;
  if (dataStart + dataLen > len) return false;

  if (outVal && (type == 0x05 || type == 0x06) && dataLen > 0 && dataLen <= 8) {
    int64_t v = 0;
    bool neg = (type == 0x05) && (buf[dataStart] & 0x80);
    for (size_t i = 0; i < dataLen; i++) v = (v << 8) | buf[dataStart + i];
    if (neg) v -= ((int64_t)1 << (dataLen * 8));
    *outVal = v;
  }

  *pos = dataStart + dataLen;
  return true;
}

// Durchsucht einen kompletten SML-Datensatz nach SML_ListEntry-Eintraegen
// (Muster: 0x77 = Liste mit 7 Elementen, gefolgt von 0x07 = 6-Byte-OBIS-Code).
// Pragmatischer Scan statt vollstaendigem Nachrichtenbaum-Abstieg — die
// SML_ListEntry-Struktur (objName, status, valTime, unit, scaler, value,
// valueSignature) ist an dieser Bytefolge eindeutig erkennbar.
//
// Die alten Werte werden erst hier verworfen, nicht schon zu Beginn eines
// Reads: waehrend des blockierenden smlReadFrame() laeuft pollTouch(), und
// ein Touch in diesem Fenster zeigte sonst "---" statt des letzten Werts.
static void parseSmlBinaryFrame(const uint8_t* buf, size_t len) {
  gvalCount = 0;
  for (size_t i = 0; i + 8 <= len; i++) {
    if (!(buf[i] == 0x77 && buf[i+1] == 0x07)) continue;

    unsigned a=buf[i+2], b=buf[i+3], c=buf[i+4], d=buf[i+5], e=buf[i+6];
    // Sanity-Check: echte OBIS-Codes fuer Stromzaehler haben medium (a) 0
    // (abstrakt) oder 1 (Elektrizitaet). Andere Bytefolgen (z.B. 0xFF FF...
    // aus Nachrichten-Header-Feldern wie serverId) sind zufaellige Treffer
    // auf "0x77 0x07" und keine echten ListEntries — ohne diesen Filter
    // verspringt sich der Scan an solchen Fehltreffern.
    if (a > 1) continue;
    size_t pos = i + 8;

    if (!smlSkipOrRead(buf, len, &pos, nullptr)) continue;      // status
    if (!smlSkipOrRead(buf, len, &pos, nullptr)) continue;      // valTime
    int64_t unitVal = 0;
    if (!smlSkipOrRead(buf, len, &pos, &unitVal)) continue;     // unit
    int64_t scalerVal = 0;
    if (!smlSkipOrRead(buf, len, &pos, &scalerVal)) continue;   // scaler
    int64_t rawVal = 0;
    if (!smlSkipOrRead(buf, len, &pos, &rawVal)) continue;      // value

    if (gvalCount < 24) {
      ParsedValue& v = gvals[gvalCount];
      snprintf(v.obis, sizeof(v.obis), "%u-%u:%u.%u.%u", a, b, c, d, e);
      double value = (double)rawVal * pow(10.0, (double)(int8_t)scalerVal);
      if (unitVal == 30) value /= 1000.0;  // Wh -> kWh (Energie-Register)
      v.value = (float)value;
      v.unit[0] = '\0';
      v.valid = true;
      gvalCount++;
    }
    i = pos - 1;  // hinter dieses Element springen (Schleife zaehlt selbst i++)
  }
}

static float getObis(const char* prefix, float def = NAN) {
  size_t pl = strlen(prefix);
  for (int i = 0; i < gvalCount; i++)
    if (strncmp(gvals[i].obis, prefix, pl) == 0 && gvals[i].valid)
      return gvals[i].value;
  return def;
}

static void serialClearRx(HardwareSerial& s) {
  while (s.available()) s.read();
}

// Liest einen kompletten binaeren SML-Datensatz: Start-Escape
// 1B1B1B1B01010101, danach beliebige Nutzdaten, Ende-Escape 1B1B1B1B1A
// gefolgt von 1 Fuellbyte-Zaehler + 2 CRC16-Byte. Bytes vor der Start-
// Sequenz werden verworfen (Rauschen/Reste einer vorherigen Uebertragung).
static bool smlReadFrame(uint8_t* buf, size_t cap, unsigned long timeoutMs, size_t* outLen = nullptr) {
  serialClearRx(meterSerial);
  static const uint8_t STARTSEQ[8] = {0x1B,0x1B,0x1B,0x1B,0x01,0x01,0x01,0x01};
  uint32_t t0 = millis();
  size_t idx = 0;
  bool haveStart = false;
  while (millis() - t0 < timeoutMs && idx < cap) {
    while (meterSerial.available() && idx < cap) {
      buf[idx++] = (uint8_t)meterSerial.read();

      if (!haveStart && idx >= 8 && memcmp(buf + idx - 8, STARTSEQ, 8) == 0) {
        haveStart = true;
        memcpy(buf, STARTSEQ, 8);
        idx = 8;
      }
      if (haveStart && idx >= 8) {
        size_t m = idx - 8;
        if (buf[m]==0x1B && buf[m+1]==0x1B && buf[m+2]==0x1B && buf[m+3]==0x1B && buf[m+4]==0x1A) {
          if (outLen) *outLen = idx;
          return true;
        }
      }
    }
    if (oledActive) pollTouch();
    delay(1);
  }
  if (outLen) *outLen = idx;
  return false;
}

static bool smlPollD0(unsigned long timeoutMs) {
  meterSerial.begin(300, SERIAL_7E1, CFG_CH1_RX, CFG_CH1_TX);
  delay(50);
  meterSerial.print("/?!\r\n");
  delay(300);

  char idLine[64] = {0};
  size_t idLen = 0;
  uint32_t t0 = millis();
  while (millis() - t0 < 1500 && idLen < sizeof(idLine)-1) {
    while (meterSerial.available() && idLen < sizeof(idLine)-1) {
      char c = meterSerial.read();
      if (c == '\n') break;
      if (c != '\r') idLine[idLen++] = c;
    }
    if (idLen > 0) break;
    if (oledActive) pollTouch();
    delay(10);
  }
  idLine[idLen] = '\0';

  if (idLen < 3 || idLine[0] != '/') {
    Serial.printf("[SML] Kein/ungueltiges Ident (idLen=%u, raw=\"%s\")\n",
                  (unsigned)idLen, idLine);
    meterSerial.begin(cfg.uartBaud, SERIAL_8N1, CFG_CH1_RX, CFG_CH1_TX);
    return false;
  }
  strncpy(gIdent, idLine, sizeof(gIdent)-1);
  gIdent[sizeof(gIdent)-1] = '\0';
  Serial.printf("[SML] Ident: %s\n", gIdent);

  // Handshake (300 Baud) ist klassisch 7E1, das eigentliche SML-Datenpaket
  // danach aber 8N1 (bestaetigt per Tasmota SerialConfig an diesem Zaehler).
  meterSerial.begin(cfg.uartBaud, SERIAL_8N1, CFG_CH1_RX, CFG_CH1_TX);
  delay(100);
  meterSerial.print("?0!A\r\n");

  static uint8_t frame[1024];
  size_t flen = 0;
  if (!smlReadFrame(frame, sizeof(frame), timeoutMs, &flen)) {
    Serial.println("[SML] Frame-Timeout nach Ident (kein vollstaendiger Datensatz)");
    return false;
  }

  wsSendRaw(frame, flen);
  parseSmlBinaryFrame(frame, flen);
  wsSendDecoded();
  Serial.printf("[SML] Frame geparst, %d Werte gefunden\n", gvalCount);
  return gvalCount > 0;
}

static bool smlListenPush(unsigned long timeoutMs) {
  // Bestaetigt per Tasmota SerialConfig an diesem Zaehler: 8N1, kein Handshake noetig.
  meterSerial.begin(cfg.uartBaud, SERIAL_8N1, CFG_CH1_RX, CFG_CH1_TX);
  static uint8_t frame[1024];
  size_t flen = 0;
  if (!smlReadFrame(frame, sizeof(frame), timeoutMs, &flen)) {
    // Byte-Anzahl + Anfang der Rohdaten mitloggen: 0 Bytes deutet auf
    // Verdrahtung/Lesekopf, Muell-Bytes auf falsche Baudrate/Invertierung.
    Serial.printf("[SML] Push-Modus: kein Frame empfangen (Timeout, %u Bytes)",
                  (unsigned)flen);
    for (size_t i = 0; i < flen && i < 32; i++) Serial.printf(" %02X", frame[i]);
    Serial.println();
    return false;
  }
  wsSendRaw(frame, flen);
  parseSmlBinaryFrame(frame, flen);
  wsSendDecoded();
  Serial.printf("[SML] Frame geparst, %d Werte gefunden\n", gvalCount);
  return gvalCount > 0;
}

// ── MQTT ────────────────────────────────────────────────────────
static WiFiClient net;
static MQTTClient mqtt;
static char topicAvail[96], topicJson[96], topicCmd[96];
static bool haDiscDone = false;

// Pro-Gerät eindeutige ID, damit mehrere Gateways nicht dieselben
// Home-Assistant-Entities/MQTT-Client-IDs belegen. Ohne konfigurierten
// cfg.deviceName wird sie aus der Efuse-MAC gebildet, sonst aus dem
// (auf MQTT/HA-taugliche Zeichen bereinigten) Klarnamen.
static char gDeviceId[40] = {};

static void initDeviceId() {
  if (cfg.deviceName[0]) {
    char clean[sizeof(cfg.deviceName)];
    size_t j = 0;
    for (const char* p = cfg.deviceName; *p && j < sizeof(clean) - 1; p++) {
      char c = *p;
      if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
      bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
      clean[j++] = ok ? c : '_';
    }
    clean[j] = '\0';
    snprintf(gDeviceId, sizeof(gDeviceId), "sgm_%s", clean);
  } else {
    uint64_t mac = ESP.getEfuseMac();
    snprintf(gDeviceId, sizeof(gDeviceId), "sgm_c3_%04X%08X",
             (unsigned)(mac >> 32), (unsigned)mac);
  }
}

static void buildTopics() {
  snprintf(topicAvail, sizeof(topicAvail), "%s/status", cfg.baseTopic);
  snprintf(topicJson,  sizeof(topicJson),  "%s/json",   cfg.baseTopic);
  snprintf(topicCmd,   sizeof(topicCmd),   "%s/cmd/#",  cfg.baseTopic);
}

static unsigned long lastPoll = 0;

static void mqttCmd(MQTTClient* client, char* topic, char* payload, int len) {
  // Der Befehl wird durch das Sub-Topic bestimmt (.../cmd/poll,
  // .../cmd/reboot, .../cmd/config) — der Payload-Inhalt ist egal.
  String t(topic);
  int slash = t.lastIndexOf('/');
  String cmd = (slash >= 0) ? t.substring(slash + 1) : t;
  cmd.toLowerCase();

  if (cmd == "poll")          lastPoll = 0;
  else if (cmd == "reboot")   ESP.restart();
  else if (cmd == "config")   requestConfigPortal();
}

// Bleibt MQTT trotz funktionierendem WLAN dauerhaft unerreichbar (z.B.
// vertippte Broker-Zugangsdaten), gibt es sonst keinen Weg zurück ins
// Config-Portal ausser per USB-Reflash — cmd/config setzt ja MQTT voraus.
// Deshalb hier automatischer Fallback nach MQTT_FAIL_AP_MS.
static uint32_t mqttFailSince = 0;
static constexpr uint32_t MQTT_FAIL_AP_MS = 5UL * 60UL * 1000UL;
// Ohne Client im AP-Portal wird nach dieser Zeit neu gestartet (siehe loop()).
static constexpr uint32_t AP_IDLE_REBOOT_MS = 3UL * 60UL * 1000UL;

static void mqttReconnect() {
  if (mqtt.connected()) { mqttFailSince = 0; return; }
  static uint32_t lastTry = 0;
  if (millis() - lastTry < 10000) return;
  lastTry = millis();

  Serial.printf("[MQTT] Verbinde zu %s:%d ...\n", cfg.mqttHost, cfg.mqttPort);

  mqtt.setHost(cfg.mqttHost, cfg.mqttPort);
  String cid = String(gDeviceId);

  if (strlen(cfg.mqttUser) > 0)
    mqtt.connect(cid.c_str(), cfg.mqttUser, cfg.mqttPass, false);
  else
    mqtt.connect(cid.c_str(), false);

  if (mqtt.connected()) {
    mqttFailSince = 0;
    Serial.printf("[MQTT] verbunden als '%s'\n", cid.c_str());
    mqtt.publish(topicAvail, "online", true, 0);
    mqtt.subscribe(topicCmd);
    if (!haDiscDone && cfg.haDiscovery) {
      char topic[192], payload[900];
      // Geraete-Anzeigename fuer HA: eigener Klarname, falls gesetzt, sonst
      // Default. Bestimmt in HA sowohl die Geraete-Anzeige als auch den
      // automatisch generierten Entity-Id-Slug (z.B. sensor.sgm_pv_voll_power).
      char devDisplayName[48];
      if (cfg.deviceName[0]) snprintf(devDisplayName, sizeof(devDisplayName), "SGM %s", cfg.deviceName);
      else                   strncpy(devDisplayName, "SGM Gateway C3", sizeof(devDisplayName));
      // "cu" = configuration_url: HA zeigt auf der Geraeteseite einen Link
      // zum Web-Portal. Wird bei jedem Boot neu gesendet, folgt also DHCP-Wechseln.
      char cfgUrl[32];
      snprintf(cfgUrl, sizeof(cfgUrl), "http://%s/", WiFi.localIP().toString().c_str());
      struct S { const char* id; const char* name; const char* unit; const char* dc; const char* sc; bool extra; };
      S arr[] = {
        {"power_w",   "Power",       "W",   "power",   "",                  false},
        {"import_kwh","Import",      "kWh", "energy",  "total_increasing",  false},
        {"export_kwh","Export",      "kWh", "energy",  "total_increasing",  false},
        {"voltage_v", "Spannung",    "V",   "voltage", "",                  true},
        {"current_a", "Strom",       "A",   "current", "",                  true},
        {"frequency_hz","Frequenz",  "Hz",  "frequency","",                 true},
      };
      for (auto& s : arr) {
        if (s.extra && !cfg.extraSensors) continue;
        snprintf(topic, sizeof(topic),
                 "homeassistant/sensor/%s/%s/config", gDeviceId, s.id);
        char statCla[40] = {};
        if (s.sc[0]) snprintf(statCla, sizeof(statCla), "\"stat_cla\":\"%s\",", s.sc);
        snprintf(payload, sizeof(payload),
          "{\"name\":\"%s\",\"obj_id\":\"%s_%s\","
          "\"stat_t\":\"%s/%s\",\"unit_of_meas\":\"%s\","
          "\"dev_cla\":\"%s\",%s\"uniq_id\":\"%s_%s\","
          "\"avail_t\":\"%s\","
          "\"dev\":{\"ids\":[\"%s\"],\"name\":\"%s\",\"mf\":\"DocBigs-Lab\","
          "\"mdl\":\"" HA_MODEL "\",\"sw\":\"" FW_VERSION "\",\"cu\":\"%s\"}}",
          s.name, gDeviceId, s.id, cfg.baseTopic, s.id,
          s.unit, s.dc, statCla, gDeviceId, s.id, topicAvail, gDeviceId, devDisplayName,
          cfgUrl);
        mqtt.publish(topic, payload, true, 0);
      }
      haDiscDone = true;
    }
  } else {
    Serial.printf("[MQTT] Verbindung fehlgeschlagen (error=%d, returnCode=%d)\n",
                   (int)mqtt.lastError(), (int)mqtt.returnCode());
    if (mqttFailSince == 0) {
      mqttFailSince = millis();
    } else if (millis() - mqttFailSince > MQTT_FAIL_AP_MS) {
      Serial.println("[MQTT] Seit 5 Min. nicht erreichbar - erzwinge Config-Portal");
      requestConfigPortal();
    }
  }
}

static void publishValues(float p, float imp, float exp, float v, float c, float freq) {
  if (!mqtt.connected()) return;
  char buf[32];
  struct P { const char* suffix; float val; int prec; };
  P pubs[] = {
    {"power_w",      p,    1},
    {"import_kwh",   imp,  3},
    {"export_kwh",   exp,  3},
    {"voltage_v",    v,    1},
    {"current_a",    c,    2},
    {"frequency_hz", freq, 2},
  };
  for (auto& x : pubs) {
    if (isnan(x.val)) continue;
    snprintf(buf, sizeof(buf), "%.*f", x.prec, x.val);
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/%s", cfg.baseTopic, x.suffix);
    mqtt.publish(topic, buf, true, 0);
  }
  if (cfg.jsonTopic) {
    StaticJsonDocument<512> doc;
    doc["power_w"]      = p;
    doc["import_kwh"]   = imp;
    doc["export_kwh"]   = exp;
    doc["voltage_v"]    = v;
    doc["current_a"]    = c;
    doc["frequency_hz"] = freq;
    doc["device"]       = "sgm-gateway-c3";
    doc["uptime_s"]     = millis() / 1000;
    char jb[512];
    serializeJson(doc, jb);
    mqtt.publish(topicJson, jb, false, 0);
  }
}

// ── Web-Config-Portal (Captive AP) ──────────────────────────────
static AsyncWebServer webServer(80);
static AsyncWebSocket ws("/ws");
static DNSServer dnsServer;

// Sendet die rohen SML-Bytes als Hex-String an alle verbundenen
// Live-Terminal-Clients (siehe /terminal-Seite). Kein Aufwand, wenn
// niemand zuschaut.
static void wsSendRaw(const uint8_t* buf, size_t len) {
  if (ws.count() == 0) return;
  String s; s.reserve(len * 3 + 8);
  s += "RAW ";
  char b[4];
  for (size_t i = 0; i < len; i++) { snprintf(b, sizeof(b), "%02X ", buf[i]); s += b; }
  ws.textAll(s);
}

// Sendet die decodierten OBIS/Wert-Paare an alle Live-Terminal-Clients.
static void wsSendDecoded() {
  if (ws.count() == 0) return;
  String s; s.reserve(gvalCount * 40 + 8);
  s += "DEC ";
  char line[48];
  for (int i = 0; i < gvalCount; i++) {
    snprintf(line, sizeof(line), "%s=%.3f; ", gvals[i].obis, gvals[i].value);
    s += line;
  }
  ws.textAll(s);
}

static const char HTML[] = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SGM Gateway C3</title>
<style>
body{font-family:system-ui;background:#1e1e2e;color:#cdd6f4;margin:0;padding:1em}
.card{max-width:500px;margin:auto;background:#313244;border-radius:12px;padding:1.2em}
h1{color:#89b4fa;font-size:1.3em;margin:0 0 .3em}
h2{color:#a6e3a1;font-size:1em;margin:1em 0 .4em;border-bottom:1px solid #45475a;padding-bottom:.3em}
label{display:block;font-size:.85em;color:#a6adc8;margin:.5em 0 .15em}
input,select{width:100%;padding:.45em;background:#45475a;border:1px solid #585b70;border-radius:6px;color:#cdd6f4;box-sizing:border-box}
.row{display:flex;gap:.5em}.row>*{flex:1}
.chk{display:flex;align-items:center;gap:.4em;margin:.4em 0}.chk input{width:auto}
button{margin-top:1.2em;width:100%;padding:.65em;background:#89b4fa;color:#1e1e2e;border:none;border-radius:6px;font-size:1.05em;font-weight:700;cursor:pointer}
.row>button.scan{flex:0 0 auto;width:auto;margin:0;padding:.45em .8em;font-size:.9em}
#nets{font-size:.85em;color:#a6adc8}
.hint{font-size:.75em;color:#7f849c;margin-top:.3em}
.net{display:flex;justify-content:space-between;padding:.4em .5em;margin-top:.25em;background:#45475a;border-radius:6px;cursor:pointer}
.net:hover{background:#585b70}.net span{color:#cdd6f4}
.q.g{color:#a6e3a1}.q.m{color:#f9e2af}.q.b{color:#f38ba8}
</style></head><body><div class="card">
<h1>&#9889; SML Gateway (ESP32-C3)</h1>
<a href="/terminal" style="color:#89b4fa;font-size:.85em">&#9654; Live-Terminal (Rohdaten &amp; decodierte Werte)</a>
<h2>Gerät</h2>
<label>Geräte-Name (optional, sonst z.B. sgm_c3_...MAC)</label>
<input id="devname" placeholder="z.B. pv-voll" value="__DEVNAME__">
<h2>WLAN</h2>
<label>SSID</label>
<div class="row"><input id="ssid" value="__SSID__"><button type="button" class="scan" id="scanbtn" onclick="scan()">Suchen</button></div>
<div id="nets"></div>
<div class="hint">Verstecktes WLAN: SSID von Hand eintragen, es taucht beim Suchen nicht auf.</div>
<label>Passwort</label><input id="wifipass" type="password" placeholder="unverändert lassen" value="">
<h2>MQTT</h2>
<div class="row"><div><label>Host</label><input id="mqtthost" value="__MQTH__"></div>
<div><label>Port</label><input id="mqttport" type="number" value="__MQTP__"></div></div>
<div class="row"><div><label>User</label><input id="mqttuser" value="__MQU__"></div>
<div><label>Passwort</label><input id="mqttpass" type="password" placeholder="unverändert lassen" value=""></div></div>
<label>Base Topic</label><input id="mqttbase" value="__TOPIC__">
<h2>Zähler</h2>
<div class="row"><div><label>Baud</label>
<select id="baud">
<option value="300">300</option><option value="1200">1200</option>
<option value="4800">4800</option><option value="9600">9600</option>
<option value="19200">19200</option></select></div>
<div><label>Poll-Intervall (ms, min. 4000)</label><input id="pollms" type="number" min="4000" value="__POLL__"></div></div>
<label>Modus</label>
<select id="mode"><option value="0">Polling (D0)</option>
<option value="1">Push (DSS)</option></select>
<h2>Optionen</h2>
<div class="chk"><input type="checkbox" id="ha" __HACHECK__><label>HA MQTT-Discovery</label></div>
<div class="chk"><input type="checkbox" id="json" __JSONCHECK__><label>JSON-Topic</label></div>
<div class="chk"><input type="checkbox" id="oled" __OLEDCHECK__><label>OLED aktiv</label></div>
<div class="chk"><input type="checkbox" id="vcf" __VCFCHECK__><label>Spannung/Strom/Frequenz melden</label></div>
<label>Touch-Display-Timeout (s)</label><input id="touchtimeout" type="number" min="1" value="__TOUCHTO__">
<button onclick="save()">Speichern &amp; Neustart</button>
</div>
<script>
// WLAN-Scan: /scan startet beim ersten Aufruf einen asynchronen Scan und
// liefert {running:true}, bis die Ergebnisse da sind -> sekündlich nachfragen.
async function scan(){
  const b=document.getElementById('scanbtn'),l=document.getElementById('nets');
  b.disabled=true;b.textContent='Suche…';l.textContent='';
  // Waehrend des Scans wechselt der ESP die Kanaele, der eigene AP ist dann
  // kurz weg und einzelne Anfragen scheitern -> nicht abbrechen, weiter fragen.
  try{
    for(let i=0;i<20;i++){
      try{
        const j=await (await fetch('/scan')).json();
        if(j.error){l.textContent=j.error;return}
        if(j.nets){showNets(j.nets);return}
      }catch(e){}
      await new Promise(r=>setTimeout(r,1000));
    }
    l.textContent='Scan fehlgeschlagen – bitte erneut versuchen';
  }finally{b.disabled=false;b.textContent='Suchen'}
}
function showNets(n){
  const l=document.getElementById('nets');
  if(!n.length){l.textContent='Keine Netze gefunden';return}
  n.forEach(x=>{
    const d=document.createElement('div');d.className='net';
    const s=document.createElement('span');s.textContent=x.ssid;
    const q=document.createElement('span');
    q.className='q '+(x.rssi>-60?'g':x.rssi>-75?'m':'b');
    q.textContent=(x.enc?'🔒 ':'')+x.rssi+' dBm';
    d.append(s,q);
    d.onclick=()=>{document.getElementById('ssid').value=x.ssid;l.textContent='';
      document.getElementById('wifipass').focus()};
    l.appendChild(d);
  });
}
async function save(){
  const d={devname:document.getElementById('devname').value,
    ssid:document.getElementById('ssid').value,
    wifipass:document.getElementById('wifipass').value,
    mqtthost:document.getElementById('mqtthost').value,
    mqttport:document.getElementById('mqttport').value,
    mqttuser:document.getElementById('mqttuser').value,
    mqttpass:document.getElementById('mqttpass').value,
    mqttbase:document.getElementById('mqttbase').value,
    baud:document.getElementById('baud').value,
    pollms:document.getElementById('pollms').value,
    mode:document.getElementById('mode').value,
    ha:document.getElementById('ha').checked?1:0,
    json:document.getElementById('json').checked?1:0,
    oled:document.getElementById('oled').checked?1:0,
    vcf:document.getElementById('vcf').checked?1:0,
    touchtimeout:document.getElementById('touchtimeout').value};
  const r=await fetch('/save',{method:'POST',
    headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:new URLSearchParams(d).toString()});
  const j=await r.json();alert(j.msg||'Gespeichert');
}
</script></body></html>)rawliteral";

static const char TERMINAL_HTML[] = R"rawliteral(<!DOCTYPE html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SGM Live-Terminal</title>
<style>
body{font-family:system-ui;background:#1e1e2e;color:#cdd6f4;margin:0;padding:1em}
.card{max-width:700px;margin:auto}
a{color:#89b4fa;font-size:.85em}
h1{color:#89b4fa;font-size:1.2em;margin:.6em 0 .3em}
h2{color:#a6e3a1;font-size:.95em;margin:1em 0 .3em}
pre{background:#11111b;border:1px solid #45475a;border-radius:8px;padding:.6em;
    height:220px;overflow-y:auto;font-size:.75em;line-height:1.4;white-space:pre-wrap;word-break:break-all}
.dot{display:inline-block;width:.6em;height:.6em;border-radius:50%;background:#f38ba8;margin-right:.4em}
.dot.on{background:#a6e3a1}
.toolbar{display:flex;gap:.5em;margin-bottom:.4em}
.toolbar button{background:#45475a;color:#cdd6f4;border:1px solid #585b70;border-radius:6px;
                padding:.3em .7em;font-size:.8em;cursor:pointer}
.toolbar button:hover{background:#585b70}
.toolbar button.active{background:#f38ba8;color:#1e1e2e;border-color:#f38ba8}
</style></head><body><div class="card">
<a href="/">&#8592; Zurück zur Konfiguration</a>
<h1>&#9889; Live-Terminal</h1>
<p><span class="dot" id="dot"></span><span id="status">Verbinde...</span></p>
<h2>Rohdaten (Hex)</h2>
<div class="toolbar">
  <button id="rawPauseBtn" onclick="togglePause('raw')">&#9208; Pause</button>
  <button onclick="copyBox('raw')">&#128203; Kopieren</button>
</div>
<pre id="raw"></pre>
<h2>Decodierte Werte</h2>
<div class="toolbar">
  <button id="decPauseBtn" onclick="togglePause('dec')">&#9208; Pause</button>
  <button onclick="copyBox('dec')">&#128203; Kopieren</button>
</div>
<pre id="dec"></pre>
</div>
<script>
const MAXLINES = 20;
const paused = {raw:false, dec:false};
function togglePause(id) {
  paused[id] = !paused[id];
  const btn = document.getElementById(id + 'PauseBtn');
  btn.textContent = paused[id] ? '▶ Fortsetzen' : '⏸ Pause';
  btn.classList.toggle('active', paused[id]);
}
function fallbackCopy(text) {
  const ta = document.createElement('textarea');
  ta.value = text; ta.style.position = 'fixed'; ta.style.opacity = '0';
  document.body.appendChild(ta); ta.focus(); ta.select();
  try { document.execCommand('copy'); } catch (e) {}
  document.body.removeChild(ta);
}
function copyBox(id) {
  const text = document.getElementById(id).textContent;
  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text).catch(() => fallbackCopy(text));
  } else {
    fallbackCopy(text);
  }
  const btn = event.target;
  const orig = btn.textContent;
  btn.textContent = 'Kopiert!';
  setTimeout(() => btn.textContent = orig, 1200);
}
function addLine(id, text) {
  if (paused[id]) return;
  const el = document.getElementById(id);
  const t = new Date().toLocaleTimeString();
  el.textContent += '[' + t + '] ' + text + '\n';
  const lines = el.textContent.split('\n');
  if (lines.length > MAXLINES + 1) el.textContent = lines.slice(lines.length - MAXLINES - 1).join('\n');
  el.scrollTop = el.scrollHeight;
}
function connect() {
  const ws = new WebSocket('ws://' + location.host + '/ws');
  const dot = document.getElementById('dot'), status = document.getElementById('status');
  ws.onopen = () => { dot.className = 'dot on'; status.textContent = 'Verbunden'; };
  ws.onclose = () => { dot.className = 'dot'; status.textContent = 'Getrennt - versuche erneut...'; setTimeout(connect, 2000); };
  ws.onerror = () => ws.close();
  ws.onmessage = (ev) => {
    const msg = ev.data;
    if (msg.startsWith('RAW ')) addLine('raw', msg.slice(4));
    else if (msg.startsWith('DEC ')) addLine('dec', msg.slice(4));
  };
}
connect();
</script></body></html>)rawliteral";

static String htmlEscape(const char* s) {
  String out;
  out.reserve(strlen(s) + 8);
  for (const char* p = s; *p; p++) {
    switch (*p) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += *p;
    }
  }
  return out;
}

static String fillHtml() {
  String h = String(HTML);
  h.replace("__DEVNAME__",   htmlEscape(cfg.deviceName));
  h.replace("__SSID__",      htmlEscape(cfg.ssid));
  h.replace("__MQTH__",      htmlEscape(cfg.mqttHost));
  h.replace("__MQTP__",      String(cfg.mqttPort));
  h.replace("__MQU__",       htmlEscape(cfg.mqttUser));
  h.replace("__TOPIC__",     htmlEscape(cfg.baseTopic));
  h.replace("__POLL__",      String(cfg.pollMs));
  h.replace("__HACHECK__",   cfg.haDiscovery ? "checked" : "");
  h.replace("__JSONCHECK__", cfg.jsonTopic   ? "checked" : "");
  h.replace("__OLEDCHECK__", cfg.oledEnabled ? "checked" : "");
  h.replace("__VCFCHECK__",  cfg.extraSensors ? "checked" : "");
  h.replace("__TOUCHTO__",   String(cfg.touchTimeoutS));
  if (cfg.pushMode)
    h.replace("<option value=\"1\">Push (DSS)</option>",
              "<option value=\"1\" selected>Push (DSS)</option>");
  // Pattern verankert auf "value="X">X<", passt nur auf das <option>-Element
  // (nicht auf mqttport/pollms-Inputs, deren Wert zufällig einer Baudrate entspricht).
  char selFind[24], selRepl[32];
  snprintf(selFind, sizeof(selFind), "value=\"%d\">%d<", cfg.uartBaud, cfg.uartBaud);
  snprintf(selRepl, sizeof(selRepl), "value=\"%d\" selected>%d<", cfg.uartBaud, cfg.uartBaud);
  h.replace(selFind, selRepl);
  return h;
}

static bool isApMode = false;
static bool restartPending = false;
static uint32_t restartAt = 0;

static constexpr const char* AP_SSID = "SGM-Setup-C3";
static constexpr const char* AP_PASS = "sgm12345";
static constexpr const char* AP_IP   = "192.168.4.1";

// Registriert die Config-Portal-Routen. Wird sowohl im AP-Modus (startAP())
// als auch im normalen WLAN-Betrieb aufgerufen, damit die Weboberflaeche
// nicht nur beim Erst-Setup, sondern jederzeit unter der STA-IP erreichbar
// ist — sonst kaeme man an bestehende Geraete nur per Neuflashen/Force-AP.
static void setupWebServer() {
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "text/html", fillHtml());
  });
  webServer.on("/terminal", HTTP_GET, [](AsyncWebServerRequest* r){
    r->send(200, "text/html", TERMINAL_HTML);
  });
  // Nur Broadcast-Richtung genutzt (siehe wsSendRaw/wsSendDecoded) — eingehende
  // Client-Nachrichten werden ignoriert, aber ein onEvent-Handler muss trotzdem
  // gesetzt sein, sonst reagiert der WS-Endpunkt nicht sauber auf Connects.
  ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient*, AwsEventType, void*, uint8_t*, size_t) {});
  webServer.addHandler(&ws);
  webServer.on("/save", HTTP_POST, [](AsyncWebServerRequest* r){
    auto gp = [&](const char* k) -> String {
      const AsyncWebParameter* p = r->getParam(k, true, false);
      return p ? p->value() : String("");
    };
    String v;
    // Kein leer=unveraendert-Guard wie bei ssid/host: leeres Feld soll
    // bewusst zurueck auf die MAC-basierte Geraete-ID fallen koennen.
    v = gp("devname");  strncpy(cfg.deviceName, v.c_str(), sizeof(cfg.deviceName)-1);
                         cfg.deviceName[sizeof(cfg.deviceName)-1] = 0;
    v = gp("ssid");     if (v.length()) strncpy(cfg.ssid, v.c_str(), sizeof(cfg.ssid)-1);
    v = gp("wifipass"); if (v.length()) strncpy(cfg.pass, v.c_str(), sizeof(cfg.pass)-1);
    v = gp("mqtthost"); if (v.length()) strncpy(cfg.mqttHost, v.c_str(), sizeof(cfg.mqttHost)-1);
    v = gp("mqttport"); if (v.length()) { int pp=v.toInt(); cfg.mqttPort = pp>0?pp:1883; }
    v = gp("mqttuser"); if (v.length()) strncpy(cfg.mqttUser, v.c_str(), sizeof(cfg.mqttUser)-1);
    v = gp("mqttpass"); if (v.length()) strncpy(cfg.mqttPass, v.c_str(), sizeof(cfg.mqttPass)-1);
    v = gp("mqttbase"); if (v.length()) strncpy(cfg.baseTopic, v.c_str(), sizeof(cfg.baseTopic)-1);
    v = gp("baud");     if (v.length()) { int bb=v.toInt(); cfg.uartBaud = bb>0?bb:9600; }
    v = gp("pollms");   if (v.length()) { int pm=v.toInt(); cfg.pollMs   = pm>=MIN_POLL_MS?pm:5000; }
    v = gp("touchtimeout"); if (v.length()) { int tt=v.toInt(); cfg.touchTimeoutS = tt>=1?tt:10; }
    cfg.pushMode    = gp("mode") == "1";
    cfg.haDiscovery = gp("ha")   == "1";
    cfg.jsonTopic   = gp("json") == "1";
    cfg.oledEnabled = gp("oled") == "1";
    cfg.extraSensors = gp("vcf") == "1";
    configSave();
    r->send(200, "application/json", "{\"msg\":\"Gespeichert – Neustart in 2s\"}");
    restartPending = true;
    restartAt = millis() + 1500;
  });
  // Asynchroner WLAN-Scan: ein blockierendes scanNetworks() im AsyncTCP-
  // Handler wuerde den Watchdog ausloesen. Erster Aufruf startet den Scan,
  // Folgeaufrufe liefern {running:true} bis die Ergebnisse da sind.
  // Laesst sich der Scan nicht starten (STA steckt noch in einem
  // Verbindungsversuch), STA trennen und beim naechsten Aufruf erneut
  // versuchen; erst nach mehreren Fehlschlaegen in Folge aufgeben.
  webServer.on("/scan", HTTP_GET, [](AsyncWebServerRequest* r){
    static int startFails = 0;
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) n = WiFi.scanNetworks(true);
    if (n == WIFI_SCAN_FAILED) {
      if (++startFails >= 5) {
        startFails = 0;
        r->send(200, "application/json", "{\"error\":\"Scan konnte nicht gestartet werden\"}");
      } else {
        WiFi.disconnect();
        r->send(200, "application/json", "{\"running\":true}");
      }
      return;
    }
    startFails = 0;
    if (n == WIFI_SCAN_RUNNING) {
      r->send(200, "application/json", "{\"running\":true}");
      return;
    }
    // Nach RSSI absteigend sortieren, dann doppelte SSIDs (Mesh/mehrere APs)
    // und versteckte Netze verwerfen — pro SSID bleibt der staerkste AP.
    static constexpr int MAX_NETS = 20;
    int idx[64];
    int cnt = n < 64 ? n : 64;
    for (int i = 0; i < cnt; i++) idx[i] = i;
    std::sort(idx, idx + cnt, [](int a, int b){ return WiFi.RSSI(a) > WiFi.RSSI(b); });
    DynamicJsonDocument doc(3072);
    JsonArray nets = doc.createNestedArray("nets");
    for (int k = 0; k < cnt && (int)nets.size() < MAX_NETS; k++) {
      String ssid = WiFi.SSID(idx[k]);
      if (!ssid.length()) continue;
      bool dup = false;
      for (JsonObject o : nets) if (ssid == o["ssid"].as<const char*>()) { dup = true; break; }
      if (dup) continue;
      JsonObject o = nets.createNestedObject();
      o["ssid"] = ssid;
      o["rssi"] = WiFi.RSSI(idx[k]);
      o["enc"]  = WiFi.encryptionType(idx[k]) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    String out;
    serializeJson(doc, out);
    r->send(200, "application/json", out);
  });
  webServer.onNotFound([](AsyncWebServerRequest* r){ r->redirect("/"); });
  webServer.begin();
}

static void startAP() {
  // AP_STA statt reinem AP: nur mit aktivem STA-Interface kann das Portal
  // nach WLANs scannen (/scan). Verbunden wird die STA-Seite hier nicht.
  // Auto-Reconnect aus: sonst startet der Core nach jedem gescheiterten
  // Verbindungsversuch (z. B. Router noch nicht da) per Event-Handler sofort
  // den naechsten, und waehrend die STA verbindet, verweigert ESP-IDF jeden
  // Scan (ESP_ERR_WIFI_STATE) — "Suchen" scheiterte dadurch sporadisch.
  WiFi.setAutoReconnect(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();
  IPAddress ip(192,168,4,1), gw(192,168,4,1), mask(255,255,255,0);
  WiFi.softAPConfig(ip, gw, mask);
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.start(53, "192.168.4.1", ip);
  setupWebServer();
}

struct DispPageInfo { const char* label; const char* obis; const char* unit; int prec; };
static const DispPageInfo DISP_PAGES[] = {
  {"Leistung",    "1-0:16.7.0", "W",   0},
  {"Bezug",       "1-0:1.8.0",  "kWh", 2},
  {"Einspeisung", "1-0:2.8.0",  "kWh", 2},
  {"Spannung",    "1-0:32.7.0", "V",   1},
  {"Strom",       "1-0:31.7.0", "A",   2},
  {"Frequenz",    "1-0:14.7.0", "Hz",  2},
};
static constexpr int DISP_PAGE_COUNT = sizeof(DISP_PAGES) / sizeof(DISP_PAGES[0]);

// Zeigt genau einen Messwert gross an (Touch-gesteuertes Durchschalten).
static void drawBigValue(int page) {
  if (page < 0 || page >= DISP_PAGE_COUNT) page = 0;
  const DispPageInfo& info = DISP_PAGES[page];
  float value = getObis(info.obis, NAN);

  char buf[24];
  dispShowsStatus = false;
  u8g2.clearBuffer();
  
  // Label oben links
  u8g2.setFont(u8g2_font_6x10_tr);
  u8g2.drawStr(0, 10, info.label);

  // Messwert formatieren
  if (isnan(value)) snprintf(buf, sizeof(buf), "---");
  else              snprintf(buf, sizeof(buf), "%.*f", info.prec, value);

  // --- MESSWERT ZENTRIEREN ---
  u8g2.setFont(u8g2_font_logisoso24_tr);
  int16_t valueWidth = u8g2.getStrWidth(buf);                 // Breite des Textes in Pixeln
  int16_t valueX = (u8g2.getWidth() - valueWidth) / 2;       // X-Start für die Mitte
  u8g2.drawStr(valueX, 44, buf);

  // --- EINHEIT RECHTSBÜNDIG ---
  u8g2.setFont(u8g2_font_6x10_tr);
  int16_t unitWidth = u8g2.getStrWidth(info.unit);             // Breite der Einheit in Pixeln
  int16_t unitX = u8g2.getWidth() - unitWidth - 2;            // X-Start (Gesamtbreite minus Textbreite minus 2px Rand)
  u8g2.drawStr(unitX, 63, info.unit);

  u8g2.sendBuffer();
}

// Naechste Seite ab (page+1), die gerade einen Messwert hat — Seiten ohne
// Wert (Zaehler sendet sie nicht, oder Checkbox "Spannung/Strom/Frequenz
// melden" ist aus) werden beim Durchschalten uebersprungen. Faellt auf 0
// zurueck, falls (voruebergehend) gar keine Seite verfuegbar ist.
static int nextAvailablePage(int page) {
  for (int i = 1; i <= DISP_PAGE_COUNT; i++) {
    int p = (page + i) % DISP_PAGE_COUNT;
    if (!isnan(getObis(DISP_PAGES[p].obis, NAN))) return p;
  }
  return 0;
}

// Touch lesen, entprellen (250ms), steigende Flanke = ein Tastendruck.
// Wird sowohl aus loop() als auch waehrend blockierender SML-Reads
// aufgerufen (siehe Vorwaertsdeklaration weiter oben), damit ein Touch
// waehrend eines laufenden Poll-Zyklus (bis zu 4s) nicht verloren geht.
static void pollTouch() {
  bool raw = digitalRead(CFG_TOUCH_PIN) == HIGH;
  if (raw && !lastTouchRaw && millis() - lastTouchEdgeMs > 250) {
    lastTouchEdgeMs = millis();
    if (!oledOn) {
      oledOn = true;
      u8g2.setPowerSave(0);
      dispPage = isnan(getObis(DISP_PAGES[0].obis, NAN)) ? nextAvailablePage(-1) : 0;
    } else {
      dispPage = nextAvailablePage(dispPage);
    }
    oledOnSince = millis();
    oledBootHold = false;
    drawBigValue(dispPage);
  }
  lastTouchRaw = raw;
}

static void pollAndPublish() {
  bool ok = cfg.pushMode ? smlListenPush(4000) : smlPollD0(3000);
  if (ok) {
    float p  = getObis("1-0:16.7.0");
    float imp= getObis("1-0:1.8.0");
    float ex = getObis("1-0:2.8.0");
    float v    = cfg.extraSensors ? getObis("1-0:32.7.0") : NAN;
    float c    = cfg.extraSensors ? getObis("1-0:31.7.0") : NAN;
    float freq = cfg.extraSensors ? getObis("1-0:14.7.0") : NAN;
    publishValues(p, imp, ex, v, c, freq);
    // Eine angezeigte Werteseite bleibt stehen (kein Flackern); nur ein
    // Status-Bildschirm wird einmalig durch die Werte ersetzt.
    if (oledActive && oledOn && dispShowsStatus) drawBigValue(dispPage);
  } else {
    if (oledActive && oledOn && dispShowsStatus)
      dispStatus("Keine Daten", cfg.pushMode?"Push-Modus":"Poll-Modus", gIdent[0]?gIdent:"");
  }
}

void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println("\n[BOOT] ESP32-C3 startet...");

  configLoad();
  initDeviceId();
  buildTopics();
  Serial.printf("[CFG] Geraete-ID=%s SSID='%s' MQTT=%s:%d Topic='%s' OLED=%s Modus=%s Poll=%dms\n",
                gDeviceId, cfg.ssid, cfg.mqttHost, cfg.mqttPort, cfg.baseTopic,
                cfg.oledEnabled ? "an" : "aus",
                cfg.pushMode ? "Push" : "Poll", cfg.pollMs);

  if (cfg.oledEnabled) {
    oledActive = dispInit();
    if (oledActive) {
      // Pull-Down statt reinem INPUT: definierter LOW-Ruhezustand, auch
      // wenn (noch) nur ein loser Draht statt Touch-Modul angeschlossen ist.
      pinMode(CFG_TOUCH_PIN, INPUT_PULLDOWN);
      oledOn = true;
      oledBootHold = true;
      oledOnSince = millis();
      dispStatus("SGM Gateway C3", "Booting...", "");
    }
  }

  meterSerial.begin(cfg.uartBaud, SERIAL_7E1, CFG_CH1_RX, CFG_CH1_TX);

  bool wifiOk = false;
  if (gForceAp) {
    Serial.println("[CFG] Config-Portal erzwungen (MQTT cmd/config)");
  } else if (strlen(cfg.ssid) > 0 && strcmp(cfg.ssid, "changeme") != 0) {
    Serial.printf("[WIFI] Verbinde mit SSID '%s' ...\n", cfg.ssid);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("sgm-gateway-c3");
    WiFi.begin(cfg.ssid, cfg.pass);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();
    wifiOk = (WiFi.status() == WL_CONNECTED);
    if (wifiOk)
      Serial.printf("[WIFI] verbunden, IP=%s\n", WiFi.localIP().toString().c_str());
    else
      Serial.printf("[WIFI] Verbindung fehlgeschlagen (status=%d)\n", (int)WiFi.status());
  } else {
    Serial.println("[WIFI] keine SSID konfiguriert (Default 'changeme')");
  }

  if (!wifiOk) {
    isApMode = true;
    startAP();
    Serial.println("[AP] Captive-Portal gestartet: SSID='SGM-Setup-C3' IP=192.168.4.1");
    if (oledActive) dispApInfo(AP_SSID, AP_PASS, AP_IP);
    return;
  }

  setupWebServer();
  Serial.printf("[WEB] Config-Portal erreichbar unter http://%s/\n",
                WiFi.localIP().toString().c_str());

  if (oledActive) dispStatus("MQTT", cfg.mqttHost, "Verbinde...");
  mqtt.begin(net);
  mqtt.setHost(cfg.mqttHost, cfg.mqttPort);
  mqtt.setKeepAlive(30);
  mqtt.setTimeout(5000);
  mqtt.setWill(topicAvail, "offline", true, 1);
  mqtt.onMessageAdvanced(mqttCmd);
  mqttReconnect();

  lastPoll = millis();
}

void loop() {
  if (restartPending && (int32_t)(millis() - restartAt) >= 0) ESP.restart();

  if (isApMode) {
    dnsServer.processNextRequest();
    // Rueckweg aus dem AP-Portal: nach einem Stromausfall bootet der ESP
    // meist schneller als der Router und landet sonst dauerhaft hier. Ist
    // AP_IDLE_REBOOT_MS lang kein Client verbunden, neu starten und das
    // gespeicherte WLAN erneut versuchen (nur wenn eins konfiguriert ist).
    static uint32_t apIdleSince = millis();
    if (WiFi.softAPgetStationNum() > 0) {
      apIdleSince = millis();
    } else if (strcmp(cfg.ssid, "changeme") != 0 &&
               millis() - apIdleSince > AP_IDLE_REBOOT_MS) {
      Serial.println("[AP] Kein Client im Portal - Neustart, versuche WLAN erneut");
      delay(100);
      ESP.restart();
    }
    delay(20);
    return;
  }

  mqttReconnect();
  mqtt.loop();

  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t lastTry = 0;
    if (millis() - lastTry > 10000) {
      lastTry = millis();
      WiFi.reconnect();
    }
    delay(100);
    return;
  }

  if (millis() - lastPoll >= (uint32_t)cfg.pollMs) {
    lastPoll = millis();
    pollAndPublish();
  }

  if (oledActive) {
    pollTouch();

    // Auto-Off nach Timeout — direkt nach dem Boot gilt die grosszuegige
    // Haltezeit, danach der (im Portal einstellbare) kurze Touch-Timeout.
    uint32_t offDelayMs = oledBootHold ? BOOT_DISPLAY_HOLD_MS
                                        : (uint32_t)cfg.touchTimeoutS * 1000UL;
    if (oledOn && millis() - oledOnSince > offDelayMs) {
      oledOn = false;
      oledBootHold = false;
      u8g2.setPowerSave(1);
    }
  }

  delay(20);
}