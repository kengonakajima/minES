#ifndef MINES_SUPPRESSOR_H_
#define MINES_SUPPRESSOR_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

enum class LagMetric {
  kNCC,
  kAMDF,
};

inline const char* LagMetricName(LagMetric metric) {
  switch (metric) {
    case LagMetric::kAMDF:
      return "amdf";
    case LagMetric::kNCC:
    default:
      return "ncc";
  }
}

inline const char* GainMeterString(float gain) {
  float g = std::clamp(gain, 0.0f, 1.0f);
  if (g <= 0.05f) return "    ";
  if (g <= 0.25f) return "*   ";
  if (g <= 0.50f) return "**  ";
  if (g <= 0.75f) return "*** ";
  return "****";
}

struct EchoSuppressorConfig {
  float rho_thresh = 0.6f;         // 相関しきい値
  float power_ratio_alpha = 1.3f;  // P_y < alpha * P_x
  float atten_db = -80.0f;         // 抑圧時ゲイン[dB]
  int hangover_blocks = 20;        // 抑圧継続ブロック数
  float attack = 0.1f;             // 抑圧時のゲイン追従（適度に速い）
  float release = 0.01f;           // 解除時のゲイン追従
  LagMetric lag_metric = LagMetric::kNCC;  // 遅延探索の類似度指標
};

class EchoSuppressor {
 public:
  explicit EchoSuppressor(int sample_rate = 16000,
                          const EchoSuppressorConfig& config = {})
      : fs_(sample_rate) {
    block_samples_ = std::max(1, fs_ / 100);         // 10ms幅のブロック
    max_lag_samples_ = std::max(block_samples_,      // ブロック長以上になるように確保
                                static_cast<int>(0.08f * static_cast<float>(fs_)));
    const size_t hist_len = static_cast<size_t>(max_lag_samples_) +
                            static_cast<size_t>(block_samples_) * 4;
    far_hist_.assign(hist_len, 0.0f);
    set_config(config);
    reset();
  }

  void reset() {
    std::fill(far_hist_.begin(), far_hist_.end(), 0.0f);
    hist_pos_ = 0;
    gate_gain_ = 1.0f;
    hang_cnt_ = 0;
  }

  int block_samples() const { return block_samples_; }

  void set_config(const EchoSuppressorConfig& config) {
    config_ = config;
    atten_linear_ = std::pow(10.0f, config_.atten_db / 20.0f);
    // ゼロに近いときの安全策
    if (atten_linear_ < 0.0f) {
      atten_linear_ = 0.0f;
    }
  }

  const EchoSuppressorConfig& config() const { return config_; }

  bool process_block(const float* far,
                     const float* mic,
                     float* out,
                     float* applied_gain = nullptr,
                     int* estimated_lag_samples = nullptr) {
    const int block = block_samples_;
    const size_t hist_size = far_hist_.size();

    // 遠端信号を履歴バッファへ格納
    for (int i = 0; i < block; ++i) {
      far_hist_[hist_pos_] = far[i];
      hist_pos_ = (hist_pos_ + 1) % hist_size;
    }

    auto wrap_index = [hist_size](size_t pos, ptrdiff_t offset) {
      ptrdiff_t idx = static_cast<ptrdiff_t>(pos) + offset;
      idx %= static_cast<ptrdiff_t>(hist_size);
      if (idx < 0) {
        idx += static_cast<ptrdiff_t>(hist_size);
      }
      return static_cast<size_t>(idx);
    };

    // 現在ブロックのマイク電力
    float mic_pow = 0.0f;
    for (int i = 0; i < block; ++i) {
      mic_pow += mic[i] * mic[i];
    }
    mic_pow = std::max(mic_pow, 1e-9f);

    float mic_abs = 0.0f;
    const bool use_amdf = (config_.lag_metric == LagMetric::kAMDF);
    if (use_amdf) {
      for (int i = 0; i < block; ++i) {
        mic_abs += std::fabs(mic[i]);
      }
      mic_abs = std::max(mic_abs, 1e-9f);
    }

    // 1msごと（最低でも1サンプルごと）に高速NCC探索
    // NCC=正規化相互相関（Normalized Cross-Correlation）で遠端信号とマイク信号の類似度を評価
    const int lag_step = std::max(1, fs_ / 1000);
    float best_score = -std::numeric_limits<float>::infinity();
    int best_lag = 0;
    float best_far_pow = 1e-9f;

    for (int lag = 0; lag <= max_lag_samples_; lag += lag_step) {
      float accum = 0.0f;
      float far_pow = 0.0f;
      float far_abs = 0.0f;
      for (int i = 0; i < block; ++i) {
        const size_t idx = wrap_index(hist_pos_,
                                      -static_cast<ptrdiff_t>(block + lag) + i);
        const float fx = far_hist_[idx];
        const float my = mic[i];
        far_pow += fx * fx;
        if (use_amdf) {
          accum += std::fabs(fx - my);
          far_abs += std::fabs(fx);
        } else {
          accum += fx * my;
        }
      }
      far_pow = std::max(far_pow, 1e-9f);
      float score = 0.0f;
      if (use_amdf) {
        float denom = mic_abs + far_abs;
        denom = std::max(denom, 1e-9f);
        score = 1.0f - (accum / denom);
        if (score > 1.0f) {
          score = 1.0f;
        } else if (score < -1.0f) {
          score = -1.0f;
        }
      } else {
        score = accum / std::sqrt(far_pow * mic_pow);
      }
      if (score > best_score) {
        best_score = score;
        best_lag = lag;
        best_far_pow = far_pow;
      }
    }

    best_far_pow = std::max(best_far_pow, 1e-9f);

    const bool echo_detected = (best_score > config_.rho_thresh) &&
                               (mic_pow < config_.power_ratio_alpha * best_far_pow);

    if (estimated_lag_samples) {
      *estimated_lag_samples = echo_detected ? best_lag : -1;
    }

    bool suppress = echo_detected;
    if (suppress) {
      hang_cnt_ = config_.hangover_blocks;
    } else if (hang_cnt_ > 0) {
      --hang_cnt_;
      suppress = true;
    }

    const float target_gain = suppress ? atten_linear_ : 1.0f;
    const float coeff = (target_gain < gate_gain_) ? config_.attack : config_.release;
    gate_gain_ = (1.0f - coeff) * gate_gain_ + coeff * target_gain;

    for (int i = 0; i < block; ++i) {
      out[i] = mic[i] * gate_gain_;
    }
    if (applied_gain) {
      *applied_gain = gate_gain_;
    }
    return suppress;
  }

 private:
  int fs_;
  int block_samples_;
  int max_lag_samples_;
  EchoSuppressorConfig config_{};
  float atten_linear_ = std::pow(10.0f, -18.0f / 20.0f);

  std::vector<float> far_hist_;
  size_t hist_pos_ = 0;
  float gate_gain_ = 1.0f;
  int hang_cnt_ = 0;
};

#endif  // MINES_SUPPRESSOR_H_
