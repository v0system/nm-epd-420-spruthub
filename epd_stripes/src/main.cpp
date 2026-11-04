// NM-EPD-420 — Weather Today + SprutHub MQTT + web config
#include <Arduino.h>
SET_LOOP_TASK_STACK_SIZE(16 * 1024);  // Wx/MQTT иначе рвут canary 8KB
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <Preferences.h>
#include <SPI.h>
#include <time.h>
#include <GxEPD2_3C.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <math.h>

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#ifndef DEF_FOOTER_TAG
#define DEF_FOOTER_TAG ""
#endif

static String g_status;

static const char *FW_VERSION = "v0s13";
static const int PIN_BOOT = 0;  // зажать BOOT при старте → SoftAP setup

static const int PIN_SCK = 2, PIN_MOSI = 1, PIN_CS = 46, PIN_DC = 4, PIN_RST = 5, PIN_BUSY = 6;

// дефолты из secrets.h — только для пустой NVS; дальше правится в портале
struct AppCfg {
  char wifiSsid[33] = DEF_WIFI_SSID;
  char wifiPass[65] = DEF_WIFI_PASS;
  char mqttHost[40] = DEF_MQTT_HOST;
  uint16_t mqttPort = DEF_MQTT_PORT;
  char mqttUser[24] = DEF_MQTT_USER;
  char mqttPass[32] = DEF_MQTT_PASS;
  // префикс моста — нельзя подписаться на весь # (HA discovery рвёт буфер)
  char mqttPrefix[48] = DEF_MQTT_PREFIX;
  char city[40] = "Moscow";  // wttr.in/<city>

  uint16_t sleepMin = 30;     // интервал обновления
  uint16_t cfgListenSec = 90; // портал после отрисовки (от батареи); 0 = только через BOOT
  bool nightOn = true;        // ночью не будить, спать до nightTo:00
  uint8_t nightFrom = 0, nightTo = 6;
  int16_t tzMin = 180;        // UTC+3
  bool mains = false;         // от сети: без deep sleep, портал доступен весь интервал
  char apPass[33] = "setup1234";
  char dns1[16] = "1.1.1.1";  // 8.8.8.8:53 в домашней сети не отвечает
  char dns2[16] = "8.8.8.8";
  uint8_t rainPct = 40;       // с какой вероятности час в «скоро» помечается осадками
  // блоки внизу экрана: строки "d|aid|порядок|вид|имя" (d=темп. в доме, c=климат)
  // вид: 0 авто, 1 скрыт, 2 добавлен вручную
  char layout[1024] = "";
  char titleHome[40] = "Температура в доме";
  char titleClim[32] = "КЛИМАТ";
  bool homeShowH = true;
  bool climCurrent = false;  // КЛИМАТ: текущая T вместо уставки
};
static AppCfg g_cfg;
static Preferences g_prefs;

static GxEPD2_3C<GxEPD2_420c_GDEY042Z98, GxEPD2_420c_GDEY042Z98::HEIGHT / 4> display(
    GxEPD2_420c_GDEY042Z98(PIN_CS, PIN_DC, PIN_RST, PIN_BUSY));
static U8G2_FOR_ADAFRUIT_GFX u8g2;

struct RoomSnap {
  int aid = -1;
  char name[40] = "";
  float t = NAN, h = NAN;
};

// правый блок КЛИМАТ: тёплый пол / кондей
struct CtrlSnap {
  int aid = -1;
  char name[40] = "";
  char kind[8] = "";  // "пол" / "AC"
  float t = NAN, target = NAN;
  bool on = false;
  bool hasOn = false;
};

struct SoonH {
  char hm[6] = "";   // "15:00"
  float t = NAN;
  char note[32] = "";  // кратко: ясно / дождь 40%
};

struct Wx {
  bool ok = false;
  float temp = NAN, feels = NAN, wind = NAN, tmin = NAN, tmax = NAN;
  int humidity = -1, pressure = -1, uv = -1, uvMax = -1, code = -1;
  int precipProb = -1;  // next hour %, -1 = нет данных
  float precipMm = NAN;
  char precipLine[16] = "";
  char soonLead[40] = "";  // "дождь через 2ч" / "без осадков"
  char soonErr[28] = "";   // отладка на экране если пусто
  SoonH soon[3];
  int soonN = 0;
  int rssi = 0;
  char windDir[8] = "";
  char sunrise[8] = "--:--";
  char sunset[8] = "--:--";
  char desc[48] = "";
  char updated[24] = "";
  char dateLine[32] = "";
  char ip[16] = "";
  RoomSnap rooms[8];
  int roomN = 0;
  CtrlSnap ctrls[8];
  int ctrlN = 0;
};

static const char *DOW_RU[] = {"Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};

static void saveCfg();

static void loadCfg() {
  // read-only begin падает, если namespace ещё не создан
  if (!g_prefs.begin("epd", true)) {
    saveCfg();
    return;
  }
  const bool seeded = g_prefs.isKey("wifiSsid");
  if (seeded) {
    g_prefs.getString("wifiSsid", g_cfg.wifiSsid, sizeof(g_cfg.wifiSsid));
    if (g_prefs.isKey("wifiPass"))
      g_prefs.getString("wifiPass", g_cfg.wifiPass, sizeof(g_cfg.wifiPass));
  }
  if (g_prefs.isKey("mqttHost"))
    g_prefs.getString("mqttHost", g_cfg.mqttHost, sizeof(g_cfg.mqttHost));
  g_cfg.mqttPort = g_prefs.getUShort("mqttPort", g_cfg.mqttPort);
  if (g_prefs.isKey("mqttUser"))
    g_prefs.getString("mqttUser", g_cfg.mqttUser, sizeof(g_cfg.mqttUser));
  if (g_prefs.isKey("mqttPass"))
    g_prefs.getString("mqttPass", g_cfg.mqttPass, sizeof(g_cfg.mqttPass));
  if (g_prefs.isKey("mqttPrefix"))
    g_prefs.getString("mqttPrefix", g_cfg.mqttPrefix, sizeof(g_cfg.mqttPrefix));
  if (g_prefs.isKey("city")) g_prefs.getString("city", g_cfg.city, sizeof(g_cfg.city));
  g_cfg.sleepMin = g_prefs.getUShort("sleepMin", g_cfg.sleepMin);
  g_cfg.cfgListenSec = g_prefs.getUShort("cfgListen", g_cfg.cfgListenSec);
  g_cfg.nightOn = g_prefs.getBool("nightOn", g_cfg.nightOn);
  g_cfg.nightFrom = g_prefs.getUChar("nightFrom", g_cfg.nightFrom);
  g_cfg.nightTo = g_prefs.getUChar("nightTo", g_cfg.nightTo);
  g_cfg.tzMin = g_prefs.getShort("tzMin", g_cfg.tzMin);
  g_cfg.mains = g_prefs.getBool("mains", g_cfg.mains);
  if (g_prefs.isKey("apPass")) g_prefs.getString("apPass", g_cfg.apPass, sizeof(g_cfg.apPass));
  if (g_prefs.isKey("dns1")) g_prefs.getString("dns1", g_cfg.dns1, sizeof(g_cfg.dns1));
  if (g_prefs.isKey("dns2")) g_prefs.getString("dns2", g_cfg.dns2, sizeof(g_cfg.dns2));
  g_cfg.rainPct = g_prefs.getUChar("rainPct", g_cfg.rainPct);
  if (g_prefs.isKey("tHome")) {
    g_prefs.getString("tHome", g_cfg.titleHome, sizeof(g_cfg.titleHome));
    if (strcmp(g_cfg.titleHome, "ДОМ") == 0)  // старый дефолт ≤v0s11
      snprintf(g_cfg.titleHome, sizeof(g_cfg.titleHome), "Температура в доме");
  }
  if (g_prefs.isKey("tClim")) g_prefs.getString("tClim", g_cfg.titleClim, sizeof(g_cfg.titleClim));
  g_cfg.homeShowH = g_prefs.getBool("homeH", g_cfg.homeShowH);
  g_cfg.climCurrent = g_prefs.getBool("climCur", g_cfg.climCurrent);
  if (g_prefs.isKey("layout")) {
    g_prefs.getString("layout", g_cfg.layout, sizeof(g_cfg.layout));
  } else if (g_prefs.isKey("aidNames")) {
    // v0s9..10: "aid=Имя" / "aid=-" → оба блока
    char old[320] = "";
    g_prefs.getString("aidNames", old, sizeof(old));
    String lay;
    for (char *ln = strtok(old, "\n"); ln; ln = strtok(nullptr, "\n")) {
      char *eq = strchr(ln, '=');
      int aid = atoi(ln);
      if (!eq || aid <= 0) continue;
      const bool hide = strcmp(eq + 1, "-") == 0;
      for (const char *b : {"d", "c"}) {
        char row[64];
        snprintf(row, sizeof(row), "%s|%d|0|%d|%s\n", b, aid, hide ? 1 : 0, hide ? "" : eq + 1);
        lay += row;
      }
    }
    snprintf(g_cfg.layout, sizeof(g_cfg.layout), "%s", lay.c_str());
  }
  g_prefs.end();
  // первый boot этой версии: перенести дефолты из secrets.h в NVS
  if (!seeded) saveCfg();
  Serial.printf("cfg wifi=%s mqtt=%s:%u user=%s prefix=%s city=%s%s\n", g_cfg.wifiSsid,
                g_cfg.mqttHost, g_cfg.mqttPort, g_cfg.mqttUser, g_cfg.mqttPrefix, g_cfg.city,
                seeded ? "" : " (seeded)");
  Serial.printf("cfg sleep=%u listen=%u night=%d %u-%u tz=%d mains=%d rain=%u dns=%s,%s\n",
                g_cfg.sleepMin, g_cfg.cfgListenSec, g_cfg.nightOn, g_cfg.nightFrom, g_cfg.nightTo,
                g_cfg.tzMin, g_cfg.mains, g_cfg.rainPct, g_cfg.dns1, g_cfg.dns2);
}

static void saveCfg() {
  if (!g_prefs.begin("epd", false)) return;
  g_prefs.putString("wifiSsid", g_cfg.wifiSsid);
  g_prefs.putString("wifiPass", g_cfg.wifiPass);
  g_prefs.putString("mqttHost", g_cfg.mqttHost);
  g_prefs.putUShort("mqttPort", g_cfg.mqttPort);
  g_prefs.putString("mqttUser", g_cfg.mqttUser);
  g_prefs.putString("mqttPass", g_cfg.mqttPass);
  g_prefs.putString("mqttPrefix", g_cfg.mqttPrefix);
  g_prefs.putString("city", g_cfg.city);
  g_prefs.putUShort("sleepMin", g_cfg.sleepMin);
  g_prefs.putUShort("cfgListen", g_cfg.cfgListenSec);
  g_prefs.putBool("nightOn", g_cfg.nightOn);
  g_prefs.putUChar("nightFrom", g_cfg.nightFrom);
  g_prefs.putUChar("nightTo", g_cfg.nightTo);
  g_prefs.putShort("tzMin", g_cfg.tzMin);
  g_prefs.putBool("mains", g_cfg.mains);
  g_prefs.putString("apPass", g_cfg.apPass);
  g_prefs.putString("dns1", g_cfg.dns1);
  g_prefs.putString("dns2", g_cfg.dns2);
  g_prefs.putUChar("rainPct", g_cfg.rainPct);
  g_prefs.putString("layout", g_cfg.layout);
  g_prefs.putString("tHome", g_cfg.titleHome);
  g_prefs.putString("tClim", g_cfg.titleClim);
  g_prefs.putBool("homeH", g_cfg.homeShowH);
  g_prefs.putBool("climCur", g_cfg.climCurrent);
  if (g_prefs.isKey("aidNames")) g_prefs.remove("aidNames");
  g_prefs.end();
}

// обрезанная посреди символа UTF-8 строка → убрать хвост
static void utf8CutTail(char *s) {
  size_t n = strlen(s), i = n;
  while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
  if (i == 0) return;
  unsigned char c = (unsigned char)s[i - 1];
  size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
  if (n - (i - 1) < need) s[i - 1] = 0;
}

static void copyUtf8(char *dst, size_t n, const char *src) {
  strncpy(dst, src, n - 1);
  dst[n - 1] = 0;
  utf8CutTail(dst);
}

static String htmlEscape(const char *s) {
  String o;
  for (; s && *s; s++) {
    if (*s == '&') o += "&amp;";
    else if (*s == '<') o += "&lt;";
    else if (*s == '>') o += "&gt;";
    else if (*s == '"') o += "&quot;";
    else o += *s;
  }
  return o;
}

static String urlEncode(const char *s) {
  String o;
  for (; s && *s; s++) {
    unsigned char c = (unsigned char)*s;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
        c == '_' || c == '.' || c == '~')
      o += (char)c;
    else if (c == ' ')
      o += '+';
    else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      o += hex;
    }
  }
  return o;
}

