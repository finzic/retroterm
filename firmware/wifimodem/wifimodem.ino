// RetroSSH WiFi modem: Hayes-style AT front end that bridges a retro computer's
// serial port to Telnet (ESP8266/ESP32) or SSH (ESP32 + LibSSH-ESP32).
#include <Arduino.h>
#include <EEPROM.h>

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#elif defined(ESP32)
#include <WiFi.h>
#else
#error "Unsupported board: use an ESP8266 or an ESP32"
#endif

#ifndef RETROSSH_SSH
#define RETROSSH_SSH 0
#endif

#if RETROSSH_SSH
#if !defined(ESP32)
#error "SSH support needs an ESP32: the ESP8266 lacks the RAM for SSH key exchange"
#endif
#include "libssh_esp32.h"
#include <libssh/libssh.h>
// libssh key exchange needs far more than the default 8 KB loop task stack.
SET_LOOP_TASK_STACK_SIZE(48 * 1024);
#endif

#define FW_VERSION "RetroSSH WiFi Modem 0.1"

// TTL-side handshake lines, active LOW like a UART (the MAX3232 inverts them).
#if defined(ESP8266)
const uint8_t PIN_CTS = 5;  // D1 on a Wemos D1 mini: LOW = computer ready to receive
const uint8_t PIN_RTS = 4;  // D2: LOW = modem ready to receive
#else
const uint8_t PIN_CTS = 18;
const uint8_t PIN_RTS = 19;
#endif

const size_t RX_BUFFER = 1024;
const size_t RX_HIGH_WATER = 768;
const uint32_t GUARD_MS = 1000;
const uint32_t CTS_TIMEOUT_MS = 2000;
const uint32_t WIFI_TIMEOUT_MS = 20000;
const uint32_t SETTINGS_MAGIC = 0x52535301;
const uint32_t BAUDS[] = {300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
const int KNOWN_HOSTS = 4;

struct KnownHost {
  char host[64];  // "host:port"
  char fp[64];    // "SHA256:..."
};

struct Settings {
  uint32_t magic;
  char ssid[33];
  char pass[65];
  char term[16];
  uint32_t baud;
  uint8_t echo;
  uint8_t flow;    // 0 = none, 1 = RTS/CTS
  uint8_t filter;  // 0 = pass everything, 1 = strip ANSI escape sequences
  uint8_t cols;
  uint8_t rows;
  KnownHost hosts[KNOWN_HOSTS];
};

enum Mode { MODE_COMMAND, MODE_ONLINE };
enum LinkType { LINK_NONE, LINK_TELNET, LINK_SSH };

Settings cfg;
Mode mode = MODE_COMMAND;
LinkType linkType = LINK_NONE;
uint32_t currentBaud = 0;
WiFiClient tcp;

char line[256];  // fits AT+CWJAP with a fully escaped 32-char SSID and 63-char passphrase
size_t lineLen = 0;

uint8_t plusCount = 0;
uint32_t lastLocalRx = 0;
bool ctsStuck = false;

// ---------------------------------------------------------------- settings

void setDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = SETTINGS_MAGIC;
  cfg.baud = 1200;
  cfg.echo = 1;
  cfg.flow = 0;
  cfg.filter = 0;
  cfg.cols = 80;
  cfg.rows = 24;
  strcpy(cfg.term, "vt100");
}

bool validBaud(uint32_t b) {
  for (uint32_t v : BAUDS) {
    if (v == b) return true;
  }
  return false;
}

void loadSettings() {
  EEPROM.get(0, cfg);
  if (cfg.magic != SETTINGS_MAGIC) {
    setDefaults();
    return;
  }
  cfg.ssid[sizeof(cfg.ssid) - 1] = 0;
  cfg.pass[sizeof(cfg.pass) - 1] = 0;
  cfg.term[sizeof(cfg.term) - 1] = 0;
  for (KnownHost &h : cfg.hosts) {
    h.host[sizeof(h.host) - 1] = 0;
    h.fp[sizeof(h.fp) - 1] = 0;
  }
  if (!validBaud(cfg.baud)) cfg.baud = 1200;
  if (!cfg.cols) cfg.cols = 80;
  if (!cfg.rows) cfg.rows = 24;
}

