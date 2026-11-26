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
  assert.deepEqual(backup.decode(stored, 4), migrated);
}

const profile = Uint8Array.from([...legacy, 13, 9]);
profile[0] = 4;
const exported = backup.encode(profile);
assert.deepEqual(exported.left.analogTriggerSource, {hand:'right', slot:0});
assert.deepEqual(exported.right.analogTriggerSource, {hand:'left', slot:8});
assert.deepEqual(backup.decode(JSON.parse(JSON.stringify(exported)), 4), profile);
assert.throws(() => backup.decode(exported, 3), /V4 firmware/);
profile[172] = profile[173] = 0;
const digital = backup.encode(profile);
assert.equal(digital.left.analogTriggerSource, null);
assert.equal(digital.right.analogTriggerSource, null);
assert.deepEqual(backup.decode(digital, 4), profile);

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
// Migrate old single assignments and implicit press travel to the V5 wire format.
for (const old of [legacy, Uint8Array.from([...legacy, 13, 9])]) {
  old[0] = old.length === 172 ? 3 : 4;
  const migrated = backup.decode(backup.encode(old));
  assert.equal(migrated.length, 1728);
  assert.equal(migrated[0], 5);
  assert.equal(migrated[1], old[1]);
  assert.ok(migrated.subarray(2, 34).every(v => v === 0));
  assert.deepEqual(migrated.subarray(34, old.length), old.subarray(34));
  for (let source = 0; source < 32; ++source) {
    const action = old[2 + source];
    assert.equal(backup.mask(migrated, source), action ? 1 << (action - 1) : 0);
  }
  assert.equal(backup.mask(migrated, 32), 1 << 15);
  assert.equal(backup.mask(migrated, 37), 0);
  assert.ok(migrated.subarray(288).every(v => v === 0));
}
const v5 = backup.decode(exported);
const highestCombo = 1 | (1 << 15) | (1 << 21);
backup.setMask(v5, 0, highestCombo);
backup.setMask(v5, 31, highestCombo);
backup.setMask(v5, 37, 1 << 21);  // A dormant mapping can be calibrated later.
backup.setMask(v5, 32, 0);        // Retain a calibration even without output.
assert.deepEqual([...v5.slice(174, 177)], [1, 128, 32]);
backup.setName(v5, 0, 'Vasen liipaisin 🎮');
backup.setName(v5, 37, 'å'.repeat(11) + 'x');   // Exactly 23 UTF-8 bytes.
backup.setName(v5, 38, '<action A>');
backup.setName(v5, 59, '\uFEFFLast');
assert.equal(backup.name(v5, 59), '\uFEFFLast');
const v5data = backup.encode(v5);
assert.deepEqual(v5data.left.buttons[0], [1, 16, 22]);
assert.deepEqual(v5data.right.buttons[15], v5data.left.buttons[0]);
assert.deepEqual(v5data.left.travelOutputs[0], []);
assert.deepEqual(v5data.right.travelOutputs[2], [22]);
assert.equal(v5data.outputNames[0], '<action A>');
assert.deepEqual(backup.decode(JSON.parse(JSON.stringify(v5data))), v5);
assert.throws(() => backup.decode(v5data, 4), /V5 firmware/);

for (const invalid of ['a'.repeat(24), 'å'.repeat(12), '\u0000', 'a\n', '\u007F', '\u009F', '\uD800', '\uDC00', null]) {
  const prior = v5.slice();
  assert.throws(() => backup.setName(v5, 37, invalid));
  assert.deepEqual(v5, prior);
}
assert.throws(() => backup.setName(v5, 60, 'Extra'));
for (const invalid of [0x400000, -1, 0.1, '1'])
  assert.throws(() => backup.setMask(v5, 0, invalid));
assert.throws(() => backup.setMask(v5, 38, 1));
assert.throws(() => backup.setMask(v5, -1, 1));
for (const corrupt of [
  b => { b[2] = 1; }, b => { b[176] |= 0x40; }, b => { b.fill(65, 288, 312); },
  b => { b.set([0, 100], 288); }, b => { b.set([0xc0, 0x80], 288); },
]) {
  const invalid = v5.slice();
  corrupt(invalid);
  assert.throws(() => backup.encode(invalid));
}
for (const mutate of [
  b => { b.right.buttons[15] = [1, 1]; },
  b => { b.right.buttons[15] = [23]; },
  b => { b.right.buttons[15] = [0]; },
  b => { b.right.buttons[15] = 3; },
  b => { delete b.left.travelNames; },
  b => { b.left.travelOutputs.pop(); },
  b => { b.left.travel[0][0] = 3; },
  b => { b.left.travel[0][1] = b.left.travel[0][2]; },
  b => { b.left.touch[10] = [0, 0, 0, 100, 1]; },
  b => { b.right.touch[1] = [...b.right.touch[0]]; },
  b => { b.right.touch[0][1] = 11; },
  b => { b.right.touch[0][4] = 23; },
  b => { b.left.buttonNames[2] = 'å'.repeat(12); },
  b => { b.outputNames[0] = '\n'; },
]) {
  const b = backup.encode(v5);
  mutate(b);
  assert.throws(() => backup.decode(b));
}
backup.setName(v5, 0, 'A');
assert.equal(backup.name(v5, 0), 'A');
assert.ok(v5.subarray(289, 312).every(v => v === 0));
backup.setName(v5, 0, '');
assert.equal(backup.name(v5, 0), '');
console.log('V3–V5 binding backups, mask migration, UTF-8 names and configuration validation passed.');
