// Node echoback: PortAudio(PAmac.node) + minES suppressor (16kHz mono, 160-sample blocks)
// Usage: node echoback.js [--passthrough] [--input-delay-ms <ms>] [--loopback-delay-ms <ms>] [latency_ms]

const assert = require('assert');
const { JsSuppressor, gainMeterString, SAMPLE_RATE, BLOCK_SAMPLES } = require('./suppressor');

let passthrough = false;
let jitterLatencyMs = 200;
let inputDelayMs = 0;
let loopbackDelayMs = 150;

function parseMs(value, flag) {
  const n = Number(value);
  if (!Number.isFinite(n) || n < 0) {
    console.error(`${flag} expects non-negative number (ms). got=${value}`);
    process.exit(1);
  }
  return Math.min(100000, n);
}

const args = process.argv.slice(2);
for (let i = 0; i < args.length; i++) {
  const arg = args[i];
  if (arg === '--passthrough' || arg === '-p') {
    passthrough = true;
  } else if (arg === '--help' || arg === '-h') {
    console.error('Usage: node echoback.js [--passthrough] [--input-delay-ms <ms>] [--loopback-delay-ms <ms>] [latency_ms]');
    process.exit(0);
  } else if (arg.startsWith('--input-delay-ms=')) {
    const value = arg.split('=', 2)[1];
    inputDelayMs = parseMs(value, '--input-delay-ms');
  } else if (arg === '--input-delay-ms') {
    if (i + 1 >= args.length) {
      console.error('--input-delay-ms requires a value');
      process.exit(1);
    }
    inputDelayMs = parseMs(args[++i], '--input-delay-ms');
  } else if (arg.startsWith('--loopback-delay-ms=')) {
    const value = arg.split('=', 2)[1];
    loopbackDelayMs = parseMs(value, '--loopback-delay-ms');
  } else if (arg === '--loopback-delay-ms') {
    if (i + 1 >= args.length) {
      console.error('--loopback-delay-ms requires a value');
      process.exit(1);
    }
    loopbackDelayMs = parseMs(args[++i], '--loopback-delay-ms');
  } else if (/^\d+$/.test(arg)) {
    jitterLatencyMs = Math.max(0, Math.min(10000, parseInt(arg, 10)));
  } else {
    console.error('Unknown arg:', arg);
    process.exit(1);
  }
}

const kSr = SAMPLE_RATE;
const kBlock = BLOCK_SAMPLES;

function roundInputDelaySamples(ms) {
  const rawSamples = Math.max(0, Math.round((ms * kSr) / 1000));
  const delayBlocks = Math.floor((rawSamples + Math.floor(kBlock / 2)) / kBlock);
  return delayBlocks * kBlock;
}

function roundDelaySamples(ms) {
  return Math.max(0, Math.floor((ms * kSr + 999) / 1000));
}

const inputDelaySamples = roundInputDelaySamples(inputDelayMs);
const loopbackDelaySamples = roundDelaySamples(loopbackDelayMs);
const jitterSamplesTarget = Math.max(0, Math.floor((kSr * jitterLatencyMs) / 1000));

let PortAudio = null;
if (process.platform === 'darwin') {
  PortAudio = require('./PAmac.node');
} else {
  console.error('Only macOS (PAmac.node) is supported here.');
  process.exit(1);
}

PortAudio.initSampleBuffers(kSr, kSr, kBlock);
PortAudio.startMic();
PortAudio.startSpeaker();

const suppressor = new JsSuppressor();
assert.strictEqual(suppressor.blockSamples, kBlock, 'Unexpected suppressor block size');

console.error(`echoback (16k mono): mode=${passthrough ? 'passthrough' : 'suppressor'}`);
if (!passthrough) {
  console.error('  config: atten=-80.0 dB, rho=0.60, ratio=1.30, hang=20, attack=0.100, release=0.010');
}
const inputDelayMsFinal = inputDelaySamples * 1000 / kSr;
const inputDelayBlocks = kBlock > 0 ? inputDelaySamples / kBlock : 0;
console.error(`input-delay-ms(final): ${inputDelayMsFinal.toFixed(1)} ms (${inputDelaySamples} samples, ${inputDelayBlocks.toFixed(1)} blocks)`);
const loopbackDelayMsFinal = loopbackDelaySamples * 1000 / kSr;
console.error(`loopback-delay-ms(final): ${loopbackDelayMsFinal.toFixed(1)} ms (${loopbackDelaySamples} samples)`);
console.error(`latency buffer target: ${jitterLatencyMs.toFixed(1)} ms (${jitterSamplesTarget} samples)`);
console.error('Running... Ctrl-C to stop.');

function pushArray(dstArr, src) {
  for (let i = 0; i < src.length; i++) dstArr.push(src[i] | 0);
}

function popBlockI16(arr) {
  if (arr.length < kBlock) return null;
  const out = new Int16Array(kBlock);
  for (let i = 0; i < kBlock; i++) out[i] = arr.shift();
  return out;
}

function toInt16Array(x) {
  if (!x) return new Int16Array();
  if (ArrayBuffer.isView(x)) {
    if (x instanceof Int16Array) return x;
    return new Int16Array(x.buffer, x.byteOffset, Math.floor(x.byteLength / 2));
  }
  if (Buffer.isBuffer(x)) {
    return new Int16Array(x.buffer, x.byteOffset, Math.floor(x.byteLength / 2));
  }
  const out = new Int16Array(x.length);
  for (let i = 0; i < x.length; i++) out[i] = x[i] | 0;
  return out;
}

