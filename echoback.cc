// Echoback (C++): 最小構成のローカル・エコーバック + スイッチ式エコーサプレッサ
// Usage:
//   ./echoback [--passthrough]
// 前提:
//   - 16 kHz モノラル固定, 16-bit I/O (PortAudio デフォルトデバイス)
//   - エコーサプレッサは 10ms（160サンプル）単位で処理
//   - 参照信号は直前にスピーカへ出したブロック（ローカル・ループバック相当）

#include <portaudio.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <cctype>

#include "suppressor.h"

constexpr int kSampleRateHz = 16000;
constexpr int kBlockLen = 160;  // 10 ms block at 16 kHz

 

struct State {
  // 1ch 固定（PortAudioデバイス設定も1ch）
  // ジッタバッファは固定遅延（最小構成: 内部固定値）

  // FIFOs in device domain (dev_sr)
  std::deque<int16_t> rec_dev;     // mic captured samples
  std::deque<int16_t> out_dev;     // to speaker

  // 処理フレーム = デバイスサンプルレート領域（10ms = 160サンプル）

  // Jitter buffer (device domain). Local echo path accumulator, mixes into speaker
  std::deque<int16_t> jitter;      // accumulate processed to emulate loopback latency

  // Optional startup delay for capture stream (educational jitter buffer)
  std::deque<int16_t> delay_line;  // raw capture samples waiting for release
  size_t delay_target_samples = static_cast<size_t>(kBlockLen) * 15; // 150 ms 初期遅延

  // --passthrough: AEC を行わず素通し再生
  bool passthrough = false;
  EchoSuppressor suppressor{kSampleRateHz};
  size_t block_counter = 0;

  // 単一インスタンス化API。個別インスタンスは不要。
};

size_t pop_samples(std::deque<int16_t>& q, int16_t* dst, size_t n){
  size_t m = q.size()<n ? q.size() : n;
  for (size_t i=0;i<m;i++){ dst[i]=q.front(); q.pop_front(); }
  return m;
}

void push_block(std::deque<int16_t>& q, const int16_t* src, size_t n){
  for (size_t i=0;i<n;i++) q.push_back(src[i]);
}

inline void enqueue_capture_sample(State& s, int16_t sample) {
  s.delay_line.push_back(sample);
  if (s.delay_line.size() <= s.delay_target_samples) {
    s.rec_dev.push_back(0);
  } else {
    s.rec_dev.push_back(s.delay_line.front());
    s.delay_line.pop_front();
  }
}

void process_available_blocks(State& s){
  // Run as many 10 ms (160-sample) blocks as possible
  while (s.rec_dev.size() >= static_cast<size_t>(kBlockLen)) {
    std::vector<int16_t> near_blk(kBlockLen), far_blk(kBlockLen), out_blk(kBlockLen);
    // 1) pop near block
    pop_samples(s.rec_dev, near_blk.data(), kBlockLen);
    // 2) pop far block（スピーカへ送る信号 = 参照）
    if (s.jitter.size() >= static_cast<size_t>(kBlockLen)) {
      pop_samples(s.jitter, far_blk.data(), kBlockLen);
    } else {
      std::fill(far_blk.begin(), far_blk.end(), 0);
    }

    float gate_gain = 1.0f;
    int estimated_lag = 0;
    if (s.passthrough) {
      std::memcpy(out_blk.data(), near_blk.data(), kBlockLen * sizeof(int16_t));
    } else {
      static constexpr float kInvScale = 1.0f / 32768.0f;
      static constexpr float kScale = 32767.0f;
      std::vector<float> far_block_f(kBlockLen), near_block_f(kBlockLen), out_block_f(kBlockLen);
      for (int i = 0; i < kBlockLen; ++i) {
        far_block_f[i] = static_cast<float>(far_blk[i]) * kInvScale;
        near_block_f[i] = static_cast<float>(near_blk[i]) * kInvScale;
      }
      s.suppressor.process_block(far_block_f.data(),
                                 near_block_f.data(),
                                 out_block_f.data(),
                                 &gate_gain,
                                 &estimated_lag);
      for (int i = 0; i < kBlockLen; ++i) {
        float sample = std::max(-1.0f, std::min(1.0f, out_block_f[i]));
        out_blk[i] = static_cast<int16_t>(std::lrintf(sample * kScale));
      }
    }

    if (!s.passthrough) {
      float mute_ratio = 1.0f - gate_gain;
      if (mute_ratio < 0.0f) mute_ratio = 0.0f;
      const char* gain_meter = GainMeterString(gate_gain);
      if (estimated_lag >= 0) {
        std::fprintf(stderr, "[block %zu] mute=%.1f%% (gain=%.3f %s, lag=%d samples)\n",
                     s.block_counter,
                     mute_ratio * 100.0f,
                     gate_gain,
                     gain_meter,
                     estimated_lag);
      } else {
        std::fprintf(stderr, "[block %zu] mute=%.1f%% (gain=%.3f %s, lag=--)\n",
                     s.block_counter,
                     mute_ratio * 100.0f,
                     gate_gain,
                     gain_meter);
      }
    }
    ++s.block_counter;

    // ローカル・ループバック: 処理後の出力を蓄積して、次々回以降の far にする
    push_block(s.jitter, out_blk.data(), kBlockLen);

    // 今回のスピーカ出力は `far_blk`
    push_block(s.out_dev, far_blk.data(), kBlockLen);
  }
}