static bool wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  IPAddress d1(1, 1, 1, 1), d2(8, 8, 8, 8);
  d1.fromString(g_cfg.dns1);
  d2.fromString(g_cfg.dns2);
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, d1, d2);
  if (!g_cfg.wifiSsid[0]) return false;  // → SoftAP портал
  WiFi.begin(g_cfg.wifiSsid, g_cfg.wifiPass);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 25000) delay(250);
  if (WiFi.status() != WL_CONNECTED) return false;
  Serial.printf("IP %s RSSI %d DNS=%s,%s\n", WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                d1.toString().c_str(), d2.toString().c_str());
  return true;
}

static void syncTime() {
  configTime((long)g_cfg.tzMin * 60, 0, "pool.ntp.org", "time.google.com");
  struct tm ti;
  for (int i = 0; i < 30; i++)
    if (getLocalTime(&ti, 500)) {
      Serial.println("NTP ok");
      return;
    }
  Serial.println("NTP fail");
}

static void stampNow(Wx &w) {
  struct tm ti;
  if (!getLocalTime(&ti, 200)) {
    strncpy(w.updated, "--", sizeof(w.updated));
    strncpy(w.dateLine, "", sizeof(w.dateLine));
    return;
  }
  strftime(w.updated, sizeof(w.updated), "%d.%m %H:%M", &ti);
  snprintf(w.dateLine, sizeof(w.dateLine), "%s, %02d.%02d", DOW_RU[ti.tm_wday], ti.tm_mday,
           ti.tm_mon + 1);
}

// WWO code → грубая категория для иконки
static int iconKind(int code) {
  if (code == 200 || (code >= 386 && code <= 395)) return 6; // гроза — до дождя/снега
  if (code == 113) return 0;                          // ясно
  if (code == 116) return 1;                          // перем
  if (code == 119 || code == 122) return 2;           // облачно
  if (code == 143 || code == 248 || code == 260) return 3; // туман
  if (code >= 176 && code <= 266) return 4;           // дождь/морось
  if (code >= 281 && code <= 314) return 4;
  if (code >= 353 && code <= 359) return 4;
  if (code >= 179 && code <= 338) return 5;           // снег
  if (code >= 368 && code <= 395) return 5;
  return 2;
}

// --- auto-discovery Sprut (retain): датчики + пол/AC ---
static uint16_t g_mqMsgN = 0;

struct MqttCache {
  char topic[96];
  char val[40];
};
static MqttCache g_mqCache[192];
static int g_mqCacheN = 0;

// сырой aid до классификации в rooms/ctrls
struct AidRec {
  int aid = 0;
  char roomName[32] = "";
  char devName[32] = "";
  char roomTopic[96] = "";
  char tTopic[96] = "";
  char tTopic2[96] = "";  // второй CurrentTemperature (термостаты)
  char hTopic[96] = "";
  char targetTopic[96] = "";
  char onTopic[96] = "";
  char fanTopic[96] = "";
  float t = NAN, t2 = NAN, h = NAN, target = NAN;
  bool on = false;
  bool hasOn = false;
  uint8_t onPrio = 0;  // On=3 Active=2 CurrentHC=1 TargetHC=1
  uint8_t blk = 0;     // BLK_HOME | BLK_CLIM — куда попал в этом опросе
};
static AidRec g_aids[48];
static int g_aidN = 0;

enum { BLK_HOME = 1, BLK_CLIM = 2 };
enum { LAY_AUTO = 0, LAY_HIDE = 1, LAY_ADD = 2 };

struct AidLay {
  int order = 0;  // 0 = не задан → после заданных, по aid
  uint8_t vis = 0;  // LAY_AUTO / LAY_HIDE / LAY_ADD
  char name[40] = "";
};

// g_cfg.layout: "d|91|1|0|Спальня 2" — блок|aid|порядок|скрыт|имя
static bool layGet(char blk, int aid, AidLay *out) {
  const char *p = g_cfg.layout;
  while (*p) {
    const char *eol = strchr(p, '\n');
    if (!eol) eol = p + strlen(p);
    if (p[0] == blk && p[1] == '|') {
      char *q = nullptr;
      long id = strtol(p + 2, &q, 10);
      if (id == aid && q < eol && *q == '|') {
        AidLay l;
        l.order = (int)strtol(q + 1, &q, 10);
        if (q < eol && *q == '|') l.vis = (uint8_t)strtol(q + 1, &q, 10);
        if (q < eol && *q == '|') {
          size_t len = (size_t)(eol - (q + 1));
          if (len >= sizeof(l.name)) len = sizeof(l.name) - 1;
          memcpy(l.name, q + 1, len);
          l.name[len] = 0;
        }
        *out = l;
        return true;
      }
    }
    p = *eol ? eol + 1 : eol;
  }
  return false;
}

static int layOrder(char blk, int aid) {
  AidLay l;
  return (layGet(blk, aid, &l) && l.order > 0) ? l.order : 1000;
}

static uint8_t layVis(char blk, int aid) {
  AidLay l;
  return layGet(blk, aid, &l) ? l.vis : (uint8_t)LAY_AUTO;
}

// показывать ли aid в блоке: авто-класс без скрытия, или добавлен вручную
static bool layShown(char blk, int aid, bool autoIn) {
  const uint8_t v = layVis(blk, aid);
  return v == LAY_ADD || (autoIn && v != LAY_HIDE);
}

// заменить/удалить строку (blk, aid); дефолтная строка не хранится
static bool layPut(char blk, int aid, const AidLay &l) {
  String out;
  const char *p = g_cfg.layout;
  while (*p) {
    const char *eol = strchr(p, '\n');
    if (!eol) eol = p + strlen(p);
    if (!(p[0] == blk && p[1] == '|' && atoi(p + 2) == aid)) {
      out.concat(p, eol - p);
      out += '\n';
    }
    p = *eol ? eol + 1 : eol;
  }
  if (l.order || l.vis || l.name[0]) {
    char row[80];
    snprintf(row, sizeof(row), "%c|%d|%d|%u|%s\n", blk, aid, l.order, l.vis, l.name);
    out += row;
  }
  if (out.length() >= sizeof(g_cfg.layout)) return false;
  snprintf(g_cfg.layout, sizeof(g_cfg.layout), "%s", out.c_str());
  return true;
}

static bool isClimateType(const char *val) {
  return strcmp(val, "CurrentTemperature") == 0 || strcmp(val, "CurrentRelativeHumidity") == 0 ||
         strcmp(val, "TargetTemperature") == 0 || strcmp(val, "C_FanSpeed") == 0 ||
         strcmp(val, "C_Room") == 0 || strcmp(val, "Active") == 0 ||
         strcmp(val, "CurrentHeatingCoolingState") == 0 ||
         strcmp(val, "TargetHeatingCoolingState") == 0;
}

static void applyOnPayload(AidRec *a, const char *v, uint8_t prio) {
  if (!a || !v || !*v) return;
  if (a->hasOn && prio < a->onPrio) return;
  a->hasOn = true;
  a->onPrio = prio;
  if (strcmp(v, "true") == 0 || strcmp(v, "false") == 0)
    a->on = (v[0] == 't');
  else
    a->on = (atoi(v) != 0);  // On/Active/HeatingCooling: 0=выкл
}

// не кэшируем meta (/type,/format,…) — иначе 480 топиков вытесняют C_Room
static bool mqIsMetaTopic(const char *topic) {
  static const char *sfx[] = {"/type",
                              "/format",
                              "/read",
                              "/write",
                              "/events",
                              "/hidden",
                              "/unit",
                              "/minValue",
                              "/maxValue",
                              "/minStep",
                              "/maxLen",
                              "/additionalAuthorization",
                              "/timedWrite",
                              "/writeResponse",
                              nullptr};
  for (int i = 0; sfx[i]; i++) {
    size_t n = strlen(sfx[i]);
    size_t t = strlen(topic);
    if (t >= n && strcmp(topic + t - n, sfx[i]) == 0) return true;
  }
  return false;
}

static void mqCachePut(const char *topic, const char *val) {
  if (!topic || !val || mqIsMetaTopic(topic)) return;
  for (int i = 0; i < g_mqCacheN; i++) {
    if (strcmp(g_mqCache[i].topic, topic) == 0) {
      strncpy(g_mqCache[i].val, val, sizeof(g_mqCache[i].val) - 1);
      g_mqCache[i].val[sizeof(g_mqCache[i].val) - 1] = 0;
      return;
    }
  }
  if (g_mqCacheN >= 192) return;
  strncpy(g_mqCache[g_mqCacheN].topic, topic, sizeof(g_mqCache[0].topic) - 1);
  g_mqCache[g_mqCacheN].topic[sizeof(g_mqCache[0].topic) - 1] = 0;
  strncpy(g_mqCache[g_mqCacheN].val, val, sizeof(g_mqCache[0].val) - 1);
  g_mqCache[g_mqCacheN].val[sizeof(g_mqCache[0].val) - 1] = 0;
  g_mqCacheN++;
}

static const char *mqCacheGet(const char *topic) {
  for (int i = 0; i < g_mqCacheN; i++)
    if (strcmp(g_mqCache[i].topic, topic) == 0) return g_mqCache[i].val;
  return nullptr;
}

static AidRec *aidFind(int aid) {
  for (int i = 0; i < g_aidN; i++)
    if (g_aids[i].aid == aid) return &g_aids[i];
  return nullptr;
}

static AidRec *aidGet(int aid) {
  if (aid <= 0) return nullptr;
  if (AidRec *a = aidFind(aid)) return a;
  if (g_aidN >= 48) return nullptr;
  g_aids[g_aidN] = AidRec{};
  g_aids[g_aidN].aid = aid;
  return &g_aids[g_aidN++];
}

static bool sameServicePath(const char *a, const char *b) {
  const char *sa = strrchr(a, '/');
  const char *sb = strrchr(b, '/');
  if (!sa || !sb) return false;
  size_t na = (size_t)(sa - a), nb = (size_t)(sb - b);
  return na == nb && strncmp(a, b, na) == 0;
}

static void aidApplyCache(AidRec *a) {
  if (!a) return;
  if (a->tTopic[0])
    if (const char *v = mqCacheGet(a->tTopic)) a->t = atof(v);
  if (a->tTopic2[0])
    if (const char *v = mqCacheGet(a->tTopic2)) a->t2 = atof(v);
  if (a->hTopic[0])
    if (const char *v = mqCacheGet(a->hTopic)) a->h = atof(v);
  if (a->targetTopic[0])
    if (const char *v = mqCacheGet(a->targetTopic)) a->target = atof(v);
  if (a->onTopic[0]) {
    if (const char *v = mqCacheGet(a->onTopic)) applyOnPayload(a, v, a->onPrio ? a->onPrio : 1);
  }
  if (a->roomTopic[0])
    if (const char *v = mqCacheGet(a->roomTopic)) {
      if (v[0] && strcmp(v, "0") != 0) {
        copyUtf8(a->roomName, sizeof(a->roomName), v);
      }
    }
  char root[80];
  snprintf(root, sizeof(root), "%s/accessories/%d", g_cfg.mqttPrefix, a->aid);
  if (!a->devName[0])
    if (const char *dn = mqCacheGet(root)) {
      copyUtf8(a->devName, sizeof(a->devName), dn);
    }
}

