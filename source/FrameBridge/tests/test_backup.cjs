const assert = require('node:assert/strict');
const backup = require('../app/backup.js');

const legacy = new Uint8Array(172);
legacy[0] = 3;
// Synthetic valid profile, including both touch polarities and a travel threshold.
for (let i = 0; i < 32; ++i) legacy[2 + i] = i % 16;
for (let i = 0; i < 24; ++i) legacy[34 + i * 5] = 255;
legacy.set([3, 0, 64, 192, 1], 34 + 12 * 5);
legacy.set([5, 1, 204, 51, 2], 34 + 8 * 5);
legacy.set([2, 32, 224], 154);
for (let flags = 0; flags <= 3; flags++) {
  legacy[1] = flags;
  const stored = JSON.parse(JSON.stringify(backup.encode(legacy)));
  assert.equal(stored.left.analogTriggerSource, undefined);
  assert.deepEqual(backup.decode(stored, 3), legacy);

  const migrated = Uint8Array.from([...legacy, 0, 0]);
  migrated[0] = 4;
  assert.deepEqual(backup.decode(stored), migrated);
}

const profile = Uint8Array.from([...legacy, 13, 9]);
profile[0] = 4;
const exported = backup.encode(profile);
assert.deepEqual(exported.left.analogTriggerSource, {hand:'right', slot:0});
assert.deepEqual(exported.right.analogTriggerSource, {hand:'left', slot:8});
assert.deepEqual(backup.decode(JSON.parse(JSON.stringify(exported))), profile);
assert.throws(() => backup.decode(exported, 3), /V4 firmware/);
profile[172] = profile[173] = 0;
const digital = backup.encode(profile);
assert.equal(digital.left.analogTriggerSource, null);
assert.equal(digital.right.analogTriggerSource, null);
assert.deepEqual(backup.decode(digital), profile);

for (const mutate of [
  b => { b.format = 'unknown'; },
  b => { b.version++; },
  b => { b.profileVersion++; },
  b => { delete b.right; },
  b => { b.left.buttons[0] = 256; },
  b => { b.right.touch[1].pop(); },
  b => { b.right.travel[2][1] = '64'; },
  b => { b.left.stickEnabled = 1; },
  b => { delete b.left.analogTriggerSource; },
  b => { b.left.analogTriggerSource = {hand:'left', slot:9}; },
  b => { b.left.analogTriggerSource = {hand:'right', slot:12}; },
  b => { b.left.analogTriggerSource = {hand:'right', slot:0.5}; },
  b => { b.left.analogTriggerSource = {hand:'anything', slot:0}; },
  b => { b.left.analogTriggerSource = {hand:'right', slot:0}; b.right.touch[0][0] = 255; }
]) {
  const b = backup.encode(profile);
  mutate(b);
  assert.throws(() => backup.decode(b));
}
console.log('V3 import, V4 binding/trigger round trips and malformed backup tests passed');
