#pragma once
// ピン・液晶・音の設定。配線は SpeechTest・InfoPanel (env:speaker) と同じ

// ---- 液晶 (ST7789 240x320) ----
// GND = GND, VCC = 3.3V, SDO(MISO) は未接続。S3 の SPI2 (FSPI) の標準ピン
#define TFT_SCK  12 // SCL
#define TFT_MOSI 11 // SDA
#define TFT_MISO -1
#define TFT_CS   10
#define TFT_DC   9
#define TFT_RST  8
#define TFT_IPS      true // 色を反転する液晶 (TFT-Tester-ESP32S3-240-320-A と同じ)。色が反転していたら false
#define TFT_ROTATION 3    // 横置き 320x240。上下が逆なら 1
// BL 端子のない液晶なので明るさは変えられない

// ---- スイッチ (片側 GND、押すと LOW) ----
// GPIO13 は SPI2 の MISO の決まったピンなので、pinMode は液晶の begin のあとでする (main.cpp の setup)
#define SW1_PIN 13 // 短押し: 時間を選ぶ。起動時に押していると WiFi の設定を消す
#define SW2_PIN 14 // 短押し: スタート・一時停止・再開 / 長押し: リセット
#define SW3_PIN 1  // 短押し: 音量

// ---- アンプ (MAX98357A。I2S) ----
// Vin = 5V, GND = GND, SD と GAIN は未接続 (左右を混ぜて鳴らす、9dB)
#define AMP_BCLK 15
#define AMP_LRC  16
#define AMP_DIN  17
#define AUDIO_RATE   24000 // 1 秒の標本数。声 (kanavoice) に合わせて 24kHz
#define TONE_VOLUME  0.08f // 音の大きさ (音量 HIGH のとき)。うるさければ下げる
#define VOICE_VOLUME 0.3f  // 声の大きさ (音量 HIGH のとき)。SpeechTest の AUDIO_VOLUME と同じ