void saveSettings() {
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

// ---------------------------------------------------------------- serial I/O

void updateRts() {
  digitalWrite(PIN_RTS, (cfg.flow && (size_t)Serial.available() > RX_HIGH_WATER) ? HIGH : LOW);
}

// With flow control, send one byte at a time and only while the computer
// asserts CTS: bit-banged ports (Spectrum Interface 1) can't buffer.
// In command mode, give up on a dead CTS so AT&K0 can still be typed blind.
void serialOut(uint8_t c) {
  if (cfg.flow) {
    uint32_t t0 = millis();
    while (digitalRead(PIN_CTS) == HIGH) {
      if (mode == MODE_COMMAND && (ctsStuck || millis() - t0 > CTS_TIMEOUT_MS)) {
        ctsStuck = true;
        return;
      }
      updateRts();
      yield();
    }
    ctsStuck = false;
    Serial.write(c);
    Serial.flush();
  } else {
    Serial.write(c);
  }
}

void say(const char *s) {
  while (*s) serialOut((uint8_t)*s++);
}

void sayf(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  say(buf);
}

void reply(const char *s) {
  say("\r\n");
  say(s);
  say("\r\n");
}

int readKey() {
  while (!Serial.available()) {
    updateRts();
    yield();
  }
  return Serial.read();
}

// Reads a line terminated by CR. Returns false if ESC or Ctrl-C aborts it.
bool readLine(char *buf, size_t n, bool echo) {
  size_t len = 0;
  for (;;) {
    int c = readKey();
    if (c == '\r') {
      buf[len] = 0;
      say("\r\n");
      return true;
    }
    if (c == 27 || c == 3) {
      buf[0] = 0;
      say("\r\n");
      return false;
    }
    if ((c == 8 || c == 127) && len) {
      len--;
      if (echo) say("\b \b");
    } else if (c >= 32 && len < n - 1) {
      buf[len++] = (char)c;
      if (echo) serialOut(c);
    }
  }
}

void secureZero(char *p, size_t n) {
  volatile char *v = p;
  while (n--) *v++ = 0;
}

// ---------------------------------------------------------------- ANSI filter

enum AnsiState { A_TEXT, A_ESC, A_CSI, A_OSC, A_OSC_ESC, A_SKIP1 };
AnsiState ansiState = A_TEXT;

// Returns true if the byte should reach the screen.
bool ansiPass(uint8_t c) {
  switch (ansiState) {
    case A_TEXT:
      if (c == 27) {
        ansiState = A_ESC;
        return false;
      }
      return c != 14 && c != 15;
    case A_ESC:
      if (c == '[') ansiState = A_CSI;
      else if (c == ']') ansiState = A_OSC;
      else if (c == '(' || c == ')' || c == '#') ansiState = A_SKIP1;
      else ansiState = A_TEXT;
      return false;
    case A_CSI:
      if (c >= 0x40 && c <= 0x7E) ansiState = A_TEXT;
      return false;
    case A_OSC:
      if (c == 7) ansiState = A_TEXT;
      else if (c == 27) ansiState = A_OSC_ESC;
      return false;
    case A_OSC_ESC:
    case A_SKIP1:
      ansiState = A_TEXT;
      return false;
  }
  return true;
}

// ---------------------------------------------------------------- Telnet

const uint8_t T_IAC = 255, T_DONT = 254, T_DO = 253, T_WONT = 252, T_WILL = 251;
const uint8_t T_SB = 250, T_SE = 240;
const uint8_t OPT_ECHO = 1, OPT_SGA = 3, OPT_TTYPE = 24, OPT_NAWS = 31;

enum TelnetState { TS_DATA, TS_IAC, TS_OPT, TS_SB, TS_SB_IAC };
TelnetState tState = TS_DATA;
uint8_t tVerb = 0;
uint8_t sb[8];
size_t sbLen = 0;
uint32_t usOn = 0;    // options we have agreed to perform (bit = option < 32)
uint32_t themOn = 0;  // options the server has agreed to perform

void telnetReset() {
  tState = TS_DATA;
  usOn = themOn = 0;
  sbLen = 0;
}

void telnetCmd(uint8_t verb, uint8_t opt) {
  uint8_t b[3] = {T_IAC, verb, opt};
  tcp.write(b, sizeof(b));
}

void telnetNaws() {
  uint8_t b[] = {T_IAC, T_SB, OPT_NAWS, 0, cfg.cols, 0, cfg.rows, T_IAC, T_SE};
  tcp.write(b, sizeof(b));
}

void telnetTtype() {
  uint8_t head[] = {T_IAC, T_SB, OPT_TTYPE, 0};
  uint8_t tail[] = {T_IAC, T_SE};
  tcp.write(head, sizeof(head));
  tcp.write((const uint8_t *)cfg.term, strlen(cfg.term));
  tcp.write(tail, sizeof(tail));
}

void telnetOption(uint8_t verb, uint8_t opt) {
  uint32_t mask = opt < 32 ? (1UL << opt) : 0;
  switch (verb) {
    case T_DO:
      if (opt == OPT_TTYPE || opt == OPT_NAWS || opt == OPT_SGA) {
        if (!(usOn & mask)) {
          usOn |= mask;
          telnetCmd(T_WILL, opt);
        }
        if (opt == OPT_NAWS) telnetNaws();
      } else {
        telnetCmd(T_WONT, opt);
      }
      break;
    case T_DONT:
      if (usOn & mask) {
        usOn &= ~mask;
        telnetCmd(T_WONT, opt);
      }
      break;
    case T_WILL:
      if (opt == OPT_ECHO || opt == OPT_SGA) {
        if (!(themOn & mask)) {
          themOn |= mask;
          telnetCmd(T_DO, opt);
        }
      } else {
        telnetCmd(T_DONT, opt);
      }
      break;
    case T_WONT:
      if (themOn & mask) {
        themOn &= ~mask;
        telnetCmd(T_DONT, opt);
      }
      break;
  }
}

// Consumes one byte from the server; returns a data byte or -1.
int telnetRx(uint8_t c) {
  switch (tState) {
    case TS_DATA:
      if (c == T_IAC) {
        tState = TS_IAC;
        return -1;
      }
      return c == 0 ? -1 : c;
    case TS_IAC:
      tState = TS_DATA;
      if (c == T_IAC) return c;
      if (c >= T_WILL && c <= T_DONT) {
        tVerb = c;
        tState = TS_OPT;
      } else if (c == T_SB) {
        sbLen = 0;
        tState = TS_SB;
      }
      return -1;
    case TS_OPT:
      telnetOption(tVerb, c);
      tState = TS_DATA;
      return -1;
    case TS_SB:
      if (c == T_IAC) tState = TS_SB_IAC;
      else if (sbLen < sizeof(sb)) sb[sbLen++] = c;
      return -1;
    case TS_SB_IAC:
      if (c == T_SE) {
        tState = TS_DATA;
        if (sbLen >= 2 && sb[0] == OPT_TTYPE && sb[1] == 1) telnetTtype();
      } else {
        if (c == T_IAC && sbLen < sizeof(sb)) sb[sbLen++] = c;
        tState = TS_SB;
      }
      return -1;
  }
  return -1;
}

// ---------------------------------------------------------------- SSH

#if RETROSSH_SSH
ssh_session sshSess = nullptr;
ssh_channel sshChan = nullptr;

void sshClose() {
  if (sshChan) {
    ssh_channel_close(sshChan);
    ssh_channel_free(sshChan);
    sshChan = nullptr;
  }
  if (sshSess) {
    ssh_disconnect(sshSess);
    ssh_free(sshSess);
    sshSess = nullptr;
  }
}

int findHost(const char *key) {
  for (int i = 0; i < KNOWN_HOSTS; i++) {
    if (strcmp(cfg.hosts[i].host, key) == 0) return i;
  }
  return -1;
}

// Trust-on-first-use host key check against the modem's known-hosts table.
bool sshCheckHostKey(const char *key) {
  ssh_key srv = nullptr;
  unsigned char *hash = nullptr;
  size_t hlen = 0;
  if (ssh_get_server_publickey(sshSess, &srv) != SSH_OK) return false;
  int rc = ssh_get_publickey_hash(srv, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen);
  ssh_key_free(srv);
  if (rc != 0) return false;
  char *fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
  ssh_clean_pubkey_hash(&hash);
  if (!fp) return false;

  bool ok = false;
  int i = findHost(key);
  if (i >= 0) {
    ok = strcmp(cfg.hosts[i].fp, fp) == 0;
    if (!ok) {
      sayf("\r\nWARNING: HOST KEY CHANGED FOR %s\r\n%s\r\n", key, fp);
      sayf("If expected, remove it with AT+KHDEL=%d\r\n", i);
    }
  } else {
    int slot = findHost("");
    if (slot < 0) {
      say("\r\nKnown hosts full: free a slot with AT+KHDEL=n\r\n");
    } else {
      sayf("\r\nUnknown host %s\r\n%s\r\nAccept and save (y/n)? ", key, fp);
      int c = readKey();
      say("\r\n");
      if (c == 'y' || c == 'Y') {
        strncpy(cfg.hosts[slot].host, key, sizeof(cfg.hosts[slot].host) - 1);
        strncpy(cfg.hosts[slot].fp, fp, sizeof(cfg.hosts[slot].fp) - 1);
        saveSettings();
        ok = true;
      }
    }
  }
  ssh_string_free_char(fp);
  return ok;
}
#endif

// ---------------------------------------------------------------- link

bool linkConnected() {
  if (linkType == LINK_TELNET) return tcp.connected() || tcp.available();
#if RETROSSH_SSH
  if (linkType == LINK_SSH)
    return sshChan && ssh_channel_is_open(sshChan) && !ssh_channel_is_eof(sshChan);
#endif
  return false;
}

void linkWrite(uint8_t c) {
  if (linkType == LINK_TELNET) {
    if (c == T_IAC) {
      uint8_t b[2] = {T_IAC, T_IAC};
      tcp.write(b, 2);
    } else if (c == '\r') {
      uint8_t b[2] = {'\r', 0};  // RFC 854: bare CR must be followed by NUL
      tcp.write(b, 2);
    } else {
      tcp.write(c);
    }
  }
#if RETROSSH_SSH
  else if (linkType == LINK_SSH) {
    ssh_channel_write(sshChan, &c, 1);
  }
#endif
}

int linkRead(uint8_t *buf, size_t n) {
  if (linkType == LINK_TELNET) return tcp.available() ? tcp.read(buf, n) : 0;
#if RETROSSH_SSH
  if (linkType == LINK_SSH) {
    int r = ssh_channel_read_nonblocking(sshChan, buf, n, 0);
    if (r <= 0) r = ssh_channel_read_nonblocking(sshChan, buf, n, 1);
    return r > 0 ? r : 0;
  }
#endif
  return 0;
}

void hangup() {
  if (linkType == LINK_TELNET) tcp.stop();
#if RETROSSH_SSH
  else if (linkType == LINK_SSH) sshClose();
#endif
  linkType = LINK_NONE;
  mode = MODE_COMMAND;
  plusCount = 0;
}

void goOnline(LinkType t) {
  linkType = t;
  mode = MODE_ONLINE;
  ansiState = A_TEXT;
  plusCount = 0;
  lastLocalRx = millis();
  char msg[24];
  snprintf(msg, sizeof(msg), "CONNECT %lu", (unsigned long)cfg.baud);
  reply(msg);
}

bool parseHostPort(const char *a, char *host, size_t n, uint16_t *port) {
  while (*a == ' ') a++;
  const char *colon = strrchr(a, ':');
  size_t len = colon ? (size_t)(colon - a) : strlen(a);
  if (!len || len >= n) return false;
  memcpy(host, a, len);
  host[len] = 0;
  if (colon) {
    long p = atol(colon + 1);
    if (p <= 0 || p > 65535) return false;
    *port = (uint16_t)p;
  }
  return true;
}

void dialTelnet(const char *a) {
  char host[64];
  uint16_t port = 23;
  if (!parseHostPort(a, host, sizeof(host), &port)) {
    reply("ERROR");
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    reply("NO DIALTONE");
    return;
  }
  hangup();
  if (!tcp.connect(host, port)) {
    reply("NO ANSWER");
    return;
  }
  tcp.setNoDelay(true);
  telnetReset();
  goOnline(LINK_TELNET);
}

void dialSsh(const char *a) {
#if RETROSSH_SSH
  char user[32], host[48], key[64], pw[64];
  uint16_t port = 22;
  while (*a == ' ') a++;
  const char *at = strchr(a, '@');
  if (!at || at == a || (size_t)(at - a) >= sizeof(user) ||
      !parseHostPort(at + 1, host, sizeof(host), &port)) {
    reply("ERROR");
    return;
  }
  memcpy(user, a, at - a);
  user[at - a] = 0;
  if (WiFi.status() != WL_CONNECTED) {
    reply("NO DIALTONE");
    return;
  }
  hangup();
  snprintf(key, sizeof(key), "%s:%u", host, port);

  sshSess = ssh_new();
  if (!sshSess) {
    reply("ERROR");
    return;
  }
  int p = port;
  long timeout = 15;
  ssh_options_set(sshSess, SSH_OPTIONS_HOST, host);
  ssh_options_set(sshSess, SSH_OPTIONS_USER, user);
  ssh_options_set(sshSess, SSH_OPTIONS_PORT, &p);
  ssh_options_set(sshSess, SSH_OPTIONS_TIMEOUT, &timeout);
  say("\r\nConnecting...");
  if (ssh_connect(sshSess) != SSH_OK) {
    sshClose();
    reply("NO ANSWER");
    return;
  }
  if (!sshCheckHostKey(key)) {
    sshClose();
    reply("NO CARRIER");
    return;
  }
  sayf("\r\n%s@%s's password: ", user, host);
  bool entered = readLine(pw, sizeof(pw), false);
  int rc = entered ? ssh_userauth_password(sshSess, nullptr, pw) : SSH_AUTH_DENIED;
  secureZero(pw, sizeof(pw));
  if (rc != SSH_AUTH_SUCCESS) {
    sshClose();
    reply("ACCESS DENIED");
    return;
  }
  sshChan = ssh_channel_new(sshSess);
  if (!sshChan || ssh_channel_open_session(sshChan) != SSH_OK ||
      ssh_channel_request_pty_size(sshChan, cfg.term, cfg.cols, cfg.rows) != SSH_OK ||
      ssh_channel_request_shell(sshChan) != SSH_OK) {
    sshClose();
    reply("NO CARRIER");
    return;
  }
  goOnline(LINK_SSH);
#else
  (void)a;
  reply("ERROR: SSH needs the ESP32 firmware");
#endif
}

// ---------------------------------------------------------------- WiFi

bool quotedField(const char *&p, char *out, size_t n) {
  while (*p == ' ' || *p == ',') p++;
  if (*p != '"') return false;
  p++;
  size_t len = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) p++;
    if (len >= n - 1) return false;
    out[len++] = *p++;
  }
  if (*p != '"') return false;
  p++;
  out[len] = 0;
  return true;
}

