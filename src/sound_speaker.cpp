// MAX98357A + スピーカーで鳴らす (InfoPanel の sound_speaker.cpp に、タイマーの音を足したもの)。I2S のタスク (コア 0) が 256 標本ずつ作って送り続ける
//   音: 鉄琴やチャイムに似た音を合成する (基音と倍音の正弦波を、減衰させながら足す)。同時に 12 音まで重ねられる
//   声: kanavoice (SpeechTest からそのまま写したもの) で、かなの文をその場で声にする
#include "sound.h"
#include <Arduino.h>
#include <driver/i2s.h>
#include <new>
#include "kanavoice.h"

namespace // この中のものはこのファイルの中だけで使う
{

// ---- 音色 ----
enum Timbre : uint8_t
{
  T_CHIME, // 打って減衰する (基音 + 2・3 倍音。倍音のほうが早く消えるので、打った瞬間だけ明るい)
  T_BEEP,  // 減衰しない警告音 (奇数倍音を少し混ぜて硬い音に)
  T_TICK,  // ごく短い (スイッチの音)
};

// 音の並びの 1 音
struct ToneDef
{
  uint16_t atMs; // 鳴らし始めるまで
  float freq;
  Timbre timbre;
  float amp;
  uint16_t tauMs; // 減衰の速さ (1/e になるまで)。T_BEEP では使わない
  uint16_t lenMs; // 鳴らす長さ
};

// 音の名前 (平均律、Hz)
const float C5 = 523.25f, E5 = 659.26f, G5 = 783.99f, B5 = 987.77f, C6 = 1046.50f, E6 = 1318.51f, G6 = 1567.98f,
            C7 = 2093.00f;

const ToneDef D_CLICK[] = {{0, C7, T_TICK, 0.35f, 10, 50}};
// ドミソド と上がる
const ToneDef D_BOOT[] = {{0, C5, T_CHIME, 0.30f, 350, 1200}, {90, E5, T_CHIME, 0.30f, 350, 1200},
                          {180, G5, T_CHIME, 0.30f, 350, 1200}, {270, C6, T_CHIME, 0.30f, 500, 1600}};
// ミソド (高め) で きらっと
const ToneDef D_DONE[] = {{0, E6, T_CHIME, 0.25f, 400, 1500}, {110, G6, T_CHIME, 0.25f, 400, 1500},
                          {220, C7, T_CHIME, 0.25f, 600, 2000}};
// 高低を 3 回くり返す
const ToneDef D_ALERT[] = {{0, E6, T_BEEP, 0.30f, 0, 140},   {160, B5, T_BEEP, 0.30f, 0, 140},
                           {320, E6, T_BEEP, 0.30f, 0, 140}, {480, B5, T_BEEP, 0.30f, 0, 140},
                           {640, E6, T_BEEP, 0.30f, 0, 140}, {800, B5, T_BEEP, 0.30f, 0, 140}};
// ピン・ポーン (玄関のチャイム)
const ToneDef D_CALL[] = {{0, E5, T_CHIME, 0.45f, 800, 2500}, {500, C5, T_CHIME, 0.45f, 900, 3000}};
const ToneDef D_VOL[] = {{0, C6, T_CHIME, 0.35f, 150, 500}, {90, G5, T_CHIME, 0.35f, 150, 500}};
// タイマー: スタートは ソ→ド と上げ、一時停止・リセットは ド→ソ と下げる
const ToneDef D_START[] = {{0, G5, T_CHIME, 0.35f, 120, 400}, {70, C6, T_CHIME, 0.35f, 150, 500}};
const ToneDef D_PAUSE[] = {{0, C6, T_CHIME, 0.35f, 120, 400}, {70, G5, T_CHIME, 0.35f, 150, 500}};
// タイマー終了: WorkingTimer と同じ「ピピピッ」(main.cpp が 1 秒ごとにくり返す)
// 目覚まし: ドミソド と短く上げる (main.cpp が 2 秒ごとにくり返す)
const ToneDef D_WAKE[] = {{0, C6, T_BEEP, 0.30f, 0, 90}, {120, E6, T_BEEP, 0.30f, 0, 90},
                          {240, G6, T_BEEP, 0.30f, 0, 90}, {360, C7, T_BEEP, 0.30f, 0, 150}};
const ToneDef D_TIMER[] = {{0, C7, T_BEEP, 0.30f, 0, 80}, {150, C7, T_BEEP, 0.30f, 0, 80}, {300, C7, T_BEEP, 0.30f, 0, 80}};

struct SoundDef
{
  const ToneDef *tones;
  int n;
  uint16_t quietMs; // ここまで鳴らせば、続けてしゃべってよい (余韻の途中から声を重ねる)
};
#define DEF(d, quiet) {d, sizeof(d) / sizeof(d[0]), quiet}
const SoundDef SOUNDS[] = {
    DEF(D_CLICK, 50),   // SND_CLICK
    DEF(D_BOOT, 900),   // SND_BOOT
    DEF(D_DONE, 900),   // SND_DONE
    DEF(D_ALERT, 1000), // SND_ALERT
    DEF(D_CALL, 1400),  // SND_CALL
    DEF(D_VOL, 400),    // SND_VOL
    DEF(D_START, 300),  // SND_START
    DEF(D_PAUSE, 300),  // SND_PAUSE
    DEF(D_TIMER, 450),  // SND_TIMER
    DEF(D_WAKE, 600),   // SND_WAKE
};
static_assert(sizeof(SOUNDS) / sizeof(SOUNDS[0]) == SND_N, "sound.h の Sound と数が合わない");

// 音量ごとの倍率
const float VOL_GAIN[VOL_N] = {1.0f, 0.4f, 0.15f, 0.0f};
volatile int volLevel = VOL_HIGH;
volatile int fixedLevel = -1; // 0 以上なら volLevel の代わりにこの音量で鳴らす (目覚まし)
int curLevel() { return fixedLevel >= 0 ? fixedLevel : volLevel; }

// ---- 鳴っている音 ----
const int TONE_MAX = 12;
struct Tone
{
  bool on;
  uint32_t start, len;   // 標本の番号と数
  uint32_t phase, inc;   // 位相 (32 ビットで 1 周)
  Timbre timbre;
  float amp;
  float env1, env2, d1, d2; // 基音と倍音の大きさ、1 標本ごとに掛ける減衰
};
Tone tones[TONE_MAX];

const int SINE_BITS = 10;
float sine[1 << SINE_BITS];
inline float sinAt(uint32_t ph) { return sine[ph >> (32 - SINE_BITS)]; }

kv::Synth *synth; // 大きい (約 100KB) ので PSRAM に置く
const kv::Voice &VOICE = kv::VOICES[0]; // SpeechTest の「ふつう」(130Hz)

SemaphoreHandle_t lock; // 音を足す・しゃべる (loop の側) と、標本を作る (音のタスク) が同時に触らないように
volatile uint32_t pos;  // 次に作る標本の番号
uint32_t quietAt;       // この標本までは、しゃべるのを待つ
char sayText[128];      // しゃべるのを待っている文
bool sayPending;

const uint32_t ATTACK = AUDIO_RATE * 2 / 1000;  // 2ms で立ち上げる (ぷつっと鳴らないように)
const uint32_t RELEASE = AUDIO_RATE * 5 / 1000; // 終わりの 5ms で消す

uint32_t msToSamples(uint32_t ms) { return ms * (AUDIO_RATE / 1000); }

void addTone(const ToneDef &d, uint32_t start)
{
  // 空きがなければ、いちばん早く鳴り始めた音を止めて使う
  int k = 0;
  for (int i = 0; i < TONE_MAX; i++)
  {
    if (!tones[i].on)
    {
      k = i;
      break;
    }
    if ((int32_t)(tones[i].start - tones[k].start) < 0)
      k = i;
  }
  Tone &t = tones[k];
  t.start = start;
  t.len = msToSamples(d.lenMs);
  t.phase = 0;
  t.inc = (uint32_t)(d.freq / AUDIO_RATE * 4294967296.0f);
  t.timbre = d.timbre;
  t.amp = d.amp;
  t.env1 = t.env2 = 1;
  float tau = d.tauMs * (AUDIO_RATE / 1000.0f);
  t.d1 = d.timbre == T_BEEP ? 1 : expf(-1 / tau);
  t.d2 = d.timbre == T_BEEP ? 1 : expf(-3 / tau); // 倍音は 3 倍の速さで消える
  t.on = true;
}

// n 標本ぶんの音を out に足す
void renderTones(float *out, int n)
{
  uint32_t p0 = pos;
  for (Tone &t : tones)
  {
    if (!t.on)
      continue;
    for (int i = 0; i < n; i++)
    {
      int32_t e = (int32_t)(p0 + i - t.start); // 鳴り始めてからの標本数
      if (e < 0)
        continue;
      if ((uint32_t)e >= t.len)
      {
        t.on = false;
        break;
      }
      float s;
      switch (t.timbre)
      {
      case T_CHIME:
        s = sinAt(t.phase) * t.env1 + (sinAt(t.phase * 2) * 0.30f + sinAt(t.phase * 3) * 0.12f) * t.env2;
        break;
      case T_BEEP:
        s = sinAt(t.phase) + sinAt(t.phase * 3) * 0.25f + sinAt(t.phase * 5) * 0.10f;
        break;
      default:
        s = sinAt(t.phase) * t.env1;
        break;
      }
      t.phase += t.inc;
      t.env1 *= t.d1;
      t.env2 *= t.d2;
      float g = t.amp;
      if ((uint32_t)e < ATTACK)
        g *= (float)e / ATTACK;
      uint32_t rest = t.len - e;
      if (rest < RELEASE)
        g *= (float)rest / RELEASE;
      out[i] += s * g;
    }
  }
}

void audioTask(void *)
{
  const int N = 256;
  static float tone[N], voice[N];
  static int16_t buf[N * 2]; // 左右の順
  for (;;)
  {
    xSemaphoreTake(lock, portMAX_DELAY);
    memset(tone, 0, sizeof(tone));
    renderTones(tone, N);
    synth->render(voice, N);
    pos += N;
    xSemaphoreGive(lock);
    float g = VOL_GAIN[curLevel()];
    for (int i = 0; i < N; i++)
    {
      float v = (tone[i] * TONE_VOLUME + voice[i] * VOICE_VOLUME) * g;
      v = v > 1 ? 1 : v < -1 ? -1 : v;
      buf[i * 2] = buf[i * 2 + 1] = v * 32767;
    }
    size_t written;
    i2s_write(I2S_NUM_0, buf, sizeof(buf), &written, portMAX_DELAY); // DMA が空くまで待つので、これで速さが合う
  }
}

} // namespace

