#pragma once
// 音を鳴らす部品 (sound_speaker.cpp)。main.cpp は「何の音か」だけを渡す
// 音を足すときは Sound に名前を足して、sound_speaker.cpp に鳴らし方を書く (数が合わないとビルドが止まる)
#include "config.h"

enum Sound
{
  SND_CLICK, // スイッチ
  SND_BOOT,  // 起動
  SND_DONE,  // ComfyUI のジョブ完了
  SND_ALERT, // 切断・ComfyUI の異常
  SND_CALL,  // 呼び出しベル
  SND_VOL,   // 音量を変えたときの試し鳴らし
  SND_START, // タイマーのスタート・再開
  SND_PAUSE, // タイマーの一時停止・リセット
  SND_TIMER, // タイマー終了 (くり返し鳴らす 1 回ぶん)
  SND_WAKE,  // 目覚まし (くり返し鳴らす 1 回ぶん)
  SND_N
};

// 音量。OFF は何も鳴らさない
enum
{
  VOL_HIGH,
  VOL_MID,
  VOL_LOW,
  VOL_OFF,
  VOL_N
};

void soundBegin();
void soundSetVolume(int level);
// 音量の設定にかかわらず level で鳴らす (目覚まし用。OFF でも鳴る)。-1 で元に戻す
void soundFixVolume(int level);
void soundPlay(Sound s);
// かなの文をしゃべる。鳴っている音が終わってからしゃべる
void soundSay(const char *kana);
void soundStop();
void soundUpdate(); // loop() から呼ぶ