bool wifiJoin(const char *a) {
  char ssid[sizeof(cfg.ssid)], pass[sizeof(cfg.pass)];
  pass[0] = 0;
  if (!quotedField(a, ssid, sizeof(ssid)) || !ssid[0]) return false;
  if (*a && !quotedField(a, pass, sizeof(pass))) return false;
  size_t plen = strlen(pass);
  if (plen && (plen < 8 || plen > 63)) return false;  // WPA2-PSK passphrase rules

  WiFi.disconnect();
  WiFi.begin(ssid, plen ? pass : nullptr);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) delay(100);
  bool ok = WiFi.status() == WL_CONNECTED;
  if (ok) {
    strcpy(cfg.ssid, ssid);
    strcpy(cfg.pass, pass);
    sayf("\r\nWIFI CONNECTED %s", WiFi.localIP().toString().c_str());
  }
  secureZero(pass, sizeof(pass));
  return ok;
}

void wifiStatus() {
  if (WiFi.status() == WL_CONNECTED)
    sayf("\r\n+CWJAP:\"%s\",%s,%d dBm", WiFi.SSID().c_str(),
         WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  else
    say("\r\nNo AP");
}

void wifiScan() {
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) sayf("\r\n+CWLAP:\"%s\",%d dBm", WiFi.SSID(i).c_str(), (int)WiFi.RSSI(i));
  WiFi.scanDelete();
}