static void mqttOnMsg(char *topic, byte *payload, unsigned int len) {
  if (!topic) return;
  g_mqMsgN++;

  char val[48];
  if (len >= sizeof(val)) len = sizeof(val) - 1;
  memcpy(val, payload, len);
  val[len] = 0;
  utf8CutTail(val);

  mqCachePut(topic, val);

  const char *acc = strstr(topic, "/accessories/");
  if (!acc) return;
  acc += 13;
  int aid = atoi(acc);
  if (aid <= 0) return;

  const char *after = acc;
  while (*after >= '0' && *after <= '9') after++;

  size_t tlen = strlen(topic);
  const bool isType = (tlen > 5 && strcmp(topic + tlen - 5, "/type") == 0);

  // слоты только под климат/пол/AC — иначе 16 лампочек съедают таблицу
  AidRec *a = aidFind(aid);
  if (!a) {
    if (isType && isClimateType(val))
      a = aidGet(aid);
    if (!a) return;
  }

  if (*after == 0) {
    copyUtf8(a->devName, sizeof(a->devName), val);
    return;
  }

  if (isType) {
    char base[96];
    size_t bl = tlen - 5;
    if (bl >= sizeof(base)) bl = sizeof(base) - 1;
    memcpy(base, topic, bl);
    base[bl] = 0;

    if (strcmp(val, "CurrentTemperature") == 0) {
      if (!a->tTopic[0])
        strncpy(a->tTopic, base, sizeof(a->tTopic) - 1);
      else if (!a->tTopic2[0] && strcmp(a->tTopic, base) != 0)
        strncpy(a->tTopic2, base, sizeof(a->tTopic2) - 1);
    } else if (strcmp(val, "CurrentRelativeHumidity") == 0) {
      strncpy(a->hTopic, base, sizeof(a->hTopic) - 1);
    } else if (strcmp(val, "TargetTemperature") == 0) {
      strncpy(a->targetTopic, base, sizeof(a->targetTopic) - 1);
    } else if (strcmp(val, "On") == 0) {
      strncpy(a->onTopic, base, sizeof(a->onTopic) - 1);
      a->onPrio = 3;
      if (const char *v = mqCacheGet(base)) applyOnPayload(a, v, 3);
    } else if (strcmp(val, "Active") == 0) {
      if (a->onPrio <= 2) {
        strncpy(a->onTopic, base, sizeof(a->onTopic) - 1);
        a->onPrio = 2;
        if (const char *v = mqCacheGet(base)) applyOnPayload(a, v, 2);
      }
    } else if (strcmp(val, "CurrentHeatingCoolingState") == 0 ||
               strcmp(val, "TargetHeatingCoolingState") == 0) {
      if (a->onPrio <= 1) {
        strncpy(a->onTopic, base, sizeof(a->onTopic) - 1);
        a->onPrio = 1;
        if (const char *v = mqCacheGet(base)) applyOnPayload(a, v, 1);
      }
    } else if (strcmp(val, "C_FanSpeed") == 0) {
      strncpy(a->fanTopic, base, sizeof(a->fanTopic) - 1);
    } else if (strcmp(val, "C_Room") == 0) {
      // несколько C_Room — берём тот, у кого в кэше уже есть имя
      if (!a->roomTopic[0]) {
        strncpy(a->roomTopic, base, sizeof(a->roomTopic) - 1);
      }
      if (const char *v = mqCacheGet(base)) {
        if (v[0] && strcmp(v, "0") != 0) {
          strncpy(a->roomTopic, base, sizeof(a->roomTopic) - 1);
          copyUtf8(a->roomName, sizeof(a->roomName), v);
        }
      }
    }
    return;
  }

  if (a->tTopic[0] && strcmp(topic, a->tTopic) == 0) a->t = atof(val);
  else if (a->tTopic2[0] && strcmp(topic, a->tTopic2) == 0) a->t2 = atof(val);
  else if (a->hTopic[0] && strcmp(topic, a->hTopic) == 0) a->h = atof(val);
  else if (a->targetTopic[0] && strcmp(topic, a->targetTopic) == 0) a->target = atof(val);
  else if (a->onTopic[0] && strcmp(topic, a->onTopic) == 0) {
    applyOnPayload(a, val, a->onPrio ? a->onPrio : 1);
  } else if (a->roomTopic[0] && strcmp(topic, a->roomTopic) == 0) {
    if (val[0] && strcmp(val, "0") != 0) {
      copyUtf8(a->roomName, sizeof(a->roomName), val);
    }
  }
}

static void labelAuto(const AidRec &a, char *out, size_t n) {
  if (a.roomName[0] && strcmp(a.roomName, "0") != 0)
    strncpy(out, a.roomName, n - 1);
  else if (a.devName[0] && strcmp(a.devName, "Датчик климата") != 0)
    strncpy(out, a.devName, n - 1);
  else
    snprintf(out, n, "aid%d", a.aid);
  out[n - 1] = 0;
}

static void labelFromAid(const AidRec &a, char blk, char *out, size_t n) {
  AidLay l;
  if (layGet(blk, a.aid, &l) && l.name[0])
    copyUtf8(out, n, l.name);
  else
    labelAuto(a, out, n);
}

static bool fetchSprutMqtt(Wx &w) {
  w.roomN = 0;
  w.ctrlN = 0;
  for (int i = 0; i < 8; i++) {
    w.rooms[i] = RoomSnap{};
    w.ctrls[i] = CtrlSnap{};
  }
  g_mqMsgN = 0;
  g_mqCacheN = 0;
  g_aidN = 0;

  if (!g_cfg.mqttPrefix[0])
    strncpy(g_cfg.mqttPrefix, DEF_MQTT_PREFIX, sizeof(g_cfg.mqttPrefix) - 1);
  if (!g_cfg.mqttHost[0] || !g_cfg.mqttPrefix[0]) {
    Serial.println("mqtt: host/prefix не заданы");
    return false;
  }

  // static — не жрём стек loopTask (~8KB)
  static WiFiClient net;
  static PubSubClient mqtt(net);
  mqtt.setServer(g_cfg.mqttHost, g_cfg.mqttPort);
  mqtt.setBufferSize(2048);
  mqtt.setSocketTimeout(5);
  mqtt.setCallback(mqttOnMsg);

  char cid[24];
  snprintf(cid, sizeof(cid), "epd-%04X", (uint16_t)(ESP.getEfuseMac() & 0xFFFF));
  if (!mqtt.connect(cid, g_cfg.mqttUser, g_cfg.mqttPass)) {
    Serial.printf("mqtt connect fail state=%d host=%s:%u\n", mqtt.state(), g_cfg.mqttHost,
                  g_cfg.mqttPort);
    return false;
  }

  char filter[64];
  snprintf(filter, sizeof(filter), "%s/#", g_cfg.mqttPrefix);
  if (!mqtt.subscribe(filter)) {
    Serial.println("mqtt subscribe fail");
    mqtt.disconnect();
    return false;
  }
  Serial.printf("mqtt sub %s\n", filter);

  uint32_t t0 = millis();
  while (millis() - t0 < 8000) mqtt.loop();
  mqtt.disconnect();

  for (int i = 0; i < g_aidN; i++) {
    AidRec &a = g_aids[i];
    aidApplyCache(&a);

    // для термостата с 2x CurrentTemperature — берём из того же service что Target
    if (a.targetTopic[0] && a.tTopic2[0]) {
      if (sameServicePath(a.tTopic2, a.targetTopic)) {
        a.t = a.t2;
        strncpy(a.tTopic, a.tTopic2, sizeof(a.tTopic) - 1);
      } else if (!sameServicePath(a.tTopic, a.targetTopic) && !isnan(a.t2)) {
        a.t = a.t2;
      }
    }

    const bool isSensor = a.hTopic[0] != 0;  // T+H → темп. в доме
    const bool isAc = a.fanTopic[0] != 0 || (a.devName[0] && strstr(a.devName, "AUX"));
    const bool isFloor =
        (a.devName[0] && (strstr(a.devName, "пол") || strstr(a.devName, "BHT"))) ||
        (a.targetTopic[0] && a.hasOn && !isAc);
    const bool isCtrl = isAc || isFloor || (a.targetTopic[0] && !isSensor);
    a.blk = (isSensor && !isnan(a.t) ? BLK_HOME : 0) | (isCtrl ? BLK_CLIM : 0);

    if (layShown('d', a.aid, a.blk & BLK_HOME) && !isnan(a.t) && w.roomN < 8) {
      RoomSnap &r = w.rooms[w.roomN++];
      r = RoomSnap{};
      r.aid = a.aid;
      r.t = a.t;
      r.h = a.h;
      labelFromAid(a, 'd', r.name, sizeof(r.name));
      Serial.printf("room aid=%d %s T=%.1f H=%.0f\n", r.aid, r.name, r.t, r.h);
    }

    if (layShown('c', a.aid, a.blk & BLK_CLIM) && w.ctrlN < 8) {
      CtrlSnap &c = w.ctrls[w.ctrlN++];
      c = CtrlSnap{};
      c.aid = a.aid;
      c.t = a.t;
      c.target = a.target;
      c.on = a.on;
      c.hasOn = a.hasOn;
      labelFromAid(a, 'c', c.name, sizeof(c.name));
      if (isAc)
        strncpy(c.kind, "AC", sizeof(c.kind) - 1);
      else if (isFloor || a.targetTopic[0])
        strncpy(c.kind, "пол", sizeof(c.kind) - 1);
      else
        strncpy(c.kind, "дат", sizeof(c.kind) - 1);  // датчик, добавлен вручную
      Serial.printf("ctrl aid=%d %s %s T=%.1f -> %.0f on=%d\n", c.aid, c.kind, c.name, c.t,
                    c.target, (int)c.on);
    }
  }

  // порядок из «Блоки на экране», остальные — по aid
  auto before = [](char blk, int a1, int a2) {
    int o1 = layOrder(blk, a1), o2 = layOrder(blk, a2);
    return o1 != o2 ? o1 < o2 : a1 < a2;
  };
  for (int i = 0; i + 1 < w.roomN; i++)
    for (int j = i + 1; j < w.roomN; j++)
      if (before('d', w.rooms[j].aid, w.rooms[i].aid)) {
        RoomSnap tmp = w.rooms[i];
        w.rooms[i] = w.rooms[j];
        w.rooms[j] = tmp;
      }
  for (int i = 0; i + 1 < w.ctrlN; i++)
    for (int j = i + 1; j < w.ctrlN; j++)
      if (before('c', w.ctrls[j].aid, w.ctrls[i].aid)) {
        CtrlSnap tmp = w.ctrls[i];
        w.ctrls[i] = w.ctrls[j];
        w.ctrls[j] = tmp;
      }

  Serial.printf("mqtt done msgs=%u aids=%d rooms=%d ctrls=%d\n", g_mqMsgN, g_aidN, w.roomN,
                w.ctrlN);
  return w.roomN > 0 || w.ctrlN > 0;
}

static bool httpsGetBody(const char *url, String &body) {
  const bool tls = strncmp(url, "https://", 8) == 0;
  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  http.setTimeout(25000);
  http.useHTTP10(true);  // иначе ESP часто даёт code=200 и пустое body на chunked/TLS
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  bool began;
  if (tls) {
    secure.setInsecure();
    secure.setHandshakeTimeout(30);
    began = http.begin(secure, url);
  } else {
    began = http.begin(plain, url);
  }
  if (!began) {
    g_status = "begin";
    return false;
  }
  http.addHeader("User-Agent", "Mozilla/5.0 (compatible; nm-epd-420/2.3)");
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Connection", "close");
  int code = http.GET();
  Serial.printf("GET %s -> %d\n", url, code);
  if (code != 200) {
    g_status = String("HTTP ") + code;
    http.end();
    return false;
  }
  // читаем стримом — getString() на ESP32+TLS иногда пустой
  // до Content-Length или закрытия сокета (HTTP/1.0); пауза в потоке ≠ конец тела
  const int len = http.getSize();
  body = "";
  body.reserve(len > 0 ? len + 1 : 2048);
  WiFiClient *stream = http.getStreamPtr();
  uint32_t t0 = millis();
  while (stream && millis() - t0 < 20000) {
    if (stream->available()) {
      char buf[256];
      int n = stream->readBytes(buf, sizeof(buf));
      if (n > 0) body.concat(buf, n);
      t0 = millis();
      if (len > 0 && (int)body.length() >= len) break;
      continue;
    }
    if (!stream->connected()) break;
    delay(5);
  }
  if (body.length() == 0) body = http.getString();
  http.end();
  Serial.printf("body %u bytes\n", (unsigned)body.length());
  if (body.length() == 0) {
    g_status = "empty body";
    return false;
  }
  return true;
}

static float parseTempToken(String s) {
  s.trim();
  s.replace("°C", "");
  s.replace("+", "");
  s.replace(" ", "");
  if (!s.length()) return NAN;
  return s.toFloat();
}

