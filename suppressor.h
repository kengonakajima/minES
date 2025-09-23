#ifndef MINES_SUPPRESSOR_H_
#define MINES_SUPPRESSOR_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

struct EchoSuppressorConfig {
  float rho_thresh = 0.6f;         // 相関しきい値
  float power_ratio_alpha = 1.3f;  // P_y < alpha * P_x
  float atten_db = -80.0f;         // 抑圧時ゲイン[dB]
  int hangover_frames = 20;        // 抑圧継続フレーム数
  float attack = 1.0f;             // 抑圧時のゲイン追従（即時）
  float release = 0.05f;           // 解除時のゲイン追従
};

class EchoSuppressor {
 public:
  explicit EchoSuppressor(int sample_rate = 16000,
                          const EchoSuppressorConfig& config = {})
      : fs_(sample_rate) {
    frame_samples_ = std::max(1, fs_ / 100);         // 10 ms frame
    max_lag_samples_ = std::max(frame_samples_,      // ensure >= frame
                                static_cast<int>(0.08f * static_cast<float>(fs_)));
    const size_t hist_len = static_cast<size_t>(max_lag_samples_) +
                            static_cast<size_t>(frame_samples_) * 4;
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

  int frame_samples() const { return frame_samples_; }

  void set_config(const EchoSuppressorConfig& config) {
    config_ = config;
    atten_linear_ = std::pow(10.0f, config_.atten_db / 20.0f);
    // ゼロに近いときの安全策
    if (atten_linear_ < 0.0f) {
      atten_linear_ = 0.0f;
    }
  }

  const EchoSuppressorConfig& config() const { return config_; }

  bool process_frame(const float* far,
                     const float* mic,
                     float* out,
                     float* applied_gain = nullptr) {
    const int frame = frame_samples_;
    const size_t hist_size = far_hist_.size();

    // push far samples into history buffer
    for (int i = 0; i < frame; ++i) {
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

    // microphone power over current frame
    float mic_pow = 0.0f;
    for (int i = 0; i < frame; ++i) {
      mic_pow += mic[i] * mic[i];
    }
    mic_pow = std::max(mic_pow, 1e-9f);

    // quick NCC search every 1 ms (or at least 1 sample)
    const int lag_step = std::max(1, fs_ / 1000);
    int best_lag = 0;
    float best_rho = 0.0f;

    for (int lag = 0; lag <= max_lag_samples_; lag += lag_step) {
      float num = 0.0f;
      float far_pow = 0.0f;
      for (int i = 0; i < frame; ++i) {
        const size_t idx = wrap_index(hist_pos_,
                                      -static_cast<ptrdiff_t>(frame + lag) + i);
        const float fx = far_hist_[idx];
        const float my = mic[i];
        num += fx * my;
        far_pow += fx * fx;
      }
      far_pow = std::max(far_pow, 1e-9f);
      const float rho = num / std::sqrt(far_pow * mic_pow);
      if (rho > best_rho) {
        best_rho = rho;
        best_lag = lag;
      }
    }

    // power at best lag
    float far_pow_best = 0.0f;
    for (int i = 0; i < frame; ++i) {
      const size_t idx = wrap_index(hist_pos_,
                                    -static_cast<ptrdiff_t>(frame + best_lag) + i);
      const float fx = far_hist_[idx];
      far_pow_best += fx * fx;
    }
    far_pow_best = std::max(far_pow_best, 1e-9f);

    const bool echo_detected = (best_rho > config_.rho_thresh) &&
                               (mic_pow < config_.power_ratio_alpha * far_pow_best);

    bool suppress = echo_detected;
    if (suppress) {
      hang_cnt_ = config_.hangover_frames;
    } else if (hang_cnt_ > 0) {
      --hang_cnt_;
      suppress = true;
    }

    const float target_gain = suppress ? atten_linear_ : 1.0f;
    const float coeff = (target_gain < gate_gain_) ? config_.attack : config_.release;
    gate_gain_ = (1.0f - coeff) * gate_gain_ + coeff * target_gain;

    for (int i = 0; i < frame; ++i) {
      out[i] = mic[i] * gate_gain_;
    }
    if (applied_gain) {
      *applied_gain = gate_gain_;
    }
    return suppress;
  }

 private:
  int fs_;
  int frame_samples_;
  int max_lag_samples_;
  EchoSuppressorConfig config_{};
  float atten_linear_ = std::pow(10.0f, -18.0f / 20.0f);

  std::vector<float> far_hist_;
  size_t hist_pos_ = 0;
  float gate_gain_ = 1.0f;
  int hang_cnt_ = 0;
};

#endif  // MINES_SUPPRESSOR_H_