// ---------------------------------------------------------------- commands

const char *cmdUp;
const char *cmdOrig;
const char *arg;

bool is(const char *name) {
  size_t n = strlen(name);
  if (strncmp(cmdUp, name, n) != 0) return false;
  arg = cmdOrig + n;
  return true;
}

bool isExact(const char *name) { return strcmp(cmdUp, name) == 0; }

void help() {
  say("\r\nAT?  ATI  ATZ  AT&F  AT&W  AT&V  ATE0/1  AT&K0/3\r\n"
      "AT+IPR=<baud>  AT+CWJAP=\"ssid\",\"pass\"  AT+CWJAP?\r\n"
      "AT+CWQAP  AT+CWLAP  AT+CIFSR\r\n"
      "AT+TERM=<name>  AT+WIN=<cols>,<rows>  AT+FILTER=0/1\r\n"
      "AT+KH?  AT+KHDEL=<n>\r\n"
      "ATDT<host>[:port]  ATDS<user>@<host>[:port]\r\n"
      "+++ (1s guard)  ATO  ATH");
}

void info() {
  sayf("\r\n%s\r\n", FW_VERSION);
#if defined(ESP8266)
  say("Board: ESP8266, Telnet only");
#else
  say(RETROSSH_SSH ? "Board: ESP32, Telnet + SSH" : "Board: ESP32, Telnet only");
#endif
}