void soundBegin()
{
  for (int i = 0; i < (1 << SINE_BITS); i++)
    sine[i] = sinf(6.2831853f * i / (1 << SINE_BITS));
  void *mem = heap_caps_malloc(sizeof(kv::Synth), MALLOC_CAP_SPIRAM);
  if (!mem)
    mem = malloc(sizeof(kv::Synth));
  synth = new (mem) kv::Synth();
  synth->begin(AUDIO_RATE);
  lock = xSemaphoreCreateMutex();

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = AUDIO_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = 8; // WiFi で音の計算が少し待たされても途切れないよう多めに
  cfg.dma_buf_len = 256;
  cfg.tx_desc_auto_clear = true;
  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE; // 0 のままだと GPIO0 (BOOT ボタン) に MCLK が出てしまう
  pins.bck_io_num = AMP_BCLK;
  pins.ws_io_num = AMP_LRC;
  pins.data_out_num = AMP_DIN;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK || i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK)
  {
    Serial.println("i2s init failed!");
    return;
  }
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 2, nullptr, 0);
}

void soundSetVolume(int level)
{
  volLevel = level;
  if (level == VOL_OFF)
    soundStop();
}

void soundFixVolume(int level)
{
  fixedLevel = level;
}

void soundPlay(Sound s)
{
  if (curLevel() == VOL_OFF || !lock)
    return;
  const SoundDef &d = SOUNDS[s];
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t now = pos;
  for (int i = 0; i < d.n; i++)
    addTone(d.tones[i], now + msToSamples(d.tones[i].atMs));
  uint32_t quiet = now + msToSamples(d.quietMs);
  if ((int32_t)(quiet - quietAt) > 0)
    quietAt = quiet;
  xSemaphoreGive(lock);
}

void soundSay(const char *kana)
{
  if (curLevel() == VOL_OFF || !lock)
    return;
  strlcpy(sayText, kana, sizeof(sayText));
  sayPending = true; // 鳴っている音が落ち着いたら soundUpdate() でしゃべり始める
}

void soundStop()
{
  if (!lock)
    return;
  xSemaphoreTake(lock, portMAX_DELAY);
  for (Tone &t : tones)
    t.on = false;
  synth->stop();
  sayPending = false;
  xSemaphoreGive(lock);
}

void soundUpdate()
{
  if (!sayPending || (int32_t)(pos - quietAt) < 0)
    return;
  sayPending = false;
  xSemaphoreTake(lock, portMAX_DELAY);
  synth->say(sayText, VOICE);
  xSemaphoreGive(lock);
}
