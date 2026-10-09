#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

constexpr int CX = 233, CY = 233, R = 214;
constexpr int MAXP = 128, TRAIL = 14;
constexpr float RANGES[] = {5, 10, 25, 50, 100};
constexpr int NRANGES = sizeof(RANGES) / sizeof(RANGES[0]);
constexpr uint32_t FETCH_MS = 10000;

struct Plane {
  char hex[8];
  char flight[10];
  char type[6];
  char reg[10];
  double lat, lon;
  int alt;
  bool ground;
  float gs, track;
  int squawk;
  uint32_t fixMs;
  float tlat[TRAIL], tlon[TRAIL];
  int tn;
  bool near;
  float sx, sy, dist, brg;
};

Plane planes[MAXP];
Plane incoming[MAXP];
int nplanes = 0;
SemaphoreHandle_t lock;
M5Canvas canvas(&M5.Display);

double homeLat = NAN, homeLon = NAN;
char homeName[40] = "";
volatile int rangeIdx = 2;
volatile bool fetchNow = true;
volatile int alertSeq = 0;
volatile bool emergency = false;
char selHex[8] = "";
char status[40] = "";
uint32_t lastOk = 0;
uint32_t backoffMs = 0;
volatile int provider = 0;
int limitedStreak = 0;
const char *PROVIDERS[] = {"adsb.lol", "adsb.fi"};
uint32_t vibeUntil = 0;
int seenAlert = 0;

uint16_t C_BG, C_RING, C_RING_DIM, C_TEXT, C_DIM, C_SEL;

void vibe(uint8_t level, uint32_t ms) {
  M5.Power.setVibration(level);
  vibeUntil = millis() + ms;
}

void deadReckon(const Plane &p, uint32_t now, double &lat, double &lon) {
  float dt = (int32_t)(now - p.fixMs) / 1000.0f;
  if (dt < 0) dt = 0;
  if (dt > 45) dt = 45;
  if (p.ground || p.gs < 1) { lat = p.lat; lon = p.lon; return; }
  float d = p.gs * dt / 3600.0f;
  float t = p.track * DEG_TO_RAD;
  lat = p.lat + d * cosf(t) / 60.0;
  lon = p.lon + d * sinf(t) / (60.0 * cos(p.lat * DEG_TO_RAD));
}

void project(double lat, double lon, float &dxNm, float &dyNm) {
  dxNm = (lon - homeLon) * 60.0 * cos(homeLat * DEG_TO_RAD);
  dyNm = (lat - homeLat) * 60.0;
}

bool wifiLocate() {
  int n = WiFi.scanNetworks(false, true);
  if (n < 2) { WiFi.scanDelete(); return false; }
  JsonDocument req;
  req["considerIp"] = false;
  JsonArray aps = req["wifiAccessPoints"].to<JsonArray>();
  for (int i = 0; i < n && i < 30; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.endsWith("_nomap")) continue;
    JsonObject ap = aps.add<JsonObject>();
    ap["macAddress"] = WiFi.BSSIDstr(i);
    ap["signalStrength"] = WiFi.RSSI(i);
    ap["channel"] = WiFi.channel(i);
  }
  WiFi.scanDelete();
  String body;
  serializeJson(req, body);

  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  http.setUserAgent("flight-radar-badge/1.0");
  http.setTimeout(8000);
  if (!http.begin(cli, "https://api.beacondb.net/v1/geolocate")) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  if (code != 200) { http.end(); return false; }
  JsonDocument d;
  DeserializationError err = deserializeJson(d, http.getString());
  http.end();
  if (err || !d["location"]["lat"].is<double>()) return false;
  float acc = d["accuracy"] | 99999.0f;
  if (acc > 3000) return false;
  homeLat = d["location"]["lat"];
  homeLon = d["location"]["lng"];
  snprintf(homeName, sizeof homeName, "wifi fix \xB1%.0fm", acc);
  return true;
}

bool ipLocate() {
  HTTPClient http;
  http.setTimeout(6000);
  if (!http.begin("http://ip-api.com/json/?fields=status,lat,lon,city")) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  JsonDocument d;
  DeserializationError err = deserializeJson(d, http.getString());
  http.end();
  if (err || d["status"] != "success") return false;
  homeLat = d["lat"];
  homeLon = d["lon"];
  snprintf(homeName, sizeof homeName, "%s (approx, IP)", (const char *)(d["city"] | "?"));
  return true;
}