void showSettings() {
  sayf("\r\nBAUD=%lu ECHO=%u FLOW=%s FILTER=%u", (unsigned long)cfg.baud, cfg.echo,
       cfg.flow ? "RTS/CTS" : "NONE", cfg.filter);
  sayf("\r\nTERM=%s WIN=%ux%u", cfg.term, cfg.cols, cfg.rows);
  sayf("\r\nSSID=\"%s\"", cfg.ssid);
}

bool setWindow(const char *a) {
  int c = atoi(a);
  const char *comma = strchr(a, ',');
  int r = comma ? atoi(comma + 1) : 0;
  if (c < 16 || c > 250 || r < 4 || r > 250) return false;
  cfg.cols = c;
  cfg.rows = r;
  if (linkType == LINK_TELNET && (usOn & (1UL << OPT_NAWS))) telnetNaws();
#if RETROSSH_SSH
  if (linkType == LINK_SSH) ssh_channel_change_pty_size(sshChan, cfg.cols, cfg.rows);
#endif
  return true;
}

bool setTerm(const char *a) {
  size_t n = strlen(a);
  if (!n || n >= sizeof(cfg.term)) return false;
  for (size_t i = 0; i < n; i++) {
    if (!isalnum((unsigned char)a[i]) && a[i] != '-') return false;
  }
  strcpy(cfg.term, a);
  return true;
}

