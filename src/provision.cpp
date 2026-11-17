// Wi-Fi setup page. See provision.h.
#include "provision.h"

#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include <utility>

static Preferences store;
static WebServer *server = nullptr;
static DNSServer *dns = nullptr;
static bool submitted = false;
static SavedNet pending;
static String note;
static String scanned[16];
static uint8_t scannedCount = 0;

// ---------------- Saved networks (NVS "wifi") ----------------

uint8_t provLoad(SavedNet *out, uint8_t max) {
  store.begin("wifi", true);
  uint8_t n = store.getUChar("n", 0);
  if (n > MAX_SAVED) n = MAX_SAVED;
  if (n > max) n = max;
  uint8_t got = 0;
  for (uint8_t i = 0; i < n; i++) {
    char k[4];
    snprintf(k, sizeof(k), "s%u", i);
    store.getString(k, out[got].ssid, sizeof(out[got].ssid));
    snprintf(k, sizeof(k), "p%u", i);
    store.getString(k, out[got].pass, sizeof(out[got].pass));
    if (out[got].ssid[0]) got++;
  }
  store.end();
  return got;
}

static void writeAll(const SavedNet *list, uint8_t n) {
  store.begin("wifi", false);
  store.clear();
  for (uint8_t i = 0; i < n; i++) {
    char k[4];
    snprintf(k, sizeof(k), "s%u", i);
    store.putString(k, list[i].ssid);
    snprintf(k, sizeof(k), "p%u", i);
    store.putString(k, list[i].pass);
  }
  store.putUChar("n", n);
  store.end();
}

void provSave(const SavedNet &n) {
  SavedNet list[MAX_SAVED + 1];
  list[0] = n;
  SavedNet old[MAX_SAVED];
  const uint8_t m = provLoad(old, MAX_SAVED);
  uint8_t k = 1;
  for (uint8_t i = 0; i < m && k < MAX_SAVED; i++)
    if (strcmp(old[i].ssid, n.ssid) != 0) list[k++] = old[i];
  writeAll(list, k);
}

void provForgetAll() { writeAll(nullptr, 0); }

// ---------------- Web page ----------------

static String esc(const String &s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

static void scan() {
  scannedCount = 0;
  const int n = WiFi.scanNetworks(false, false);
  // Strongest first, names only once.
  int order[64];
  const int m = n > 64 ? 64 : (n < 0 ? 0 : n);
  for (int i = 0; i < m; i++) order[i] = i;
  for (int i = 0; i < m; i++)
    for (int j = i + 1; j < m; j++)
      if (WiFi.RSSI(order[j]) > WiFi.RSSI(order[i])) std::swap(order[i], order[j]);
  for (int i = 0; i < m && scannedCount < 16; i++) {
    const String s = WiFi.SSID(order[i]);
    if (!s.length()) continue;
    bool dup = false;
    for (uint8_t j = 0; j < scannedCount; j++) dup |= scanned[j] == s;
    if (!dup) scanned[scannedCount++] = s;
  }
  WiFi.scanDelete();
}

static const char PAGE_HEAD[] PROGMEM =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>tinytram Wi-Fi</title><style>"
    "body{font:16px system-ui,sans-serif;background:#000;color:#eee;margin:0;padding:24px;max-width:420px}"
    "h1{font-size:20px;margin:0 0 16px}label.n{display:block;padding:12px;border:1px solid #333;margin:6px 0}"
    "input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:12px;font-size:16px;"
    "background:#111;color:#eee;border:1px solid #444;margin:6px 0 14px}"
    "button{padding:12px 18px;font-size:16px;background:#eee;color:#000;border:0}"
    "a,.s{color:#999}.e{color:#f0b03c;margin:0 0 14px}form.f{margin-top:32px}"
    "form.f button{background:#222;color:#bbb}</style></head><body>";

static void handleRoot() {
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>Wi-Fi for tinytram</h1>";
  if (note.length()) h += "<p class=e>" + esc(note) + "</p>";
  h += "<form method=post action=/save><p class=s>Choose your network. "
       "tinytram only sees 2.4&nbsp;GHz networks.</p>";
  for (uint8_t i = 0; i < scannedCount; i++) {
    const String e = esc(scanned[i]);
    h += "<label class=n><input type=radio name=ssid value=\"" + e + "\"";
    if (i == 0) h += " checked";
    h += "> " + e + "</label>";
  }
  h += "<p class=s>Not in the list? Type its name:</p>"
       "<input type=text name=other maxlength=32 autocapitalize=off autocorrect=off>"
       "<p class=s>Password</p>"
       "<input type=password name=pass maxlength=63>"
       "<button>Join</button> &nbsp; <a href=/scan>Scan again</a></form>"
       "<form class=f method=post action=/forget>"
       "<button>Forget saved networks</button></form></body></html>";
  server->send(200, "text/html", h);
}

static void sendMessage(const char *msg) {
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>tinytram</h1><p>";
  h += msg;
  h += "</p></body></html>";
  server->send(200, "text/html", h);
}

static void handleSave() {
  String ssid = server->arg("other");
  ssid.trim();
  if (!ssid.length()) ssid = server->arg("ssid");
  const String pass = server->arg("pass");
  // Validate at the boundary: 802.11 limits.
  if (ssid.length() < 1 || ssid.length() > 32) {
    note = "Choose a network or type its name.";
    return handleRoot();
  }
  if (pass.length() != 0 && (pass.length() < 8 || pass.length() > 63)) {
    note = "A Wi-Fi password is 8 to 63 characters.";
    return handleRoot();
  }
  strlcpy(pending.ssid, ssid.c_str(), sizeof(pending.ssid));
  strlcpy(pending.pass, pass.c_str(), sizeof(pending.pass));
  submitted = true;
  sendMessage("tinytram is joining your network now. Look at its screen. "
              "If this page stops responding, that is normal: your phone left "
              "the tinytram hotspot.");
}

static void handleForget() {
  provForgetAll();
  note = "Saved networks forgotten.";
  handleRoot();
}

static void handleScan() {
  scan();
  server->sendHeader("Location", "/");
  server->send(302, "text/plain", "");
}

// Every other address (phones probe for captive portals) lands on the page.
static void handleOther() {
  server->sendHeader("Location", "http://192.168.4.1/");
  server->send(302, "text/plain", "");
}

void provStart(const char *apName, const char *apPass) {
  scan();
  WiFi.softAP(apName, apPass);
  delay(100);
  dns = new DNSServer();
  dns->setErrorReplyCode(DNSReplyCode::NoError);
  dns->start(53, "*", WiFi.softAPIP());
  server = new WebServer(80);
  server->on("/", HTTP_GET, handleRoot);
  server->on("/save", HTTP_POST, handleSave);
  server->on("/forget", HTTP_POST, handleForget);
  server->on("/scan", HTTP_GET, handleScan);
  server->onNotFound(handleOther);
  server->begin();
  submitted = false;
}

void provSetNote(const char *n) { note = n ? n : ""; }

bool provLoop(SavedNet *out) {
  if (!server) return false;
  dns->processNextRequest();
  server->handleClient();
  if (!submitted) return false;
  submitted = false;
  *out = pending;
  return true;
}

void provStop() {
  if (server) { server->stop(); delete server; server = nullptr; }
  if (dns) { dns->stop(); delete dns; dns = nullptr; }
  WiFi.softAPdisconnect(true);
  note = "";
}
