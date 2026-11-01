// Livico: WorkingTimer のタイマーと、PC1〜PC3 の負荷・温度 (InfoPanel の 02 SYSTEMS) を 1 つの画面に出す
// 2 画面目はタイマーの代わりに目覚まし。3・4 画面目はタイマー・目覚ましを選びやすくした設定の画面 (SW1 長押しで切り替え)
// MQTT・アラート・描画の部品は InfoPanel から、タイマーの動きは WorkingTimer から持ってきている
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "WiFiSetup.h"
#include "config.h" // ピン・液晶・音
#include "sound.h"

// ---- WiFi (起動時に WiFiManager の設定ポータルで設定する。WiFiSetup.h は WorkingTimer・InfoPanel と同じファイル) ----
#define DEVICE_BASE_NAME "Livico" // 後ろに MAC の末尾 4 桁を付けて AP 名・ホスト名・MQTT クライアント ID に使う (例 Livico-3FA2)
#define WIFI_PORTAL_SEC  180      // 設定モードの上限。過ぎたらオフラインでタイマーとして動く

// ---- MQTT (仕様は WorkingTimer の MQTT_TOPICS.md が唯一の正) ----
#define MQTT_HOST_DEFAULT "192.168.11.32" // 設定ページの「MQTT server」欄の初期値。空欄で保存すると MQTT を使わない
#define MQTT_PORT         1883
#define MQTT_CALL_TOPIC   "doorbell/call/+" // 呼び出しベル (受け取るだけ)。1 つの部屋のベルだけにするなら doorbell/call/device3 など

#define SCREEN_W 320
#define SCREEN_H 240

Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
Arduino_GFX *panel = new Arduino_ST7789(bus, TFT_RST, TFT_ROTATION, TFT_IPS, 240, 320);
// 画面全体のキャンバス (320x240x2 = 150KB、PSRAM に取る) に描いてから一度に送る
Arduino_Canvas *cv = new Arduino_Canvas(SCREEN_W, SCREEN_H, panel);

WiFiClient mqttNet;
PubSubClient mqtt(mqttNet);
Preferences prefs;
String deviceName;                 // makeDeviceName() の値 (例 Livico-3FA2)。MQTT のクライアント ID も兼ねる
char mqttHost[41] = "";            // 設定ページで決めたブローカー。空なら MQTT を使わない
const char *setupApName = nullptr; // WiFi の設定モード中は AP 名 (画面を設定モードの表示にする)

bool mqttEnabled() { return mqttHost[0] != 0; }

const char *MQTT_SUBS[] = {"info/pc/+/status", "info/pc/+/sys", "info/pc/+/comfy", MQTT_CALL_TOPIC};

// ---- 色 (InfoPanel と同じ。暗い紺の地に、シアンを差し色にする) ----
uint16_t c565(uint32_t rgb) { return ((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F); }
const uint32_t C_BG = 0x04070C, C_PANEL = 0x0A111C, C_LINE = 0x1C3448, C_SEGOFF = 0x0F1C28,
               C_DIM = 0x5A7084, C_TEXT = 0xD8E6F0, C_CYAN = 0x00D0FF,
               C_GREEN = 0x30E080, C_YELLOW = 0xF0C020, C_AMBER = 0xFFA800, C_RED = 0xFF4050;

// a と b を t (0〜1) で混ぜる
uint32_t mix(uint32_t a, uint32_t b, float t)
{
  t = constrain(t, 0.0f, 1.0f);
  uint32_t out = 0;
  for (int sh = 0; sh <= 16; sh += 8)
  {
    int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
    out |= (uint32_t)(ca + (cb - ca) * t) << sh;
  }
  return out;
}

bool blinkOn() { return millis() % 1000 < 500; }

// ---- PC の情報 (info/pc/<PC>/status・sys・comfy) ----
struct PcInfo
{
  const char *name;
  const char *nick; // 利用者が付けた愛称 (名前の横に控えめに出す)
  uint32_t rgb;     // この PC を表す色
  char status[10];  // online / offline / sleep / lost、未受信は空
  bool statusBase;  // 最初の値 (retain) を受け取った。以後の変化だけ知らせる
  int cpu, cpuT, ram, gpu, gpuT;
  float vram, vramTot;
  uint32_t sysAt;   // sys を最後に受け取った millis。0 は未受信
  char comfy[10];   // off / idle / run / error / down / stalled、未受信は空
  bool comfyBase;
  int q, step, steps, el;
};
PcInfo pcs[] = {
    {"PC1", "CONDOR", 0x00D0FF},
    {"PC2", "FALCON", 0xFF4FD8},
    {"PC3", "KIWI", 0xFFB020},
};
const int PC_N = sizeof(pcs) / sizeof(pcs[0]);

void pcReset(PcInfo &pc)
{
  pc.status[0] = pc.comfy[0] = 0;
  pc.statusBase = pc.comfyBase = false;
  pc.cpu = pc.cpuT = pc.ram = pc.gpu = pc.gpuT = -1;
  pc.vram = pc.vramTot = -1;
  pc.sysAt = 0;
  pc.q = pc.step = pc.steps = pc.el = -1;
}

// sys は 5 秒ごとに来る。15 秒来なければ古いとみなして表示しない
bool sysFresh(const PcInfo &pc) { return pc.sysAt && millis() - pc.sysAt < 15000; }
bool isRun(const PcInfo &pc) { return !strcmp(pc.comfy, "run"); }

// ---- 時刻 ----
bool timeSynced(struct tm &t)
{
  time_t now = time(nullptr);
  localtime_r(&now, &t);
  return t.tm_year >= (2024 - 1900);
}

void nowHHMM(char *buf)
{
  struct tm t;
  if (timeSynced(t))
    strftime(buf, 6, "%H:%M", &t);
  else
    strcpy(buf, "--:--");
}

// 時刻がまだないときの表示。オフライン (設定モードが時間切れ) なら OFFLINE
const char *netWaitLabel()
{
  if (WiFi.status() == WL_CONNECTED)
    return "NTP SYNC...";
  return wifiSetupOffline() ? "OFFLINE" : "WIFI...";
}

// 秒を 3m12s / 1h23m の形に
void fmtDur(char *buf, size_t n, uint32_t sec)
{
  if (sec < 3600)
    snprintf(buf, n, "%um%02us", (unsigned)(sec / 60), (unsigned)(sec % 60));
  else
    snprintf(buf, n, "%uh%02um", (unsigned)(sec / 3600), (unsigned)(sec / 60 % 60));
}

// ---- 音量 (鳴らし方は sound_speaker.cpp) ----
const char *VOL_NAMES[VOL_N] = {"HIGH", "MID", "LOW", "OFF"};
int volLevel = VOL_LOW; // 保存した値がなければ LOW
bool muted() { return volLevel == VOL_OFF; }

// ---- スイッチ: 同じ状態が 25ms 続いたら確定 (チャタリング除去)。0.7 秒押し続けると長押し ----
enum BtnEv
{
  EV_NONE,
  EV_SHORT, // 離したとき (長押しにならなかった)
  EV_LONG,  // 押し続けて 0.7 秒たったとき
};
struct Button
{
  int8_t pin;
  bool down, raw, longFired;
  uint32_t changedAt, downAt;
};
Button sw1 = {SW1_PIN}, sw2 = {SW2_PIN}, sw3 = {SW3_PIN};
Button *const SWS[] = {&sw1, &sw2, &sw3};

BtnEv buttonUpdate(Button &b)
{
  uint32_t now = millis();
  bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.raw)
  {
    b.raw = raw;
    b.changedAt = now;
  }
  else if (raw != b.down && now - b.changedAt >= 25)
  {
    b.down = raw;
    if (raw)
    {
      b.downAt = now;
      b.longFired = false;
    }
    else if (!b.longFired)
      return EV_SHORT;
  }
  if (b.down && !b.longFired && now - b.downAt >= 700)
  {
    b.longFired = true;
    return EV_LONG;
  }
  return EV_NONE;
}