int pa_callback(const void* inputBuffer,
                void* outputBuffer,
                unsigned long blockSize,
                const PaStreamCallbackTimeInfo* /*timeInfo*/,
                PaStreamCallbackFlags /*statusFlags*/,
                void* userData){
  auto* st = reinterpret_cast<State*>(userData);
  const int16_t* in = reinterpret_cast<const int16_t*>(inputBuffer);
  int16_t* out = reinterpret_cast<int16_t*>(outputBuffer);
  const unsigned long n = blockSize; // mono (framesPerBuffer)
  // Single-threaded実行のため終了フラグは不要

  // 1) enqueue capture
  for (unsigned long i = 0; i < n; ++i) {
    int16_t sample = in ? in[i] : 0;
    enqueue_capture_sample(*st, sample);
  }

  // 2) run AEC3 on as many blocks as ready
  process_available_blocks(*st);

  // 3) emit output
  for (unsigned long i=0;i<n;i++){
    if (!st->out_dev.empty()){ out[i]=st->out_dev.front(); st->out_dev.pop_front(); }
    else { out[i]=0; }
  }
  return paContinue;
}

int main(int argc, char** argv){
  EchoSuppressorConfig sup_config;
  State s;
  // 16k/10msブロック固定

  auto parse_metric = [](const std::string& value, LagMetric* metric) {
    std::string lowered = value;
    for (char& ch : lowered) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (lowered == "ncc") {
      *metric = LagMetric::kNCC;
      return true;
    }
    if (lowered == "amdf") {
      *metric = LagMetric::kAMDF;
      return true;
    }
    std::fprintf(stderr,
                 "Unknown lag metric '%s'. Use 'ncc' or 'amdf'.\n",
                 value.c_str());
    return false;
  };

  // 引数パース
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i] ? argv[i] : "");
    if (arg == "--passthrough" || arg == "-p") {
      s.passthrough = true;
    } else if (arg.rfind("--atten-db=", 0) == 0) {
      std::string value = arg.substr(strlen("--atten-db="));
      sup_config.atten_db = std::stof(value);
    } else if (arg == "--atten-db" && i + 1 < argc) {
      sup_config.atten_db = std::stof(argv[++i] ? argv[i] : "-18");
    } else if (arg.rfind("--rho=", 0) == 0) {
      std::string value = arg.substr(strlen("--rho="));
      sup_config.rho_thresh = std::stof(value);
    } else if (arg == "--rho" && i + 1 < argc) {
      sup_config.rho_thresh = std::stof(argv[++i] ? argv[i] : "0.6");
    } else if (arg.rfind("--ratio=", 0) == 0) {
      std::string value = arg.substr(strlen("--ratio="));
      sup_config.power_ratio_alpha = std::stof(value);
    } else if (arg == "--ratio" && i + 1 < argc) {
      sup_config.power_ratio_alpha = std::stof(argv[++i] ? argv[i] : "1.5");
    } else if (arg.rfind("--hang=", 0) == 0) {
      std::string value = arg.substr(strlen("--hang="));
      sup_config.hangover_blocks = std::max(0, std::stoi(value));
    } else if (arg == "--hang" && i + 1 < argc) {
      sup_config.hangover_blocks = std::max(0, std::stoi(argv[++i] ? argv[i] : "5"));
    } else if (arg.rfind("--attack=", 0) == 0) {
      std::string value = arg.substr(strlen("--attack="));
      sup_config.attack = std::stof(value);
    } else if (arg == "--attack" && i + 1 < argc) {
      sup_config.attack = std::stof(argv[++i] ? argv[i] : "0.2");
    } else if (arg.rfind("--release=", 0) == 0) {
      std::string value = arg.substr(strlen("--release="));
      sup_config.release = std::stof(value);
    } else if (arg == "--release" && i + 1 < argc) {
      sup_config.release = std::stof(argv[++i] ? argv[i] : "0.02");
    } else if (arg.rfind("--lag-metric=", 0) == 0) {
      std::string value = arg.substr(strlen("--lag-metric="));
      if (!parse_metric(value, &sup_config.lag_metric)) {
        return 1;
      }
    } else if (arg == "--lag-metric" && i + 1 < argc) {
      std::string value(argv[++i] ? argv[i] : "");
      if (!parse_metric(value, &sup_config.lag_metric)) {
        return 1;
      }
    } else if (arg.rfind("--input-delay-ms=", 0) == 0) {
      std::string value = arg.substr(strlen("--input-delay-ms="));
      long long delay_ms = std::stoll(value);
      size_t raw_samples = static_cast<size_t>(
          (delay_ms * static_cast<long long>(kSampleRateHz) + 999) / 1000);
      const size_t block = static_cast<size_t>(kBlockLen);
      size_t delay_blocks = (raw_samples + block / 2) / block;
      s.delay_target_samples = delay_blocks * block;
    } else if (arg == "--input-delay-ms" && i + 1 < argc) {
      std::string value(argv[++i] ? argv[i] : "0");
      long long delay_ms = std::stoll(value);
      size_t raw_samples = static_cast<size_t>(
          (delay_ms * static_cast<long long>(kSampleRateHz) + 999) / 1000);
      const size_t block = static_cast<size_t>(kBlockLen);
      size_t delay_blocks = (raw_samples + block / 2) / block;
      s.delay_target_samples = delay_blocks * block;
    } else if (arg == "--help" || arg == "-h") {
      std::fprintf(stderr,
                   "Usage: %s [--passthrough] [--input-delay-ms <ms>] [--atten-db <db>] [--rho <val>] [--ratio <val>] [--hang <blocks>] [--attack <0-1>] [--release <0-1>] [--lag-metric <ncc|amdf>]\n",
                   argv[0]);
      return 0;
    }
  }
  s.suppressor.set_config(sup_config);
  const char* mode = s.passthrough ? "passthrough" : "suppressor";
  std::fprintf(stderr, "echoback (16k mono): mode=%s\n", mode);
  if (!s.passthrough) {
    std::fprintf(stderr,
                 "  config: atten=%.1f dB, rho=%.2f, ratio=%.2f, hang=%d, attack=%.3f, release=%.3f, lag-metric=%s\n",
                 sup_config.atten_db,
                 sup_config.rho_thresh,
                 sup_config.power_ratio_alpha,
                 sup_config.hangover_blocks,
                 sup_config.attack,
                 sup_config.release,
                 LagMetricName(sup_config.lag_metric));
  }
  // 固定設定のため追加初期化不要

  if (s.delay_target_samples > 0) {
    double delay_ms = static_cast<double>(s.delay_target_samples) * 1000.0 /
                      static_cast<double>(kSampleRateHz);
    double blocks = static_cast<double>(s.delay_target_samples) /
                    static_cast<double>(kBlockLen);
    std::fprintf(stderr, "capture delay: %.1f ms (%.0f samples, %.1f blocks)\n",
                 delay_ms,
                 static_cast<double>(s.delay_target_samples),
                 blocks);
  }

  PaError err = Pa_Initialize();
  if (err!=paNoError){ std::fprintf(stderr, "Pa_Initialize error %s\n", Pa_GetErrorText(err)); return 1; }

  PaStream* stream=nullptr; PaStreamParameters inP{}, outP{};
  inP.device = Pa_GetDefaultInputDevice();
  outP.device = Pa_GetDefaultOutputDevice();
  if (inP.device<0 || outP.device<0){ std::fprintf(stderr, "No default device.\n"); Pa_Terminate(); return 1; }
  inP.channelCount = 1; inP.sampleFormat = paInt16;
  inP.suggestedLatency = Pa_GetDeviceInfo(inP.device)->defaultLowInputLatency;
  outP.channelCount = 1; outP.sampleFormat = paInt16;
  outP.suggestedLatency = Pa_GetDeviceInfo(outP.device)->defaultLowOutputLatency;

  err = Pa_OpenStream(&stream, &inP, &outP, kSampleRateHz, kBlockLen, paClipOff, pa_callback, &s);
  if (err!=paNoError){ std::fprintf(stderr, "Pa_OpenStream error %s\n", Pa_GetErrorText(err)); Pa_Terminate(); return 1; }
  err = Pa_StartStream(stream);
  if (err!=paNoError){ std::fprintf(stderr, "Pa_StartStream error %s\n", Pa_GetErrorText(err)); Pa_CloseStream(stream); Pa_Terminate(); return 1; }

  std::fprintf(stderr, "Running... Ctrl-C to stop.\n");
  while (Pa_IsStreamActive(stream)==1) {
    Pa_Sleep(100);
  }
  Pa_StopStream(stream); Pa_CloseStream(stream);
  Pa_Terminate();
  std::fprintf(stderr, "stopped.\n");
  return 0;
}
