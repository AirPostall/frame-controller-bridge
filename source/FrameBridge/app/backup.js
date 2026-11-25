'use strict';

const BindingBackup = (() => {
  const format = 'framebridge-bindings';
  const triggerOffset = 172;
  const size = version => version === 3 ? triggerOffset : version === 4 ? triggerOffset + 2 : 0;
  const handNames = ['left', 'right'];

  function encode(profile) {
    if (!size(profile[0]) || size(profile[0]) !== profile.length) throw Error('Unsupported binding profile.');
    const bytes = (start, length) => Array.from(profile.slice(start, start + length));
    function hand(side) {
      const settings = {
        stickEnabled: Boolean(profile[1] & (1 << side)),
        buttons: bytes(2 + side * 16, 16),
        touch: Array.from({length: 12}, (_, slot) => bytes(34 + (side * 12 + slot) * 5, 5)),
        travel: Array.from({length: 3}, (_, slot) => bytes(154 + (side * 3 + slot) * 3, 3))
      };
      if (profile[0] === 4) {
        const index = profile[triggerOffset + side] - 1;
        settings.analogTriggerSource = index < 0 ? null : {hand: handNames[Math.floor(index / 12)], slot: index % 12};
      }
      return settings;
    }
    return {format, version: 1, profileVersion: profile[0], left: hand(0), right: hand(1)};
  }

  function decode(data, targetVersion = 4) {
    if (data?.format !== format || data.version !== 1 || !size(data.profileVersion) || !size(targetVersion))
      throw Error('Choose a Frame Bridge V3 or V4 bindings backup.');
    if (data.profileVersion > targetVersion)
      throw Error('This backup requires Frame Bridge V4 firmware.');
    function array(value, size) {
      if (!Array.isArray(value) || value.length !== size) throw Error('Invalid binding array length.');
      return value;
    }
    function bytes(value, size) {
      if (!array(value, size).every(n => Number.isInteger(n) && n >= 0 && n <= 255))
        throw Error('Binding values must be whole bytes.');
      return value;
    }
    const result = new Uint8Array(size(targetVersion));
    result[0] = targetVersion;
    [data.left, data.right].forEach((hand, side) => {
      if (typeof hand?.stickEnabled !== 'boolean') throw Error('Missing stick setting.');
      if (hand.stickEnabled) result[1] |= 1 << side;
      result.set(bytes(hand.buttons, 16), 2 + side * 16);
      array(hand.touch, 12).forEach((row, i) => result.set(bytes(row, 5), 34 + (side * 12 + i) * 5));
      array(hand.travel, 3).forEach((row, i) => result.set(bytes(row, 3), 154 + (side * 3 + i) * 3));
    });
    if (data.profileVersion === 4) {
      [data.left, data.right].forEach((hand, side) => {
        const source = hand.analogTriggerSource;
        if (source === null) return;
        const sourceHand = handNames.indexOf(source?.hand);
        const slot = source?.slot;
        if (sourceHand < 0 || !Number.isInteger(slot) || slot < 0 || slot >= (sourceHand ? 12 : 9))
          throw Error('Invalid analog trigger touch source.');
        const index = sourceHand * 12 + slot;
        if (result[34 + index * 5] === 255) throw Error('The analog trigger source needs a touch calibration.');
        result[triggerOffset + side] = 1 + index;
      });
    }
    return result;
  }

  return {encode, decode, size, triggerOffset};
})();
if (typeof module !== 'undefined') module.exports = BindingBackup;
