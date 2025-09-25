#ifndef MINES_SUPPRESSOR_H_
#define MINES_SUPPRESSOR_H_

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

inline const char* GainMeterString(float gain) {
  float g = std::clamp(gain, 0.0f, 1.0f);
  if (g <= 0.05f) return "    ";
  if (g <= 0.25f) return "*   ";
  if (g <= 0.50f) return "**  ";
  if (g <= 0.75f) return "*** ";
  return "****";
}

class EchoSuppressor {
 public:
  static constexpr int kSampleRate = 16000;
  static constexpr int kBlockSamples = kSampleRate / 100;   // 10 ms
  static constexpr int kMaxLagSamples = static_cast<int>(0.08f * static_cast<float>(kSampleRate));
  static constexpr int kLagStep = kSampleRate / 1000;       // 1 ms
  static constexpr float kRhoThresh = 0.6f;
  static constexpr float kPowerRatioAlpha = 1.3f;
  static constexpr float kAttenLinear = 0.0001f;            // 10^(-80/20)
  static constexpr int kHangoverBlocks = 20;
  static constexpr float kAttack = 0.1f;
  static constexpr float kRelease = 0.01f;

  EchoSuppressor() {
    const size_t hist_len = static_cast<size_t>(kMaxLagSamples) +
                            static_cast<size_t>(kBlockSamples) * 4;
    far_hist_.assign(hist_len, 0.0f);
    reset();
  }

  void reset() {
    std::fill(far_hist_.begin(), far_hist_.end(), 0.0f);
    hist_pos_ = 0;
    gate_gain_ = 1.0f;
    hang_cnt_ = 0;
  }

  int block_samples() const { return kBlockSamples; }

  bool process_block(const float* far,
                     const float* mic,
                     float* out,
                     float* applied_gain = nullptr,
                     int* estimated_lag_samples = nullptr) {
    const int block = kBlockSamples;
    const size_t hist_size = far_hist_.size();

    // Step 1: 遠端ブロックを履歴バッファに書き込む
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

    // Step 2: マイクブロックの電力を計算
    float mic_pow = 0.0f;
    for (int i = 0; i < block; ++i) {
      mic_pow += mic[i] * mic[i];
    }
    mic_pow = std::max(mic_pow, 1e-9f);

    float mic_abs = 0.0f;
    for (int i = 0; i < block; ++i) {
      mic_abs += std::fabs(mic[i]);
    }
    mic_abs = std::max(mic_abs, 1e-9f);

    // Step 3: AMDFベースで遅延探索
    const int lag_step = kLagStep;
    float best_score = -std::numeric_limits<float>::infinity();
    int best_lag = 0;
    float best_far_pow = 1e-9f;

    for (int lag = 0; lag <= kMaxLagSamples; lag += lag_step) {
      float accum = 0.0f;
      float far_pow = 0.0f;
      float far_abs = 0.0f;
      for (int i = 0; i < block; ++i) {
        const size_t idx = wrap_index(hist_pos_,
                                      -static_cast<ptrdiff_t>(block + lag) + i);
        const float fx = far_hist_[idx];
        const float my = mic[i];
        far_pow += fx * fx;
        accum += std::fabs(fx - my);
        far_abs += std::fabs(fx);
      }
      far_pow = std::max(far_pow, 1e-9f);
      float denom = mic_abs + far_abs;
      denom = std::max(denom, 1e-9f);
      float score = 1.0f - (accum / denom);
      if (score > 1.0f) {
        score = 1.0f;
      } else if (score < -1.0f) {
        score = -1.0f;
      }
      if (score > best_score) {
        best_score = score;
        best_lag = lag;
        best_far_pow = far_pow;
      }
    }

    // Step 4: 最良ラグでの遠端電力を確定
    best_far_pow = std::max(best_far_pow, 1e-9f);

    // Step 5: スコア・パワー条件でエコー検出
    const bool echo_detected = (best_score > kRhoThresh) &&
                               (mic_pow < kPowerRatioAlpha * best_far_pow);

    if (estimated_lag_samples) {
      *estimated_lag_samples = echo_detected ? best_lag : -1;
    }

    // Step 6: ハングオーバ制御
    bool suppress = echo_detected;
    if (suppress) {
      hang_cnt_ = kHangoverBlocks;
    } else if (hang_cnt_ > 0) {
      --hang_cnt_;
      suppress = true;
    }

    // Step 7: ターゲットゲイン設定
    const float target_gain = suppress ? kAttenLinear : 1.0f;
    // Step 8: ゲイン追従（攻撃/解放）
    const float coeff = (target_gain < gate_gain_) ? kAttack : kRelease;
    gate_gain_ = (1.0f - coeff) * gate_gain_ + coeff * target_gain;

    // Step 9: 出力生成
    for (int i = 0; i < block; ++i) {
      out[i] = mic[i] * gate_gain_;
    }
    if (applied_gain) {
      *applied_gain = gate_gain_;
    }
    return suppress;
  }

 private:
  std::vector<float> far_hist_;
  size_t hist_pos_ = 0;
  float gate_gain_ = 1.0f;
  int hang_cnt_ = 0;
};

#endif  // MINES_SUPPRESSOR_H_
