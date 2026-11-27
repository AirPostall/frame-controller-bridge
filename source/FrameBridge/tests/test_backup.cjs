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
  const migrated = backup.decode(backup.encode(old), 5);
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
const v5 = backup.decode(exported, 5);
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
assert.deepEqual(backup.decode(JSON.parse(JSON.stringify(v5data)), 5), v5);
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
// V6 keeps all V5 bindings and enabled flags and adds the old stick response by default.
const defaultStick = {invertX:false, invertY:true, deadzone:0, sensitivity:100};
for (const data of [backup.encode(legacy), exported, v5data]) {
  const prior = backup.decode(data, 5);
  const six = backup.decode(data, 6);
  assert.equal(six.length, 1734);
  assert.equal(six[0], 6);
  assert.deepEqual(six.subarray(1, 1728), prior.subarray(1));
  assert.deepEqual([...six.subarray(1728)], [2,0,100,2,0,100]);
  assert.deepEqual(backup.stick(six, 0), defaultStick);
  assert.deepEqual(backup.stick(six, 1), defaultStick);
}
const six = backup.decode(v5data, 6);
const left = {invertX:true, invertY:false, deadzone:50, sensitivity:25};
const right = {invertX:true, invertY:true, deadzone:18, sensitivity:200};
backup.setStick(six, 0, left);
backup.setStick(six, 1, right);
assert.deepEqual([...six.subarray(1728)], [1,50,25,3,18,200]);
const sixData = backup.encode(six);
assert.deepEqual(sixData.left.stick, left);
assert.deepEqual(sixData.right.stick, right);
assert.deepEqual(backup.decode(JSON.parse(JSON.stringify(sixData)), 6), six);
assert.throws(() => backup.decode(sixData, 5), /V6 firmware/);
for (const invalid of [undefined, {...left, invertX:0}, {...left, invertY:'false'},
  {...left, deadzone:51}, {...left, deadzone:-1}, {...left, deadzone:2.5},
  {...left, sensitivity:24}, {...left, sensitivity:201}, {...left, sensitivity:'100'}]) {
  const prior = six.slice();
  assert.throws(() => backup.setStick(six, 0, invalid), /joystick/i);
  assert.deepEqual(six, prior);
  assert.throws(() => backup.decode({...sixData, left:{...sixData.left, stick:invalid}}), /joystick/i);
}
for (const [index, value] of [[1728,4], [1729,51], [1730,24], [1733,201]]) {
  const corrupt = six.slice();
  corrupt[index] = value;
  assert.throws(() => backup.encode(corrupt), /joystick/i);
}
assert.throws(() => backup.stick(six, 2));
assert.throws(() => backup.setStick(six, -1, left));
assert.throws(() => backup.setStick(v5, 0, left));
// A 5.3.1 board may already have saved a V7 backup with configurable trigger sources.
const leftTriggerTouch = 34 + 5 * 2, rightTriggerTouch = 34 + 5 * (12 + 5);
const lt = 1 << 15, rt = 1 << 16;
assert.equal(backup.physicalTravel, 25);
for (const version of [6, 7]) {
  const prior = six.slice();
  prior[0] = version;
  prior.set([6,0,14,130,4], leftTriggerTouch);   // Continuous touch mapped to an ordinary button.
  prior.set([7,3,14,130,16], rightTriggerTouch);
  prior.set([18,3], 172);                       // Cross-hand analog touch routes.
  backup.setMask(prior, 7, lt | rt | 1);
  backup.setMask(prior, 23, rt | 2);
  prior.set([1,180,90], 154);                   // Falling thresholds learned on byte 6.
  prior.set([0,0,0], 163);                      // Disabled right trigger.
  backup.setMask(prior, 32, 4);
  prior[34 + 5 * 8 + 4] = 17;                   // Unrelated calibrated surface with a digital output.
  const migrated = backup.decode(backup.encode(prior), 7);
  const expected = prior.slice();
  expected[0] = 7;
  expected.set([25,25], 172);
  expected.set([2,180,90], 154);
  expected.set([2,64,128], 163);
  expected[leftTriggerTouch + 4] = expected[rightTriggerTouch + 4] = 0;
  if (version < 7) expected[34 + 5 * 8 + 4] = 0;
  backup.setMask(expected, 7, 0);
  backup.setMask(expected, 23, 0);
  backup.setMask(expected, 32, lt | 4);
  backup.setMask(expected, 35, rt);
  assert.deepEqual(migrated, expected);

  // A later restore must preserve the user's byte-7 calibration and choice of digital outputs.
  backup.setMask(migrated, 32, 4);
  backup.setMask(migrated, 35, 0);
  const stored = JSON.parse(JSON.stringify(backup.encode(migrated)));
  assert.equal(stored.left.analogTriggerSource, 'travel');
  assert.equal(stored.right.analogTriggerSource, 'travel');
  assert.deepEqual(backup.decode(stored, 7), migrated);
  assert.throws(() => backup.decode(stored, 6), /V7 firmware/);
  assert.throws(() => backup.decode({...stored, profileVersion:6}), /trigger/i);
  const invalidSource = migrated.slice();
  invalidSource[172] = 26;
  assert.throws(() => backup.encode(invalidSource), /trigger/i);
  invalidSource[0] = 6;
  invalidSource[172] = 25;
  assert.throws(() => backup.encode(invalidSource), /trigger/i);
}