// wttr %C / lang_ru часто на EN — мапим в RU + WWO code для иконки
static void localizeDesc(const char *raw, char *out, size_t outn, int *code) {
  String d = raw ? raw : "";
  d.trim();
  String low = d;
  low.toLowerCase();

  struct Map {
    const char *key;
    const char *ru;
    int code;
  };
  // более специфичные фразы выше
  static const Map maps[] = {
      {"torrential", "ливень", 359},
      {"moderate or heavy snow with thunder", "снег с грозой", 395},
      {"patchy light snow with thunder", "небольшой снег с грозой", 392},
      {"moderate or heavy rain with thunder", "дождь с грозой", 389},
      {"patchy light rain with thunder", "небольшой дождь с грозой", 386},
      {"moderate or heavy showers of ice pellets", "град", 377},
      {"light showers of ice pellets", "небольшой град", 374},
      {"moderate or heavy snow showers", "снегопад", 371},
      {"light snow showers", "небольшой снегопад", 368},
      {"moderate or heavy sleet showers", "мокрый снег", 365},
      {"light sleet showers", "небольшой мокрый снег", 362},
      {"moderate or heavy rain shower", "ливень", 356},
      {"light rain shower", "небольшой ливень", 353},
      {"ice pellets", "ледяная крупа", 350},
      {"patchy heavy snow", "местами сильный снег", 335},
      {"heavy snow", "сильный снег", 338},
      {"patchy moderate snow", "местами умеренный снег", 329},
      {"moderate snow", "умеренный снег", 332},
      {"patchy light snow", "местами небольшой снег", 323},
      {"light snow", "небольшой снег", 326},
      {"moderate or heavy sleet", "мокрый снег", 320},
      {"light sleet", "небольшой мокрый снег", 317},
      {"moderate or heavy freezing rain", "ледяной дождь", 314},
      {"light freezing rain", "небольшой ледяной дождь", 311},
      {"heavy rain at times", "временами сильный дождь", 305},
      {"heavy rain", "сильный дождь", 308},
      {"moderate rain at times", "временами дождь", 299},
      {"moderate rain", "умеренный дождь", 302},
      {"patchy light rain", "местами небольшой дождь", 293},
      {"light rain", "небольшой дождь", 296},
      {"heavy freezing drizzle", "сильная ледяная морось", 284},
      {"freezing drizzle", "ледяная морось", 281},
      {"light drizzle", "морось", 266},
      {"patchy light drizzle", "местами морось", 263},
      {"thundery outbreaks", "гроза", 200},
      {"blizzard", "метель", 230},
      {"blowing snow", "поземок", 227},
      {"freezing fog", "ледяной туман", 260},
      {"patchy rain", "местами дождь", 176},
      {"partly cloudy", "переменная облачность", 116},
      {"mostly cloudy", "облачно", 119},
      {"overcast", "пасмурно", 122},
      {"cloudy", "облачно", 119},
      {"mist", "дымка", 143},
      {"fog", "туман", 248},
      {"clear", "ясно", 113},
      {"sunny", "ясно", 113},
  };

  for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) {
    if (low.indexOf(maps[i].key) >= 0) {
      strncpy(out, maps[i].ru, outn - 1);
      out[outn - 1] = 0;
      if (code) *code = maps[i].code;
      return;
    }
  }

  // уже по-русски?
  bool hasCyr = false;
  for (size_t i = 0; i < d.length(); i++) {
    if ((unsigned char)d[i] >= 0xD0) {
      hasCyr = true;
      break;
    }
  }
  if (hasCyr) {
    strncpy(out, d.c_str(), outn - 1);
    out[outn - 1] = 0;
    if (!code) return;
    if (low.indexOf("снег") >= 0) *code = 338;
    else if (low.indexOf("дожд") >= 0 || low.indexOf("лив") >= 0) *code = 296;
    else if (low.indexOf("гроз") >= 0) *code = 200;
    else if (low.indexOf("туман") >= 0 || low.indexOf("дымк") >= 0) *code = 143;
    else if (low.indexOf("ясно") >= 0) *code = 113;
    else if (low.indexOf("облач") >= 0 || low.indexOf("пасмур") >= 0) *code = 122;
    else *code = 119;
    return;
  }

  // грубый EN fallback
  if (low.indexOf("snow") >= 0) {
    strncpy(out, "снег", outn - 1);
    if (code) *code = 338;
  } else if (low.indexOf("rain") >= 0 || low.indexOf("drizzle") >= 0) {
    strncpy(out, "дождь", outn - 1);
    if (code) *code = 296;
  } else if (low.indexOf("thunder") >= 0) {
    strncpy(out, "гроза", outn - 1);
    if (code) *code = 200;
  } else if (low.indexOf("cloud") >= 0) {
    strncpy(out, "облачно", outn - 1);
    if (code) *code = 119;
  } else {
    strncpy(out, d.c_str(), outn - 1);
    if (code) *code = 119;
  }
  out[outn - 1] = 0;
}

// +3°C|+0°C|86%|→10km/h|Слабый туман|1018hPa|0.0mm|0
static bool parseWttrText(const String &body, Wx &w) {
  String line = body;
  line.trim();
  // срезать CR и хвост
  int nl = line.indexOf('\n');
  if (nl > 0) line = line.substring(0, nl);
  line.trim();

  String parts[10];
  int n = 0, start = 0;
  for (int i = 0; i <= line.length() && n < 10; i++) {
    if (i == line.length() || line[i] == '|') {
      parts[n++] = line.substring(start, i);
      start = i + 1;
    }
  }
  if (n < 5) {
    Serial.printf("text parts=%d body=[%s]\n", n, line.c_str());
    g_status = "text parse";
    return false;
  }

  w.temp = parseTempToken(parts[0]);
  w.feels = parseTempToken(parts[1]);
  String hum = parts[2];
  hum.replace("%", "");
  w.humidity = hum.toInt();

  String wind = parts[3];
  String num;
  for (size_t i = 0; i < wind.length(); i++) {
    char c = wind[i];
    if ((c >= '0' && c <= '9') || c == '.' || c == '-') num += c;
  }
  // wttr %w даёт км/ч → храним м/с
  w.wind = num.length() ? num.toFloat() / 3.6f : NAN;
  w.windDir[0] = 0;

  if (n > 5) {
    String pr = parts[5];
    pr.replace("hPa", "");
    pr.replace(" ", "");
    w.pressure = pr.toInt();
  }
  if (n > 7) w.uv = parts[7].toInt();

  // %S / %s → "07:41:16" — берём HH:MM
  auto copyHm = [](const String &src, char *out, size_t n) {
    if (src.length() >= 5) {
      snprintf(out, n, "%.5s", src.c_str());
    }
  };
  if (n > 8) copyHm(parts[8], w.sunrise, sizeof(w.sunrise));
  if (n > 9) copyHm(parts[9], w.sunset, sizeof(w.sunset));

  // wttr lang_ru часто отдаёт английский — локализуем сами
  localizeDesc(parts[4].c_str(), w.desc, sizeof(w.desc), &w.code);

  stampNow(w);
  w.ok = !isnan(w.temp);
  return w.ok;
}

static void windDirRu(float deg, char *out, size_t n) {
  if (isnan(deg) || n < 3) {
    if (n) out[0] = 0;
    return;
  }
  static const char *dirs[] = {"С", "СВ", "В", "ЮВ", "Ю", "ЮЗ", "З", "СЗ"};
  int i = (int)floorf(fmodf(deg + 22.5f, 360.f) / 45.f) & 7;
  strncpy(out, dirs[i], n - 1);
  out[n - 1] = 0;
}

// j1 отдаёт числа строками: "2", "1020"
static float jnum(JsonVariantConst v) {
  if (v.is<float>()) return v.as<float>();
  const char *s = v.as<const char *>();
  return (s && *s) ? atof(s) : NAN;
}

// "07:42 AM" / "04:44 PM" → "07:42" / "16:44"; "No sunset" → не трогаем
static void ampmToHm(const char *s, char *out, size_t n) {
  int h, m;
  char ap[3] = "";
  if (!s || sscanf(s, "%d:%d %2s", &h, &m, ap) < 2) return;
  if ((ap[0] == 'P' || ap[0] == 'p') && h < 12) h += 12;
  if ((ap[0] == 'A' || ap[0] == 'a') && h == 12) h = 0;
  snprintf(out, n, "%02d:%02d", h, m);
}

static void j1Desc(JsonVariantConst o, char *out, size_t n) {
  const char *raw = o["lang_ru"][0]["value"] | "";
  if (!*raw) raw = o["weatherDesc"][0]["value"] | "";
  localizeDesc(raw, out, n, nullptr);
}

// wttr j1: улица + мин/макс + восход/закат + слоты по 3ч (сегодня+завтра) одним запросом
static bool fetchWttrJ1(Wx &w) {
  String url = String("http://wttr.in/") + urlEncode(g_cfg.city) + "?format=j1&lang=ru";
  String body;
  if (!httpsGetBody(url.c_str(), body) || !body.startsWith("{")) {
    url = String("https://wttr.in/") + urlEncode(g_cfg.city) + "?format=j1&lang=ru";
    body = "";
    if (!httpsGetBody(url.c_str(), body) || !body.startsWith("{")) {
      snprintf(w.soonErr, sizeof(w.soonErr), "wttr %s", g_status.c_str());
      return false;
    }
  }

  JsonDocument filter;
  JsonObject fc = filter["current_condition"].add<JsonObject>();
  for (const char *k : {"temp_C", "FeelsLikeC", "humidity", "windspeedKmph", "winddirDegree",
                        "pressure", "uvIndex", "weatherCode", "precipMM"})
    fc[k] = true;
  fc["lang_ru"] = true;
  fc["weatherDesc"] = true;
  JsonObject fd = filter["weather"].add<JsonObject>();
  fd["mintempC"] = true;
  fd["maxtempC"] = true;
  fd["uvIndex"] = true;
  fd["astronomy"] = true;
  JsonObject fh = fd["hourly"].add<JsonObject>();
  for (const char *k : {"time", "tempC", "chanceofrain", "chanceofsnow", "weatherCode"})
    fh[k] = true;
  fh["lang_ru"] = true;
  fh["weatherDesc"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  body = String();  // 26KB назад в кучу до MQTT
  if (err) {
    Serial.printf("j1 json %s heap=%u\n", err.c_str(), (unsigned)ESP.getFreeHeap());
    snprintf(w.soonErr, sizeof(w.soonErr), "j1 %s", err.c_str());
    return false;
  }

  JsonObjectConst cur = doc["current_condition"][0];
  if (cur.isNull()) return false;
  w.temp = jnum(cur["temp_C"]);
  w.feels = jnum(cur["FeelsLikeC"]);
  float hum = jnum(cur["humidity"]);
  w.humidity = isnan(hum) ? -1 : (int)hum;
  float kmh = jnum(cur["windspeedKmph"]);
  w.wind = isnan(kmh) ? NAN : kmh / 3.6f;
  windDirRu(jnum(cur["winddirDegree"]), w.windDir, sizeof(w.windDir));
  float pr = jnum(cur["pressure"]);
  w.pressure = isnan(pr) ? -1 : (int)pr;
  float uv = jnum(cur["uvIndex"]);
  w.uv = isnan(uv) ? -1 : (int)uv;
  float code = jnum(cur["weatherCode"]);
  w.code = isnan(code) ? 119 : (int)code;
  j1Desc(cur, w.desc, sizeof(w.desc));
  w.precipMm = jnum(cur["precipMM"]);

  JsonArrayConst days = doc["weather"];
  JsonObjectConst d0 = days[0];
  if (!d0.isNull()) {
    w.tmin = jnum(d0["mintempC"]);
    w.tmax = jnum(d0["maxtempC"]);
    float um = jnum(d0["uvIndex"]);
    w.uvMax = isnan(um) ? -1 : (int)um;
    ampmToHm(d0["astronomy"][0]["sunrise"] | "", w.sunrise, sizeof(w.sunrise));
    ampmToHm(d0["astronomy"][0]["sunset"] | "", w.sunset, sizeof(w.sunset));
  }

  int nowH = 12;
  struct tm ti;
  if (getLocalTime(&ti, 200)) nowH = ti.tm_hour;

  // слоты: абсолютный час от полуночи сегодня (завтра = +24)
  w.precipProb = -1;
  w.precipLine[0] = 0;
  w.soonN = 0;
  w.soonLead[0] = 0;
  w.soonErr[0] = 0;
  char rainAt[6] = "";
  for (int d = 0; d < 2 && d < (int)days.size(); d++) {
    for (JsonObjectConst h : days[d]["hourly"].as<JsonArrayConst>()) {
      int absH = d * 24 + (int)jnum(h["time"]) / 100;
      float cr = jnum(h["chanceofrain"]), cs = jnum(h["chanceofsnow"]);
      int prob = (int)fmaxf(isnan(cr) ? -1.f : cr, isnan(cs) ? -1.f : cs);
      if (absH <= nowH && nowH < absH + 3) {
        w.precipProb = prob;
        continue;
      }
      if (absH <= nowH || w.soonN >= 3) continue;
      SoonH &s = w.soon[w.soonN++];
      s = SoonH{};
      snprintf(s.hm, sizeof(s.hm), "%02u:00", (unsigned)absH % 24u);
      s.t = jnum(h["tempC"]);
      char dshort[24];
      j1Desc(h, dshort, sizeof(dshort));
      if (prob >= g_cfg.rainPct) {
        snprintf(s.note, sizeof(s.note), "%.12s %d%%", dshort, prob);
        if (!rainAt[0]) strncpy(rainAt, s.hm, sizeof(rainAt) - 1);
      } else {
        strncpy(s.note, dshort, sizeof(s.note) - 1);
      }
    }
  }
  if (rainAt[0])
    snprintf(w.soonLead, sizeof(w.soonLead), "осадки к %s", rainAt);
  else if (w.soonN > 0)
    snprintf(w.soonLead, sizeof(w.soonLead), "без осадков");

  if (w.precipProb >= 0) {
    bool wet = !isnan(w.precipMm) && w.precipMm >= 0.05f;
    if (w.precipProb == 0 && !wet)
      snprintf(w.precipLine, sizeof(w.precipLine), "нет");
    else if (wet)
      snprintf(w.precipLine, sizeof(w.precipLine), "%d%% %.1f", w.precipProb, w.precipMm);
    else
      snprintf(w.precipLine, sizeof(w.precipLine), "%d%%", w.precipProb);
  }

  stampNow(w);
  w.ok = !isnan(w.temp);
  Serial.printf("j1 T=%.0f %s soon=%d lead=%s precip=%s heap=%u\n", w.temp, w.desc, w.soonN,
                w.soonLead[0] ? w.soonLead : "-", w.precipLine[0] ? w.precipLine : "-",
                (unsigned)ESP.getFreeHeap());
  return w.ok;
}

static bool fetchWeather(Wx &w) {
  if (fetchWttrJ1(w)) {
    g_status = "ok";
    return true;
  }
  // j1 упал (куча/парс) — короткий текст с того же хоста, без «скоро»
  Serial.println("j1 fail — wttr text");
  String body;
  String url = String("https://wttr.in/") + urlEncode(g_cfg.city) +
               "?lang=ru&format=%25t%7C%25f%7C%25h%7C%25w%7C%25C%7C%25P%7C%25p%7C%25u%7C%25S%7C%"
               "25s";
  if (!httpsGetBody(url.c_str(), body) || !parseWttrText(body, w)) return false;
  g_status = "ok";
  return true;
}

static void textAt(int16_t x, int16_t y, const char *s, uint16_t color) {
  u8g2.setForegroundColor(color);
  u8g2.setCursor(x, y);
  u8g2.print(s);
}

// режет по границе UTF-8, пока текущий шрифт u8g2 не влезет в maxW
static void fitUtf8(char *s, int maxW) {
  while (s[0] && u8g2.getUTF8Width(s) > maxW) {
    size_t cut = strlen(s);
    do {
      cut--;
    } while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80);
    s[cut] = 0;
  }
}

