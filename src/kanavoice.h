#pragma once
// かなの文から日本語の声を作る (フォルマント合成)。Arduino に頼らないので、PC でも同じものを動かせる。
//   声の元 (声帯の振動 = のこぎり波に近い波、息 = 雑音) を、口の形に当たる共鳴 (フォルマント) に通して母音にする。
//   子音は、閉じ (無音)・破裂 (雑音の一瞬)・摩擦 (雑音を帯域で絞る)・鼻音 (低い共鳴) を組み合わせて作る。
// 読めるのは ひらがな・カタカナ・ー・、。！？ と空白と ' (アクセント。その拍のあとで下げる)。漢字は読み飛ばす。
// 助詞の「は」「へ」はそのまま「ハ」「ヘ」と読むので、「わ」「え」と書く。
#include <stdint.h>
#include <stddef.h>

namespace kv
{

// 声の種類
struct Voice
{
  const char *name;
  float pitch;   // 基本の高さ (Hz)
  float formant; // 口の大きさ。1 = 大人の男、大きいほど子どもっぽい
  float speed;   // 1 = ふつう
  float inton;   // 抑揚の強さ。0 = 平ら (ロボット)
  float breath;  // 息の混ざり具合。1 = ささやき
  float robot;   // 1 = 高さのぶれをなくして機械らしくする
};

// 文を音にする道具。say() で文を渡し、render() で標本を取り出す
class Synth
{
public:
  static const int MAX_MORA = 200;
  static const int MAX_KEY = 1800;

  struct Mora
  {
    uint8_t c, v;      // 子音と母音 (kanavoice.cpp の Cons と Vow)
    bool pal;          // 拗音 (きゃ など)
    bool devoiced;     // 母音を声にしない (です の す など)
    int8_t accent;     // ' の印があった
    uint16_t src, len; // 文の中の位置 (バイト)
  };

  void begin(int rate);
  // 文を音の並びにする。読めた拍の数を返す。unknown に読めなかった文字の数を入れる
  int say(const char *utf8, const Voice &v, int *unknown = nullptr);
  void stop();
  // n 個の標本 (-1〜1 くらい) を作る。しゃべっていないときは 0 を埋めて false
  bool render(float *out, int n);
  bool busy() const { return playing; }

  // 表示用
  int moraCount() const { return nMora; }
  const Mora &mora(int i) const { return moras[i]; }
  volatile int curMora = -1; // 今の拍。しゃべっていないときは -1
  volatile float curF0, curF1, curF2;
  const char *moraName(int i, char *buf, size_t n) const; // 拍のローマ字 (シリアルの表示用)

  struct P // ある時刻の音の設定
  {
    float av, ah, af;   // 声・息・摩擦 の大きさ
    float f1, f2, f3;   // フォルマント (Hz)
    float b1, b2, b3;   // その幅 (Hz)
    float ff, bf;       // 摩擦の雑音を絞る帯域の中心と幅 (Hz)
  };

private:
  struct Key
  {
    float t; // ms
    P p;
    int16_t mora;
  };
  struct PKey
  {
    float t, f0;
  };
  struct Res // 共鳴 1 つ (2 次の IIR)。直流で 1 倍
  {
    float a, b, c, y1, y2;
    void set(float f, float bw, float rate);
    float run(float x)
    {
      float y = a * x + b * y1 + c * y2;
      y2 = y1;
      y1 = y;
      return y;
    }
  };
  struct Bpf // 帯域を通す (山の高さが 1 倍)
  {
    float b0, a1, a2, x1, x2, y1, y2;
    void set(float f, float bw, float rate);
    float run(float x)
    {
      float y = b0 * (x - x2) - a1 * y1 - a2 * y2;
      x2 = x1;
      x1 = x;
      y2 = y1;
      y1 = y;
      return y;
    }
  };

  int rate = 24000;
  Voice voice;
  Mora moras[MAX_MORA];
  int nMora = 0;
  Key keys[MAX_KEY];
  int nKey = 0;
  PKey pkeys[MAX_MORA * 2 + 4];
  int nPKey = 0;
  float f0a[MAX_MORA], f0b[MAX_MORA]; // 拍の母音の初めと終わりの高さ (Hz)

  // 作るとき
  float bt;    // 今の時刻 (ms)
  P last;      // 最後の設定
  int bMora;   // 今作っている拍
  void key(const P &p);
  void jump(const P &p) { key(p); }
  void ramp(const P &p, float ms);
  void hold(float ms);
  P silent() const;
  void pkey(float f0);
  void build();
  void buildMora(int i, float moraMs);
  void intonation();

  // 鳴らすとき
  volatile bool playing = false;
  uint32_t pos;       // 標本の番号
  int kIdx, pIdx;
  int blk;            // ブロック内の位置
  float endMs;
  float av, ah, af;   // なめらかにした大きさ
  float tav, tah, taf;
  float f0;
  float phase = 0, jit = 1, tilt = 0, hp = 0, hpx = 0;
  uint32_t seed = 12345;
  Res r[4];
  float lp1 = 0, lp2 = 0; // 高い音だけ素通しする道 (共鳴を直列にすると 4kHz より上が無くなるので足す)
  Bpf fric, fric2; // 摩擦の雑音は 2 回絞って、帯域の外を急に落とす
  void update();
  float noise()
  {
    seed = seed * 1664525u + 1013904223u;
    return (int32_t)seed * (1.0f / 2147483648.0f);
  }
};

extern const Voice VOICES[];
extern const int VOICE_N;

// ローマ字 (konnichiwa など) をひらがなにする。英字のないところはそのまま写す
void romajiToKana(const char *in, char *out, size_t n);

} // namespace kv
