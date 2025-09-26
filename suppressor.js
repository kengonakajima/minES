'use strict';

const SAMPLE_RATE = 16000; // サプレッサが扱うサンプリング周波数(Hz)
const BLOCK_SAMPLES = SAMPLE_RATE / 100; // 1ブロックあたりのサンプル数(10ms)
const MAX_LAG_SAMPLES = Math.floor(0.5 * SAMPLE_RATE); // 遅延探索の最大サンプル数
const LAG_STEP = Math.max(1, Math.floor(SAMPLE_RATE / 1000)); // 遅延探索のステップ幅(約1ms)
const RHO_THRESH = 0.6; // エコー判定に用いるAMDFスコアしきい値
const POWER_RATIO_ALPHA = 1.3; // マイク/遠端パワー比の許容上限
const ATTENUATION_LINEAR = 0.0001; // 抑圧時に掛ける線形ゲイン(約-80dB)
const HANGOVER_BLOCKS = 20; // 抑圧継続のためのハングオーバブロック数
const ATTACK = 0.1; // ゲインを下げるときの平滑係数
const RELEASE = 0.01; // ゲインを戻すときの平滑係数
const HIST_LEN = MAX_LAG_SAMPLES + BLOCK_SAMPLES * 4; // 遠端履歴バッファ長

class JsSuppressor {
  constructor() {
    this.sampleRate = SAMPLE_RATE;
    this.blockSamples = BLOCK_SAMPLES;
    this.maxLagSamples = MAX_LAG_SAMPLES;
    this.lagStep = LAG_STEP;
    this.rhoThresh = RHO_THRESH;
    this.powerRatioAlpha = POWER_RATIO_ALPHA;
    this.attenLinear = ATTENUATION_LINEAR;
    this.hangoverBlocks = HANGOVER_BLOCKS;
    this.attack = ATTACK;
    this.release = RELEASE;
    this.histLen = HIST_LEN;

    this.farHist = new Float32Array(this.histLen);
    this.histPos = 0;
    this.gateGain = 1.0;
    this.hangCnt = 0;

    this.farBlock = new Float32Array(this.blockSamples);
    this.nearBlock = new Float32Array(this.blockSamples);
    this.outBlock = new Float32Array(this.blockSamples);
  }

  static get SAMPLE_RATE() { return SAMPLE_RATE; }
  static get BLOCK_SAMPLES() { return BLOCK_SAMPLES; }

  reset() {
    this.farHist.fill(0);
    this.histPos = 0;
    this.gateGain = 1.0;
    this.hangCnt = 0;
  }

  processInt16(farInt16, nearInt16) {
    const block = this.blockSamples;
    const scale = 1 / 32768;
    for (let i = 0; i < block; i++) {
      this.farBlock[i] = farInt16[i] * scale;
      this.nearBlock[i] = nearInt16[i] * scale;
    }
    const result = this.processFloat(this.farBlock, this.nearBlock);
    const out = new Int16Array(block);
    for (let i = 0; i < block; i++) {
      const sample = Math.max(-1, Math.min(1, this.outBlock[i]));
      out[i] = Math.round(sample * 32767);
    }
    return { out, gain: result.gain, lag: result.lag };
  }

  processFloat(farFloat, nearFloat) {
    const block = this.blockSamples;
    const histSize = this.histLen;

    // 手順1: 遠端ブロックを履歴バッファへ書き込みリング更新を行う
    for (let i = 0; i < block; i++) {
      this.farHist[this.histPos] = farFloat[i];
      this.histPos++;
      if (this.histPos >= histSize) this.histPos = 0;
    }

    let micPow = 0;
    let micAbs = 0;
    // 手順2: マイクブロックの電力と絶対値和を計算し下限値でクリップ
    for (let i = 0; i < block; i++) {
      const x = nearFloat[i];
      micPow += x * x;
      micAbs += Math.abs(x);
    }
    micPow = Math.max(micPow, 1e-9);
    micAbs = Math.max(micAbs, 1e-9);

    let bestScore = -Infinity;
    let bestLag = 0;
    let bestFarPow = 1e-9;

    // 手順3: AMDFベースの遅延探索で類似度スコアと遠端電力を求める
    for (let lag = 0; lag <= this.maxLagSamples; lag += this.lagStep) {
      let accum = 0;
      let farPow = 0;
      let farAbs = 0;
      const base = -(block + lag);
      for (let i = 0; i < block; i++) {
        let idx = this.histPos + base + i;
        idx %= histSize;
        if (idx < 0) idx += histSize;
        const fx = this.farHist[idx];
        const my = nearFloat[i];
        farPow += fx * fx;
        accum += Math.abs(fx - my);
        farAbs += Math.abs(fx);
      }
      farPow = Math.max(farPow, 1e-9);
      let denom = micAbs + farAbs;
      denom = Math.max(denom, 1e-9);
      let score = 1.0 - (accum / denom);
      if (score > 1.0) score = 1.0;
      else if (score < -1.0) score = -1.0;
      if (score > bestScore) {
        bestScore = score;
        bestLag = lag;
        bestFarPow = farPow;
      }
    }

    // 手順4: 最良ラグに対する遠端電力をクリップして保持
    bestFarPow = Math.max(bestFarPow, 1e-9);

    // 手順5: AMDFスコアと電力比に基づいてエコー抑圧の判定を行う
    const echoDetected = (bestScore > this.rhoThresh) && (micPow < this.powerRatioAlpha * bestFarPow);
    const estimatedLag = echoDetected ? bestLag : -1;

    let suppress = echoDetected;
    // 手順6: ハングオーバ制御で抑圧状態を継続させる
    if (suppress) {
      this.hangCnt = this.hangoverBlocks;
    } else if (this.hangCnt > 0) {
      this.hangCnt -= 1;
      suppress = true;
    }

    // 手順7: 抑圧状態に応じて目標ゲインを設定
    const targetGain = suppress ? this.attenLinear : 1.0;
    // 手順8: 攻撃/解放係数を用いてゲインを平滑追従させる
    const coeff = (targetGain < this.gateGain) ? this.attack : this.release;
    this.gateGain = (1 - coeff) * this.gateGain + coeff * targetGain;
    const appliedGain = this.gateGain;

    // 手順9: 決定したゲインをマイクブロックに適用して出力を生成
    for (let i = 0; i < block; i++) {
      this.outBlock[i] = nearFloat[i] * appliedGain;
    }

    return { gain: appliedGain, lag: estimatedLag };
  }
}

function gainMeterString(gain) {
  const g = Math.max(0, Math.min(1, gain));
  if (g <= 0.05) return '    ';
  if (g <= 0.25) return '*   ';
  if (g <= 0.50) return '**  ';
  if (g <= 0.75) return '*** ';
  return '****';
}

module.exports = {
  JsSuppressor,
  gainMeterString,
  SAMPLE_RATE,
  BLOCK_SAMPLES,
  MAX_LAG_SAMPLES,
  LAG_STEP,
  RHO_THRESH,
  POWER_RATIO_ALPHA,
  ATTENUATION_LINEAR,
  HANGOVER_BLOCKS,
  ATTACK,
  RELEASE,
};
