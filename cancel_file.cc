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
constexpr int kFrameSamples = 160;

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
  EchoSuppressorConfig config;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i] ? argv[i] : "");
    if (arg == "--atten-db" && i + 1 < argc) {
      config.atten_db = std::stof(argv[++i] ? argv[i] : "-18");
    } else if (arg.rfind("--atten-db=", 0) == 0) {
      config.atten_db = std::stof(arg.substr(strlen("--atten-db=")));
    } else if (arg == "--rho" && i + 1 < argc) {
      config.rho_thresh = std::stof(argv[++i] ? argv[i] : "0.6");
    } else if (arg.rfind("--rho=", 0) == 0) {
      config.rho_thresh = std::stof(arg.substr(strlen("--rho=")));
    } else if (arg == "--ratio" && i + 1 < argc) {
      config.power_ratio_alpha = std::stof(argv[++i] ? argv[i] : "1.5");
    } else if (arg.rfind("--ratio=", 0) == 0) {
      config.power_ratio_alpha = std::stof(arg.substr(strlen("--ratio=")));
    } else if (arg == "--hang" && i + 1 < argc) {
      config.hangover_frames = std::max(0, std::stoi(argv[++i] ? argv[i] : "5"));
    } else if (arg.rfind("--hang=", 0) == 0) {
      config.hangover_frames = std::max(0, std::stoi(arg.substr(strlen("--hang="))));
    } else if (arg == "--attack" && i + 1 < argc) {
      config.attack = std::stof(argv[++i] ? argv[i] : "0.2");
    } else if (arg.rfind("--attack=", 0) == 0) {
      config.attack = std::stof(arg.substr(strlen("--attack=")));
    } else if (arg == "--release" && i + 1 < argc) {
      config.release = std::stof(argv[++i] ? argv[i] : "0.02");
    } else if (arg.rfind("--release=", 0) == 0) {
      config.release = std::stof(arg.substr(strlen("--release=")));
    } else if (arg == "--help" || arg == "-h") {
      std::fprintf(stderr,
                   "Usage: %s [options] <render.wav> <capture.wav>\n"
                   "  options: --atten-db <db> --rho <val> --ratio <val> --hang <frames> --attack <0-1> --release <0-1>\n",
                   argv[0]);
      return 0;
    } else {
      positional.push_back(arg);
    }
  }

  if (positional.size() < 2){
    std::fprintf(stderr, "Usage: %s [options] <render.wav> <capture.wav>\n", argv[0]);
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
  size_t frames = std::min(x.samples.size(), y.samples.size()) /
                  static_cast<size_t>(kFrameSamples);
  if (frames == 0) {
    std::fprintf(stderr, "Not enough samples to process.\n");
    return 1;
  }

  EchoSuppressor suppressor(kSampleRateHz, config);
  std::vector<int16_t> processed(frames * kFrameSamples);
  std::vector<float> far_frame(kFrameSamples), near_frame(kFrameSamples), out_frame(kFrameSamples);
  static constexpr float kInvScale = 1.0f / 32768.0f;
  static constexpr float kScale = 32767.0f;

  std::fprintf(stderr,
               "config: atten=%.1f dB, rho=%.2f, ratio=%.2f, hang=%d, attack=%.3f, release=%.3f\n",
               config.atten_db,
               config.rho_thresh,
               config.power_ratio_alpha,
               config.hangover_frames,
               config.attack,
               config.release);

  for (size_t n = 0; n < frames; ++n) {
    const size_t offset = n * kFrameSamples;
    for (int i = 0; i < kFrameSamples; ++i) {
      far_frame[i] = static_cast<float>(x.samples[offset + i]) * kInvScale;
      near_frame[i] = static_cast<float>(y.samples[offset + i]) * kInvScale;
    }
    float gate_gain = 1.0f;
    suppressor.process_frame(far_frame.data(),
                             near_frame.data(),
                             out_frame.data(),
                             &gate_gain);
    float mute_ratio = std::max(0.0f, 1.0f - gate_gain);
    for (int i = 0; i < kFrameSamples; ++i) {
      float sample = std::max(-1.0f, std::min(1.0f, out_frame[i]));
      processed[offset + i] = static_cast<int16_t>(std::lrintf(sample * kScale));
    }
    std::fprintf(stderr, "[block %zu] mute=%.1f%% (gain=%.3f)\n",
                 n,
                 mute_ratio * 100.0f,
                 gate_gain);
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