// ---- タイマー (WorkingTimer と同じ動き) ----
const uint16_t PRESETS[] = {1, 3, 10, 25, 40, 60, 90, 120}; // 分
const int PRESET_N = sizeof(PRESETS) / sizeof(PRESETS[0]);

enum TimerState
{
  READY,
  RUNNING,
  PAUSED,
  DONE
};
TimerState state = READY;
int presetIdx = 3;     // 25 分
uint32_t totalMs = 0;
uint32_t remainMs = 0; // READY / PAUSED のときの残り
uint32_t endAt = 0;    // RUNNING のときの終了時刻 (millis)

uint32_t timerRemaining()
{
  if (state == RUNNING)
  {
    int32_t r = (int32_t)(endAt - millis());
    return r > 0 ? r : 0;
  }
  return state == DONE ? 0 : remainMs;
}

void resetTimer()
{
  state = READY;
  totalMs = (uint32_t)PRESETS[presetIdx] * 60000UL;
  remainMs = totalMs;
}

void pauseTimer()
{
  remainMs = timerRemaining();
  state = PAUSED;
}

// 残り時間 (秒は切り上げ: 開始直後は 25:00、終了で 00:00)
void formatRemain(char *buf, uint32_t ms)
{
  uint32_t s = (ms + 999) / 1000;
  sprintf(buf, PRESETS[presetIdx] >= 100 ? "%03lu:%02lu" : "%02lu:%02lu", (unsigned long)(s / 60), (unsigned long)(s % 60));
}

// 残り割合の色: 50% 以上はシアン → 20% でアンバー → 0% で赤
uint32_t phaseRGB(float frac)
{
  if (frac >= 0.5f)
    return C_CYAN;
  if (frac >= 0.2f)
    return mix(C_AMBER, C_CYAN, (frac - 0.2f) / 0.3f);
  return mix(C_RED, C_AMBER, frac / 0.2f);
}

// 今日最後まで終わったタイマーの回数と合計分 (日付が変わると 0 から)
uint32_t doneDay = 0; // 日本時間の 1970-01-01 からの日数
uint16_t doneCount = 0, doneMins = 0;

int32_t todayIndex()
{
  time_t now = time(nullptr);
  if (now < 1600000000)
    return -1; // 時刻がまだ合っていない
  return (now + 9 * 3600) / 86400;
}

void recordDone()
{
  int32_t today = todayIndex();
  if (today < 0)
    return;
  if ((uint32_t)today != doneDay)
  {
    doneCount = doneMins = 0;
    doneDay = today;
  }
  doneCount++;
  doneMins += PRESETS[presetIdx];
  prefs.putUInt("tDay", doneDay);
  prefs.putUShort("tCnt", doneCount);
  prefs.putUShort("tMin", doneMins);
}

uint16_t todayCount() { return todayIndex() == (int32_t)doneDay ? doneCount : 0; }

// ---- 画面 (SW1 長押しで切り替え) ----
enum Page
{
  PG_TIMER,     // 1 画面目: カウントダウンタイマーと PC
  PG_ALARM,     // 2 画面目: 目覚ましと PC
  PG_TIMER_SET, // 3 画面目: タイマーの設定 (現在時刻を大きく、プリセットを大きなボタンで)
  PG_ALARM_SET, // 4 画面目: 目覚ましの設定 (現在時刻を大きく、起きる時刻を大きなボタンで)
  PG_N
};
int page = PG_TIMER;

// ---- 目覚まし (2 画面目と 4 画面目) ----
// 起きる時刻は 6:00 から 30 分おきに 12 個 (11:30 まで) から選ぶ。毎日その時刻に鳴る
const int WAKE_FIRST_MIN = 6 * 60, WAKE_STEP_MIN = 30, WAKE_N = 12;
const uint32_t WAKE_RING_MS = 5UL * 60 * 1000; // 止めなければ 5 分で鳴りやむ
const int WAKE_VOLUME = VOL_MID;                // 目覚ましの音量。SW3 の音量 (OFF も) にかかわらずこの大きさ
int wakeIdx = 0;
bool wakeOn = false;
bool wakeRinging = false;
uint32_t wakeAt = 0, wakeBeepAt = 0, wakeSayAt = 0;
int32_t wakeFiredKey = -1; // 鳴らした日と分 (同じ分に 2 回鳴らさない)

int wakeMin(int i) { return WAKE_FIRST_MIN + i * WAKE_STEP_MIN; }

// ---- アラート (画面中央に重ねて出す。新しいものが来たら置き換える) ----
enum AlertKind
{
  AK_LOST,  // PC の予期せぬ切断
  AK_COMFY, // ComfyUI の error / down / stalled
  AK_CALL,  // 呼び出しベル
};
struct Alert
{
  bool active;
  int pc;
  AlertKind kind;
  uint32_t rgb;
  char title[24], detail[44], at[6];
  uint32_t shownAt;
  uint32_t rangAt; // 呼び出し音を最後に鳴らした millis
  bool offPending; // 呼び出しの OFF が来た (CALL_HOLD_MS たったら閉じる)
} alert;

// 呼び出しは ON と OFF の間が短いので、ON から 20 秒は CALL を出し続け、その間 5 秒ごとに鳴らす
#define CALL_HOLD_MS   20000
#define CALL_REPEAT_MS 5000

void ringCall()
{
  alert.rangAt = millis();
  soundPlay(SND_CALL);
  soundSay("よびだされました。"); // チャイムのあとにしゃべる
}

void raiseAlert(int pc, AlertKind kind, uint32_t rgb, const char *title, const char *detail)
{
  alert.active = true;
  alert.pc = pc;
  alert.kind = kind;
  alert.rgb = rgb;
  strlcpy(alert.title, title, sizeof(alert.title));
  strlcpy(alert.detail, detail, sizeof(alert.detail));
  nowHHMM(alert.at);
  alert.shownAt = millis();
  alert.offPending = false;
  Serial.printf("alert: %s %s\n", title, detail);
  if (kind == AK_CALL)
    ringCall();
  else
    soundPlay(SND_ALERT);
}

// 正常に戻ったら、その PC のアラートを閉じる。呼び出しは CALL_HOLD_MS たつまで閉じない (callUpdate で閉じる)
void retractAlert(int pc, AlertKind kind)
{
  if (!alert.active || alert.pc != pc || alert.kind != kind)
    return;
  if (kind == AK_CALL)
    alert.offPending = true;
  else
    alert.active = false;
}

// 呼び出しの間: 5 秒ごとに鳴らし直し、20 秒たって OFF が来ていれば閉じる (OFF がまだなら来るまで出したまま、音は止める)
void callUpdate()
{
  if (!alert.active || alert.kind != AK_CALL)
    return;
  if (millis() - alert.shownAt >= CALL_HOLD_MS)
  {
    if (alert.offPending)
      alert.active = false;
    return;
  }
  if (millis() - alert.rangAt >= CALL_REPEAT_MS)
    ringCall();
}

// ---- 画面の上に数秒だけ出す通知 ----
char toastMsg[40] = "";
uint32_t toastRgb = 0, toastUntil = 0;

void toast(uint32_t rgb, const char *msg)
{
  strlcpy(toastMsg, msg, sizeof(toastMsg));
  toastRgb = rgb;
  toastUntil = millis() + 4000;
}

// ---- MQTT ----
int findPc(const char *name, size_t len)
{
  for (int i = 0; i < PC_N; i++)
    if (strlen(pcs[i].name) == len && strncmp(pcs[i].name, name, len) == 0)
      return i;
  return -1;
}