bool geolocate() {
#ifdef HOME_LAT
  homeLat = HOME_LAT;
  homeLon = HOME_LON;
  strlcpy(homeName, "HOME", sizeof homeName);
  return true;
#else
  strlcpy(status, "scanning wifi for location", sizeof status);
  if (wifiLocate()) return true;
  return ipLocate();
#endif
}

void fetchPlanes() {
  WiFiClientSecure cli;
  cli.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setUserAgent("flight-radar-badge/1.0");
  http.setTimeout(9000);
  int radius = (int)ceilf(RANGES[rangeIdx] * 1.2f);
  if (radius > 250) radius = 250;
  char url[128];
  if (provider == 0) snprintf(url, sizeof url, "https://api.adsb.lol/v2/point/%.4f/%.4f/%d", homeLat, homeLon, radius);
  else snprintf(url, sizeof url, "https://opendata.adsb.fi/api/v2/lat/%.4f/lon/%.4f/dist/%d", homeLat, homeLon, radius);
  if (!http.begin(cli, url)) { strlcpy(status, "connect failed", sizeof status); return; }
  int code = http.GET();
  if (code == 429) {
    http.end();
    provider = (provider + 1) % 2;
    if (++limitedStreak < 2) { fetchNow = true; return; }
    backoffMs = backoffMs ? min<uint32_t>(backoffMs * 2, 240000) : 30000;
    snprintf(status, sizeof status, "rate limited, retry in %us", (unsigned)(backoffMs / 1000));
    return;
  }
  if (code != 200) {
    snprintf(status, sizeof status, "feed HTTP %d", code);
    http.end();
    return;
  }

  JsonDocument filter;
  const char *key = provider == 0 ? "ac" : "aircraft";
  JsonObject f = filter[key][0].to<JsonObject>();
  for (const char *k : {"hex", "flight", "t", "r", "lat", "lon", "alt_baro", "gs", "track", "squawk", "seen_pos"}) f[k] = true;

  int len = http.getSize();
  size_t cap = len > 0 ? len : 512 * 1024;
  char *body = (char *)ps_malloc(cap + 1);
  if (!body) { strlcpy(status, "out of memory", sizeof status); http.end(); return; }
  WiFiClient *s = http.getStreamPtr();
  size_t got = 0;
  uint32_t idle = millis();
  while (got < cap && (s->connected() || s->available())) {
    int avail = s->available();
    if (avail > 0) {
      got += s->read((uint8_t *)body + got, min((size_t)avail, cap - got));
      idle = millis();
    } else if (millis() - idle > 8000) {
      break;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  http.end();
  body[got] = 0;
  if (len > 0 && got < (size_t)len) {
    snprintf(status, sizeof status, "short read %u/%d", (unsigned)got, len);
    free(body);
    return;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, got, DeserializationOption::Filter(filter));
  free(body);
  if (err) { snprintf(status, sizeof status, "parse %s", err.c_str()); return; }

  uint32_t now = millis();
  float nearNm = RANGES[rangeIdx] * 0.25f;
  int n = 0;
  bool emerg = false;
  int newAlerts = 0;

  xSemaphoreTake(lock, portMAX_DELAY);
  for (JsonObject a : doc[key].as<JsonArray>()) {
    if (n >= MAXP) break;
    if (!a["lat"].is<double>() || !a["lon"].is<double>()) continue;
    Plane &p = incoming[n];
    memset(&p, 0, sizeof p);
    strlcpy(p.hex, a["hex"] | "", sizeof p.hex);
    const char *fl = a["flight"] | "";
    strlcpy(p.flight, fl, sizeof p.flight);
    for (int i = strlen(p.flight) - 1; i >= 0 && p.flight[i] == ' '; i--) p.flight[i] = 0;
    strlcpy(p.type, a["t"] | "", sizeof p.type);
    strlcpy(p.reg, a["r"] | "", sizeof p.reg);
    p.lat = a["lat"];
    p.lon = a["lon"];
    if (a["alt_baro"].is<int>()) { p.alt = a["alt_baro"]; p.ground = false; }
    else { p.alt = 0; p.ground = true; }
    p.gs = a["gs"] | 0.0f;
    p.track = a["track"] | 0.0f;
    p.squawk = atoi(a["squawk"] | "0");
    float seen = a["seen_pos"] | 0.0f;
    p.fixMs = now - (uint32_t)(seen * 1000);

    float dx, dy;
    project(p.lat, p.lon, dx, dy);
    float dist = sqrtf(dx * dx + dy * dy);

    const Plane *old = nullptr;
    for (int i = 0; i < nplanes; i++) if (!strcmp(planes[i].hex, p.hex)) { old = &planes[i]; break; }
    if (old) {
      memcpy(p.tlat, old->tlat, sizeof p.tlat);
      memcpy(p.tlon, old->tlon, sizeof p.tlon);
      p.tn = old->tn;
      if (fabs(old->lat - p.lat) > 1e-5 || fabs(old->lon - p.lon) > 1e-5) {
        if (p.tn == TRAIL) {
          memmove(p.tlat, p.tlat + 1, sizeof(float) * (TRAIL - 1));
          memmove(p.tlon, p.tlon + 1, sizeof(float) * (TRAIL - 1));
          p.tn--;
        }
        p.tlat[p.tn] = old->lat;
        p.tlon[p.tn] = old->lon;
        p.tn++;
      }
      p.near = old->near;
    }
    bool isNear = !p.ground && dist < nearNm;
    if (isNear && !p.near && old) newAlerts++;
    p.near = isNear;
    if (p.squawk == 7500 || p.squawk == 7600 || p.squawk == 7700) emerg = true;
    n++;
  }
  memcpy(planes, incoming, sizeof(Plane) * n);
  nplanes = n;
  emergency = emerg;
  if (newAlerts) alertSeq++;
  lastOk = millis();
  backoffMs = 0;
  limitedStreak = 0;
  status[0] = 0;
  xSemaphoreGive(lock);
}

void netTask(void *) {
  uint32_t last = 0;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      strlcpy(status, "wifi lost, retrying", sizeof status);
      WiFi.reconnect();
      vTaskDelay(pdMS_TO_TICKS(3000));
      continue;
    }
    if (isnan(homeLat)) {
      if (!geolocate()) { strlcpy(status, "locating...", sizeof status); vTaskDelay(pdMS_TO_TICKS(3000)); continue; }
      fetchNow = true;
    }
    if ((fetchNow && !backoffMs) || millis() - last > FETCH_MS + backoffMs) {
      fetchNow = false;
      last = millis();
      fetchPlanes();
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

uint16_t altColor(const Plane &p) {
  if (p.ground) return canvas.color565(120, 120, 120);
  if (p.alt < 3000) return canvas.color565(255, 110, 60);
  if (p.alt < 10000) return canvas.color565(255, 200, 40);
  if (p.alt < 25000) return canvas.color565(120, 255, 120);
  if (p.alt < 35000) return canvas.color565(60, 210, 255);
  return canvas.color565(190, 130, 255);
}

uint16_t dim(uint16_t c, float k) {
  uint8_t r = ((c >> 11) & 0x1F) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
  return canvas.color565(r * k, g * k, b * k);
}

bool isEmergency(const Plane &p) { return p.squawk == 7500 || p.squawk == 7600 || p.squawk == 7700; }

void drawPlaneIcon(float x, float y, float trk, uint16_t col, float s) {
  float t = trk * DEG_TO_RAD;
  auto pt = [&](float ang, float len, float &ox, float &oy) { ox = x + sinf(t + ang) * len; oy = y - cosf(t + ang) * len; };
  float nx, ny, lx, ly, rx, ry, bx, by;
  pt(0, 9 * s, nx, ny);
  pt(2.4f, 7 * s, lx, ly);
  pt(-2.4f, 7 * s, rx, ry);
  pt(PI, 3 * s, bx, by);
  canvas.fillTriangle(nx, ny, lx, ly, bx, by, col);
  canvas.fillTriangle(nx, ny, rx, ry, bx, by, col);
}

void drawScope(float sweep) {
  canvas.fillScreen(C_BG);
  for (int i = 0; i < 28; i++) {
    float a0 = sweep - i * 2.2f, a1 = a0 - 2.4f;
    float k = 0.55f * (1.0f - i / 28.0f);
    k = k * k;
    uint16_t c = canvas.color565(10 + 40 * k, 30 + 225 * k, 20 + 90 * k);
    float r0 = a0 * DEG_TO_RAD, r1 = a1 * DEG_TO_RAD;
    canvas.fillTriangle(CX, CY, CX + sinf(r0) * R, CY - cosf(r0) * R, CX + sinf(r1) * R, CY - cosf(r1) * R, c);
  }
  for (int i = 1; i <= 4; i++) canvas.drawCircle(CX, CY, R * i / 4, i == 4 ? C_RING : C_RING_DIM);
  canvas.drawCircle(CX, CY, R + 1, C_RING);
  for (int a = 0; a < 360; a += 10) {
    float r = a * DEG_TO_RAD;
    int len = (a % 30 == 0) ? 10 : 4;
    canvas.drawLine(CX + sinf(r) * (R - len), CY - cosf(r) * (R - len), CX + sinf(r) * R, CY - cosf(r) * R, C_RING);
  }
  canvas.drawFastHLine(CX - R, CY, 2 * R, C_RING_DIM);
  canvas.drawFastVLine(CX, CY - R, 2 * R, C_RING_DIM);
  float sr = sweep * DEG_TO_RAD;
  canvas.drawWideLine(CX, CY, CX + sinf(sr) * R, CY - cosf(sr) * R, 2, canvas.color565(140, 255, 170));

  canvas.setFont(&fonts::Font2);
  canvas.setTextDatum(middle_center);
  canvas.setTextColor(C_DIM);
  char buf[16];
  for (int i = 1; i <= 3; i++) {
    float v = RANGES[rangeIdx] * i / 4;
    snprintf(buf, sizeof buf, v < 10 ? "%.1f" : "%.0f", v);
    canvas.drawString(buf, CX + R * i / 4 * 0.707f + 12, CY + R * i / 4 * 0.707f);
  }
  canvas.setTextColor(C_TEXT);
  canvas.setFont(&fonts::FreeSansBold9pt7b);
  canvas.drawString("N", CX, CY - R + 22);
  canvas.setTextColor(C_DIM);
  canvas.setFont(&fonts::Font2);
  canvas.drawString("E", CX + R - 20, CY);
  canvas.drawString("S", CX, CY + R - 20);
  canvas.drawString("W", CX - R + 20, CY);
  canvas.fillCircle(CX, CY, 4, C_TEXT);
  canvas.drawCircle(CX, CY, 8, C_RING);
}

void drawHud(int count) {
  char buf[48];
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextColor(C_TEXT);
  snprintf(buf, sizeof buf, "%g NM", RANGES[rangeIdx]);
  canvas.drawString(buf, CX, 62);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(C_DIM);
  snprintf(buf, sizeof buf, "%d aircraft  %s  %d%%", count, PROVIDERS[provider], M5.Power.getBatteryLevel());
  canvas.drawString(buf, CX, 86);
  if (status[0]) {
    canvas.setTextColor(canvas.color565(255, 120, 80));
    canvas.drawString(status, CX, 104);
  } else if (lastOk && millis() - lastOk > 20000) {
    canvas.setTextColor(canvas.color565(255, 120, 80));
    canvas.drawString("feed stale", CX, 104);
  }
}

void drawCard(const Plane &p, float dist, float brg) {
  int w = 270, h = 92, x = CX - w / 2, y = 318;
  canvas.fillRoundRect(x, y, w, h, 14, canvas.color565(8, 22, 14));
  canvas.drawRoundRect(x, y, w, h, 14, altColor(p));
  canvas.setTextDatum(top_left);
  canvas.setFont(&fonts::FreeSansBold12pt7b);
  canvas.setTextColor(altColor(p));
  canvas.drawString(p.flight[0] ? p.flight : p.hex, x + 14, y + 8);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(C_DIM);
  canvas.setTextDatum(top_right);
  char buf[48];
  snprintf(buf, sizeof buf, "%s %s", p.type, p.reg);
  canvas.drawString(buf, x + w - 14, y + 12);
  canvas.setTextDatum(top_left);
  canvas.setTextColor(C_TEXT);
  if (p.ground) snprintf(buf, sizeof buf, "GROUND   %.0f kt", p.gs);
  else snprintf(buf, sizeof buf, "%d ft   %.0f kt   %03.0f\xB0", p.alt, p.gs, p.track);
  canvas.drawString(buf, x + 14, y + 40);
  canvas.setTextColor(C_DIM);
  snprintf(buf, sizeof buf, "%.1f nm  bearing %03.0f", dist, brg);
  if (p.squawk) snprintf(buf + strlen(buf), sizeof buf - strlen(buf), "  sq %04d", p.squawk);
  canvas.drawString(buf, x + 14, y + 62);
}

int sortedIdx[MAXP];

void render() {
  static Plane snap[MAXP];
  xSemaphoreTake(lock, portMAX_DELAY);
  int n = nplanes;
  memcpy(snap, planes, sizeof(Plane) * n);
  xSemaphoreGive(lock);

  uint32_t now = millis();
  float sweep = fmodf(now * 0.09f, 360.0f);
  float scale = R / RANGES[rangeIdx];
  drawScope(sweep);

  if (isnan(homeLat)) {
    canvas.setFont(&fonts::FreeSansBold12pt7b);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(C_TEXT);
    canvas.drawString(status[0] ? status : "locating...", CX, CY + 60);
    canvas.pushSprite(0, 0);
    return;
  }

  int visible = 0;
  Plane *sel = nullptr;
  bool flash = (now / 300) % 2;
  for (int i = 0; i < n; i++) {
    Plane &p = snap[i];
    double lat, lon;
    deadReckon(p, now, lat, lon);
    float dx, dy;
    project(lat, lon, dx, dy);
    p.dist = sqrtf(dx * dx + dy * dy);
    p.brg = fmodf(atan2f(dx, dy) * RAD_TO_DEG + 360, 360);
    p.sx = CX + dx * scale;
    p.sy = CY - dy * scale;
    if (p.dist > RANGES[rangeIdx]) continue;
    sortedIdx[visible++] = i;
    if (selHex[0] && !strcmp(p.hex, selHex)) sel = &p;
  }
  std::sort(sortedIdx, sortedIdx + visible, [&](int a, int b) { return snap[a].dist < snap[b].dist; });

  for (int k = visible - 1; k >= 0; k--) {
    Plane &p = snap[sortedIdx[k]];
    uint16_t col = altColor(p);
    float lag = fmodf(sweep - p.brg + 360, 360);
    float glow = lag < 60 ? 1.0f - lag / 60.0f * 0.45f : 0.55f;
    if (&p == sel) glow = 1.0f;

    float px = p.sx, py = p.sy;
    for (int t = p.tn - 1; t >= 0; t--) {
      float dx, dy;
      project(p.tlat[t], p.tlon[t], dx, dy);
      float tx = CX + dx * scale, ty = CY - dy * scale;
      float fade = 0.15f + 0.5f * (t + 1) / (float)p.tn;
      canvas.drawLine(px, py, tx, ty, dim(col, fade * glow));
      px = tx;
      py = ty;
    }

    uint16_t c = dim(col, glow);
    if (isEmergency(p)) c = flash ? canvas.color565(255, 40, 40) : canvas.color565(255, 255, 255);
    if (p.ground) canvas.fillCircle(p.sx, p.sy, 3, c);
    else drawPlaneIcon(p.sx, p.sy, p.track, c, &p == sel ? 1.4f : 1.0f);
    if (lag < 8) canvas.drawCircle(p.sx, p.sy, 10 + lag, dim(col, 0.5f));

    if (k < 8 || &p == sel) {
      canvas.setFont(&fonts::Font0);
      canvas.setTextDatum(top_left);
      canvas.setTextColor(dim(col, glow * 0.95f));
      char lbl[24];
      if (p.ground) snprintf(lbl, sizeof lbl, "%s", p.flight[0] ? p.flight : p.hex);
      else snprintf(lbl, sizeof lbl, "%s %03d", p.flight[0] ? p.flight : p.hex, p.alt / 100);
      canvas.drawString(lbl, p.sx + 10, p.sy - 4);
    }
  }

  if (sel) {
    canvas.drawCircle(sel->sx, sel->sy, 16, C_SEL);
    canvas.drawCircle(sel->sx, sel->sy, 17, C_SEL);
    for (int d = 0; d < 1000; d += 12) {
      float k0 = d / 1000.0f, k1 = (d + 6) / 1000.0f;
      canvas.drawLine(CX + (sel->sx - CX) * k0, CY + (sel->sy - CY) * k0, CX + (sel->sx - CX) * k1, CY + (sel->sy - CY) * k1, dim(C_SEL, 0.6f));
    }
    drawCard(*sel, sel->dist, sel->brg);
  } else {
    if (selHex[0]) selHex[0] = 0;
    canvas.setFont(&fonts::Font2);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(C_DIM);
    canvas.drawString(homeName, CX, 372);
    canvas.drawString("A range   B next   tap select", CX, 392);
  }

  if (emergency && flash) {
    canvas.drawCircle(CX, CY, R + 4, canvas.color565(255, 40, 40));
    canvas.drawCircle(CX, CY, R + 5, canvas.color565(255, 40, 40));
  }

  drawHud(visible);
  canvas.pushSprite(0, 0);

  if (M5.BtnB.wasPressed() && visible) {
    int cur = -1;
    for (int k = 0; k < visible; k++) if (!strcmp(snap[sortedIdx[k]].hex, selHex)) cur = k;
    int nxt = (cur + 1) % visible;
    strlcpy(selHex, snap[sortedIdx[nxt]].hex, sizeof selHex);
    vibe(120, 25);
  }
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    float best = 36 * 36;
    int pick = -1;
    for (int k = 0; k < visible; k++) {
      Plane &p = snap[sortedIdx[k]];
      float d = (p.sx - t.x) * (p.sx - t.x) + (p.sy - t.y) * (p.sy - t.y);
      if (d < best) { best = d; pick = k; }
    }
    if (pick >= 0) { strlcpy(selHex, snap[sortedIdx[pick]].hex, sizeof selHex); vibe(120, 25); }
    else selHex[0] = 0;
  }
}

void bootScreen(const char *msg, float spin) {
  canvas.fillScreen(C_BG);
  for (int i = 1; i <= 4; i++) canvas.drawCircle(CX, CY, R * i / 4, C_RING_DIM);
  float r = spin * DEG_TO_RAD;
  canvas.drawWideLine(CX, CY, CX + sinf(r) * R, CY - cosf(r) * R, 2, canvas.color565(140, 255, 170));
  canvas.setTextDatum(middle_center);
  canvas.setFont(&fonts::FreeSansBold18pt7b);
  canvas.setTextColor(C_TEXT);
  canvas.drawString("FLIGHT RADAR", CX, CY - 30);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(C_DIM);
  canvas.drawString(msg, CX, CY + 20);
  canvas.pushSprite(0, 0);
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  M5.Display.setBrightness(200);
  canvas.setPsram(true);
  canvas.setColorDepth(16);
  canvas.createSprite(466, 466);

  C_BG = canvas.color565(2, 10, 6);
  C_RING = canvas.color565(30, 120, 70);
  C_RING_DIM = canvas.color565(14, 55, 32);
  C_TEXT = canvas.color565(200, 255, 215);
  C_DIM = canvas.color565(90, 150, 110);
  C_SEL = canvas.color565(255, 255, 255);

  lock = xSemaphoreCreateMutex();
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    char msg[64];
    if (millis() - t0 > 20000) snprintf(msg, sizeof msg, "can't join \"%s\", check secrets.h", WIFI_SSID);
    else snprintf(msg, sizeof msg, "joining %s", WIFI_SSID);
    bootScreen(msg, fmodf(millis() * 0.2f, 360));
    delay(16);
  }
  vibe(160, 60);
  xTaskCreatePinnedToCore(netTask, "net", 16384, nullptr, 1, nullptr, 0);
}

void loop() {
  M5.update();
  if (vibeUntil && (int32_t)(millis() - vibeUntil) > 0) { M5.Power.setVibration(0); vibeUntil = 0; }
  if (M5.BtnA.wasPressed()) {
    rangeIdx = (rangeIdx + 1) % NRANGES;
    fetchNow = true;
    vibe(120, 25);
  }
  if (alertSeq != seenAlert) {
    seenAlert = alertSeq;
    vibe(200, 120);
  }
  render();
}