static char g_nextWake[8] = "--:--";

static bool inNight(int h) {
  const int f = g_cfg.nightFrom, t = g_cfg.nightTo;
  return f < t ? (h >= f && h < t) : (h >= f || h < t);
}

// сколько спать: интервал, но если пробуждение попадает в ночь — до nightTo:00
static uint32_t sleepSeconds(bool ok) {
  uint32_t secs = ok ? (uint32_t)g_cfg.sleepMin * 60U : 180U;
  time_t now = time(nullptr);
  time_t nx = now + secs;
  if (g_cfg.nightOn && g_cfg.nightFrom != g_cfg.nightTo && now > 1700000000) {
    struct tm t;
    localtime_r(&nx, &t);
    if (inNight(t.tm_hour)) {
      if (t.tm_hour >= g_cfg.nightTo) t.tm_mday += 1;
      t.tm_hour = g_cfg.nightTo;
      t.tm_min = 0;
      t.tm_sec = 30;  // RTC в deep sleep плывёт — не проснуться в 05:59
      t.tm_isdst = -1;
      time_t target = mktime(&t);
      if (target > now) {
        secs = (uint32_t)(target - now);
        nx = target;
      }
    }
  }
  if (now > 1700000000) {
    struct tm t;
    localtime_r(&nx, &t);
    strftime(g_nextWake, sizeof(g_nextWake), "%H:%M", &t);
  }
  return secs;
}

static void formField(String &h, const char *label, const char *name, const char *val,
                      const char *type = "text") {
  h += F("<label>");
  h += label;
  h += F("</label><input name=");
  h += name;
  h += F(" type=");
  h += type;
  h += F(" value=\"");
  h += htmlEscape(val);
  h += F("\">");
}

static void formNum(String &h, const char *label, const char *name, long val, long mn, long mx) {
  char v[12], type[40];
  snprintf(v, sizeof(v), "%ld", val);
  snprintf(type, sizeof(type), "number min=%ld max=%ld", mn, mx);
  formField(h, label, name, v, type);
}

static void formCheck(String &h, const char *label, const char *name, bool on) {
  h += F("<label class=chk><input type=checkbox name=");
  h += name;
  if (on) h += F(" checked");
  h += F("> ");
  h += label;
  h += F("</label>");
}

// пароль не отдаём в HTML: пустое поле = оставить как есть
static void formPass(String &h, const char *label, const char *name, bool isSet) {
  h += F("<label>");
  h += label;
  h += F("</label><input name=");
  h += name;
  h += F(" type=password autocomplete=new-password placeholder=\"");
  h += isSet ? F("•••• (пусто — не менять)") : F("не задан");
  h += F("\">");
}

static void pageHead(String &h, const char *title) {
  h += F("<!DOCTYPE html><html><head><meta charset=utf-8><meta name=viewport "
         "content=\"width=device-width,initial-scale=1\"><title>");
  h += title;
  h += F("</title><style>body{font-family:sans-serif;max-width:440px;margin:16px auto;padding:0 12px}"
         "label{display:block;margin:10px 0 4px;font-size:14px;color:#444}"
         "input,textarea{width:100%;padding:8px;box-sizing:border-box;font-size:16px}"
         "label.chk{display:flex;align-items:center;gap:8px;font-size:15px;color:#222}"
         "label.chk input{width:auto;margin:0}"
         "button{margin-top:16px;padding:10px 16px;font-size:16px;width:100%}"
         "h1{font-size:20px}h2{font-size:16px;margin:22px 0 0}"
         ".hint{color:#666;font-size:13px;margin:4px 0}.row{display:flex;gap:8px}"
         ".row>*{flex:1}table{border-collapse:collapse;font-size:13px;width:100%}"
         "td,th{border-bottom:1px solid #ddd;padding:4px;text-align:left}"
         "a.btn{display:block;text-align:center;margin-top:10px;padding:10px;border:1px solid #888;"
         "border-radius:4px;color:#000;text-decoration:none}</style></head><body>");
}

static String cfgFormHtml() {
  String h;
  h.reserve(6000);
  pageHead(h, "EPD config");
  h += F("<h1>Настройки панели</h1><p class=hint>Режим: ");
  h += g_cfg.mains ? F("от сети") : F("батарея");
  h += F(" · следующее обновление ~");
  h += g_nextWake;
  h += F("</p>");
  h += F("<img src=/screen.bmp alt=экран style=\"width:100%;border:1px solid #ccc;margin:8px 0\">"
         "<a class=btn href=/refresh>Обновить сейчас</a>"
         "<a class=btn href=/layout>Блоки на экране: Температура в доме / Климат</a>"
         "<a class=btn href=/mqtt>Устройства Sprut (последний опрос)</a>");
  h += F("<form method=POST action=/save>");

  h += F("<h2>Wi-Fi</h2>");
  formField(h, "SSID", "wifiSsid", g_cfg.wifiSsid);
  formPass(h, "Пароль", "wifiPass", g_cfg.wifiPass[0] != 0);
  h += F("<div class=row><div>");
  formField(h, "DNS 1", "dns1", g_cfg.dns1);
  h += F("</div><div>");
  formField(h, "DNS 2", "dns2", g_cfg.dns2);
  h += F("</div></div>");

  h += F("<h2>Питание и обновление</h2>");
  formCheck(h, "Работает от сети (без сна, портал доступен всегда)", "mains", g_cfg.mains);
  formNum(h, "Интервал обновления, мин", "sleepMin", g_cfg.sleepMin, 5, 720);
  formNum(h, "Портал после обновления (батарея), сек; 0 — только через BOOT", "cfgListen",
          g_cfg.cfgListenSec, 0, 600);
  formCheck(h, "Ночной режим: не обновлять ночью", "nightOn", g_cfg.nightOn);
  h += F("<div class=row><div>");
  formNum(h, "Ночь с, ч", "nightFrom", g_cfg.nightFrom, 0, 23);
  h += F("</div><div>");
  formNum(h, "Проснуться в, ч", "nightTo", g_cfg.nightTo, 0, 23);
  h += F("</div></div>");
  {
    char tz[24];
    snprintf(tz, sizeof(tz), "%g", g_cfg.tzMin / 60.0);
    formField(h, "Часовой пояс, UTC±ч", "tz", tz, "number step=0.5 min=-12 max=14");
  }

  h += F("<h2>SprutHub MQTT</h2>");
  formField(h, "Host", "mqttHost", g_cfg.mqttHost);
  formNum(h, "Port", "mqttPort", g_cfg.mqttPort, 1, 65535);
  formField(h, "Login", "mqttUser", g_cfg.mqttUser);
  formPass(h, "Password", "mqttPass", g_cfg.mqttPass[0] != 0);
  formField(h, "Prefix (Sprut.hub-…)", "mqttPrefix", g_cfg.mqttPrefix);

  h += F("<h2>Погода</h2>");
  formField(h, "Город (wttr.in)", "city", g_cfg.city);
  formNum(h, "Помечать осадки в «скоро» от, %", "rainPct", g_cfg.rainPct, 1, 100);

  h += F("<h2>Точка доступа настройки</h2>");
  formPass(h, "Пароль SoftAP nm-epd-setup (8–63)", "apPass", true);

  h += F("<button type=submit>Сохранить и перезагрузить</button></form>");
  h += F("<form method=POST action=/reset onsubmit=\"return confirm('Сбросить все настройки "
         "к заводским?')\"><button type=submit>Сброс настроек</button></form>");
  h += F("<p class=hint>Неверный Wi-Fi → при старте поднимется SoftAP nm-epd-setup, "
         "портал на http://192.168.4.1/. Зажать BOOT при старте — то же принудительно.</p>");
  h += F("</body></html>");
  return h;
}

static void sendRestart(WebServer &srv, const char *msg);

