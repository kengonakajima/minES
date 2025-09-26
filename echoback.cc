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
  size_t delay_target_samples = 0;  // optional capture-side delay in samples (default off)

  // Optional extra delay applied to far-end reference before suppression
  std::deque<int16_t> far_delay_line;  // buffered far samples for loopback delay simulation
  size_t loopback_delay_target_samples = 0;  // additional far-end delay in samples

  // --passthrough: AEC を行わず素通し再生
  bool passthrough = false;
  EchoSuppressor suppressor;
  size_t block_counter = 0;

  // 遅延推定の観察用統計
  std::deque<int> lag_history;
  long long lag_sum = 0;
  size_t lag_history_limit = 10;
  bool lag_stats_ready = false;
  double lag_avg_latest = 0.0;
  int lag_min_window = 0;
  int lag_max_window = 0;
  int lag_last = -1;

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

inline void apply_loopback_delay(State& s, int16_t* block) {
  const size_t target = s.loopback_delay_target_samples; // 回り込み音を意図的に遅らせる
  if (target == 0) return;

  for (int i = 0; i < kBlockLen; ++i) {
    s.far_delay_line.push_back(block[i]);
    int16_t delayed = 0;
    if (s.far_delay_line.size() > target) {
      delayed = s.far_delay_line.front();
      s.far_delay_line.pop_front();
    }
    block[i] = delayed;
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
    std::vector<int16_t> speaker_blk = far_blk;  // スピーカへ送る信号は個別に遅延する

    apply_loopback_delay(s, speaker_blk.data());

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

    if (estimated_lag >= 0) {
      s.lag_history.push_back(estimated_lag);
      s.lag_sum += estimated_lag;
      if (s.lag_history.size() > s.lag_history_limit) {
        s.lag_sum -= s.lag_history.front();
        s.lag_history.pop_front();
      }
      const size_t window_size = s.lag_history.size();
      if (window_size > 0) {
        int min_val = s.lag_history.front();
        int max_val = s.lag_history.front();
        for (int lag_sample : s.lag_history) {
          if (lag_sample < min_val) {
            min_val = lag_sample;
          }
          if (lag_sample > max_val) {
            max_val = lag_sample;
          }
        }
        s.lag_avg_latest = static_cast<double>(s.lag_sum) / static_cast<double>(window_size);
        s.lag_min_window = min_val;
        s.lag_max_window = max_val;
        s.lag_last = estimated_lag;
        s.lag_stats_ready = true;
      }
    }
    const size_t lag_window_size = s.lag_history.size();

    if (!s.passthrough) {
      float mute_ratio = 1.0f - gate_gain;
      if (mute_ratio < 0.0f) mute_ratio = 0.0f;
      const char* gain_meter = GainMeterString(gate_gain);
      if (s.lag_stats_ready) {
        char lag_current_buf[32];
        const char* lag_current_str = "--";
        if (estimated_lag >= 0) {
          std::snprintf(lag_current_buf, sizeof(lag_current_buf), "%d", estimated_lag);
          lag_current_str = lag_current_buf;
        }
        const size_t avg_window = lag_window_size > 0 ? lag_window_size : s.lag_history_limit;
        std::fprintf(stderr,
                     "[block %zu] mute=%.1f%% (gain=%.3f %s, lag=%s samples; avg%zu=%.1f, min=%d, max=%d, last=%d)\n",
                     s.block_counter,
                     mute_ratio * 100.0f,
                     gate_gain,
                     gain_meter,
                     lag_current_str,
                     avg_window,
                     s.lag_avg_latest,
                     s.lag_min_window,
                     s.lag_max_window,
                     s.lag_last);
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

    // 今回のスピーカ出力はループバック遅延を適用した信号
    push_block(s.out_dev, speaker_blk.data(), kBlockLen);
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
  State s;
  // 16k/10msブロック固定

  // 引数パース
  for (int i = 1; i < argc; ++i) {
    std::string arg(argv[i] ? argv[i] : "");
    if (arg == "--passthrough" || arg == "-p") {
      s.passthrough = true;
    } else if (arg.rfind("--input-delay-ms=", 0) == 0) {
      std::string value = arg.substr(strlen("--input-delay-ms="));
      long long delay_ms = std::stoll(value);
      size_t raw_samples = static_cast<size_t>(
          (delay_ms * static_cast<long long>(kSampleRateHz) + 999) / 1000);
      const size_t block = static_cast<size_t>(kBlockLen);
      size_t delay_blocks = (raw_samples + block / 2) / block;
      s.delay_target_samples = delay_blocks * block;
    } else if (arg.rfind("--loopback-delay-ms=", 0) == 0) {
      std::string value = arg.substr(strlen("--loopback-delay-ms="));
      long long delay_ms = std::stoll(value);
      if (delay_ms < 0) delay_ms = 0;
      size_t raw_samples = static_cast<size_t>(
          (delay_ms * static_cast<long long>(kSampleRateHz) + 999) / 1000);
      s.loopback_delay_target_samples = raw_samples;
      s.far_delay_line.clear();
    } else if (arg == "--input-delay-ms" && i + 1 < argc) {
      std::string value(argv[++i] ? argv[i] : "0");
      long long delay_ms = std::stoll(value);
      size_t raw_samples = static_cast<size_t>(
          (delay_ms * static_cast<long long>(kSampleRateHz) + 999) / 1000);
      const size_t block = static_cast<size_t>(kBlockLen);
      size_t delay_blocks = (raw_samples + block / 2) / block;
      s.delay_target_samples = delay_blocks * block;
    } else if (arg == "--loopback-delay-ms" && i + 1 < argc) {
      std::string value(argv[++i] ? argv[i] : "0");
      long long delay_ms = std::stoll(value);
      if (delay_ms < 0) delay_ms = 0;
      size_t raw_samples = static_cast<size_t>(
          (delay_ms * static_cast<long long>(kSampleRateHz) + 999) / 1000);
      s.loopback_delay_target_samples = raw_samples;
      s.far_delay_line.clear();
    } else if (arg == "--help" || arg == "-h") {
      std::fprintf(stderr,
                   "Usage: %s [--passthrough] [--input-delay-ms <ms>] [--loopback-delay-ms <ms>]\n",
                   argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
      return 1;
    }
  }
  const char* mode = s.passthrough ? "passthrough" : "suppressor";
  std::fprintf(stderr, "echoback (16k mono): mode=%s\n", mode);
  if (!s.passthrough) {
    std::fprintf(stderr,
                 "  config: atten=%.1f dB, rho=%.2f, ratio=%.2f, hang=%d, attack=%.3f, release=%.3f\n",
                 -80.0f,
                 EchoSuppressor::kRhoThresh,
                 EchoSuppressor::kPowerRatioAlpha,
                 EchoSuppressor::kHangoverBlocks,
                 EchoSuppressor::kAttack,
                 EchoSuppressor::kRelease);
  }
  // 固定設定のため追加初期化不要

  {
    const double input_delay_ms = static_cast<double>(s.delay_target_samples) * 1000.0 /
                                  static_cast<double>(kSampleRateHz);
    const double input_delay_blocks = static_cast<double>(s.delay_target_samples) /
                                      static_cast<double>(kBlockLen);
    std::fprintf(stderr,
                 "input-delay-ms(final): %.1f ms (%zu samples, %.1f blocks)\n",
                 input_delay_ms,
                 s.delay_target_samples,
                 input_delay_blocks);
  }

  {
    const double loopback_delay_ms = static_cast<double>(s.loopback_delay_target_samples) * 1000.0 /
                                     static_cast<double>(kSampleRateHz);
    std::fprintf(stderr,
                 "loopback-delay-ms(final): %.1f ms (%zu samples)\n",
                 loopback_delay_ms,
                 s.loopback_delay_target_samples);
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
