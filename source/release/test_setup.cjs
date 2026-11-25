'use strict';

const assert = require('node:assert/strict');
const {createHash, webcrypto} = require('node:crypto');
const {readFileSync} = require('node:fs');
const {resolve} = require('node:path');
const vm = require('node:vm');

async function run() {
  const root = resolve(__dirname, '../..');
  const page = readFileSync(process.argv[2] || resolve(root, 'Set-up-controllers.html'), 'utf8');
  const stock = readFileSync(process.argv[3] || resolve(root, 'firmware/frame_bridge.bin'));
  const scripts = [...page.matchAll(/<script([^>]*)>([\s\S]*?)<\/script>/g)];
  const image = scripts.find(item => item[1].includes('id="image"'))[2].trim();
  assert.deepEqual(Buffer.from(image, 'base64'), stock);
  for (const id of ['left', 'right']) assert.match(page, new RegExp('id="' + id + '" value="" required'));
  const nodes = {
    image: {textContent: image}, layout: {},
    settings: {addEventListener() {}}
  };
  const ctx = vm.createContext({document: {getElementById: id => nodes[id]}, atob, TextEncoder, crypto: webcrypto});
  vm.runInContext(scripts.find(item => !item[1])[2], ctx);
  assert.equal(nodes.layout.value, '2');
  const zero = Array(6).fill('00').join(':');
  const left = ['C0', '00', '00', '00', '00', '01'].join(':');
  const right = ['C0', '00', '00', '00', '00', '02'].join(':');
  for (const [a, b, mode] of [[zero, right, '2'], [left, zero, '2'], [left, left, '2'], [left, left.toLowerCase(), '2'],
    ['', right, '2'], [left, 'abc', '2'], [left, Array(6).fill('FF').join(':'), '2'], [left, right, '3']])
    await assert.rejects(() => ctx.pairedImage(a, b, mode));

  const marker = Buffer.from('FRAMEBRIDGE-PAIR\0');
  const offset = stock.indexOf(marker) + marker.length;
  assert.ok(offset >= marker.length);
  assert.equal(stock.indexOf(marker, offset), -1);
  assert.equal(stock.toString('ascii', offset, offset + 36), zero + '\0' + zero + '\0');
  const checksum = stock.length - 33;
  for (const mode of [0, 1, 2]) {
    const bytes = Buffer.from(await ctx.pairedImage(' ' + left.toLowerCase() + ' ', right, String(mode)));
    const expected = Buffer.from(stock);
    expected.write(left + '\0' + right + '\0', offset, 'ascii');
    expected[offset + 36] = mode;
    let value = 0xef;
    let pos = 24;
    for (let i = 0; i < expected[1]; ++i) {
      const length = expected.readUInt32LE(pos + 4);
      pos += 8;
      for (const byte of expected.subarray(pos, pos + length)) value ^= byte;
      pos += length;
    }
    assert.equal(checksum, Math.floor(pos / 16) * 16 + 15);
    expected[checksum] = value;
    createHash('sha256').update(expected.subarray(0, checksum + 1)).digest().copy(expected, checksum + 1);
    assert.deepEqual(bytes, expected);
  }
  console.log('Offline setup passed for empty defaults, address validation and all three personalized USB layouts.');
}

run().catch(error => { console.error(error); process.exitCode = 1; });