void upper(char *dst, const char *src, size_t n)
{
  size_t i = 0;
  for (; src[i] && i < n - 1; i++)
    dst[i] = toupper((unsigned char)src[i]);
  dst[i] = 0;
}

void jobDone(int i, int el)
{
  char dur[12] = "", msg[40];
  if (el >= 0)
    fmtDur(dur, sizeof(dur), el);
  snprintf(msg, sizeof(msg), "%s JOB DONE %s", pcs[i].name, dur);
  Serial.printf("comfy: %s\n", msg);
  toast(pcs[i].rgb, msg);
  soundPlay(SND_DONE);
}

void handleStatus(int i, const byte *payload, unsigned int length)
{
  PcInfo &pc = pcs[i];
  char s[10];
  size_t n = min((size_t)length, sizeof(s) - 1);
  memcpy(s, payload, n);
  s[n] = 0;
  // 予期せぬ切断 (online → lost) だけアラートにする。offline・sleep は出すだけ。最初の値 (retain) は基準値
  if (pc.statusBase && !strcmp(pc.status, "online") && !strcmp(s, "lost"))
    raiseAlert(i, AK_LOST, C_RED, (String(pc.name) + " LOST").c_str(), "UNEXPECTED DISCONNECT");
  strcpy(pc.status, s);
  pc.statusBase = true;
  if (!strcmp(s, "online"))
    retractAlert(i, AK_LOST);
}

void handleSys(int i, const byte *payload, unsigned int length)
{
  JsonDocument doc;
  if (deserializeJson(doc, payload, length))
    return;
  PcInfo &pc = pcs[i];
  pc.cpu = doc["cpu"] | -1;
  pc.cpuT = doc["cpu_t"] | -1;
  pc.ram = doc["ram"] | -1;
  pc.gpu = doc["gpu"] | -1;
  pc.gpuT = doc["gpu_t"] | -1;
  pc.vram = doc["vram"] | -1.0f;
  pc.vramTot = doc["vram_tot"] | -1.0f;
  pc.sysAt = millis();
}

void handleComfy(int i, const byte *payload, unsigned int length)
{
  PcInfo &pc = pcs[i];
  char st[10] = "";
  int el = -1, step = -1, steps = -1, q = -1;
  JsonDocument doc;
  if (length > 0) // 空は retain の削除 (データなしに戻す)
  {
    if (deserializeJson(doc, payload, length))
      return;
    strlcpy(st, doc["st"] | "", sizeof(st));
    el = doc["el"] | -1;
    step = doc["step"] | -1;
    steps = doc["steps"] | -1;
    q = doc["q"] | -1;
  }

  if (pc.comfyBase)
  {
    bool wasRun = isRun(pc), nowRun = !strcmp(st, "run");
    if (wasRun && !strcmp(st, "idle"))
      jobDone(i, pc.el);
    else if (wasRun && nowRun && el >= 0 && pc.el >= 0 && el + 2 < pc.el)
      jobDone(i, pc.el); // 待ち行列の次のジョブに移った (経過秒が戻った)

    bool bad = !strcmp(st, "error") || !strcmp(st, "down") || !strcmp(st, "stalled");
    if (bad && strcmp(st, pc.comfy) != 0)
    {
      char title[24], detail[44], up[10];
      upper(up, st, sizeof(up));
      snprintf(title, sizeof(title), "%s %s", pc.name, up);
      if (!strcmp(st, "error"))
        snprintf(detail, sizeof(detail), "%s %s", (const char *)(doc["node"] | ""), (const char *)(doc["err"] | ""));
      else if (!strcmp(st, "down"))
        strcpy(detail, "COMFYUI NOT RESPONDING");
      else
        snprintf(detail, sizeof(detail), "%s NO PROGRESS", (const char *)(doc["node"] | ""));
      raiseAlert(i, AK_COMFY, C_RED, title, detail);
    }
  }

  if (!strcmp(st, "idle") || !strcmp(st, "run"))
    retractAlert(i, AK_COMFY);

  strcpy(pc.comfy, st);
  pc.el = el;
  pc.step = step;
  pc.steps = steps;
  pc.q = q;
  pc.comfyBase = true;
}

void handleDoorbell(const char *dev, const byte *payload, unsigned int length)
{
  char p[8];
  size_t n = min((size_t)length, sizeof(p) - 1);
  memcpy(p, payload, n);
  p[n] = 0;
  char up[16];
  upper(up, dev, sizeof(up));
  if (!strcmp(p, "ON"))
  {
    // 呼ばれたら、動いているタイマーを止める (再開は SW2 で。WorkingTimer と同じ)
    if (state == RUNNING)
      pauseTimer();
    raiseAlert(-1, AK_CALL, C_YELLOW, "CALL", up);
  }
  else if (!strcmp(p, "OFF"))
    retractAlert(-1, AK_CALL);
  // TIMER は無視する
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  if (strncmp(topic, "doorbell/call/", 14) == 0)
  {
    handleDoorbell(topic + 14, payload, length);
    return;
  }
  // info/pc/<PC>/<kind>
  if (strncmp(topic, "info/pc/", 8) != 0)
    return;
  const char *p = topic + 8;
  const char *slash = strchr(p, '/');
  if (!slash)
    return;
  int i = findPc(p, slash - p);
  if (i < 0)
  {
    Serial.printf("mqtt: unknown pc in %s\n", topic);
    return;
  }
  const char *kind = slash + 1;
  if (!strcmp(kind, "status"))
    handleStatus(i, payload, length);
  else if (!strcmp(kind, "sys"))
    handleSys(i, payload, length);
  else if (!strcmp(kind, "comfy"))
    handleComfy(i, payload, length);
}

// つながるまで 5 秒から 30 秒まで間隔を延ばしながら試す
void mqttUpdate()
{
  static uint32_t lastTry = 0, retryMs = 0;
  static bool wasConnected = false;
  if (!mqttEnabled() || WiFi.status() != WL_CONNECTED)
    return;
  if (!mqtt.connected())
  {
    if (wasConnected)
    {
      wasConnected = false;
      Serial.println("mqtt: disconnected");
    }
    if (retryMs && millis() - lastTry < retryMs)
      return;
    lastTry = millis();
    // クライアント ID は機体ごとに別 (同じ ID の機体が 2 台あると Mosquitto が互いを切断し合う)
    const char *clientId = deviceName.c_str();
    Serial.printf("mqtt: connecting to %s:%d as %s ...\n", mqttHost, MQTT_PORT, clientId);
    if (!mqtt.connect(clientId))
    {
      retryMs = retryMs ? min(retryMs * 2, (uint32_t)30000) : 5000;
      Serial.printf("mqtt: connect failed (state %d), retry in %lus\n", mqtt.state(), (unsigned long)(retryMs / 1000));
      return;
    }
    retryMs = 5000;
    wasConnected = true;
    // 再接続のあとに届く retain は基準値として受け直す (古い lost や error でアラートを出さない)
    for (PcInfo &pc : pcs)
      pc.statusBase = pc.comfyBase = false;
    for (const char *t : MQTT_SUBS)
      Serial.printf("mqtt: subscribe %s %s\n", t, mqtt.subscribe(t) ? "ok" : "FAILED");
  }
  mqtt.loop();
}

// ---- 描画の部品 (InfoPanel と同じ) ----
// 文字は画面の内側 (x, y >= 0) にだけ書く。Arduino_GFX の文字描画は上端・左端のはみ出しを切り取らず、
// キャンバスの外のメモリを壊すため (WorkingTimer で再起動ループの原因になった)
void text(int x, int y, int size, uint32_t rgb, const char *s)
{
  if (x < 0 || y < 0)
    return;
  cv->setTextSize(size);
  cv->setTextColor(c565(rgb));
  cv->setCursor(x, y);
  cv->print(s);
}