const recQ = [];
const refQ = [];
const jitterQ = [];
const captureDelayLine = [];
let captureDelayStart = 0;
const loopbackDelayLine = [];
let loopbackDelayStart = 0;
let needJitter = jitterSamplesTarget > 0;
let blockCounter = 0;
const lagHistory = [];
let lagSum = 0;
const lagHistoryLimit = 10;
let lagStatsReady = false;
let lagAvgLatest = 0.0;
let lagMinWindow = 0;
let lagMaxWindow = 0;
let lagLast = -1;

function trimCaptureQueue() {
  if (captureDelayStart > 4096 && captureDelayStart > captureDelayLine.length / 2) {
    captureDelayLine.splice(0, captureDelayStart);
    captureDelayStart = 0;
  }
}

function trimLoopbackQueue() {
  if (loopbackDelayStart > 4096 && loopbackDelayStart > loopbackDelayLine.length / 2) {
    loopbackDelayLine.splice(0, loopbackDelayStart);
    loopbackDelayStart = 0;
  }
}

function enqueueCaptureSamples(samples) {
  if (inputDelaySamples === 0) {
    pushArray(recQ, samples);
    return;
  }
  for (let i = 0; i < samples.length; i++) {
    captureDelayLine.push(samples[i] | 0);
    if (captureDelayLine.length - captureDelayStart > inputDelaySamples) {
      recQ.push(captureDelayLine[captureDelayStart++]);
      trimCaptureQueue();
    } else {
      recQ.push(0);
    }
  }
}

function applyLoopbackDelayBlock(block) {
  if (loopbackDelaySamples <= 0) {
    return block;
  }
  const delayed = new Int16Array(block.length);
  for (let i = 0; i < block.length; i++) {
    loopbackDelayLine.push(block[i] | 0);
    if (loopbackDelayLine.length - loopbackDelayStart > loopbackDelaySamples) {
      delayed[i] = loopbackDelayLine[loopbackDelayStart++];
      trimLoopbackQueue();
    } else {
      delayed[i] = 0;
    }
  }
  return delayed;
}

function updateLagStats(lagSamples) {
  if (lagSamples < 0) {
    return;
  }
  lagHistory.push(lagSamples);
  lagSum += lagSamples;
  if (lagHistory.length > lagHistoryLimit) {
    lagSum -= lagHistory.shift();
  }
  if (lagHistory.length > 0) {
    let minVal = lagHistory[0];
    let maxVal = lagHistory[0];
    for (let i = 1; i < lagHistory.length; i++) {
      const v = lagHistory[i];
      if (v < minVal) minVal = v;
      if (v > maxVal) maxVal = v;
    }
    lagAvgLatest = lagSum / lagHistory.length;
    lagMinWindow = minVal;
    lagMaxWindow = maxVal;
    lagLast = lagSamples;
    lagStatsReady = true;
  }
}

function processBlocks() {
  while (recQ.length >= kBlock) {
    const rec = popBlockI16(recQ);
    const ref = refQ.length >= kBlock ? popBlockI16(refQ) : new Int16Array(kBlock);

    let out = rec;
    let gain = 1.0;
    let lagSamples = -1;

    if (!passthrough) {
      const result = suppressor.processInt16(ref, rec);
      out = result.out;
      gain = result.gain;
      lagSamples = result.lag;
      updateLagStats(lagSamples);
      const muteRatio = Math.max(0, Math.min(1, 1 - gain));
      const gainMeter = gainMeterString(gain);
      const currentBlock = blockCounter;
      const lagWindowSize = lagHistory.length;
      if (lagStatsReady) {
        const lagCurrentStr = lagSamples >= 0 ? String(lagSamples) : '--';
        const avgWindow = lagWindowSize > 0 ? lagWindowSize : lagHistoryLimit;
        console.error(`[block ${currentBlock}] mute=${(muteRatio * 100).toFixed(1)}% (gain=${gain.toFixed(3)} ${gainMeter}, lag=${lagCurrentStr} samples; avg${avgWindow}=${lagAvgLatest.toFixed(1)}, min=${lagMinWindow}, max=${lagMaxWindow}, last=${lagLast})`);
      } else {
        console.error(`[block ${currentBlock}] mute=${(muteRatio * 100).toFixed(1)}% (gain=${gain.toFixed(3)} ${gainMeter}, lag=--)`);
      }
    }

    blockCounter += 1;

    for (let i = 0; i < kBlock; i++) refQ.push(out[i]);
    for (let i = 0; i < kBlock; i++) jitterQ.push(out[i]);
    if (needJitter && jitterQ.length >= jitterSamplesTarget) needJitter = false;

    let speakerBlock = new Int16Array(kBlock);
    if (!needJitter && jitterQ.length >= kBlock) {
      for (let i = 0; i < kBlock; i++) speakerBlock[i] = jitterQ.shift();
    } else {
      speakerBlock.fill(0);
    }
    speakerBlock = applyLoopbackDelayBlock(speakerBlock);
    PortAudio.pushSamplesForPlay(speakerBlock);
  }
}

const tickMs = Math.max(1, Math.round((kBlock / kSr) * 1000));
const timer = setInterval(() => {
  const recBuf = PortAudio.getRecordedSamples();
  const recArr = toInt16Array(recBuf);
  if (recArr.length > 0) {
    enqueueCaptureSamples(recArr);
    if (typeof PortAudio.discardRecordedSamples === 'function') {
      PortAudio.discardRecordedSamples(recArr.length);
    }
  }
  processBlocks();
}, tickMs);

function shutdown() {
  clearInterval(timer);
  try { PortAudio && PortAudio.stopMic && PortAudio.stopMic(); } catch (_) {}
  try { PortAudio && PortAudio.stopSpeaker && PortAudio.stopSpeaker(); } catch (_) {}
}

process.on('SIGINT', () => {
  console.error('stopped.');
  shutdown();
  process.exit(0);
});