static String mqttPageHtml() {
  String h;
  h.reserve(4500);
  pageHead(h, "Sprut aids");
  h += F("<h1>Устройства Sprut</h1><p class=hint>С последнего опроса MQTT. Галки — в какой "
         "блок выводить. В «Температура в доме» можно всё, у чего есть температура.</p>"
         "<form method=POST action=/mqtt><table><tr><th>aid</th><th>комната / устройство</th>"
         "<th>T</th><th>H</th><th>уст</th><th>вкл</th><th>Темп. в доме</th><th>Климат</th></tr>");
  String ids;
  for (int i = 0; i < g_aidN; i++) {
    const AidRec &a = g_aids[i];
    char buf[40];
    h += F("<tr><td>");
    h += a.aid;
    h += F("</td><td>");
    h += htmlEscape(a.roomName);
    h += F("<br><small>");
    h += htmlEscape(a.devName);
    h += F("</small></td><td>");
    if (!isnan(a.t)) { snprintf(buf, sizeof(buf), "%.1f", a.t); h += buf; }
    h += F("</td><td>");
    if (!isnan(a.h)) { snprintf(buf, sizeof(buf), "%.0f", a.h); h += buf; }
    h += F("</td><td>");
    if (!isnan(a.target)) { snprintf(buf, sizeof(buf), "%.0f", a.target); h += buf; }
    h += F("</td><td>");
    if (a.hasOn) h += a.on ? F("да") : F("нет");
    for (int b = 0; b < 2; b++) {
      const char blk = b ? 'c' : 'd';
      const bool autoIn = a.blk & (b ? BLK_CLIM : BLK_HOME);
      const bool can = b ? true : !isnan(a.t);
      h += F("</td><td><input type=checkbox style=width:auto name=");
      h += blk;
      h += '_';
      h += a.aid;
      if (layShown(blk, a.aid, autoIn)) h += F(" checked");
      if (!can) h += F(" disabled");
      h += '>';
      if (autoIn) h += F(" <small>авто</small>");
    }
    h += F("</td></tr>");
    if (ids.length()) ids += ',';
    ids += a.aid;
  }
  h += F("</table><input type=hidden name=ids value=\"");
  h += ids;
  h += F("\">");
  if (g_aidN)
    h += F("<button type=submit>Сохранить и перерисовать</button>");
  else
    h += F("<p class=hint>В этом цикле устройств нет (MQTT не ответил?)</p>");
  h += F("</form><a class=btn href=/layout>Блоки на экране: порядок и имена</a>"
         "<a class=btn href=/>Назад</a></body></html>");
  return h;
}

// галка == авто-класс → LAY_AUTO; иначе явно скрыть/добавить. Имя и порядок не трогаем
static void handleMqttSave(WebServer &srv) {
  String ids = srv.arg("ids");
  int pos = 0;
  bool ok = true;
  while (pos < (int)ids.length()) {
    int comma = ids.indexOf(',', pos);
    if (comma < 0) comma = ids.length();
    const int aid = ids.substring(pos, comma).toInt();
    pos = comma + 1;
    const AidRec *a = aidFind(aid);
    if (!a) continue;
    for (int b = 0; b < 2; b++) {
      const char blk = b ? 'c' : 'd';
      const bool autoIn = a->blk & (b ? BLK_CLIM : BLK_HOME);
      char key[16];
      snprintf(key, sizeof(key), "%c_%d", blk, aid);
      bool want = srv.hasArg(key);
      if (!b && isnan(a->t)) want = false;
      AidLay l;
      layGet(blk, aid, &l);
      l.vis = want == autoIn ? LAY_AUTO : (want ? LAY_ADD : LAY_HIDE);
      ok &= layPut(blk, aid, l);
    }
  }
  if (!ok) {
    srv.send(400, "text/plain; charset=utf-8", "слишком много настроек блоков");
    return;
  }
  saveCfg();
  sendRestart(srv, "Сохранено");
}

static void argTo(WebServer &srv, const char *name, char *dst, size_t n, bool keepIfEmpty = false) {
  if (!srv.hasArg(name)) return;
  String v = srv.arg(name);
  if (keepIfEmpty && v.length() == 0) return;
  copyUtf8(dst, n, v.c_str());
}

static bool argNum(WebServer &srv, const char *name, long mn, long mx, long *out) {
  if (!srv.hasArg(name) || srv.arg(name).length() == 0) return false;
  long v = srv.arg(name).toInt();
  if (v < mn || v > mx) return false;
  *out = v;
  return true;
}

static void argIp(WebServer &srv, const char *name, char *dst, size_t n) {
  if (!srv.hasArg(name)) return;
  IPAddress ip;
  if (ip.fromString(srv.arg(name))) snprintf(dst, n, "%s", ip.toString().c_str());
}

static void sendRestart(WebServer &srv, const char *msg) {
  String h;
  pageHead(h, "EPD");
  h += F("<h2>");
  h += msg;
  h += F("</h2><p>Перезагрузка… страница обновится через 40 с.</p>"
         "<script>setTimeout(()=>location.href='/',40000)</script></body></html>");
  srv.send(200, "text/html; charset=utf-8", h);
  delay(400);
  ESP.restart();
}

static void handleCfgSave(WebServer &srv) {
  long v;
  argTo(srv, "wifiSsid", g_cfg.wifiSsid, sizeof(g_cfg.wifiSsid));
  argTo(srv, "wifiPass", g_cfg.wifiPass, sizeof(g_cfg.wifiPass), true);
  argIp(srv, "dns1", g_cfg.dns1, sizeof(g_cfg.dns1));
  argIp(srv, "dns2", g_cfg.dns2, sizeof(g_cfg.dns2));

  g_cfg.mains = srv.hasArg("mains");
  if (argNum(srv, "sleepMin", 5, 720, &v)) g_cfg.sleepMin = (uint16_t)v;
  if (argNum(srv, "cfgListen", 0, 600, &v)) g_cfg.cfgListenSec = (uint16_t)v;
  g_cfg.nightOn = srv.hasArg("nightOn");
  if (argNum(srv, "nightFrom", 0, 23, &v)) g_cfg.nightFrom = (uint8_t)v;
  if (argNum(srv, "nightTo", 0, 23, &v)) g_cfg.nightTo = (uint8_t)v;
  if (srv.hasArg("tz") && srv.arg("tz").length()) {
    float hrs = srv.arg("tz").toFloat();
    if (hrs >= -12.f && hrs <= 14.f) g_cfg.tzMin = (int16_t)lroundf(hrs * 60.f);
  }

  argTo(srv, "mqttHost", g_cfg.mqttHost, sizeof(g_cfg.mqttHost));
  if (argNum(srv, "mqttPort", 1, 65535, &v)) g_cfg.mqttPort = (uint16_t)v;
  argTo(srv, "mqttUser", g_cfg.mqttUser, sizeof(g_cfg.mqttUser));
  argTo(srv, "mqttPass", g_cfg.mqttPass, sizeof(g_cfg.mqttPass), true);
  argTo(srv, "mqttPrefix", g_cfg.mqttPrefix, sizeof(g_cfg.mqttPrefix));

  argTo(srv, "city", g_cfg.city, sizeof(g_cfg.city));
  if (argNum(srv, "rainPct", 1, 100, &v)) g_cfg.rainPct = (uint8_t)v;

  if (srv.hasArg("apPass")) {
    String ap = srv.arg("apPass");
    if (ap.length() >= 8 && ap.length() <= 63)
      snprintf(g_cfg.apPass, sizeof(g_cfg.apPass), "%s", ap.c_str());
  }

  saveCfg();
  sendRestart(srv, "Сохранено");
}

static void handleCfgReset(WebServer &srv) {
  if (g_prefs.begin("epd", false)) {
    g_prefs.clear();
    g_prefs.end();
  }
  sendRestart(srv, "Настройки сброшены");
}

// строки блока в текущем порядке панели
static int layRows(uint8_t bit, char blk, int *idx, int maxN) {
  int n = 0;
  for (int i = 0; i < g_aidN && n < maxN; i++)
    if ((g_aids[i].blk & bit) || layVis(blk, g_aids[i].aid) == LAY_ADD) idx[n++] = i;
  for (int i = 0; i + 1 < n; i++)
    for (int j = i + 1; j < n; j++) {
      int oi = layOrder(blk, g_aids[idx[i]].aid), oj = layOrder(blk, g_aids[idx[j]].aid);
      if (oj < oi || (oj == oi && g_aids[idx[j]].aid < g_aids[idx[i]].aid)) {
        int t = idx[i];
        idx[i] = idx[j];
        idx[j] = t;
      }
    }
  return n;
}

static void layTable(String &h, uint8_t bit, char blk) {
  int idx[24];
  int n = layRows(bit, blk, idx, 24);
  h += F("<table data-b=");
  h += blk;
  h += F("><tr><th></th><th>aid</th><th>сейчас</th><th>имя на панели</th><th>показ</th></tr>");
  String ids;
  for (int k = 0; k < n; k++) {
    const AidRec &a = g_aids[idx[k]];
    AidLay l;
    layGet(blk, a.aid, &l);
    char key[16], buf[48], autoName[40];
    labelAuto(a, autoName, sizeof(autoName));
    h += F("<tr data-aid=");
    h += a.aid;
    h += F("><td style=white-space:nowrap><button type=button onclick=mv(this,-1)>↑</button>"
           "<button type=button onclick=mv(this,1)>↓</button></td><td>");
    h += a.aid;
    h += F("</td><td>");
    h += htmlEscape(autoName);
    if (bit == BLK_HOME && !isnan(a.t)) {
      snprintf(buf, sizeof(buf), " %.1f°", a.t);
      h += buf;
    } else if (bit == BLK_CLIM && !isnan(a.target)) {
      snprintf(buf, sizeof(buf), " →%.0f", a.target);
      h += buf;
    }
    snprintf(key, sizeof(key), "%c_%d", blk, a.aid);
    h += F("</td><td><input name=n_");
    h += key;
    h += F(" maxlength=39 value=\"");
    h += htmlEscape(l.name);
    h += F("\" placeholder=\"");
    h += htmlEscape(autoName);
    h += F("\"></td><td><input type=checkbox name=s_");
    h += key;
    if (l.vis != LAY_HIDE) h += F(" checked");
    h += F("><input type=hidden name=o_");
    h += key;
    h += F(" value=");
    h += k + 1;
    h += F("></td></tr>");
    if (ids.length()) ids += ',';
    ids += a.aid;
  }
  h += F("</table><input type=hidden name=ids_");
  h += blk;
  h += F(" value=\"");
  h += ids;
  h += F("\">");
  if (!n) h += F("<p class=hint>В этом цикле устройств нет (MQTT не ответил?)</p>");
}

static String layoutPageHtml() {
  String h;
  h.reserve(7000);
  pageHead(h, "Блоки на экране");
  h += F("<style>td input{padding:4px;font-size:14px}td button{padding:2px 8px;margin:0 2px;"
         "width:auto;font-size:14px}select{width:100%;padding:8px;font-size:16px}</style>"
         "<h1>Блоки на экране</h1><p class=hint>↑↓ — порядок строк. На панели влезает 6 строк "
         "в блоке. Пустое имя — авто (комната из Sprut).</p>"
         "<form method=POST action=/layout onsubmit=renum()>");
  h += F("<h2>Левый блок — Температура в доме</h2>");
  formField(h, "Заголовок", "tHome", g_cfg.titleHome);
  formCheck(h, "Колонка влажности H%", "homeH", g_cfg.homeShowH);
  layTable(h, BLK_HOME, 'd');
  h += F("<h2>Правый блок — Климат</h2>");
  formField(h, "Заголовок", "tClim", g_cfg.titleClim);
  h += F("<label>Колонка температуры</label><select name=climCur><option value=0");
  if (!g_cfg.climCurrent) h += F(" selected");
  h += F(">уставка</option><option value=1");
  if (g_cfg.climCurrent) h += F(" selected");
  h += F(">текущая</option></select>");
  layTable(h, BLK_CLIM, 'c');
  h += F("<button type=submit>Сохранить и перерисовать</button></form>"
         "<a class=btn href=/>Назад</a>"
         "<script>function mv(b,d){var r=b.closest('tr'),t=r.parentNode;"
         "if(d<0){var p=r.previousElementSibling;if(p&&p.dataset.aid)t.insertBefore(r,p);}"
         "else{var n=r.nextElementSibling;if(n)t.insertBefore(n,r);}}"
         "function renum(){document.querySelectorAll('table[data-b]').forEach(function(t){var i=1;"
         "t.querySelectorAll('tr[data-aid]').forEach(function(r){"
         "r.querySelector('input[name^=o_]').value=i++;});});}</script></body></html>");
  return h;
}