int textW(int size, const char *s) { return strlen(s) * 6 * size; }
void textR(int xr, int y, int size, uint32_t rgb, const char *s) { text(xr - textW(size, s), y, size, rgb, s); }
void textC(int xc, int y, int size, uint32_t rgb, const char *s) { text(xc - textW(size, s) / 2, y, size, rgb, s); }

// 角に差し色のかぎを付けた枠
void frame(int x, int y, int w, int h, uint32_t accent)
{
  cv->fillRect(x, y, w, h, c565(C_PANEL));
  cv->drawRect(x, y, w, h, c565(C_LINE));
  uint16_t a = c565(accent);
  const int k = 6;
  cv->drawFastHLine(x, y, k, a);
  cv->drawFastVLine(x, y, k, a);
  cv->drawFastHLine(x + w - k, y, k, a);
  cv->drawFastVLine(x + w - 1, y, k, a);
  cv->drawFastHLine(x, y + h - 1, k, a);
  cv->drawFastVLine(x, y + h - k, k, a);
  cv->drawFastHLine(x + w - k, y + h - 1, k, a);
  cv->drawFastVLine(x + w - 1, y + h - k, k, a);
}

// 区切りのある横棒 (1 区切り 4px + すき間 1px)
void segBar(int x, int y, int w, int h, float pct, uint32_t rgb)
{
  int n = w / 5;
  int lit = pct < 0 ? 0 : (int)(n * min(pct, 100.0f) / 100.0f + 0.5f);
  for (int i = 0; i < n; i++)
    cv->fillRect(x + i * 5, y, 4, h, c565(i < lit ? rgb : C_SEGOFF));
}

// 愛称の色: その PC の色を灰色に寄せて控えめにする
uint32_t nickRGB(const PcInfo &pc) { return mix(pc.rgb, C_DIM, 0.6f); }

// 負荷・温度の色: warn 以上で黄、crit 以上で赤
uint32_t levelRGB(int v, int warn, int crit, uint32_t normal = C_CYAN)
{
  if (v < 0)
    return C_DIM;
  return v >= crit ? C_RED : v >= warn ? C_YELLOW : normal;
}

uint32_t statusRGB(const char *st)
{
  if (!st[0])
    return C_DIM;
  if (!strcmp(st, "online") || !strcmp(st, "idle"))
    return C_GREEN;
  if (!strcmp(st, "run"))
    return C_CYAN;
  if (!strcmp(st, "lost") || !strcmp(st, "error") || !strcmp(st, "down") || !strcmp(st, "stalled"))
    return C_RED;
  if (!strcmp(st, "sleep"))
    return C_YELLOW;
  return C_DIM; // offline / off
}

// 7 セグの 1 区画。中心線から離れた行ほど両端を短くして、先を尖らせる
void segH(int x0, int x1, int yc, int t, uint16_t c)
{
  for (int r = 0; r < t; r++)
  {
    int in = abs(2 * r - (t - 1)) / 2;
    cv->drawFastHLine(x0 + in, yc - t / 2 + r, x1 - x0 + 1 - 2 * in, c);
  }
}
void segV(int xc, int y0, int y1, int t, uint16_t c)
{
  for (int r = 0; r < t; r++)
  {
    int in = abs(2 * r - (t - 1)) / 2;
    cv->drawFastVLine(xc - t / 2 + r, y0 + in, y1 - y0 + 1 - 2 * in, c);
  }
}

