'use strict';

const BindingBackup = (() => {
  const format = 'framebridge-bindings';
  const triggerOffset = 172, maskOffset = 174, nameOffset = 288, nameBytes = 24;
  const sourceCount = 38, actionCount = 22, nameCount = sourceCount + actionCount;
  const allOutputs = (1 << actionCount) - 1;
  const handNames = ['left', 'right'];
  const legacyTravel = [16, 18, 21, 17, 19, 22];
  const size = version => version === 3 ? 172 : version === 4 ? 174 : version === 5 ? 1728 : 0;
  const encoder = new TextEncoder();
  const decoder = new TextDecoder('utf-8', {fatal:true, ignoreBOM:true});

  function bounded(value, max) {
    return Number.isInteger(value) && value >= 0 && value <= max;
  }
  function checkSource(source) {
    if (!bounded(source, sourceCount - 1)) throw Error('Invalid press source.');
  }
  function mask(profile, source) {
    checkSource(source);
    if (profile[0] < 5) {
      const travel = source - 32;
      const action = travel < 0 ? profile[2 + source] : profile[154 + 3 * travel] && legacyTravel[travel];
      return action ? 1 << (action - 1) : 0;
    }
    const offset = maskOffset + 3 * source;
    return profile[offset] | (profile[offset + 1] << 8) | (profile[offset + 2] << 16);
  }
  function setMask(profile, source, value) {
    checkSource(source);
    if (profile[0] !== 5 || !bounded(value, allOutputs)) throw Error('Invalid button output mask.');
    const offset = maskOffset + 3 * source;
    profile.set([value & 255, (value >>> 8) & 255, value >>> 16], offset);
  }

  function nameStart(index) {
    if (!bounded(index, nameCount - 1)) throw Error('Invalid button name index.');
    return nameOffset + index * nameBytes;
  }
  function checkName(value) {
    // In Unicode mode the surrogate range matches only unpaired surrogate code units.
    if (typeof value !== 'string' || /[\u0000-\u001F\u007F-\u009F\uD800-\uDFFF]/u.test(value))
      throw Error('Use a button name without control characters.');
  }
  function name(profile, index) {
    const start = nameStart(index);
    if (profile[0] < 5) return '';
    const field = profile.subarray(start, start + nameBytes);
    const end = field.indexOf(0);
    if (end < 0 || field.subarray(end + 1).some(Boolean)) throw Error('Invalid button name padding.');
    const value = decoder.decode(field.subarray(0, end));
    checkName(value);
    return value;
  }
  function setName(profile, index, value) {
    const start = nameStart(index);
    if (profile[0] !== 5) throw Error('Button names require profile version 5.');
    checkName(value);
    const bytes = encoder.encode(value);
    if (bytes.length >= nameBytes) throw Error('Button names can use up to 23 UTF-8 bytes.');
    profile.fill(0, start, start + nameBytes);
    profile.set(bytes, start);
  }

  function array(value, length) {
    if (!Array.isArray(value) || value.length !== length) throw Error('Invalid binding array length.');
    return value;
  }
  function bytes(value, length) {
    for (const n of array(value, length))
      if (!bounded(n, 255)) throw Error('Binding values must be whole bytes.');
    return value;
  }
  function actionList(profile, source) {
    const assigned = mask(profile, source);
    if (!bounded(assigned, allOutputs)) throw Error('Invalid button output mask.');
    const list = [];
    for (let action = 1; action <= actionCount; ++action)
      if (assigned & (1 << (action - 1))) list.push(action);
    return list;
  }
  function readActions(list) {
    if (!Array.isArray(list)) throw Error('Expected a list of gamepad outputs.');
    let bits = 0;
    for (const action of list) {
      if (!bounded(action, actionCount) || !action) throw Error('Invalid gamepad output.');
      const flag = 1 << (action - 1);
      if (bits & flag) throw Error('Duplicate gamepad output.');
      bits |= flag;
    }
    return bits;
  }

  function validate(profile) {
    const version = profile[0];
    if (!size(version) || size(version) !== profile.length) throw Error('Unsupported binding profile.');
    if (profile[1] > 3) throw Error('Invalid stick settings.');
    if (profile.subarray(2, 34).some(v => v > (version === 5 ? 0 : actionCount)))
      throw Error('Invalid click binding.');
    for (let side = 0; side < 2; ++side) {
      const used = new Uint8Array(18);
      for (let slot = 0; slot < 12; ++slot) {
        const offset = 34 + 5 * (side * 12 + slot);
        const [channel, mode, release, engage, action] = profile.subarray(offset, offset + 5);
        if (action > actionCount) throw Error('Invalid touch output.');
        if (channel === 255) continue;
        if ((!side && slot >= 9) || channel >= used.length || mode > 10 || release === engage)
          throw Error('Invalid touch calibration.');
        const field = [255, 15, 240][mode] ?? 1 << (mode - 3);
        if (used[channel] & field) throw Error('Overlapping touch calibrations.');
        used[channel] |= field;
      }
      for (let slot = 0; slot < 3; ++slot) {
        const offset = 154 + 3 * (side * 3 + slot);
        const [channel, release, engage] = profile.subarray(offset, offset + 3);
        if (channel > 2 || (channel && release === engage)) throw Error('Invalid press travel calibration.');
      }
      const source = version >= 4 ? profile[triggerOffset + side] : 0;
      if (source && (source > 24 || profile[34 + (source - 1) * 5] === 255))
        throw Error('The analog trigger source needs a touch calibration.');
    }
  }

  function encode(profile) {
    validate(profile);
    const version = profile[0];
    const slice = (start, length) => Array.from(profile.slice(start, start + length));
    function hand(side) {
      const settings = {
        stickEnabled: Boolean(profile[1] & (1 << side)),
        buttons: version === 5 ? Array.from({length:16}, (_, i) => actionList(profile, side * 16 + i))
          : slice(2 + side * 16, 16),
        touch: Array.from({length:12}, (_, slot) => slice(34 + (side * 12 + slot) * 5, 5)),
        travel: Array.from({length:3}, (_, slot) => slice(154 + (side * 3 + slot) * 3, 3)),
      };
      if (version >= 4) {
        const index = profile[triggerOffset + side] - 1;
        settings.analogTriggerSource = index < 0 ? null : {hand:handNames[Math.floor(index / 12)], slot:index % 12};
      }
      if (version === 5) {
        settings.travelOutputs = Array.from({length:3}, (_, i) => actionList(profile, 32 + side * 3 + i));
        settings.buttonNames = Array.from({length:16}, (_, i) => name(profile, side * 16 + i));
        settings.travelNames = Array.from({length:3}, (_, i) => name(profile, 32 + side * 3 + i));
      }
      return settings;
    }
    const result = {format, version:1, profileVersion:version, left:hand(0), right:hand(1)};
    if (version === 5) result.outputNames = Array.from({length:actionCount}, (_, i) => name(profile, sourceCount + i));
    return result;
  }

  function decode(data, targetVersion = 5) {
    if (data?.format !== format || data.version !== 1 || !size(data.profileVersion) || !size(targetVersion))
      throw Error('Choose a Frame Bridge bindings backup (profile versions 3–5).');
    const version = data.profileVersion;
    if (version > targetVersion) throw Error('This backup requires V' + version + ' firmware.');
    const result = new Uint8Array(size(targetVersion));
    result[0] = targetVersion;
    [data.left, data.right].forEach((hand, side) => {
      if (typeof hand?.stickEnabled !== 'boolean') throw Error('Missing stick setting.');
      if (hand.stickEnabled) result[1] |= 1 << side;
      array(hand.touch, 12).forEach((row, i) => result.set(bytes(row, 5), 34 + (side * 12 + i) * 5));
      array(hand.travel, 3).forEach((row, i) => result.set(bytes(row, 3), 154 + (side * 3 + i) * 3));
      if (version >= 4 && hand.analogTriggerSource !== null) {
        const source = hand.analogTriggerSource;
        const sourceHand = handNames.indexOf(source?.hand);
        if (sourceHand < 0 || !bounded(source?.slot, sourceHand ? 11 : 8)) throw Error('Invalid analog trigger touch source.');
        result[triggerOffset + side] = 1 + sourceHand * 12 + source.slot;
      }
      if (version === 5) {
        array(hand.buttons, 16).forEach((list, i) => setMask(result, side * 16 + i, readActions(list)));
        array(hand.travelOutputs, 3).forEach((list, i) => setMask(result, 32 + side * 3 + i, readActions(list)));
        array(hand.buttonNames, 16).forEach((v, i) => setName(result, side * 16 + i, v));
        array(hand.travelNames, 3).forEach((v, i) => setName(result, 32 + side * 3 + i, v));
      } else {
        const buttons = bytes(hand.buttons, 16);
        if (buttons.some(v => v > actionCount)) throw Error('Invalid click binding.');
        if (targetVersion === 5) {
          buttons.forEach((action, i) => setMask(result, side * 16 + i, action ? 1 << (action - 1) : 0));
          hand.travel.forEach(([channel], i) => {
            const slot = side * 3 + i;
            if (channel) setMask(result, 32 + slot, 1 << (legacyTravel[slot] - 1));
          });
        } else result.set(buttons, 2 + side * 16);
      }
    });
    if (version === 5) array(data.outputNames, actionCount).forEach((v, i) => setName(result, sourceCount + i, v));
    validate(result);
    return result;
  }

  return {encode, decode, mask, setMask, name, setName, size, triggerOffset};
})();
if (typeof module !== 'undefined') module.exports = BindingBackup;