// пишем строки блока из формы, чужие (не в форме) строки сохраняем как были
static void handleLayoutSave(WebServer &srv) {
  argTo(srv, "tHome", g_cfg.titleHome, sizeof(g_cfg.titleHome));
  argTo(srv, "tClim", g_cfg.titleClim, sizeof(g_cfg.titleClim));
  g_cfg.homeShowH = srv.hasArg("homeH");
  if (srv.hasArg("climCur")) g_cfg.climCurrent = srv.arg("climCur") == "1";

  String out;
  int formAid[2][24], formN[2] = {0, 0};
  for (int b = 0; b < 2; b++) {
    const char blk = b ? 'c' : 'd';
    String ids = srv.arg(b ? "ids_c" : "ids_d");
    int pos = 0;
    while (pos < (int)ids.length()) {
      int comma = ids.indexOf(',', pos);
      if (comma < 0) comma = ids.length();
      int aid = ids.substring(pos, comma).toInt();
      pos = comma + 1;
      if (aid <= 0 || formN[b] >= 24) continue;
      formAid[b][formN[b]++] = aid;
      char key[16];
      snprintf(key, sizeof(key), "%c_%d", blk, aid);
      String name = srv.arg(String("n_") + key);
      name.replace("|", "/");
      name.replace("\n", " ");
      name.replace("\r", "");
      name.trim();
      char nm[40];
      copyUtf8(nm, sizeof(nm), name.c_str());
      int order = srv.arg(String("o_") + key).toInt();
      // снятая галка: авто-строку скрыть, ручную — убрать из блока
      const AidRec *a = aidFind(aid);
      const bool autoIn = a && (a->blk & (b ? BLK_CLIM : BLK_HOME));
      uint8_t vis;
      if (srv.hasArg(String("s_") + key))
        vis = autoIn ? LAY_AUTO : LAY_ADD;
      else
        vis = autoIn ? LAY_HIDE : LAY_AUTO;
      char row[80];
      snprintf(row, sizeof(row), "%c|%d|%d|%u|%s\n", blk, aid, order, vis, nm);
      out += row;
    }
  }
  const char *p = g_cfg.layout;
  while (*p) {
    const char *eol = strchr(p, '\n');
    if (!eol) eol = p + strlen(p);
    const int b = p[0] == 'c' ? 1 : 0;
    const int aid = atoi(p + 2);
    bool inForm = false;
    for (int k = 0; k < formN[b]; k++) inForm |= formAid[b][k] == aid;
    if ((p[0] == 'd' || p[0] == 'c') && p[1] == '|' && aid > 0 && !inForm) {
      out.concat(p, eol - p);
      out += '\n';
    }
    p = *eol ? eol + 1 : eol;
  }
  if (out.length() >= sizeof(g_cfg.layout)) {
    srv.send(400, "text/plain; charset=utf-8", "слишком много строк/длинные имена");
    return;
  }
  snprintf(g_cfg.layout, sizeof(g_cfg.layout), "%s", out.c_str());
  saveCfg();
  sendRestart(srv, "Сохранено");
}

static const Wx *g_wx = nullptr;  // последний отрисованный кадр — для /screen.bmp
static void renderWeather(Adafruit_GFX &g, const Wx &w);

// 3 цвета панели в двух 1-битных плоскостях (2×15 КБ)
class Canvas3C : public Adafruit_GFX {
 public:
  Canvas3C(int16_t w, int16_t h) : Adafruit_GFX(w, h), stride_((w + 7) / 8) {
    blk_ = (uint8_t *)calloc((size_t)stride_ * h, 1);
    red_ = (uint8_t *)calloc((size_t)stride_ * h, 1);
  }
  ~Canvas3C() {
    free(blk_);
    free(red_);
  }
  bool ok() const { return blk_ && red_; }
  void drawPixel(int16_t x, int16_t y, uint16_t c) override {
    if (x < 0 || y < 0 || x >= _width || y >= _height) return;
    const size_t i = (size_t)y * stride_ + x / 8;
    const uint8_t m = 0x80 >> (x & 7);
    blk_[i] &= ~m;
    red_[i] &= ~m;
    if (c == GxEPD_BLACK) blk_[i] |= m;
    else if (c == GxEPD_RED) red_[i] |= m;
  }
  uint8_t at(int16_t x, int16_t y) const {  // 0 белый, 1 чёрный, 2 красный
    const size_t i = (size_t)y * stride_ + x / 8;
    const uint8_t m = 0x80 >> (x & 7);
    return (red_[i] & m) ? 2 : (blk_[i] & m) ? 1 : 0;
  }

 private:
  int16_t stride_;
  uint8_t *blk_ = nullptr, *red_ = nullptr;
};

static void put16(uint8_t *p, uint16_t v) {
  p[0] = v & 0xFF;
  p[1] = v >> 8;
}
static void put32(uint8_t *p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

// 24-bit BMP стримом по строкам — 360 КБ целиком в RAM не держим
static void handleScreenBmp(WebServer &srv) {
  if (!g_wx || !g_wx->ok) {
    srv.send(404, "text/plain", "no frame");
    return;
  }
  Canvas3C cv(display.width(), display.height());
  if (!cv.ok()) {
    srv.send(500, "text/plain", "no memory");
    return;
  }
  renderWeather(cv, *g_wx);
  const int W = cv.width(), H = cv.height();
  const uint32_t row = (uint32_t)W * 3, size = 54 + row * H;
  uint8_t hdr[54] = {'B', 'M'};
  put32(hdr + 2, size);
  put32(hdr + 10, 54);
  put32(hdr + 14, 40);
  put32(hdr + 18, W);
  put32(hdr + 22, H);
  put16(hdr + 26, 1);
  put16(hdr + 28, 24);
  put32(hdr + 34, row * H);
  srv.setContentLength(size);
  srv.send(200, "image/bmp", "");
  srv.sendContent((const char *)hdr, sizeof(hdr));
  static const uint8_t pal[3][3] = {{255, 255, 255}, {0, 0, 0}, {0, 0, 210}};  // BGR
  uint8_t *line = (uint8_t *)malloc(row);
  if (!line) return;
  for (int y = H - 1; y >= 0; y--) {
    for (int x = 0; x < W; x++) memcpy(line + x * 3, pal[cv.at(x, y)], 3);
    srv.sendContent((const char *)line, row);
  }
  free(line);
}

// веб на STA IP; SoftAP если softAp
static void runConfigPortal(bool softAp, uint32_t seconds) {
  if (seconds == 0) return;
  if (softAp) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP("nm-epd-setup", g_cfg.apPass);
    Serial.printf("SoftAP nm-epd-setup  http://%s/\n", WiFi.softAPIP().toString().c_str());
  }

  WebServer srv(80);
  srv.on("/", HTTP_GET, [&]() { srv.send(200, "text/html; charset=utf-8", cfgFormHtml()); });
  srv.on("/save", HTTP_POST, [&]() { handleCfgSave(srv); });
  srv.on("/reset", HTTP_POST, [&]() { handleCfgReset(srv); });
  srv.on("/refresh", HTTP_GET, [&]() { sendRestart(srv, "Обновляю"); });
  srv.on("/layout", HTTP_GET,
         [&]() { srv.send(200, "text/html; charset=utf-8", layoutPageHtml()); });
  srv.on("/layout", HTTP_POST, [&]() { handleLayoutSave(srv); });
  srv.on("/screen.bmp", HTTP_GET, [&]() { handleScreenBmp(srv); });
  srv.on("/mqtt", HTTP_POST, [&]() { handleMqttSave(srv); });
  srv.on("/mqtt", HTTP_GET, [&]() { srv.send(200, "text/html; charset=utf-8", mqttPageHtml()); });
  srv.onNotFound([&]() { srv.send(404, "text/plain", "not found"); });
  srv.begin();
  Serial.printf("cfg portal %us  http://%s/\n", (unsigned)seconds,
                WiFi.localIP().toString().c_str());

  uint32_t t0 = millis();
  while (millis() - t0 < seconds * 1000UL) {
    srv.handleClient();
    delay(2);
  }
  srv.stop();
  if (softAp) WiFi.softAPdisconnect(true);
}

static void drawIcon(Adafruit_GFX &g, int16_t cx, int16_t cy, int code, int scale = 1) {
  int k = iconKind(code);
  int r = 14 * scale;
  switch (k) {
    case 0: // sun
      g.fillCircle(cx, cy, r, GxEPD_RED);
      for (int i = 0; i < 8; i++) {
        float a = i * 0.785398f;
        int x1 = cx + (int)(cosf(a) * (r + 3));
        int y1 = cy + (int)(sinf(a) * (r + 3));
        int x2 = cx + (int)(cosf(a) * (r + 8 * scale));
        int y2 = cy + (int)(sinf(a) * (r + 8 * scale));
        g.drawLine(x1, y1, x2, y2, GxEPD_RED);
      }
      break;
    case 1: // sun+cloud
      g.fillCircle(cx - 6, cy - 4, r - 4, GxEPD_RED);
      g.fillCircle(cx + 2, cy + 2, r - 2, GxEPD_BLACK);
      g.fillCircle(cx - 8, cy + 4, r - 5, GxEPD_BLACK);
      g.fillCircle(cx + 10, cy + 4, r - 5, GxEPD_BLACK);
      break;
    case 2: // cloud
      g.fillCircle(cx, cy, r - 2, GxEPD_BLACK);
      g.fillCircle(cx - 10, cy + 2, r - 5, GxEPD_BLACK);
      g.fillCircle(cx + 10, cy + 2, r - 5, GxEPD_BLACK);
      break;
    case 3: // fog
      for (int i = 0; i < 4; i++)
        g.fillRect(cx - 16, cy - 10 + i * 7, 32, 3, GxEPD_BLACK);
      break;
    case 4: // rain
      g.fillCircle(cx, cy - 4, r - 3, GxEPD_BLACK);
      g.fillCircle(cx - 9, cy - 2, r - 6, GxEPD_BLACK);
      g.fillCircle(cx + 9, cy - 2, r - 6, GxEPD_BLACK);
      for (int i = -1; i <= 1; i++)
        g.drawLine(cx + i * 7, cy + 8, cx + i * 7 - 2, cy + 16, GxEPD_RED);
      break;
    case 5: // snow
      g.fillCircle(cx, cy - 4, r - 3, GxEPD_BLACK);
      g.fillCircle(cx - 9, cy - 2, r - 6, GxEPD_BLACK);
      g.fillCircle(cx + 9, cy - 2, r - 6, GxEPD_BLACK);
      for (int i = -1; i <= 1; i++) {
        g.drawCircle(cx + i * 8, cy + 12, 2, GxEPD_RED);
      }
      break;
    case 6: // storm
      g.fillCircle(cx, cy - 6, r - 3, GxEPD_BLACK);
      g.fillTriangle(cx - 2, cy + 2, cx + 8, cy + 2, cx - 4, cy + 18, GxEPD_RED);
      break;
  }
}

static void drawStat(int16_t x, int16_t y, const char *label, const char *value) {
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  textAt(x, y, label, GxEPD_RED);
  textAt(x + 78, y, value, GxEPD_BLACK);
}

static void drawFail(const char *a, const char *b, const char *c) {
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.fillRect(0, 0, display.width(), 28, GxEPD_RED);
    u8g2.begin(display);
    u8g2.setFontMode(1);
    u8g2.setFont(u8g2_font_10x20_t_cyrillic);
    u8g2.setBackgroundColor(GxEPD_RED);
    textAt(8, 20, "ПОГОДА СЕГОДНЯ", GxEPD_WHITE);
    u8g2.setBackgroundColor(GxEPD_WHITE);
    textAt(8, 70, a ? a : "", GxEPD_BLACK);
    textAt(8, 100, b ? b : "", GxEPD_BLACK);
    textAt(8, 130, c ? c : "", GxEPD_RED);
  } while (display.nextPage());
}

