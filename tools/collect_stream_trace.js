#!/usr/bin/env node
// Record the exact NVE1 packets received by this PC over the browser's
// WebSocket path. Compare capture_sequence with depthd --trace-dir outputs.
const fs = require('fs');
const path = require('path');

function arg(name, fallback = '') {
  const index = process.argv.indexOf(name);
  return index < 0 ? fallback : process.argv[index + 1];
}
const origin = arg('--origin', 'http://127.0.0.1:8767');
const output = arg('--output');
const count = Number(arg('--count', '60'));
const seconds = Number(arg('--timeout-seconds', '180'));
const excludedRunUuid = arg('--exclude-run-uuid');
if (!output || !Number.isInteger(count) || count < 1 || count > 120 ||
    !Number.isFinite(seconds) || seconds < 1 || seconds > 600) {
  throw new Error('usage: collect_stream_trace.js --output EMPTY_DIR [--origin URL] [--count 60] [--timeout-seconds 180]');
}
if (fs.existsSync(output) && fs.readdirSync(output).length) {
  throw new Error('output directory must be empty');
}
fs.mkdirSync(output, {recursive: true});
const streamUrl = new URL('/api/v1/encoded/stream', origin);
streamUrl.protocol = streamUrl.protocol === 'https:' ? 'wss:' : 'ws:';
const socket = new WebSocket(streamUrl);
socket.binaryType = 'arraybuffer';
const received = new Set();
let closeFallback;
const timer = setTimeout(() => {
  console.error(`Timed out with ${received.size}/${count} encoded frames`);
  socket.close();
  process.exitCode = 2;
}, seconds * 1000);
socket.addEventListener('message', event => {
  try {
    const bytes = Buffer.from(event.data);
    if (bytes.length < 32 || bytes.toString('ascii', 0, 4) !== 'NVE1') return;
    const metadataLength = bytes.readUInt32BE(8);
    const payloadLength = Number(bytes.readBigUInt64BE(12));
    if (metadataLength > 65536 || payloadLength > 33554432 ||
        bytes.length !== 32 + metadataLength + payloadLength) return;
    const metadata = JSON.parse(bytes.toString('utf8', 32, 32 + metadataLength));
    // A stopped producer leaves its last snapshot available to the gateway.
    // Ignore that previous run while the bounded trace starts up.
    if (excludedRunUuid && metadata.run_uuid === excludedRunUuid) return;
    const sequence = metadata.capture_sequence;
    if (!Number.isSafeInteger(sequence) || sequence < 1 || received.has(sequence)) return;
    fs.writeFileSync(path.join(output, `pc_stream_${sequence}.nve`), bytes);
    received.add(sequence);
    console.log(`${received.size}/${count} capture_sequence=${sequence}`);
    if (received.size >= count) {
      clearTimeout(timer);
      socket.close();
      // The appliance may leave the close handshake pending after a bounded
      // stream stops. All packet writes are synchronous and already complete.
      closeFallback = setTimeout(() => {
        console.log(`Saved ${received.size} encoded packets to ${output}`);
        process.exit(0);
      }, 2000);
    }
  } catch (error) {
    console.error(error.message);
  }
});
socket.addEventListener('error', event => {
  console.error(`WebSocket error: ${event.message || 'connection failed'}`);
  process.exitCode = 2;
});
socket.addEventListener('close', () => {
  clearTimeout(timer);
  clearTimeout(closeFallback);
  if (received.size < count) process.exitCode = 2;
  console.log(`Saved ${received.size} encoded packets to ${output}`);
});
