'use strict';

const fs = require('fs');
const path = require('path');
const {
  JsSuppressor,
  gainMeterString,
  SAMPLE_RATE,
  BLOCK_SAMPLES,
  ATTENUATION_LINEAR,
  RHO_THRESH,
  POWER_RATIO_ALPHA,
  HANGOVER_BLOCKS,
  ATTACK,
  RELEASE,
} = require('./suppressor');

function usage() {
  console.error('Usage: node cancel_file.js <render.wav> <capture.wav>');
}

function readWavPcm16Mono16k(filePath) {
  const buf = fs.readFileSync(filePath);
  if (buf.length < 44) {
    throw new Error('file too short to be WAV');
  }
  if (buf.toString('ascii', 0, 4) !== 'RIFF' || buf.toString('ascii', 8, 12) !== 'WAVE') {
    throw new Error('not a RIFF/WAVE file');
  }

  let pos = 12;
  let sampleRate = 0;
  let channels = 0;
  let bitsPerSample = 0;
  let dataOffset = -1;
  let dataSize = 0;

  while (pos + 8 <= buf.length) {
    const chunkId = buf.toString('ascii', pos, pos + 4);
    const chunkSize = buf.readUInt32LE(pos + 4);
    pos += 8;
    const chunkStart = pos;
    if (chunkId === 'fmt ') {
      if (chunkSize < 16) {
        throw new Error('fmt chunk too short');
      }
      const audioFormat = buf.readUInt16LE(chunkStart);
      channels = buf.readUInt16LE(chunkStart + 2);
      sampleRate = buf.readUInt32LE(chunkStart + 4);
      bitsPerSample = buf.readUInt16LE(chunkStart + 14);
      if (audioFormat !== 1) {
        throw new Error('unsupported WAV encoding (need PCM)');
      }
    } else if (chunkId === 'data') {
      dataOffset = chunkStart;
      dataSize = chunkSize;
      break;
    }
    pos = chunkStart + chunkSize;
    if (chunkSize % 2 === 1) pos += 1;
  }

  if (dataOffset < 0 || dataSize === 0) {
    throw new Error('data chunk not found');
  }
  if (sampleRate !== SAMPLE_RATE || channels !== 1 || bitsPerSample !== 16) {
    throw new Error('expecting 16kHz mono PCM16 WAV');
  }
  if (dataOffset + dataSize > buf.length) {
    throw new Error('data chunk truncated');
  }

  const sampleCount = Math.floor(dataSize / 2);
  const samples = new Int16Array(sampleCount);
  for (let i = 0; i < sampleCount; i++) {
    samples[i] = buf.readInt16LE(dataOffset + i * 2);
  }
  return samples;
}

function writeWavPcm16Mono16k(filePath, samples) {
  const dataBytes = samples.length * 2;
  const header = Buffer.alloc(44);
  header.write('RIFF', 0);
  header.writeUInt32LE(36 + dataBytes, 4);
  header.write('WAVE', 8);
  header.write('fmt ', 12);
  header.writeUInt32LE(16, 16);
  header.writeUInt16LE(1, 20); // PCM
  header.writeUInt16LE(1, 22); // mono
  header.writeUInt32LE(SAMPLE_RATE, 24);
  header.writeUInt32LE(SAMPLE_RATE * 2, 28); // byte rate
  header.writeUInt16LE(2, 32); // block align
  header.writeUInt16LE(16, 34); // bits per sample
  header.write('data', 36);
  header.writeUInt32LE(dataBytes, 40);

  const dataBuf = Buffer.alloc(dataBytes);
  for (let i = 0; i < samples.length; i++) {
    dataBuf.writeInt16LE(samples[i], i * 2);
  }

  fs.writeFileSync(filePath, Buffer.concat([header, dataBuf]));
}

function main() {
  const args = process.argv.slice(2);
  if (args.includes('--help') || args.includes('-h')) {
    usage();
    process.exit(0);
  }
  if (args.length < 2) {
    usage();
    process.exit(1);
  }

  const renderPath = path.resolve(args[0]);
  const capturePath = path.resolve(args[1]);

  let render;
  let capture;
  try {
    render = readWavPcm16Mono16k(renderPath);
    capture = readWavPcm16Mono16k(capturePath);
  } catch (err) {
    console.error(`Failed to read WAVs: ${err.message}`);
    process.exit(1);
  }

  const sampleCount = Math.min(render.length, capture.length);
  const blocks = Math.floor(sampleCount / BLOCK_SAMPLES);
  if (blocks <= 0) {
    console.error('Not enough samples to process.');
    process.exit(1);
  }

  const suppressor = new JsSuppressor();
  const processed = new Int16Array(blocks * BLOCK_SAMPLES);

  console.error(
    `config: atten=${(20 * Math.log10(ATTENUATION_LINEAR)).toFixed(1)} dB, ` +
      `rho=${RHO_THRESH.toFixed(2)}, ratio=${POWER_RATIO_ALPHA.toFixed(2)}, ` +
      `hang=${HANGOVER_BLOCKS}, attack=${ATTACK.toFixed(3)}, release=${RELEASE.toFixed(3)}`
  );

  for (let n = 0; n < blocks; n++) {
    const offset = n * BLOCK_SAMPLES;
    const farBlock = render.subarray(offset, offset + BLOCK_SAMPLES);
    const nearBlock = capture.subarray(offset, offset + BLOCK_SAMPLES);
    const { out, gain, lag } = suppressor.processInt16(farBlock, nearBlock);
    processed.set(out, offset);

    const muteRatio = Math.max(0, 1 - gain);
    const gainMeter = gainMeterString(gain);
    if (lag >= 0) {
      console.error(
        `[block ${n}] mute=${(muteRatio * 100).toFixed(1)}% (gain=${gain.toFixed(3)} ${gainMeter}, lag=${lag} samples)`
      );
    } else {
      console.error(
        `[block ${n}] mute=${(muteRatio * 100).toFixed(1)}% (gain=${gain.toFixed(3)} ${gainMeter}, lag=--)`
      );
    }
  }

  const outputPath = path.resolve('processed.wav');
  writeWavPcm16Mono16k(outputPath, processed);
  console.error(`Wrote ${outputPath}`);
}

main();