// один кадр экрана на любой GFX: панель (по страницам) или Canvas3C для /screen.bmp
static void renderWeather(Adafruit_GFX &g, const Wx &w) {
  const int W = g.width();   // 400
  const int H = g.height();  // 300
  // погода верх: soon + УФ + осадки справа
  const int wxBottom = 158;
  const int footH = 16;

  g.fillScreen(GxEPD_WHITE);
  char buf[80];

  // header 22px: бренд + город/дата + updated
  g.fillRect(0, 0, W, 22, GxEPD_RED);
  u8g2.begin(g);
  u8g2.setFontMode(1);
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  u8g2.setBackgroundColor(GxEPD_RED);
  textAt(6, 15, "УЛИЦА", GxEPD_WHITE);
  snprintf(buf, sizeof(buf), "%.20s  %s", g_cfg.city, w.dateLine);
  textAt(70, 15, buf, GxEPD_WHITE);
  int tw = u8g2.getUTF8Width(w.updated);
  textAt(W - tw - 6, 15, w.updated, GxEPD_WHITE);
  u8g2.setBackgroundColor(GxEPD_WHITE);

  // left: T сверху, soon сразу под ним (выше — чтобы не пропадал на 2-й page)
  drawIcon(g, 24, 44, w.code, 1);
  u8g2.setFont(u8g2_font_logisoso28_tn);
  snprintf(buf, sizeof(buf), "%.0f", w.temp);
  int tempW = u8g2.getUTF8Width(buf);
  textAt(54, 54, buf, GxEPD_BLACK);
  u8g2.setFont(u8g2_font_10x20_t_cyrillic);
  textAt(54 + tempW + 2, 40, "C", GxEPD_RED);

  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  {
    char nowl[40];
    strncpy(nowl, w.desc[0] ? w.desc : "-", sizeof(nowl) - 1);
    nowl[sizeof(nowl) - 1] = 0;
    fitUtf8(nowl, 120);
    textAt(54, 68, nowl, GxEPD_BLACK);
  }

  // заголовок всегда «скоро», статус осадков рядом
  if (w.soonLead[0])
    snprintf(buf, sizeof(buf), "скоро  %s", w.soonLead);
  else
    snprintf(buf, sizeof(buf), "скоро");
  textAt(6, 84, buf, GxEPD_RED);
  int16_t sy = 98;
  if (w.soonN <= 0) {
    textAt(6, sy, w.soonErr[0] ? w.soonErr : "нет данных часа", GxEPD_BLACK);
    if (!isnan(w.tmax) && !isnan(w.tmin)) {
      snprintf(buf, sizeof(buf), "день %.0f..%.0f", w.tmin, w.tmax);
      textAt(6, sy + 14, buf, GxEPD_BLACK);
    }
  } else {
    for (int i = 0; i < w.soonN; i++) {
      const SoonH &s = w.soon[i];
      // без ° — глифа может не быть в 6x12
      if (!isnan(s.t))
        snprintf(buf, sizeof(buf), "%s %+.0f %s", s.hm, s.t, s.note);
      else
        snprintf(buf, sizeof(buf), "%s %s", s.hm, s.note[0] ? s.note : "-");
      fitUtf8(buf, 190);
      textAt(6, sy, buf, GxEPD_BLACK);
      sy += 14;
    }
  }

  // divider
  g.drawFastVLine(198, 24, wxBottom - 26, GxEPD_BLACK);
  g.drawFastVLine(199, 24, wxBottom - 26, GxEPD_RED);

  // right stats — шаг 15, один шрифт
  char v[32];
  int y = 38;
  const int sx = 206;
  const int step = 15;
  snprintf(v, sizeof(v), "%.0f°", w.feels);
  drawStat(sx, y, "Ощущается", v); y += step;
  snprintf(v, sizeof(v), "%d%%", w.humidity);
  drawStat(sx, y, "Влажность", v); y += step;
  if (w.windDir[0])
    snprintf(v, sizeof(v), "%.1f м/с %s", w.wind, w.windDir);
  else
    snprintf(v, sizeof(v), "%.1f м/с", w.wind);
  drawStat(sx, y, "Ветер", v); y += step;
  snprintf(v, sizeof(v), "%d гПа", w.pressure);
  drawStat(sx, y, "Давление", v); y += step;
  drawStat(sx, y, "Восход", w.sunrise); y += step;
  drawStat(sx, y, "Закат", w.sunset); y += step;
  if (w.uv >= 0 && y + 12 < wxBottom) {
    if (w.uvMax > w.uv)
      snprintf(v, sizeof(v), "%d /%d", w.uv, w.uvMax);
    else
      snprintf(v, sizeof(v), "%d", w.uv);
    drawStat(sx, y, "УФ", v);
    y += step;
  }
  if (w.precipProb >= 0 && w.precipLine[0] && y + 12 < wxBottom)
    drawStat(sx, y, "Осадки", w.precipLine);

  // низ: слева температура в доме (T/H), справа климат (пол/AC)
  const int midX = 200;
  const int leftW = midX;
  const int rightW = W - midX;
  g.drawFastHLine(0, wxBottom, W, GxEPD_BLACK);
  g.fillRect(0, wxBottom + 1, leftW, 14, GxEPD_RED);
  g.fillRect(midX, wxBottom + 1, rightW, 14, GxEPD_RED);
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  u8g2.setBackgroundColor(GxEPD_RED);
  // без H% колонка T уезжает вправо — имени больше места
  const int xName = 6, xH = 152;
  const int xT = g_cfg.homeShowH ? 118 : xH;
  {
    char title[40];
    snprintf(title, sizeof(title), "%s", g_cfg.titleHome);
    fitUtf8(title, xT - 8);
    textAt(6, wxBottom + 12, title, GxEPD_WHITE);
    snprintf(title, sizeof(title), "%s", g_cfg.titleClim);
    fitUtf8(title, 108);
    textAt(midX + 6, wxBottom + 12, title, GxEPD_WHITE);
  }
  textAt(xT, wxBottom + 12, "T°", GxEPD_WHITE);
  if (g_cfg.homeShowH) textAt(xH, wxBottom + 12, "H%", GxEPD_WHITE);
  textAt(midX + 118, wxBottom + 12, g_cfg.climCurrent ? "T°" : "уст", GxEPD_WHITE);
  textAt(W - 36, wxBottom + 12, "вкл", GxEPD_WHITE);
  u8g2.setBackgroundColor(GxEPD_WHITE);

  g.drawFastVLine(midX - 1, wxBottom + 1, H - footH - wxBottom - 1, GxEPD_BLACK);
  g.drawFastVLine(midX, wxBottom + 1, H - footH - wxBottom - 1, GxEPD_RED);

  const int listTop = wxBottom + 28;
  const int rowH = 16;
  const int maxRows = (H - footH - listTop) / rowH;
  int shown = 0;
  if (w.roomN <= 0) {
    u8g2.setFont(u8g2_font_6x12_t_cyrillic);
    textAt(xName, listTop + 12, "нет MQTT", GxEPD_BLACK);
  } else {
    for (int i = 0; i < w.roomN && shown < maxRows; i++) {
      const RoomSnap &r = w.rooms[i];
      int16_t ry = listTop + shown * rowH + 11;
      u8g2.setFont(u8g2_font_6x12_t_cyrillic);
      char nm[40];
      strncpy(nm, r.name[0] ? r.name : "—", sizeof(nm) - 1);
      nm[sizeof(nm) - 1] = 0;
      fitUtf8(nm, xT - xName - 4);
      textAt(xName, ry, nm, GxEPD_BLACK);
      if (!isnan(r.t)) {
        snprintf(buf, sizeof(buf), "%.1f", r.t);
        textAt(xT, ry, buf, GxEPD_BLACK);
      } else {
        textAt(xT, ry, "--", GxEPD_RED);
      }
      if (g_cfg.homeShowH) {
        if (!isnan(r.h)) {
          snprintf(buf, sizeof(buf), "%.0f", r.h);
          textAt(xH, ry, buf, GxEPD_BLACK);
        } else {
          textAt(xH, ry, "--", GxEPD_RED);
        }
      }
      if (shown + 1 < maxRows && i + 1 < w.roomN)
        g.drawFastHLine(4, ry + 3, leftW - 8, GxEPD_BLACK);
      shown++;
    }
  }

  // правый список: пол / AC
  const int cxKind = midX + 6;
  const int cxName = midX + 36;
  const int cxTT = midX + 108;
  const int cxOn = W - 36;
  int cshown = 0;
  if (w.ctrlN <= 0) {
    u8g2.setFont(u8g2_font_6x12_t_cyrillic);
    textAt(cxKind, listTop + 12, "—", GxEPD_BLACK);
  } else {
    for (int i = 0; i < w.ctrlN && cshown < maxRows; i++) {
      const CtrlSnap &c = w.ctrls[i];
      int16_t ry = listTop + cshown * rowH + 11;
      u8g2.setFont(u8g2_font_6x12_t_cyrillic);
      textAt(cxKind, ry, c.kind[0] ? c.kind : "?", GxEPD_BLACK);

      char nm[40];
      strncpy(nm, c.name[0] ? c.name : "—", sizeof(nm) - 1);
      nm[sizeof(nm) - 1] = 0;
      fitUtf8(nm, cxTT - cxName - 4);
      textAt(cxName, ry, nm, GxEPD_BLACK);

      // одно число — стрелка → в 6x12 часто нет глифа → цифры слипались
      const float first = g_cfg.climCurrent ? c.t : c.target;
      const float second = g_cfg.climCurrent ? c.target : c.t;
      if (!isnan(first))
        snprintf(buf, sizeof(buf), "%.0f", first);
      else if (!isnan(second))
        snprintf(buf, sizeof(buf), "%.0f", second);
      else
        snprintf(buf, sizeof(buf), "--");
      textAt(cxTT, ry, buf, GxEPD_BLACK);

      if (c.hasOn)
        textAt(cxOn, ry, c.on ? "вкл" : "выкл", c.on ? GxEPD_RED : GxEPD_BLACK);
      else
        textAt(cxOn, ry, "—", GxEPD_BLACK);

      if (cshown + 1 < maxRows && i + 1 < w.ctrlN)
        g.drawFastHLine(midX + 4, ry + 3, rightW - 8, GxEPD_BLACK);
      cshown++;
    }
  }

  // footer
  g.fillRect(0, H - footH, W, footH, GxEPD_BLACK);
  u8g2.setFont(u8g2_font_6x12_t_cyrillic);
  u8g2.setBackgroundColor(GxEPD_BLACK);
  snprintf(buf, sizeof(buf), "%s  RSSI %d", FW_VERSION, w.rssi);
  textAt(6, H - 4, buf, GxEPD_WHITE);
  snprintf(buf, sizeof(buf), "%s%s%s", w.ip, (w.ip[0] && DEF_FOOTER_TAG[0]) ? " " : "",
           DEF_FOOTER_TAG);
  if (buf[0]) textAt(W - u8g2.getUTF8Width(buf) - 6, H - 4, buf, GxEPD_WHITE);
  u8g2.setBackgroundColor(GxEPD_WHITE);
}

static void drawWeather(const Wx &w) {
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    renderWeather(display, w);
  } while (display.nextPage());
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== weather today RU ===");
  Serial.printf("PSRAM free=%u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  pinMode(PIN_BOOT, INPUT_PULLUP);
  bool forceAp = digitalRead(PIN_BOOT) == LOW;
  loadCfg();

  SPI.begin(PIN_SCK, -1, PIN_MOSI, PIN_CS);
  display.init(115200, true, 2, false);
  display.epd2.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));

  static Wx w;  // не на стеке loopTask
  w = Wx{};
  g_wx = &w;

  if (!wifiConnect()) {
    // без STA — всё равно поднимем SoftAP для настроек
    char apLine[48];
    snprintf(apLine, sizeof(apLine), "pass %s", g_cfg.apPass);
    drawFail("Wi-Fi ошибка", "SoftAP nm-epd-setup", apLine);
    display.hibernate();
    runConfigPortal(true, 180);
    if (g_cfg.mains) ESP.restart();
    esp_sleep_enable_timer_wakeup(3ULL * 60 * 1000000ULL);
    esp_deep_sleep_start();
  }

  syncTime();
  {
    String ip = WiFi.localIP().toString();
    strncpy(w.ip, ip.c_str(), sizeof(w.ip) - 1);
    w.rssi = WiFi.RSSI();
  }
  // сначала погода (JSON), потом MQTT — иначе куча фрагментируется и hourly пустой
  if (!fetchWeather(w)) {
    drawFail("нет данных", g_status.c_str(), w.ip);
  } else {
    fetchSprutMqtt(w);
    Serial.printf("WX %.0f %s feels=%.0f H=%d wind=%.0f ip=%s rooms=%d ctrls=%d soon=%d\n", w.temp,
                  w.desc, w.feels, w.humidity, w.wind, w.ip, w.roomN, w.ctrlN, w.soonN);
    drawWeather(w);
  }

  display.hibernate();
  const uint32_t sleepSec = sleepSeconds(w.ok);
  Serial.printf("next wake ~%s (%us) mode=%s\n", g_nextWake, (unsigned)sleepSec,
                g_cfg.mains ? "mains" : "battery");

  // от сети: портал живёт весь интервал, потом перезапуск = новый цикл
  if (g_cfg.mains) {
    runConfigPortal(forceAp, sleepSec);
    ESP.restart();
  }

  // окно настроек: http://<IP>/  (IP на чёрной строке)
  runConfigPortal(forceAp, forceAp ? 180 : g_cfg.cfgListenSec);

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_sleep_enable_timer_wakeup((uint64_t)sleepSec * 1000000ULL);
  esp_deep_sleep_start();
}

void loop() {}