for (const older of [legacy, profile, v5]) {
  const data = backup.encode(older);
  for (const [side, action] of [['left',16],['right',17]]) {
    data[side].buttons[7] = data.profileVersion >= 5 ? [action] : action;
    data[side].travel[0] = [0,0,0];
    if (data.profileVersion >= 4) data[side].analogTriggerSource = null;
  }
  const upgraded = backup.decode(data);
  assert.deepEqual([...upgraded.subarray(172,174)], [25,25]);
  assert.deepEqual([...upgraded.subarray(154,157)], [2,64,128]);
  assert.deepEqual([...upgraded.subarray(163,166)], [2,64,128]);
  assert.equal(backup.mask(upgraded, 7), 0);
  assert.equal(backup.mask(upgraded, 23), 0);
  assert.equal(backup.mask(upgraded, 32), lt);
  assert.equal(backup.mask(upgraded, 35), rt);
}
for (const v of [3, 4, 5, 6, 7]) {
  const older = backup.decode(backup.encode(legacy), v);
  const stored = backup.encode(older);
  assert.equal(stored.motion_hand, undefined);
  assert.equal(stored.gyro_gain, undefined);
  const v8 = backup.decode(stored, 8);
  assert.equal(v8[0], 8);
  assert.equal(backup.motionHand(v8), 0);
  assert.equal(backup.gyroGain(v8), 100);
  const v7 = backup.decode(stored, 7);
  assert.deepEqual(v8.slice(1, v7.length), v7.slice(1));
  assert.equal(backup.gyroResetButton(backup.decode(stored)), 0);
}
const v8 = backup.decode(sixData, 8);
for (const hand of [0, 1, 2]) {
  backup.setMotionHand(v8, hand);
  const gain = [25, 100, 250][hand];
  backup.setGyroGain(v8, gain);
  assert.equal(backup.motionHand(v8), hand);
  assert.equal(backup.gyroGain(v8), gain);
  const data = backup.encode(v8);
  assert.equal(data.motion_hand, hand);
  assert.equal(data.gyro_gain, gain);
  assert.equal(data.gyro_reset_button, undefined);
  assert.deepEqual(backup.decode(JSON.parse(JSON.stringify(data)), 8), v8);
  const migrated = backup.decode(data);
  assert.deepEqual(migrated.slice(1, -1), v8.slice(1));
  assert.equal(backup.gyroResetButton(migrated), 0);
  assert.throws(() => backup.decode(data, 7), /V8 firmware/);
}
for (const invalid of [-1, 3, 0.5, '1', null, undefined]) {
  assert.throws(() => backup.setMotionHand(v8, invalid), /motion/i);
  assert.throws(() => backup.decode({...backup.encode(v8), motion_hand:invalid}), /motion/i);
}
assert.throws(() => backup.setMotionHand(six, 1), /V8/);
assert.throws(() => backup.setGyroGain(six, 100), /V8/);
assert.throws(() => backup.setGyroResetButton(v8, 1), /V9/);
const v9 = backup.decode(backup.encode(v8));
assert.equal(v9.length, 1737);
for (let button = 0; button <= 38; ++button) {
  backup.setGyroResetButton(v9, button);
  assert.equal(backup.gyroResetButton(v9), button);
  const data = backup.encode(v9);
  assert.equal(data.gyro_reset_button, button);
  assert.deepEqual(backup.decode(JSON.parse(JSON.stringify(data))), v9);
  assert.throws(() => backup.decode(data, 8), /V9 firmware/);
}
for (const invalid of [-1, 39, 1.5, '1', null, undefined]) {
  assert.throws(() => backup.setGyroResetButton(v9, invalid), /reset button/i);
  assert.throws(() => backup.decode({...backup.encode(v9), gyro_reset_button:invalid}), /reset button/i);
}
v9[v9.length - 1] = 39;
assert.throws(() => backup.encode(v9), /reset button/i);
for (const invalid of [-1, 24, 251, 100.5, '100', null, undefined]) {
  assert.throws(() => backup.setGyroGain(v8, invalid), /gyroscope/i);
  assert.throws(() => backup.decode({...backup.encode(v8), gyro_gain:invalid}), /gyroscope/i);
}
v8[v8.length - 1] = 0;
assert.throws(() => backup.encode(v8), /gyroscope/i);
backup.setGyroGain(v8, 100);
v8[backup.motionOffset] = 3;
assert.throws(() => backup.encode(v8), /motion/i);
console.log('V3–V9 backup round trips, motion source, gyro reset, trigger and joystick settings, migrations and validation passed.');