void listHosts() {
  for (int i = 0; i < KNOWN_HOSTS; i++) {
    if (cfg.hosts[i].host[0]) sayf("\r\n%d %s %s", i, cfg.hosts[i].host, cfg.hosts[i].fp);
  }
}

bool delHost(const char *a) {
  int i = atoi(a);
  if (!isdigit((unsigned char)*a) || i >= KNOWN_HOSTS) return false;
  memset(&cfg.hosts[i], 0, sizeof(cfg.hosts[i]));
  saveSettings();
  return true;
}

void execute(char *s) {
  while (*s == ' ') s++;
  if (!*s) return;
  char up[sizeof(line)];
  size_t i = 0;
  for (; s[i] && i < sizeof(up) - 1; i++) up[i] = toupper((unsigned char)s[i]);
  up[i] = 0;
  if (strncmp(up, "AT", 2) != 0) {
    reply("ERROR");
    return;
  }
  cmdUp = up + 2;
  cmdOrig = s + 2;

  bool ok = true;
  if (!*cmdUp) {
  } else if (isExact("?")) {
    help();
  } else if (isExact("I")) {
    info();
  } else if (isExact("Z")) {
    loadSettings();
  } else if (isExact("&F")) {
    setDefaults();
  } else if (isExact("&W")) {
    saveSettings();
  } else if (isExact("&V")) {
    showSettings();
  } else if (isExact("E0") || isExact("E1")) {
    cfg.echo = cmdUp[1] == '1';
  } else if (isExact("&K0") || isExact("&K3")) {
    cfg.flow = cmdUp[2] == '3';
  } else if (is("+IPR=")) {
    uint32_t b = strtoul(arg, nullptr, 10);
    ok = validBaud(b);
    if (ok) cfg.baud = b;
  } else if (isExact("+CWJAP?")) {
    wifiStatus();
  } else if (is("+CWJAP=")) {
    ok = wifiJoin(arg);
  } else if (isExact("+CWQAP")) {
    WiFi.disconnect();
  } else if (isExact("+CWLAP")) {
    wifiScan();
  } else if (isExact("+CIFSR")) {
    sayf("\r\n+CIFSR:%s", WiFi.localIP().toString().c_str());
  } else if (is("+TERM=")) {
    ok = setTerm(arg);
  } else if (is("+WIN=")) {
    ok = setWindow(arg);
  } else if (isExact("+FILTER=0") || isExact("+FILTER=1")) {
    cfg.filter = cmdUp[8] == '1';
  } else if (isExact("+KH?")) {
    listHosts();
  } else if (is("+KHDEL=")) {
    ok = delHost(arg);
  } else if (is("DS")) {
    dialSsh(arg);
    return;
  } else if (is("DT") || is("D")) {
    dialTelnet(arg);
    return;
  } else if (isExact("O")) {
    if (linkType != LINK_NONE && linkConnected()) {
      goOnline(linkType);
    } else {
      reply("NO CARRIER");
    }
    return;
  } else if (isExact("H") || isExact("H0")) {
    hangup();
  } else {
    ok = false;
  }
  reply(ok ? "OK" : "ERROR");

  // Baud changes take effect after the reply, as on a Hayes modem.
  if (cfg.baud != currentBaud) {
    Serial.flush();
    Serial.updateBaudRate(cfg.baud);
    currentBaud = cfg.baud;
  }
}

