// Offline comparator: feed two WAVs (render x, capture y) into the switch gate suppressor and write processed.wav
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <cstring>

#include "suppressor.h"

constexpr int kSampleRateHz = 16000;
constexpr int kBlockSamples = 160;

struct Wav {
  // モノラル16kHz固定。sr/chは保持しない。
  std::vector<int16_t> samples;
};

uint32_t rd32le(const uint8_t* p){ return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24); }
uint16_t rd16le(const uint8_t* p){ return p[0] | (p[1]<<8); }

bool read_wav_pcm16_mono16k(const std::string& path, Wav* out){
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (buf.size() < 44) return false;
  if (std::memcmp(buf.data(), "RIFF",4) || std::memcmp(buf.data()+8,"WAVE",4)) return false;
  size_t pos = 12; int sr=0,ch=0,bps=0; size_t data_off=0,data_size=0;
  while (pos + 8 <= buf.size()){
    uint32_t id = rd32le(&buf[pos]); pos+=4; uint32_t sz = rd32le(&buf[pos]); pos+=4; size_t start=pos;
    if (id == 0x20746d66){ // 'fmt '
      uint16_t fmt = rd16le(&buf[start+0]); ch = rd16le(&buf[start+2]); sr = rd32le(&buf[start+4]); bps = rd16le(&buf[start+14]);
      if (fmt != 1 || bps != 16) return false;
    } else if (id == 0x61746164){ // 'data'
      data_off = start; data_size = sz; break;
    }
    pos = start + sz;
  }
  if (!data_off || !data_size) return false;
  if (sr != 16000 || ch != 1) return false; // 16k/mono固定
  size_t ns = data_size/2; out->samples.resize(ns);
  const int16_t* p = reinterpret_cast<const int16_t*>(&buf[data_off]);
  for (size_t i=0;i<ns;i++) out->samples[i] = p[i];
  return true;
}

int main(int argc, char** argv){
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i] ? argv[i] : "");
    if (arg == "--help" || arg == "-h") {
      std::fprintf(stderr,
                   "Usage: %s <render.wav> <capture.wav>\n",
                   argv[0]);
      return 0;
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
      return 1;
    } else {
      positional.push_back(arg);
    }
  }

  if (positional.size() < 2){
    std::fprintf(stderr, "Usage: %s <render.wav> <capture.wav>\n", argv[0]);
    return 1;
  }

  const std::string& render_path = positional[0];
  const std::string& capture_path = positional[1];

  Wav x, y;
  if (!read_wav_pcm16_mono16k(render_path, &x) ||
      !read_wav_pcm16_mono16k(capture_path, &y)){
    std::fprintf(stderr, "Failed to read 16k-mono wavs\n");
    return 1;
  }
  size_t blocks = std::min(x.samples.size(), y.samples.size()) /
                  static_cast<size_t>(kBlockSamples);
  if (blocks == 0) {
    std::fprintf(stderr, "Not enough samples to process.\n");
    return 1;
  }

  EchoSuppressor suppressor;
  std::vector<int16_t> processed(blocks * kBlockSamples);
  std::vector<float> far_block(kBlockSamples), near_block(kBlockSamples), out_block(kBlockSamples);
  static constexpr float kInvScale = 1.0f / 32768.0f;
  static constexpr float kScale = 32767.0f;

  std::fprintf(stderr,
               "config: atten=%.1f dB, rho=%.2f, ratio=%.2f, hang=%d, attack=%.3f, release=%.3f\n",
               -80.0f,
               EchoSuppressor::kRhoThresh,
               EchoSuppressor::kPowerRatioAlpha,
               EchoSuppressor::kHangoverBlocks,
               EchoSuppressor::kAttack,
               EchoSuppressor::kRelease);

  for (size_t n = 0; n < blocks; ++n) {
    const size_t offset = n * kBlockSamples;
    for (int i = 0; i < kBlockSamples; ++i) {
      far_block[i] = static_cast<float>(x.samples[offset + i]) * kInvScale;
      near_block[i] = static_cast<float>(y.samples[offset + i]) * kInvScale;
    }
    float gate_gain = 1.0f;
    int estimated_lag = 0;
    suppressor.process_block(far_block.data(),
                             near_block.data(),
                             out_block.data(),
                             &gate_gain,
                             &estimated_lag);
    float mute_ratio = std::max(0.0f, 1.0f - gate_gain);
    for (int i = 0; i < kBlockSamples; ++i) {
      float sample = std::max(-1.0f, std::min(1.0f, out_block[i]));
      processed[offset + i] = static_cast<int16_t>(std::lrintf(sample * kScale));
    }
    const char* gain_meter = GainMeterString(gate_gain);
    if (estimated_lag >= 0) {
      std::fprintf(stderr, "[block %zu] mute=%.1f%% (gain=%.3f %s, lag=%d samples)\n",
                   n,
                   mute_ratio * 100.0f,
                   gate_gain,
                   gain_meter,
                   estimated_lag);
    } else {
      std::fprintf(stderr, "[block %zu] mute=%.1f%% (gain=%.3f %s, lag=--)\n",
                   n,
                   mute_ratio * 100.0f,
                   gate_gain,
                   gain_meter);
    }
  }
  // Save processed signal as processed.wav (PCM16 mono 16kHz)
  const uint32_t sr = kSampleRateHz;
  const uint16_t ch = 1;
  const uint16_t bps = 16;
  const uint32_t data_bytes = static_cast<uint32_t>(processed.size() * sizeof(int16_t));
  const uint32_t byte_rate = sr * ch * (bps/8);
  const uint16_t block_align = ch * (bps/8);
  std::ofstream wf("processed.wav", std::ios::binary);
  if (wf){
    // RIFF header
    wf.write("RIFF",4);
    uint32_t file_size_minus_8 = 36 + data_bytes; wf.write(reinterpret_cast<const char*>(&file_size_minus_8),4);
    wf.write("WAVE",4);
    // fmt chunk
    wf.write("fmt ",4);
    uint32_t fmt_size = 16; wf.write(reinterpret_cast<const char*>(&fmt_size),4);
    uint16_t audio_format = 1; wf.write(reinterpret_cast<const char*>(&audio_format),2);
    wf.write(reinterpret_cast<const char*>(&ch),2);
    wf.write(reinterpret_cast<const char*>(&sr),4);
    wf.write(reinterpret_cast<const char*>(&byte_rate),4);
    wf.write(reinterpret_cast<const char*>(&block_align),2);
    wf.write(reinterpret_cast<const char*>(&bps),2);
    // data chunk
    wf.write("data",4);
    wf.write(reinterpret_cast<const char*>(&data_bytes),4);
    wf.write(reinterpret_cast<const char*>(processed.data()), data_bytes);
  }
  return 0;
}