// 7 セグメントの数字 (細い線で先を尖らせる)。消えている区画も薄く描く。d < 0 は全部消す
void seg7(int x, int y, int w, int h, int t, int d, uint32_t on)
{
  static const uint8_t SEG[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
  uint8_t m = d >= 0 && d <= 9 ? SEG[d] : 0;
  uint16_t con = c565(on), coff = c565(C_SEGOFF);
  int xl = x + t / 2, xr = x + w - 1 - t / 2;
  int yt = y + t / 2, ym = y + h / 2, yb = y + h - 1 - t / 2;
  const int g = 2;
  segH(xl + g, xr - g, yt, t, m & 0x01 ? con : coff); // a
  segV(xr, yt + g, ym - g, t, m & 0x02 ? con : coff); // b
  segV(xr, ym + g, yb - g, t, m & 0x04 ? con : coff); // c
  segH(xl + g, xr - g, yb, t, m & 0x08 ? con : coff); // d
  segV(xl, ym + g, yb - g, t, m & 0x10 ? con : coff); // e
  segV(xl, yt + g, ym - g, t, m & 0x20 ? con : coff); // f
  segH(xl + g, xr - g, ym, t, m & 0x40 ? con : coff); // g
}

// 数字と ':' の並びを 7 セグで描く ('-' は全部消えた数字)。描いた幅を返す
int segText(int x, int y, int w, int h, int t, int gap, const char *s, uint32_t on, uint32_t colon)
{
  int x0 = x;
  int dot = t + 1;
  for (const char *p = s; *p; p++)
  {
    if (*p == ':')
    {
      cv->fillRect(x, y + h * 3 / 10 - dot / 2, dot, dot, c565(colon));
      cv->fillRect(x, y + h * 7 / 10 - dot / 2, dot, dot, c565(colon));
      x += dot + gap;
      continue;
    }
    seg7(x, y, w, h, t, *p >= '0' && *p <= '9' ? *p - '0' : -1, on);
    x += w + gap;
  }
  return x - x0 - gap;
}

// segText で描いたときの幅
int segTextW(int w, int t, int gap, const char *s)
{
  int x = 0;
  for (const char *p = s; *p; p++)
    x += (*p == ':' ? t + 1 : w) + gap;
  return x - gap;
}

// ---- 画面 ----
// 上の段 (y 0〜93): 左に現在時刻 (大)、右にタイマーか目覚まし。一番上の行の左は日付・音量・接続
const int TIMER_H = 94;

// 一番上の行の左: 日付・曜日、音量、接続ランプ (緑 = WiFi と MQTT、黄 = WiFi だけ、赤 = WiFi なし)
void drawStatusLine(bool synced, const struct tm &t)
{
  char buf[16];
  if (synced)
  {
    static const char *WDAY[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
    snprintf(buf, sizeof(buf), "%02d-%02d", t.tm_mon + 1, t.tm_mday);
    text(8, 7, 1, C_DIM, buf);
    text(44, 7, 1, t.tm_wday == 0 ? C_RED : t.tm_wday == 6 ? C_CYAN : C_DIM, WDAY[t.tm_wday]);
  }
  else
    text(8, 7, 1, C_DIM, netWaitLabel());
  text(80, 7, 1, muted() ? C_RED : C_DIM, muted() ? "MUTE" : VOL_NAMES[volLevel]);
  bool wifiOk = WiFi.status() == WL_CONNECTED;
  bool mqttOk = mqttEnabled() && mqtt.state() == MQTT_CONNECTED;
  cv->fillRect(108, 8, 5, 5, c565(wifiOk && (mqttOk || !mqttEnabled()) ? C_GREEN : wifiOk ? C_YELLOW : C_RED));
}

// 左: 現在時刻 (大きな 7 セグ) と秒
void drawBigClock(bool synced, const struct tm &t)
{
  char buf[8];
  if (synced)
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  else
    strcpy(buf, "--:--");
  bool colon = !synced || blinkOn();
  int w = segText(8, 22, 34, 56, 5, 6, buf, C_TEXT, colon ? C_CYAN : C_SEGOFF);
  if (synced)
  {
    snprintf(buf, sizeof(buf), "%02d", t.tm_sec);
    text(8 + w + 6, 64, 2, C_CYAN, buf);
  }
}

// 1 画面目: カウントダウンタイマー
void drawTimer()
{
  char buf[24];
  uint32_t rem = timerRemaining();
  float frac = totalMs ? (float)rem / totalMs : 1.0f;
  uint32_t accent = state == DONE ? C_RED : phaseRGB(frac);

  struct tm t;
  bool synced = timeSynced(t);
  drawStatusLine(synced, t);
  drawBigClock(synced, t);

  // 一番上の行の右: プリセット (分)
  for (int i = 0; i < PRESET_N; i++)
  {
    int x = 123 + i * 24, y = 4, w = 21, h = 13;
    snprintf(buf, sizeof(buf), "%u", PRESETS[i]);
    if (i == presetIdx)
    {
      cv->fillRect(x, y, w, h, c565(accent));
      textC(x + w / 2 + 1, y + 3, 1, C_BG, buf);
    }
    else
    {
      cv->drawRect(x, y, w, h, c565(C_LINE));
      textC(x + w / 2 + 1, y + 3, 1, C_DIM, buf);
    }
  }

  // 右: 残り時間。120 分のときは 5 桁になるので数字を細くする
  const int RX = 214;
  formatRemain(buf, rem);
  uint32_t on = state == DONE ? (blinkOn() ? C_RED : C_SEGOFF) : state == PAUSED ? mix(accent, 0x787878, 0.5f) : accent;
  segText(RX, 22, strlen(buf) > 5 ? 14 : 18, 30, 3, 3, buf, on, on);

  // 状態 (RUN のときは左に点滅する丸)
  static const char *STATE_NAMES[] = {"READY", "RUN", "PAUSE", "DONE"};
  uint32_t sc = state == RUNNING ? accent : state == PAUSED ? (blinkOn() ? C_AMBER : C_DIM) : state == DONE ? (blinkOn() ? C_RED : C_SEGOFF) : C_TEXT;
  if (state == RUNNING && blinkOn())
    cv->fillCircle(RX + 3, 63, 3, c565(sc));
  text(RX + 11, 56, 2, sc, STATE_NAMES[state]);

  // 終わる時刻 (動いているとき) か、選んだ長さ。右は今日終えた回数
  if (state == RUNNING && synced)
  {
    time_t e = time(nullptr) + (rem + 999) / 1000;
    struct tm et;
    localtime_r(&e, &et);
    snprintf(buf, sizeof(buf), "END %02d:%02d", et.tm_hour, et.tm_min);
    text(RX, 74, 1, accent, buf);
  }
  else
  {
    snprintf(buf, sizeof(buf), "%u MIN", PRESETS[presetIdx]);
    text(RX, 74, 1, C_DIM, buf);
  }
  snprintf(buf, sizeof(buf), "TODAY %u", todayCount());
  textR(314, 74, 1, todayCount() ? C_TEXT : C_DIM, buf);

  // タイマーの進み具合 (残りの割合)
  segBar(8, 84, 304, 6, frac * 100, accent);

  // 終わったら上の段の縁を赤く点滅
  if (state == DONE && blinkOn())
    for (int k = 0; k < 3; k++)
      cv->drawRect(k, k, SCREEN_W - 2 * k, TIMER_H - 2 * k, c565(C_RED));
}

// 2 画面目: 目覚まし。右に起きる時刻の一覧 (3 列 x 4 行)
void drawAlarm()
{
  char buf[32];
  struct tm t;
  bool synced = timeSynced(t);
  drawStatusLine(synced, t);
  drawBigClock(synced, t);

  uint32_t accent = wakeOn ? C_YELLOW : C_DIM;
  textR(314, 7, 1, accent, wakeOn ? "ALARM ON" : "ALARM OFF");

  for (int i = 0; i < WAKE_N; i++)
  {
    int x = 206 + (i % 3) * 36, y = 20 + (i / 3) * 14, w = 33, h = 12;
    int m = wakeMin(i);
    snprintf(buf, sizeof(buf), "%d:%02d", m / 60, m % 60);
    if (i == wakeIdx)
    {
      cv->fillRect(x, y, w, h, c565(accent));
      textC(x + w / 2 + 1, y + 2, 1, C_BG, buf);
    }
    else
    {
      cv->drawRect(x, y, w, h, c565(C_LINE));
      textC(x + w / 2 + 1, y + 2, 1, C_DIM, buf);
    }
  }

  // 下の行: 次に鳴る時刻とあと何時間か。右は動いているタイマー
  cv->drawFastHLine(8, 79, 304, c565(C_LINE));
  int m = wakeMin(wakeIdx);
  if (!wakeOn)
    snprintf(buf, sizeof(buf), "WAKE %02d:%02d  OFF", m / 60, m % 60);
  else if (synced)
  {
    int left = (m - (t.tm_hour * 60 + t.tm_min) + 1440 - 1) % 1440 + 1; // 次に鳴るまでの分 (1〜1440)
    snprintf(buf, sizeof(buf), "WAKE %02d:%02d  IN %dH%02dM", m / 60, m % 60, left / 60, left % 60);
  }
  else
    snprintf(buf, sizeof(buf), "WAKE %02d:%02d", m / 60, m % 60);
  text(8, 83, 1, wakeOn ? C_TEXT : C_DIM, buf);
  if (state != READY)
  {
    char rem[10];
    formatRemain(rem, timerRemaining());
    snprintf(buf, sizeof(buf), "TIMER %s", state == DONE ? "DONE" : rem);
    float frac = totalMs ? (float)timerRemaining() / totalMs : 1.0f;
    textR(314, 83, 1, state == DONE ? C_RED : state == PAUSED ? C_DIM : phaseRGB(frac), buf);
  }
}

// 3・4 画面目: 現在時刻をいちばん大きく (上の真ん中) と秒
void drawHugeClock(bool synced, const struct tm &t)
{
  char buf[8];
  if (synced)
    snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  else
    strcpy(buf, "--:--");
  bool colon = !synced || blinkOn();
  const int W = 50, H = 90, T = 8, GAP = 8;
  int x = (SCREEN_W - segTextW(W, T, GAP, buf) - (synced ? 8 + 36 : 0)) / 2;
  int w = segText(x, 24, W, H, T, GAP, buf, C_TEXT, colon ? C_CYAN : C_SEGOFF);
  if (synced)
  {
    snprintf(buf, sizeof(buf), "%02d", t.tm_sec);
    text(x + w + 8, 24 + H - 24, 3, C_CYAN, buf);
  }
}

// 3 画面目: タイマーの設定。下にプリセットを 4 列 x 2 行の大きなボタンで
void drawTimerSet()
{
  char buf[24];
  uint32_t rem = timerRemaining();
  float frac = totalMs ? (float)rem / totalMs : 1.0f;
  uint32_t accent = state == DONE ? C_RED : phaseRGB(frac);

  struct tm t;
  bool synced = timeSynced(t);
  drawStatusLine(synced, t);
  int m = wakeMin(wakeIdx);
  if (wakeOn)
    snprintf(buf, sizeof(buf), "ALARM %d:%02d", m / 60, m % 60);
  else
    strcpy(buf, "ALARM OFF");
  textR(314, 7, 1, wakeOn ? C_YELLOW : C_DIM, buf);
  drawHugeClock(synced, t);

  // 残り時間 (小さな 7 セグ)・状態・終わる時刻か選んだ長さ・今日終えた回数
  cv->drawFastHLine(8, 123, 304, c565(C_LINE));
  formatRemain(buf, rem);
  uint32_t on = state == DONE ? (blinkOn() ? C_RED : C_SEGOFF) : state == PAUSED ? mix(accent, 0x787878, 0.5f) : accent;
  int w = segText(8, 129, strlen(buf) > 5 ? 14 : 18, 28, 3, 3, buf, on, on);
  static const char *STATE_NAMES[] = {"READY", "RUN", "PAUSE", "DONE"};
  uint32_t sc = state == RUNNING ? accent : state == PAUSED ? (blinkOn() ? C_AMBER : C_DIM) : state == DONE ? (blinkOn() ? C_RED : C_SEGOFF) : C_TEXT;
  if (state == RUNNING && blinkOn())
    cv->fillCircle(8 + w + 12, 142, 3, c565(sc));
  text(8 + w + 20, 135, 2, sc, STATE_NAMES[state]);
  if (state == RUNNING && synced)
  {
    time_t e = time(nullptr) + (rem + 999) / 1000;
    struct tm et;
    localtime_r(&e, &et);
    snprintf(buf, sizeof(buf), "END %02d:%02d", et.tm_hour, et.tm_min);
    textR(314, 131, 1, accent, buf);
  }
  else
  {
    snprintf(buf, sizeof(buf), "%u MIN", PRESETS[presetIdx]);
    textR(314, 131, 1, C_DIM, buf);
  }
  snprintf(buf, sizeof(buf), "TODAY %u", todayCount());
  textR(314, 145, 1, todayCount() ? C_TEXT : C_DIM, buf);
  segBar(8, 160, 304, 4, frac * 100, accent);

  // プリセット (分)
  for (int i = 0; i < PRESET_N; i++)
  {
    int x = 8 + (i % 4) * 77, y = 168 + (i / 4) * 36, bw = 73, bh = 32;
    snprintf(buf, sizeof(buf), "%u", PRESETS[i]);
    if (i == presetIdx)
    {
      cv->fillRect(x, y, bw, bh, c565(accent));
      textC(x + bw / 2 + 1, y + 5, 3, C_BG, buf);
    }
    else
    {
      cv->drawRect(x, y, bw, bh, c565(C_LINE));
      textC(x + bw / 2 + 1, y + 5, 3, C_DIM, buf);
    }
  }

  // 終わったら画面の縁を赤く点滅
  if (state == DONE && blinkOn())
    for (int k = 0; k < 3; k++)
      cv->drawRect(k, k, SCREEN_W - 2 * k, SCREEN_H - 2 * k, c565(C_RED));
}

// 4 画面目: 目覚ましの設定。下に起きる時刻を 4 列 x 3 行の大きなボタンで
void drawAlarmSet()
{
  char buf[32];
  struct tm t;
  bool synced = timeSynced(t);
  drawStatusLine(synced, t);
  if (state != READY)
  {
    char rem[10];
    formatRemain(rem, timerRemaining());
    snprintf(buf, sizeof(buf), "TIMER %s", state == DONE ? "DONE" : rem);
    float frac = totalMs ? (float)timerRemaining() / totalMs : 1.0f;
    textR(314, 7, 1, state == DONE ? C_RED : state == PAUSED ? C_DIM : phaseRGB(frac), buf);
  }
  drawHugeClock(synced, t);

  // ON / OFF と、次に鳴るまで
  cv->drawFastHLine(8, 123, 304, c565(C_LINE));
  uint32_t accent = wakeOn ? C_YELLOW : C_DIM;
  text(8, 131, 2, accent, wakeOn ? "ALARM ON" : "ALARM OFF");
  int m = wakeMin(wakeIdx);
  if (wakeOn && synced)
  {
    int left = (m - (t.tm_hour * 60 + t.tm_min) + 1440 - 1) % 1440 + 1; // 次に鳴るまでの分 (1〜1440)
    snprintf(buf, sizeof(buf), "IN %dH%02dM", left / 60, left % 60);
    textR(314, 131, 2, C_TEXT, buf);
  }

  // 起きる時刻
  for (int i = 0; i < WAKE_N; i++)
  {
    int x = 8 + (i % 4) * 77, y = 154 + (i / 4) * 28, bw = 73, bh = 25;
    int wm = wakeMin(i);
    snprintf(buf, sizeof(buf), "%d:%02d", wm / 60, wm % 60);
    if (i == wakeIdx)
    {
      cv->fillRect(x, y, bw, bh, c565(accent));
      textC(x + bw / 2 + 1, y + 5, 2, C_BG, buf);
    }
    else
    {
      cv->drawRect(x, y, bw, bh, c565(C_LINE));
      textC(x + bw / 2 + 1, y + 5, 2, C_DIM, buf);
    }
  }
}

// ComfyUI の状態を 1 行に (枠の左の欄、15 文字まで)
void comfyLine(const PcInfo &pc, char *buf, size_t n)
{
  if (!pc.comfy[0])
    snprintf(buf, n, "COMFY --");
  else if (isRun(pc) && pc.steps > 0)
    snprintf(buf, n, "RUN %d/%d", pc.step, pc.steps);
  else if (isRun(pc) && pc.el >= 0)
    snprintf(buf, n, "RUN %d:%02d", pc.el / 60, pc.el % 60);
  else if (!strcmp(pc.comfy, "idle") && pc.q > 0)
    snprintf(buf, n, "IDLE Q%d", pc.q);
  else if (!strcmp(pc.comfy, "off"))
    snprintf(buf, n, "COMFY OFF");
  else
  {
    char up[10];
    upper(up, pc.comfy, sizeof(up));
    snprintf(buf, n, "%s", up);
  }
}

// 下の段: 1 台ぶん (InfoPanel の 02 SYSTEMS を低くしたもの)。左に名前・状態・ComfyUI、右に CPU・GPU・RAM・VRAM の棒
void drawPcRow(int i)
{
  PcInfo &pc = pcs[i];
  char buf[32];
  int x = 6, y = TIMER_H + 2 + i * 48, w = 308, h = 46;
  frame(x, y, w, h, pc.rgb);
  text(x + 8, y + 5, 2, pc.rgb, pc.name);
  text(x + 46, y + 12, 1, nickRGB(pc), pc.nick);
  char up[10];
  upper(up, pc.status[0] ? pc.status : "--", sizeof(up));
  text(x + 8, y + 25, 1, statusRGB(pc.status), up);
  comfyLine(pc, buf, 16);
  text(x + 8, y + 35, 1, statusRGB(pc.comfy), buf);

  bool f = sysFresh(pc);
  float vramPct = pc.vramTot > 0 ? 100 * pc.vram / pc.vramTot : -1;
  struct
  {
    const char *label;
    float pct;
    uint32_t rgb;
  } rows[] = {
      {"CPU", (float)pc.cpu, levelRGB(pc.cpu, 80, 95)},
      {"GPU", (float)pc.gpu, levelRGB(pc.gpu, 101, 101)},
      {"RAM", (float)pc.ram, levelRGB(pc.ram, 80, 92)},
      {"VRAM", vramPct, levelRGB(vramPct, 85, 95)},
  };
  for (int r = 0; r < 4; r++)
  {
    int ry = y + 5 + r * 10;
    text(x + 104, ry, 1, C_DIM, rows[r].label);
    segBar(x + 134, ry, 100, 6, f ? rows[r].pct : -1, rows[r].rgb);
    if (!f || rows[r].pct < 0)
    {
      textR(x + w - 8, ry, 1, C_DIM, "--");
      continue;
    }
    uint32_t c = C_TEXT;
    if (r <= 1)
    {
      int temp = r == 0 ? pc.cpuT : pc.gpuT;
      if (temp >= 0)
        snprintf(buf, sizeof(buf), "%3d%% %2dC", (int)rows[r].pct, temp);
      else
        snprintf(buf, sizeof(buf), "%3d%%", (int)rows[r].pct);
      c = levelRGB(temp, 75, 85, C_TEXT);
    }
    else if (r == 2)
      snprintf(buf, sizeof(buf), "%3d%%", (int)rows[r].pct);
    else
      snprintf(buf, sizeof(buf), "%.1f/%.0fG", pc.vram, pc.vramTot);
    textR(x + w - 8, ry, 1, c, buf);
  }
}

// アラートを画面中央に重ねる。枠が点滅する
void drawAlert(const Alert &alert)
{
  int x = 18, y = 62, w = 284, h = 116;
  bool blink = (millis() - alert.shownAt) % 800 < 400;
  cv->fillRect(x, y, w, h, c565(mix(C_BG, alert.rgb, 0.12f)));
  uint16_t b = c565(blink ? alert.rgb : mix(C_BG, alert.rgb, 0.4f));
  cv->drawRect(x, y, w, h, b);
  cv->drawRect(x + 1, y + 1, w - 2, h - 2, b);
  // 斜めの縞 (警告らしく)
  for (int k = 0; k < w - 4; k += 12)
    cv->fillTriangle(x + 2 + k, y + 2, x + 8 + k, y + 2, x + 2 + k, y + 8, c565(mix(C_BG, alert.rgb, 0.5f)));
  textC(SCREEN_W / 2, y + 22, 3, alert.rgb, alert.title);
  textC(SCREEN_W / 2, y + 58, 1, C_TEXT, alert.detail);
  char buf[24];
  snprintf(buf, sizeof(buf), "AT %s", alert.at);
  textC(SCREEN_W / 2, y + 74, 2, C_TEXT, buf);
  textC(SCREEN_W / 2, y + h - 14, 1, C_DIM, "PRESS ANY SWITCH");
}

// 画面の一番上 (プリセットの上) に重ねる
void drawToast()
{
  int h = 20;
  cv->fillRect(0, 0, SCREEN_W, h, c565(C_PANEL));
  cv->drawFastHLine(0, h - 1, SCREEN_W, c565(toastRgb));
  cv->fillRect(0, 0, 4, h, c565(toastRgb));
  text(12, 6, 1, C_TEXT, toastMsg);
}

// WiFi の設定モード (設定ポータルの間は loop() が止まっているので、一度描くだけ)
void drawSetup()
{
  char buf[48];
  cv->fillScreen(c565(C_BG));
  textC(SCREEN_W / 2, 30, 3, C_CYAN, "WIFI SETUP MODE");
  cv->drawFastHLine(16, 62, SCREEN_W - 32, c565(C_LINE));
  textC(SCREEN_W / 2, 80, 1, C_DIM, "CONNECT YOUR PHONE TO");
  textC(SCREEN_W / 2, 96, 3, C_TEXT, setupApName);
  textC(SCREEN_W / 2, 136, 2, C_TEXT, "THEN OPEN 192.168.4.1");
  snprintf(buf, sizeof(buf), "NO SETUP IN %d MIN -> OFFLINE", WIFI_PORTAL_SEC / 60);
  textC(SCREEN_W / 2, 176, 1, C_DIM, buf);
  textC(SCREEN_W / 2, 192, 1, C_DIM, "HOLD SW1 AT BOOT TO RESET WIFI");
  cv->flush();
}

void renderFrame()
{
  if (setupApName)
  {
    drawSetup();
    return;
  }
  cv->fillScreen(c565(C_BG));
  if (page == PG_TIMER_SET)
    drawTimerSet();
  else if (page == PG_ALARM_SET)
    drawAlarmSet();
  else
  {
    if (page == PG_ALARM)
      drawAlarm();
    else
      drawTimer();
    for (int i = 0; i < PC_N; i++)
      drawPcRow(i);
  }
  if (millis() < toastUntil && !alert.active && !wakeRinging)
    drawToast();
  if (wakeRinging) // 目覚ましはほかのアラートより上に出す
  {
    Alert box = {};
    box.rgb = C_YELLOW;
    int m = wakeMin(wakeIdx);
    snprintf(box.title, sizeof(box.title), "ALARM %d:%02d", m / 60, m % 60);
    strcpy(box.detail, "GOOD MORNING");
    nowHHMM(box.at);
    box.shownAt = wakeAt;
    drawAlert(box);
  }
  else if (alert.active)
    drawAlert(alert);
  cv->flush();
}

// ---- タイマーの終了アラーム ----
// 終わったら「ピピピッ」と「じかんに なりました」、そのあと 30 秒まで 1 秒ごとに「ピピピッ」。
// アラート・呼び出し・目覚ましの間は待たせ、閉じたあとに最初から鳴らす (WorkingTimer と同じ)
const uint32_t ALARM_MS = 30000;
bool alarmPending = false, alarmOn = false;
uint32_t alarmAt = 0, alarmBeepAt = 0;

void timerUpdate()
{
  if (state == RUNNING && timerRemaining() == 0)
  {
    state = DONE;
    alarmPending = true;
    recordDone();
    if (page != PG_TIMER_SET) // タイマーの設定の画面ならそのまま
      page = PG_TIMER;
    Serial.printf("timer: done (%u min, today %u)\n", PRESETS[presetIdx], todayCount());
  }
  if (state != DONE)
  {
    alarmPending = alarmOn = false;
    return;
  }
  uint32_t now = millis();
  if (alert.active || wakeRinging)
  {
    if (alarmOn && now - alarmAt < ALARM_MS)
    {
      alarmOn = false;
      alarmPending = true;
    }
    return;
  }
  if (alarmPending)
  {
    alarmPending = false;
    alarmOn = true;
    alarmAt = alarmBeepAt = now;
    soundPlay(SND_TIMER);
    soundSay("じかんに なりました。");
  }
  // しゃべり終わるころ (3 秒) からくり返す
  if (alarmOn && now - alarmAt < ALARM_MS && now - alarmAt >= 3000 && now - alarmBeepAt >= 1000)
  {
    alarmBeepAt = now;
    soundPlay(SND_TIMER);
  }
}

// ---- 目覚ましを鳴らす ----
// 鳴り始めに音と「ろくじ さんじゅっぷんに なりました。 きしょう じかんです。」、そのあと 2 秒ごとに音、30 秒ごとに声。どれかのスイッチで止める
const uint32_t WAKE_SAY_MS = 7000; // しゃべり終わるまで (この間は音を鳴らさない)

// 起きる時刻をしゃべる (時刻は 6:00〜11:30 の 30 分おき)
void sayWakeTime()
{
  static const char *HOURS[] = {"れいじ", "いちじ", "にじ", "さんじ", "よじ", "ごじ", "ろくじ", "しちじ",
                                "はちじ", "くじ", "じゅうじ", "じゅういちじ", "じゅうにじ"};
  int m = wakeMin(wakeIdx);
  char buf[128];
  snprintf(buf, sizeof(buf), "%s%sに なりました。 きしょう じかんです。", HOURS[m / 60 % 13],
           m % 60 == 30 ? " さんじゅっぷん" : "");
  soundSay(buf);
}

void startWake()
{
  wakeRinging = true;
  wakeAt = wakeBeepAt = wakeSayAt = millis();
  if (page != PG_ALARM_SET) // 目覚ましの設定の画面ならそのまま
    page = PG_ALARM;
  soundStop();                 // ほかの音は止めて
  soundFixVolume(WAKE_VOLUME); // 寝るときに音を消していても鳴るように、音量の設定とは別に決めた大きさで
  soundPlay(SND_WAKE);
  sayWakeTime();
  Serial.printf("wake: ringing %d:%02d\n", wakeMin(wakeIdx) / 60, wakeMin(wakeIdx) % 60);
}

void stopWake()
{
  wakeRinging = false;
  soundStop();
  soundFixVolume(-1);
  Serial.println("wake: stopped");
}

void wakeUpdate()
{
  struct tm t;
  if (wakeOn && !wakeRinging && timeSynced(t))
  {
    int32_t key = t.tm_yday * 1440 + t.tm_hour * 60 + t.tm_min;
    if (t.tm_hour * 60 + t.tm_min == wakeMin(wakeIdx) && key != wakeFiredKey)
    {
      wakeFiredKey = key;
      startWake();
    }
  }
  if (!wakeRinging)
    return;
  uint32_t now = millis();
  if (now - wakeAt >= WAKE_RING_MS)
  {
    stopWake();
    return;
  }
  if (now - wakeSayAt >= 30000)
  {
    wakeSayAt = now;
    sayWakeTime();
  }
  else if (now - wakeSayAt >= WAKE_SAY_MS && now - wakeBeepAt >= 2000) // しゃべっている間は鳴らさない
  {
    wakeBeepAt = now;
    soundPlay(SND_WAKE);
  }
}

// ---- スイッチの操作 ----
void savePrefs()
{
  prefs.putUChar("preset", presetIdx);
  prefs.putUChar("vol", volLevel);
  prefs.putUChar("page", page);
  prefs.putUChar("wake", wakeIdx);
  prefs.putBool("wakeOn", wakeOn);
}

void handleButtons()
{
  BtnEv e1 = buttonUpdate(sw1), e2 = buttonUpdate(sw2), e3 = buttonUpdate(sw3);
  if (e1 == EV_NONE && e2 == EV_NONE && e3 == EV_NONE)
    return;
  // 目覚まし・アラート中はどれかを押すと止める・閉じるだけ (この押下はほかの操作に使わない)
  if (wakeRinging || alert.active)
  {
    if (e1 == EV_SHORT || e2 == EV_SHORT || e3 == EV_SHORT)
    {
      if (wakeRinging)
        stopWake();
      else
      {
        alert.active = false;
        soundStop();
      }
      soundPlay(SND_CLICK);
    }
    return;
  }

  // SW1 長押し: 画面の切り替え (タイマー → 目覚まし → タイマーの設定 → 目覚ましの設定)
  if (e1 == EV_LONG)
  {
    page = (page + 1) % PG_N;
    savePrefs();
    soundPlay(SND_CLICK);
  }

  if (page == PG_TIMER || page == PG_TIMER_SET)
  {
    // SW1: 時間を選ぶ (動いている間は変えない)
    if (e1 == EV_SHORT && state != RUNNING)
    {
      presetIdx = (presetIdx + 1) % PRESET_N;
      resetTimer();
      savePrefs();
      soundPlay(SND_CLICK);
    }

    // SW2: スタート・一時停止・再開。終わったあとは READY に戻す。長押しでリセット
    if (e2 == EV_SHORT)
    {
      switch (state)
      {
      case READY:
      case PAUSED:
        endAt = millis() + remainMs;
        state = RUNNING;
        soundPlay(SND_START);
        break;
      case RUNNING:
        pauseTimer();
        soundPlay(SND_PAUSE);
        break;
      case DONE:
        resetTimer();
        soundStop(); // 鳴っているアラームと声を止める
        soundPlay(SND_PAUSE);
        break;
      }
    }
    else if (e2 == EV_LONG)
    {
      resetTimer();
      soundStop();
      soundPlay(SND_PAUSE);
    }
  }
  else
  {
    // SW1: 起きる時刻を選ぶ (選んだら ON にする)。SW2: ON / OFF
    if (e1 == EV_SHORT)
    {
      wakeIdx = (wakeIdx + 1) % WAKE_N;
      wakeOn = true;
      savePrefs();
      soundPlay(SND_CLICK);
    }
    if (e2 == EV_SHORT)
    {
      wakeOn = !wakeOn;
      savePrefs();
      soundPlay(wakeOn ? SND_START : SND_PAUSE);
    }
  }

  // SW3: 音量 (HIGH → MID → LOW → OFF の順に回る)
  if (e3 == EV_SHORT)
  {
    volLevel = (volLevel + 1) % VOL_N;
    soundSetVolume(volLevel); // OFF にしたら鳴っている音も止まる
    savePrefs();
    toast(muted() ? C_RED : C_GREEN, (String("SOUND ") + VOL_NAMES[volLevel]).c_str());
    soundPlay(SND_VOL); // 新しい音量で試し鳴らし (OFF なら鳴らない)
  }
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.printf("livico: psram %s, size %u\n", psramFound() ? "found" : "NOT FOUND", ESP.getPsramSize());

  prefs.begin("livico", false);
  presetIdx = prefs.getUChar("preset", 3) % PRESET_N;
  volLevel = prefs.getUChar("vol", VOL_LOW) % VOL_N;
  page = prefs.getUChar("page", PG_TIMER) % PG_N;
  wakeIdx = prefs.getUChar("wake", 0) % WAKE_N;
  wakeOn = prefs.getBool("wakeOn", false);
  doneDay = prefs.getUInt("tDay", 0);
  doneCount = prefs.getUShort("tCnt", 0);
  doneMins = prefs.getUShort("tMin", 0);
  resetTimer();

  soundBegin();
  soundSetVolume(volLevel);
  if (!cv->begin(40000000))
    Serial.println("canvas begin failed!");

  // スイッチは液晶のあとで設定する (GPIO13 は SPI2 の MISO の決まったピンなので、先にするとプルアップが外れることがある)
  for (Button *b : SWS)
    pinMode(b->pin, INPUT_PULLUP);

  for (PcInfo &pc : pcs)
    pcReset(pc);

  mqtt.setBufferSize(1024);

  // WiFi: 保存済みの WiFi につなぐ。つながらなければ設定モード (SW1 を押しながら起動すると設定を消して設定モード)
  deviceName = makeDeviceName(DEVICE_BASE_NAME);
  wifiSetupAddParam("mqtt", "MQTT server (empty = no MQTT)", MQTT_HOST_DEFAULT, 40);
  renderFrame(); // つなぐ間はいつもの画面 (WIFI...) を出しておく
  setupWiFi(deviceName.c_str(), [](const char *ap) {
    setupApName = ap;
    renderFrame();
  }, SW1_PIN, WIFI_PORTAL_SEC);
  setupApName = nullptr;
  Serial.printf("device: %s (%s)\n", deviceName.c_str(), deviceDisplayName().c_str());

  strlcpy(mqttHost, wifiSetupGet("mqtt").c_str(), sizeof(mqttHost));
  if (mqttEnabled())
  {
    IPAddress broker;
    if (broker.fromString(mqttHost))
      mqtt.setServer(broker, MQTT_PORT);
    else
      mqtt.setServer(mqttHost, MQTT_PORT); // 名前で書いたとき (mqttHost は消えない配列なので渡したままでよい)
    mqtt.setCallback(mqttCallback);
    mqtt.setSocketTimeout(2); // CONNACK などを待つ上限 (秒)
    mqttNet.setTimeout(1);    // TCP 接続を待つ上限 (秒)
  }
  else
    Serial.println("mqtt: disabled (no server set)");

  configTzTime("JST-9", "ntp.nict.jp", "time.google.com", "pool.ntp.org"); // オフラインでも、後でつながれば合う

  // 設定を消すために押した SW1 を、離したときの操作 (時間を選ぶ・画面の切り替え) として拾わない
  if (digitalRead(SW1_PIN) == LOW)
    sw1.down = sw1.raw = sw1.longFired = true;

  soundPlay(SND_BOOT);
  Serial.printf("heap: free %u, psram free %u\n", ESP.getFreeHeap(), ESP.getFreePsram());
}

void loop()
{
  wifiKeepAlive();
  mqttUpdate();
  handleButtons();
  timerUpdate();
  wakeUpdate();
  callUpdate();
  soundUpdate();

  // メモリ不足による再起動がないか、1 時間ごとに残りを出す
  static uint32_t lastHeapLog = 0;
  if (millis() - lastHeapLog >= 3600000)
  {
    lastHeapLog = millis();
    Serial.printf("heap: free %u, psram free %u\n", ESP.getFreeHeap(), ESP.getFreePsram());
  }

  static uint32_t lastDraw = 0;
  if (millis() - lastDraw >= 50)
  {
    lastDraw = millis();
    renderFrame();
  }
}