// ---------------------------------------------------------------- main loop

void commandInput() {
  while (Serial.available() && mode == MODE_COMMAND) {
    int c = Serial.read();
    if (c == '\r') {
      if (cfg.echo) say("\r\n");
      line[lineLen] = 0;
      lineLen = 0;
      execute(line);
    } else if (c == 8 || c == 127) {
      if (lineLen) {
        lineLen--;
        if (cfg.echo) say("\b \b");
      }
    } else if (c >= 32 && lineLen < sizeof(line) - 1) {
      line[lineLen++] = (char)c;
      if (cfg.echo) serialOut(c);
    }
  }
}

void flushPluses() {
  while (plusCount) {
    linkWrite('+');
    plusCount--;
  }
}

void pumpOnline() {
  uint32_t now = millis();
  while (Serial.available()) {
    uint8_t c = Serial.read();
    bool quietBefore = now - lastLocalRx >= GUARD_MS;
    lastLocalRx = now;
    if (c == '+' && plusCount < 3 && (plusCount > 0 || quietBefore)) {
      plusCount++;
      continue;
    }
    flushPluses();
    linkWrite(c);
  }
  if (plusCount && now - lastLocalRx >= GUARD_MS) {
    if (plusCount == 3) {
      plusCount = 0;
      mode = MODE_COMMAND;
      reply("OK");
      return;
    }
    flushPluses();
  }

  if (cfg.flow && digitalRead(PIN_CTS) == HIGH) return;
  uint8_t buf[64];
  int n = linkRead(buf, sizeof(buf));
  for (int i = 0; i < n; i++) {
    int c = linkType == LINK_TELNET ? telnetRx(buf[i]) : buf[i];
    if (c < 0 || (cfg.filter && !ansiPass((uint8_t)c))) continue;
    serialOut((uint8_t)c);
  }
}

void setup() {
  EEPROM.begin(sizeof(Settings));
  loadSettings();
  pinMode(PIN_CTS, INPUT_PULLUP);
  pinMode(PIN_RTS, OUTPUT);
  digitalWrite(PIN_RTS, LOW);
  Serial.setRxBufferSize(RX_BUFFER);
  Serial.begin(cfg.baud);
  currentBaud = cfg.baud;
#if RETROSSH_SSH
  libssh_begin();
#endif
  WiFi.persistent(false);  // credentials live only in our settings (AT&W / AT&F)
  WiFi.mode(WIFI_STA);
  if (cfg.ssid[0]) WiFi.begin(cfg.ssid, cfg.pass[0] ? cfg.pass : nullptr);
  reply(FW_VERSION " READY");
}

void loop() {
  updateRts();
  if (mode == MODE_COMMAND) commandInput();
  else pumpOnline();
  if (linkType != LINK_NONE && !linkConnected()) {
    hangup();
    reply("NO CARRIER");
  }
}
