// かなの文から日本語の声を作る (フォルマント合成)。しくみは kanavoice.h の頭に書いてある
#include "kanavoice.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

namespace kv
{

const Voice VOICES[] = {
    // name, pitch, formant, speed, inton, breath, robot
    {"ふつう", 130, 1.10f, 1.0f, 1.0f, 0.0f, 0},
    {"ロボット", 120, 1.10f, 0.9f, 0.0f, 0.0f, 1},
    {"高い声", 230, 1.15f, 1.0f, 1.2f, 0.05f, 0},
    {"低い声", 80, 0.92f, 0.85f, 0.8f, 0.0f, 0},
    {"ささやき", 120, 1.10f, 0.9f, 1.0f, 1.0f, 0},
};
const int VOICE_N = sizeof(VOICES) / sizeof(VOICES[0]);

enum Cons
{
  C_NONE, C_K, C_G, C_S, C_SH, C_Z, C_J, C_T, C_CH, C_TS, C_D,
  C_N, C_H, C_F, C_B, C_P, C_M, C_Y, C_R, C_W, C_V
};
static const char *const CONS_NAME[] = {"", "k", "g", "s", "sh", "z", "j", "t", "ch", "ts", "d",
                                        "n", "h", "f", "b", "p", "m", "y", "r", "w", "v"};

enum Vow
{
  V_A, V_I, V_U, V_E, V_O,
  V_N,     // ん
  V_Q,     // っ
  V_LONG,  // ー
  V_PAUSE, // 区切り。c が 0 = 空白 (間を空けない)、1 = 、 2 = 。 3 = ？
};

// 母音の F1 F2 F3 と幅 (大人の男。声の種類の formant でまとめて上げ下げする)
static const float VF[5][6] = {
    {750, 1250, 2600, 90, 110, 160}, // a
    {290, 2250, 3000, 50, 100, 160}, // i
    {340, 1400, 2400, 60, 100, 160}, // u (日本語のウは唇を丸めないので F2 が高め)
    {480, 1900, 2600, 70, 100, 160}, // e
    {470, 850, 2600, 70, 100, 160},  // o
};

// ひらがな U+3041〜U+3096 の 子音 と 母音。子音の字: - なし、S し、C ち、T つ、Q っ、N ん、
// x 小さい母音、Y 小さいや行、W 小さいわ。ぢ・づ は じ・ず と同じ
static const char *const KANA[] = {
    "xa", "-a", "xi", "-i", "xu", "-u", "xe", "-e", "xo", "-o",
    "ka", "ga", "ki", "gi", "ku", "gu", "ke", "ge", "ko", "go",
    "sa", "za", "Si", "ji", "su", "zu", "se", "ze", "so", "zo",
    "ta", "da", "Ci", "ji", "Q-", "Tu", "zu", "te", "de", "to", "do",
    "na", "ni", "nu", "ne", "no",
    "ha", "ba", "pa", "hi", "bi", "pi", "fu", "bu", "pu", "he", "be", "pe", "ho", "bo", "po",
    "ma", "mi", "mu", "me", "mo",
    "Ya", "ya", "Yu", "yu", "Yo", "yo",
    "ra", "ri", "ru", "re", "ro",
    "Wa", "wa", "-i", "-e", "-o", "N-",
    "vu", "ka", "ke",
};
static_assert(sizeof(KANA) / sizeof(KANA[0]) == 0x3096 - 0x3041 + 1, "KANA");

static uint8_t consOf(char c)
{
  switch (c)
  {
  case 'k': return C_K;
  case 'g': return C_G;
  case 's': return C_S;
  case 'S': return C_SH;
  case 'z': return C_Z;
  case 'j': return C_J;
  case 't': return C_T;
  case 'C': return C_CH;
  case 'T': return C_TS;
  case 'd': return C_D;
  case 'n': return C_N;
  case 'h': return C_H;
  case 'f': return C_F;
  case 'b': return C_B;
  case 'p': return C_P;
  case 'm': return C_M;
  case 'y': return C_Y;
  case 'r': return C_R;
  case 'w': return C_W;
  case 'v': return C_V;
  default: return C_NONE;
  }
}

static uint8_t vowOf(char c)
{
  switch (c)
  {
  case 'a': return V_A;
  case 'i': return V_I;
  case 'u': return V_U;
  case 'e': return V_E;
  default: return V_O;
  }
}

static bool voiceless(uint8_t c)
{
  return c == C_K || c == C_S || c == C_SH || c == C_T || c == C_CH || c == C_TS || c == C_H || c == C_F || c == C_P;
}

static uint32_t utf8(const char *&p)
{
  uint8_t c = *p++;
  if (c < 0x80)
    return c;
  int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  uint32_t cp = c & (0x3F >> n);
  while (n-- > 0 && (*p & 0xC0) == 0x80)
    cp = (cp << 6) | (*p++ & 0x3F);
  return cp;
}

static float semis(float st) { return powf(2.0f, st / 12); }

// ---- 共鳴 ----
static const float TAU = 6.2831853f;

void Synth::Res::set(float f, float bw, float rate)
{
  if (f > rate * 0.45f)
    f = rate * 0.45f;
  float T = 1.0f / rate;
  c = -expf(-TAU * bw * T);
  b = 2 * expf(-TAU / 2 * bw * T) * cosf(TAU * f * T);
  a = 1 - b - c;
}

void Synth::Bpf::set(float f, float bw, float rate)
{
  if (f > rate * 0.45f)
    f = rate * 0.45f;
  float w = TAU * f / rate;
  float alpha = sinf(w) * bw / (2 * f);
  float a0 = 1 + alpha;
  b0 = alpha / a0;
  a1 = -2 * cosf(w) / a0;
  a2 = (1 - alpha) / a0;
}

void Synth::begin(int r)
{
  rate = r;
  voice = VOICES[0];
}

// ---- 文を拍に分ける ----
int Synth::say(const char *s, const Voice &v, int *unknown)
{
  playing = false;
  voice = v;
  nMora = 0;
  int unk = 0;
  const char *p = s;
  while (*p && nMora < MAX_MORA)
  {
    const char *st = p;
    uint32_t cp = utf8(p);
    if (cp >= 0x30A1 && cp <= 0x30F6)
      cp -= 0x60; // カタカナはひらがなにして読む
    Mora m = {};
    m.src = st - s;
    m.len = p - st;
    if (cp >= 0x3041 && cp <= 0x3096)
    {
      const char *k = KANA[cp - 0x3041];
      char c = k[0];
      if (c == 'x' || c == 'Y' || c == 'W')
      {
        // 小さい字は前の字にくっつける (きゃ ふぁ てぃ うぃ など)
        Mora *pr = nMora > 0 ? &moras[nMora - 1] : nullptr;
        if (pr && pr->v <= V_O && pr->src + pr->len == m.src)
        {
          uint8_t nv = vowOf(k[1]);
          if (c == 'Y')
          {
            if (pr->c == C_NONE)
              pr->c = C_Y; // いゃ
            else if (pr->c != C_SH && pr->c != C_CH && pr->c != C_J)
              pr->pal = true;
          }
          else if (c == 'x' && pr->c == C_NONE)
            pr->c = pr->v == V_U ? C_W : pr->v == V_I ? C_Y : C_NONE; // うぃ いぇ
          pr->v = nv;
          pr->len += m.len;
          continue;
        }
        c = c == 'x' ? '-' : c == 'Y' ? 'y' : 'w'; // くっつける字がなければふつうの大きさで読む
      }
      if (c == 'Q')
        m.v = V_Q;
      else if (c == 'N')
        m.v = V_N;
      else
      {
        m.c = consOf(c);
        m.v = vowOf(k[1]);
      }
    }
    else if (cp == 0x30FC || cp == '-' || cp == 0x2015 || cp == 0x2212 || cp == 0xFF5E || cp == '~')
      m.v = V_LONG;
    else if (cp == '\'' || cp == 0x2019)
    {
      if (nMora > 0)
        moras[nMora - 1].accent = 1;
      continue;
    }
    else if (cp == ' ' || cp == 0x3000 || cp == '\t')
    {
      m.v = V_PAUSE;
      m.c = 0;
    }
    else if (cp == 0x3001 || cp == ',' || cp == 0xFF0C)
    {
      m.v = V_PAUSE;
      m.c = 1;
    }
    else if (cp == 0x3002 || cp == '.' || cp == '!' || cp == 0xFF01 || cp == 0xFF0E)
    {
      m.v = V_PAUSE;
      m.c = 2;
    }
    else if (cp == '?' || cp == 0xFF1F)
    {
      m.v = V_PAUSE;
      m.c = 3;
    }
    else
    {
      if (cp > ' ')
        unk++;
      continue;
    }
    moras[nMora++] = m;
  }
  if (unknown)
    *unknown = unk;

  // 無声化: 声のない子音にはさまれた イ・ウ と、文の終わりの す は声にしない (です ます の す、ひと の ひ)
  for (int i = 0; i < nMora; i++)
  {
    Mora &m = moras[i];
    if ((m.v != V_I && m.v != V_U) || !voiceless(m.c) || m.accent || (i > 0 && moras[i - 1].devoiced))
      continue;
    const Mora *n = i + 1 < nMora ? &moras[i + 1] : nullptr;
    if (n && n->v <= V_O && voiceless(n->c))
      m.devoiced = true;
    else if ((!n || (n->v == V_PAUSE && n->c >= 1)) && m.v == V_U && m.c == C_S)
      m.devoiced = true;
  }

  build();
  pos = 0;
  kIdx = pIdx = 0;
  blk = 0;
  av = ah = af = 0;
  phase = 0;
  playing = nKey > 1;
  return nMora;
}

void Synth::stop()
{
  playing = false;
  curMora = -1;
}

const char *Synth::moraName(int i, char *buf, size_t n) const
{
  const Mora &m = moras[i];
  static const char VCH[] = "aiueo";
  if (m.v == V_N)
    snprintf(buf, n, "N");
  else if (m.v == V_Q)
    snprintf(buf, n, "Q");
  else if (m.v == V_LONG)
    snprintf(buf, n, "-");
  else if (m.v == V_PAUSE)
    snprintf(buf, n, m.c == 0 ? "_" : m.c == 1 ? "," : m.c == 2 ? "." : "?");
  else
    snprintf(buf, n, "%s%s%c%s%s", CONS_NAME[m.c], m.pal ? "y" : "", VCH[m.v], m.devoiced ? "*" : "", m.accent ? "'" : "");
  return buf;
}

// ---- 抑揚 ----
// アクセントの辞書はないので、句 (空白や 、 で区切ったまとまり) ごとに「1 拍目が低く、2 拍目から高い」形にする。
// ' の印があれば、その拍のあとで下げる。文の中では少しずつ下げ、文の終わりは下げる (？ なら上げる)
void Synth::intonation()
{
  const float I = voice.inton;
  int s0 = 0;
  while (s0 < nMora)
  {
    int s1 = s0; // 文の終わり (。 ？ か最後)
    while (s1 < nMora && !(moras[s1].v == V_PAUSE && moras[s1].c >= 2))
      s1++;
    int L = 0;
    for (int i = s0; i < s1; i++)
      L += moras[i].v != V_PAUSE;
    int j = 0;
    int p0 = s0;
    while (p0 < s1)
    {
      int p1 = p0; // 句の終わり
      while (p1 < s1 && moras[p1].v != V_PAUSE)
        p1++;
      int acc = -1;
      for (int i = p0; i < p1; i++)
        if (moras[i].accent)
        {
          acc = i - p0;
          break;
        }
      for (int i = p0; i < p1; i++)
      {
        int q = i - p0;
        float lv;
        if (p1 - p0 == 1)
          lv = 0;
        else if (acc < 0)
          lv = q == 0 ? -1 : 1;
        else if (acc == 0)
          lv = q == 0 ? 1.3f : -1;
        else
          lv = q == 0 ? -1 : q <= acc ? 1 : -1;
        float st = (lv * 2.0f - 4.0f * j / (L > 1 ? L : 1)) * I;
        f0a[i] = f0b[i] = voice.pitch * semis(st);
        j++;
      }
      p0 = p1 + 1;
    }
    // 文の終わりの拍
    int last = s1 - 1;
    while (last >= s0 && moras[last].v == V_PAUSE)
      last--;
    if (last >= s0)
    {
      bool q = s1 < nMora && moras[s1].c == 3;
      f0b[last] = f0a[last] * semis(q ? 5.0f * (I > 0.5f ? I : 0.5f) : -3.0f * I);
      if (q)
        f0a[last] *= semis(-1.0f * I);
    }
    for (int i = s0; i < s1 && i < nMora; i++)
      if (moras[i].v == V_PAUSE)
        f0a[i] = f0b[i] = i > 0 ? f0b[i - 1] : voice.pitch;
    if (s1 < nMora)
      f0a[s1] = f0b[s1] = last >= 0 ? f0b[last] : voice.pitch;
    s0 = s1 + 1;
  }
}

// ---- 拍を時刻ごとの設定 (キー) にする ----
void Synth::key(const P &p)
{
  last = p;
  if (nKey >= MAX_KEY)
    return;
  keys[nKey].t = bt;
  keys[nKey].p = p;
  keys[nKey].mora = bMora;
  nKey++;
}

void Synth::ramp(const P &p, float ms)
{
  bt += ms;
  key(p);
}

void Synth::hold(float ms)
{
  if (ms <= 0)
    return;
  bt += ms;
  key(last);
}

Synth::P Synth::silent() const
{
  P p = last;
  p.av = p.ah = p.af = 0;
  return p;
}

void Synth::pkey(float f)
{
  if (nPKey >= (int)(sizeof(pkeys) / sizeof(pkeys[0])))
    return;
  pkeys[nPKey].t = bt;
  pkeys[nPKey].f0 = f;
  nPKey++;
}

static Synth::P vowelP(int v)
{
  const float *f = VF[v];
  Synth::P p = {1, 0.02f, 0, f[0], f[1], f[2], f[3], f[4], f[5], 3000, 2000};
  return p;
}

static Synth::P formants(float f1, float f2, float f3, float av)
{
  Synth::P p = {av, 0, 0, f1, f2, f3, 70, 110, 170, 3000, 2000};
  return p;
}

static Synth::P nasal(float f2)
{
  Synth::P p = {0.55f, 0, 0, 250, f2, 2300, 70, 600, 800, 3000, 2000}; // 鼻に抜けて高い共鳴はぼやける
  return p;
}

void Synth::build()
{
  nKey = nPKey = 0;
  bt = 0;
  bMora = 0;
  intonation();
  P s = vowelP(V_A);
  s.av = s.ah = 0;
  key(s);
  pkey(nMora > 0 ? f0a[0] : voice.pitch);
  hold(30);
  const float D = 135 / voice.speed; // 1 拍の長さ (ms)
  for (int i = 0; i < nMora; i++)
    buildMora(i, D);
  ramp(silent(), 40);
  pkey(nMora > 0 ? f0b[nMora - 1] : voice.pitch);
  hold(120); // 共鳴の響きが消えるまで
  endMs = bt;
}

void Synth::buildMora(int i, float D)
{
  const Mora &m = moras[i];
  bMora = i;
  const Mora *nm = i + 1 < nMora ? &moras[i + 1] : nullptr;
  const bool fromVoice = last.av > 0.2f; // 前が声 (母音・鼻音など) でつながっている

  if (m.v == V_PAUSE)
  {
    float ms = m.c == 0 ? 0 : m.c == 1 ? 220 / voice.speed : 420 / voice.speed;
    if (ms > 0 && (last.av > 0 || last.ah > 0 || last.af > 0))
    {
      ramp(silent(), 25);
      hold(ms - 25);
    }
    else
      hold(ms);
    return;
  }
  if (m.v == V_LONG) // 前の音をのばす
  {
    pkey(f0a[i]);
    hold(D);
    pkey(f0b[i]);
    return;
  }
  if (m.v == V_Q) // っ。次の子音の閉じを長くする
  {
    ramp(silent(), 10);
    hold(D * 0.8f - 10);
    return;
  }
  if (m.v == V_N) // ん。次の子音の口の形に合わせる
  {
    uint8_t nc = nm && nm->v <= V_O ? nm->c : C_NONE;
    float f2 = 1250; // 後ろに子音がなければ のどの奥の ン
    if (nc == C_M || nc == C_B || nc == C_P)
      f2 = 1000;
    else if (nc == C_T || nc == C_D || nc == C_N || nc == C_R || nc == C_TS || nc == C_CH || nc == C_S ||
             nc == C_Z || nc == C_J || nc == C_SH)
      f2 = 1650;
    else if (nc == C_K || nc == C_G)
      f2 = 2000;
    P n = nasal(f2);
    if (fromVoice)
      ramp(n, 30);
    else
    {
      P z = n;
      z.av = 0;
      jump(z);
      ramp(n, 20);
    }
    pkey(f0a[i]);
    hold(D - 30);
    pkey(f0b[i]);
    return;
  }

  // ---- 子音 ----
  const float *vf = VF[m.v];
  P pv = vowelP(m.v);
  const bool front = m.v == V_I || m.v == V_E;
  const float t0 = bt;
  float trans = 40; // 母音へ移る長さ (ms)
  P on = pv;        // 母音の出だし
  auto locus = [&](float f2l, float f3l, float k) {
    on.f1 = vf[0] < 300 ? vf[0] : 300;
    on.f2 = f2l + k * (vf[1] - f2l);
    on.f3 = f3l + k * (vf[2] - f3l);
  };
  // 閉じて (無音か、声だけ低く鳴らす) 破裂させ、無声なら息を吹く
  auto stopC = [&](float f2l, float f3l, float k, float bff, float bbf, float bamp, float asp, bool voiced) {
    locus(f2l, f3l, k);
    float clos = fromVoice ? (voiced ? 40 : 60) : (voiced ? 30 : 25);
    if (i > 0 && moras[i - 1].v == V_Q)
      clos = 20;
    if (!voiced)
    {
      ramp(silent(), 8);
      hold(clos);
    }
    else
    {
      P vb = silent();
      vb.av = 0.2f;
      vb.f1 = 180;
      vb.b1 = 60;
      vb.f2 = on.f2;
      vb.b2 = 300;
      vb.f3 = on.f3;
      vb.b3 = 400;
      ramp(vb, 8);
      hold(clos);
    }
    P b = silent();
    b.f1 = on.f1;
    b.f2 = on.f2;
    b.f3 = on.f3;
    b.af = voiced ? bamp * 0.5f : bamp;
    b.ff = bff;
    b.bf = bbf;
    b.av = voiced ? 0.2f : 0;
    jump(b);
    hold(voiced ? 4 : 6);
    if (!voiced)
    {
      P a = on;
      a.av = 0;
      a.ah = 0.2f;
      a.af = 0.05f;
      a.ff = bff;
      a.bf = bbf;
      jump(a);
      hold(asp);
    }
    on.av = voiced ? 0.8f : 0.6f;
  };
  // 摩擦 (雑音を帯域で絞る)。voiced なら声も低く混ぜる
  auto fricC = [&](float ff, float bf, float amp, float ms, bool voiced) {
    P f = silent();
    f.f1 = voiced ? 220 : on.f1;
    f.b1 = 80;
    f.f2 = on.f2;
    f.f3 = on.f3;
    f.af = amp;
    f.ff = ff;
    f.bf = bf;
    f.av = voiced ? 0.3f : 0;
    if (i > 0 && moras[i - 1].v == V_Q)
      ms += D * 0.8f; // っ のあとは摩擦をのばす
    ramp(f, 15);
    hold(ms - 15);
    on.av = voiced ? 0.8f : 0.6f;
  };
  // 閉じたあとに摩擦 (つ ち)
  auto affC = [&](float ff, float bf, float amp, float ms) {
    ramp(silent(), 8);
    hold(i > 0 && moras[i - 1].v == V_Q ? 20 : fromVoice ? 45 : 20);
    P f = silent();
    f.f1 = on.f1;
    f.f2 = on.f2;
    f.f3 = on.f3;
    f.af = amp;
    f.ff = ff;
    f.bf = bf;
    jump(f);
    hold(ms - 12);
    f.af = amp * 0.4f;
    ramp(f, 12);
    on.av = 0.6f;
  };
  // 母音の形に近い声から滑らかに移る (や行 わ行 ら行)
  auto glideC = [&](const P &g, float ms) {
    if (fromVoice)
      ramp(g, 25);
    else
    {
      P z = g;
      z.av = 0;
      jump(z);
      ramp(g, 15);
    }
    hold(ms);
    on = g;
  };

  switch (m.c)
  {
  case C_NONE:
    if (fromVoice)
    {
      on = last; // 母音から母音へ
      trans = 50;
    }
    else
    {
      on.av = 0; // 立ち上がりをなめらかに
      trans = 20;
    }
    break;
  case C_K:
  case C_G:
    stopC(front ? 2300 : 1400, front ? 2900 : 2000, 0.3f, front ? 3000 : 1800, 1000, 0.5f, 22, m.c == C_G);
    break;
  case C_T:
  case C_D:
    stopC(1800, 2700, 0.35f, 4500, 2500, 0.25f, 15, m.c == C_D);
    break;
  case C_P:
  case C_B:
  case C_V:
    stopC(900, 2300, 0.4f, 1200, 2500, 0.2f, 15, m.c != C_P);
    break;
  case C_S:
    locus(1700, 2700, 0.35f);
    fricC(6000, 3000, 0.3f, 85, false);
    break;
  case C_Z:
    locus(1700, 2700, 0.35f);
    fricC(6000, 3000, 0.2f, 60, true);
    break;
  case C_SH:
    locus(2200, 2900, 0.3f);
    fricC(3500, 2200, 0.3f, 90, false);
    trans = 60;
    break;
  case C_J:
    locus(2200, 2900, 0.3f);
    fricC(3500, 2200, 0.2f, 60, true);
    trans = 60;
    break;
  case C_TS:
    locus(1700, 2700, 0.35f);
    affC(6000, 3000, 0.35f, 60);
    break;
  case C_CH:
    locus(2200, 2900, 0.3f);
    affC(3500, 2200, 0.35f, 65);
    trans = 60;
    break;
  case C_H:
  {
    // ハ ヘ ホ は母音の口の形のまま息だけ出す。ヒ ヒャ は舌を上あごに寄せた摩擦 (ç)
    P h = m.v == V_I || m.pal ? vowelP(V_I) : pv;
    h.av = 0;
    if (m.v == V_I || m.pal)
    {
      h.ah = 0.2f;
      h.af = 0.4f;
      h.ff = 4000;
      h.bf = 2000;
    }
    else
      h.ah = 0.55f;
    if (i > 0 && moras[i - 1].v == V_Q)
      hold(D * 0.8f);
    ramp(h, 15);
    hold(60);
    on = pv;
    on.av = 0.7f;
    trans = 25;
    break;
  }
  case C_F:
  {
    // フ は両唇を寄せた弱い摩擦 (ɸ)
    locus(900, 2300, 0.4f);
    P f = formants(300, 900, 2300, 0);
    f.ah = 0.25f;
    f.af = 0.2f;
    f.ff = 1500;
    f.bf = 3000;
    ramp(f, 15);
    hold(55);
    on.av = 0.6f;
    break;
  }
  case C_M:
  case C_N:
  {
    bool mm = m.c == C_M;
    P n = nasal(mm ? 1100 : m.pal ? 2000 : 1600);
    if (fromVoice)
      ramp(n, 15);
    else
    {
      P z = n;
      z.av = 0;
      jump(z);
      ramp(n, 15);
    }
    hold(35);
    if (mm)
      locus(900, 2300, 0.4f);
    else
      locus(1750, 2700, 0.35f);
    on.av = 0.9f;
    trans = 35;
    break;
  }
  case C_Y:
    glideC(formants(280, 2250, 3000, 0.8f), 20);
    trans = 80;
    break;
  case C_W:
    glideC(formants(320, 750, 2300, 0.8f), 20);
    trans = 80;
    break;
  case C_R:
  {
    // ラ行は舌で 1 回はじく。短く弱くなる
    P r = formants(280, 1650, 2600, 0.45f);
    r.b2 = 160;
    glideC(r, 12);
    on.av = 0.8f;
    trans = 40;
    break;
  }
  }
  if (m.pal) // きゃ などは イ の口の形から
  {
    on.f1 = 280;
    on.f2 = 2200;
    on.f3 = 2900;
    trans = 75;
  }

  // ---- 母音 ----
  float cdur = bt - t0;
  float vdur = D - cdur * 0.55f;
  if (vdur < D * 0.45f)
    vdur = D * 0.45f;
  if (m.devoiced)
  {
    pv.av = on.av = 0;
    pv.ah = on.ah = 0.12f;
    if (m.c == C_S || m.c == C_SH || m.c == C_TS || m.c == C_CH)
    {
      pv.af = on.af = 0.15f; // す し は摩擦のまま
      pv.ff = on.ff = last.ff;
      pv.bf = on.bf = last.bf;
    }
    vdur *= 0.6f;
  }
  if (trans > vdur * 0.7f)
    trans = vdur * 0.7f;
  jump(on);
  pkey(f0a[i]);
  ramp(pv, trans);
  if (!nm || (nm->v == V_PAUSE && nm->c >= 1))
  {
    // 句の終わりの母音は弱めて終わる
    hold((vdur - trans) * 0.4f);
    pv.av *= 0.5f;
    pv.ah *= 0.5f;
    pv.af *= 0.5f;
    ramp(pv, (vdur - trans) * 0.6f);
  }
  else
    hold(vdur - trans);
  pkey(f0b[i]);
}

// ---- 鳴らす ----
static const int BLK = 16; // 設定を変える間隔 (標本)
static const float HF_GAIN = 0.15f;
static const float JITTER = 0.02f;

void Synth::update()
{
  float t = pos * 1000.0f / rate;
  if (t >= endMs)
  {
    playing = false;
    curMora = -1;
    return;
  }
  while (kIdx + 1 < nKey && keys[kIdx + 1].t <= t)
    kIdx++;
  const Key &a = keys[kIdx];
  P p = a.p;
  if (kIdx + 1 < nKey)
  {
    const Key &b = keys[kIdx + 1];
    float u = (t - a.t) / (b.t - a.t);
    const float *pa = &a.p.av, *pb = &b.p.av;
    float *pp = &p.av;
    for (size_t j = 0; j < sizeof(P) / sizeof(float); j++)
      pp[j] = pa[j] + (pb[j] - pa[j]) * u;
  }
  while (pIdx + 1 < nPKey && pkeys[pIdx + 1].t <= t)
    pIdx++;
  f0 = pkeys[pIdx].f0;
  if (pIdx + 1 < nPKey)
  {
    const PKey &b = pkeys[pIdx + 1];
    f0 += (b.f0 - f0) * (t - pkeys[pIdx].t) / (b.t - pkeys[pIdx].t);
  }

  const float fs = voice.formant;
  r[0].set(p.f1 * fs, p.b1, rate);
  r[1].set(p.f2 * fs, p.b2, rate);
  r[2].set(p.f3 * fs, p.b3, rate);
  r[3].set(3500 * fs, 250, rate);
  fric.set(p.ff, p.bf * 1.5f, rate);
  fric2.set(p.ff, p.bf * 1.5f, rate);
  const float br = voice.breath;
  tav = p.av * (1 - br);
  tah = p.ah + p.av * br * 0.5f;
  taf = p.af;
  curMora = a.mora;
  curF0 = f0;
  curF1 = p.f1 * fs;
  curF2 = p.f2 * fs;
}

bool Synth::render(float *out, int n)
{
  int i = 0;
  const float sm = 1 - expf(-1.0f / (rate * 0.003f)); // 大きさを 3ms でなめらかに変える
  const float hpc = 1 - TAU * 90 / rate;               // 90Hz より下を切る (小さいスピーカーでは鳴らないので)
  const float hfk = 1 - expf(-TAU * 3500 / rate);      // 素通しの道は 3.5kHz より上だけ
  const float OQ = 0.6f;                               // 声帯が開いている割合
  for (; i < n && playing; i++)
  {
    if (blk == 0)
    {
      update();
      if (!playing)
        break;
    }
    if (++blk >= BLK)
      blk = 0;
    av += (tav - av) * sm;
    ah += (tah - ah) * sm;
    af += (taf - af) * sm;
    // 声帯の振動: 開いて閉じる息の流れの微分 (閉じる瞬間に鋭く下がる)
    phase += f0 * jit / rate;
    if (phase >= 1)
    {
      phase -= 1;
      jit = 1 + noise() * JITTER * (1 - voice.robot); // 1 周期ごとに高さをほんの少しぶらす (人の声らしくなる)
    }
    float g = 0;
    if (phase < OQ)
    {
      float x = phase / OQ;
      g = 2 * x - 3 * x * x;
    }
    tilt += 0.85f * (g - tilt); // 高い音をほんの少し弱めて、やわらかくする
    float vo = av * tilt;
    float src = vo + ah * noise() * 0.5f;
    float y = src;
    for (Res &q : r)
      y = q.run(y);
    lp1 += hfk * (vo - lp1); // 息の雑音は通さない (通すと気息が全部シャーという音になる)
    float hf = vo - lp1;
    lp2 += hfk * (hf - lp2);
    hf -= lp2;
    float o = y * 0.25f + hf * HF_GAIN + fric2.run(fric.run(noise())) * af * 0.6f;
    hp = o - hpx + hpc * hp;
    hpx = o;
    out[i] = hp * 0.4f;
    pos++;
  }
  for (; i < n; i++)
    out[i] = 0;
  return playing;
}

// ---- ローマ字 ----
static const char *const ROMA[][6] = {
    {"", "あ", "い", "う", "え", "お"},
    {"k", "か", "き", "く", "け", "こ"},
    {"g", "が", "ぎ", "ぐ", "げ", "ご"},
    {"s", "さ", "し", "す", "せ", "そ"},
    {"z", "ざ", "じ", "ず", "ぜ", "ぞ"},
    {"t", "た", "ち", "つ", "て", "と"},
    {"d", "だ", "ぢ", "づ", "で", "ど"},
    {"n", "な", "に", "ぬ", "ね", "の"},
    {"h", "は", "ひ", "ふ", "へ", "ほ"},
    {"b", "ば", "び", "ぶ", "べ", "ぼ"},
    {"p", "ぱ", "ぴ", "ぷ", "ぺ", "ぽ"},
    {"m", "ま", "み", "む", "め", "も"},
    {"y", "や", "い", "ゆ", "いぇ", "よ"},
    {"r", "ら", "り", "る", "れ", "ろ"},
    {"l", "ら", "り", "る", "れ", "ろ"},
    {"w", "わ", "うぃ", "う", "うぇ", "を"},
    {"f", "ふぁ", "ふぃ", "ふ", "ふぇ", "ふぉ"},
    {"v", "ゔぁ", "ゔぃ", "ゔ", "ゔぇ", "ゔぉ"},
    {"j", "じゃ", "じ", "じゅ", "じぇ", "じょ"},
    {"c", "か", "し", "く", "せ", "こ"},
    {"sh", "しゃ", "し", "しゅ", "しぇ", "しょ"},
    {"ch", "ちゃ", "ち", "ちゅ", "ちぇ", "ちょ"},
    {"ts", "つぁ", "つぃ", "つ", "つぇ", "つぉ"},
    {"th", "てゃ", "てぃ", "てゅ", "てぇ", "てょ"},
    {"dh", "でゃ", "でぃ", "でゅ", "でぇ", "でょ"},
    {"ky", "きゃ", "きぃ", "きゅ", "きぇ", "きょ"},
    {"gy", "ぎゃ", "ぎぃ", "ぎゅ", "ぎぇ", "ぎょ"},
    {"sy", "しゃ", "し", "しゅ", "しぇ", "しょ"},
    {"zy", "じゃ", "じ", "じゅ", "じぇ", "じょ"},
    {"jy", "じゃ", "じ", "じゅ", "じぇ", "じょ"},
    {"ty", "ちゃ", "ち", "ちゅ", "ちぇ", "ちょ"},
    {"cy", "ちゃ", "ち", "ちゅ", "ちぇ", "ちょ"},
    {"dy", "ぢゃ", "ぢぃ", "ぢゅ", "ぢぇ", "ぢょ"},
    {"ny", "にゃ", "にぃ", "にゅ", "にぇ", "にょ"},
    {"hy", "ひゃ", "ひぃ", "ひゅ", "ひぇ", "ひょ"},
    {"by", "びゃ", "びぃ", "びゅ", "びぇ", "びょ"},
    {"py", "ぴゃ", "ぴぃ", "ぴゅ", "ぴぇ", "ぴょ"},
    {"my", "みゃ", "みぃ", "みゅ", "みぇ", "みょ"},
    {"ry", "りゃ", "りぃ", "りゅ", "りぇ", "りょ"},
};

static int vowelIndex(char c)
{
  switch (c)
  {
  case 'a': return 1;
  case 'i': return 2;
  case 'u': return 3;
  case 'e': return 4;
  case 'o': return 5;
  default: return 0;
  }
}

static bool isAlpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static char lower(char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

void romajiToKana(const char *in, char *out, size_t n)
{
  size_t o = 0;
  auto put = [&](const char *s) {
    size_t l = strlen(s);
    if (o + l < n)
    {
      memcpy(out + o, s, l);
      o += l;
    }
  };
  while (*in)
  {
    char c = lower(*in);
    if (!isAlpha(*in))
    {
      if (*in == '-' && o > 0)
        put("ー");
      else if (o + 1 < n)
        out[o++] = *in;
      in++;
      continue;
    }
    char c1 = lower(in[1]);
    if (int v = vowelIndex(c))
    {
      put(ROMA[0][v]);
      in++;
      continue;
    }
    // ん: n のあとが母音・y でない (nn n' も ん。konnichiwa の nn は ん + に)
    if (c == 'n' && !vowelIndex(c1) && c1 != 'y')
    {
      put("ん");
      char c2 = lower(in[2]);
      in += c1 == '\'' || (c1 == 'n' && !vowelIndex(c2) && c2 != 'y') ? 2 : 1;
      continue;
    }
    // っ: 同じ子音が 2 つ (tch も)
    if ((c == c1 && isAlpha(in[1])) || (c == 't' && c1 == 'c'))
    {
      put("っ");
      in++;
      continue;
    }
    // 長い子音から順に当てる
    bool done = false;
    for (int len = 2; len >= 1 && !done; len--)
      for (size_t r = 1; r < sizeof(ROMA) / sizeof(ROMA[0]); r++)
      {
        const char *pre = ROMA[r][0];
        if ((int)strlen(pre) != len)
          continue;
        bool match = true;
        for (int k = 0; k < len; k++)
          if (lower(in[k]) != pre[k])
            match = false;
        int v = match ? vowelIndex(lower(in[len])) : 0;
        if (v)
        {
          put(ROMA[r][v]);
          in += len + 1;
          done = true;
          break;
        }
      }
    if (!done)
      in++; // 読めない英字は飛ばす
  }
  out[o] = 0;
}

} // namespace kv
