'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const app = path.join(__dirname, '../app');
const html = fs.readFileSync(path.join(app, 'index.html'), 'utf8');

// The page builds its rows with createElement. Model just the DOM surface it uses
// so this interaction test also runs on build hosts without a browser or packages.
function fixtureDocument() {
  const document = {hidden:false, activeElement:null, addEventListener() {}};
  class Element {
    constructor(tag) {
      this.tagName = tag.toUpperCase();
      this.children = [];
      this.attributes = {};
      this.dataset = {};
      this.style = {};
      this.content = '';
      this.classList = {
        toggle:(name, on) => {
          const classes = new Set((this.className || '').split(/\s+/).filter(Boolean));
          if (on) classes.add(name); else classes.delete(name);
          this.className = [...classes].join(' ');
        },
        remove:name => this.classList.toggle(name, false),
      };
    }
    get textContent() { return this.content + this.children.map(node => node.textContent).join(''); }
    set textContent(value) { this.content = String(value); this.children = []; }
    get valueAsNumber() { return this.value === '' ? NaN : Number(this.value); }
    contains(other) {
      for (let node = other; node; node = node.parentElement) if (node === this) return true;
      return false;
    }
    setAttribute(name, value) { this.attributes[name] = String(value); }
    getAttribute(name) { return this.attributes[name] ?? null; }
    append(...nodes) {
      for (const node of nodes) {
        if (node.parentElement) {
          node.parentElement.children = node.parentElement.children.filter(child => child !== node);
        }
        node.parentElement = this;
        this.children.push(node);
      }
    }
    replaceChildren(...nodes) { this.children = []; this.content = ''; this.append(...nodes); }
    focus() { document.activeElement = this; }
    click() { if (!this.disabled) this.onclick?.({target:this}); }
    dispatchEvent(event) { this['on' + event.type]?.({target:this}); }
  }
  document.createElement = tag => new Element(tag);
  const stack = [new Element('html-document')];
  for (const token of html.match(/<[^>]+>|[^<]+/g)) {
    if (token.startsWith('<!')) continue;
    if (token.startsWith('</')) { stack.pop(); continue; }
    if (!token.startsWith('<')) { stack.at(-1).content += token; continue; }
    const tag = token.match(/^<(\w+)/)[1];
    const node = new Element(tag);
    for (const [, name, value] of token.slice(1 + tag.length, -1).matchAll(/([\w-]+)(?:="([^"]*)")?/g)) {
      node.setAttribute(name, value ?? '');
      if (['id','value'].includes(name)) node[name] = value;
      if (['hidden','disabled'].includes(name)) node[name] = true;
    }
    stack.at(-1).append(node);
    if (!['meta','link','input','br','hr','img'].includes(tag)) stack.push(node);
  }
  const ids = (node, id) => node.id === id ? node : node.children.reduce((found, child) => found || ids(child, id), null);
  document.getElementById = id => ids(stack[0], id);
  return document;
}

async function testUI(document, EventType = Event) {
  const profile = new Uint8Array(1737);
  profile[0] = 9;
  profile[1735] = 100;
  profile.set([2,0,100,2,0,100],1728);
  for (let offset = 34; offset < 154; offset += 5) profile[offset] = 255;
  function readMask(bytes, source) {
    const i = 174 + source * 3;
    return bytes[i] | bytes[i+1] << 8 | bytes[i+2] << 16;
  }
  function outputs(...actions) {
    return actions.reduce((bits, action) => bits | 1 << (action - 1), 0);
  }
  for (const [source, action] of [[5,5],[6,5],[16,1],[24,20],[32,16],[33,18]]) {
    const mask = outputs(action);
    profile.set([mask & 255, (mask >> 8) & 255, mask >> 16], 174 + source * 3);
  }
  profile.set([2,64,128], 154);          // Left trigger click assigned to its own travel.
  profile.set([2,30,70], 157);           // Left grab on press travel byte 7.
  profile.set([2,64,128], 163);          // Right analog trigger has no digital click binding.
  profile.set([5,0,20,120,0], 44);       // Calibrated trigger contact on each hand.
  profile.set([3,0,20,120,0], 119);
  profile.set([10,0,20,120,2], 94);      // Right A touch assigned to B.
  profile[172] = profile[173] = 25;
  let boardProfile = profile.slice(), online = true;
  const writes = [], raw = [Buffer.alloc(18), Buffer.alloc(18)];
  raw[1][5] = 81;
  raw[1][10] = 70;
  const state = {
    up:[1,1], epoch:[1,1], seen:[3,3], masks:[32,1], travel:[2,0], active:[0,0],
    packets:[50,60], axes:[[0,0],[0,0]], usb:1, bluetoothMode:0, bluetooth:0, usbLayout:2, standardUsb:0,
    buttons:1 | 1<<4 | 1<<13, triggers:[0,0], hat:8, bad:0, overflows:0,
    motion:{hand:0, samples:0, ageMs:-1, route:'none', zeroCount:0},
  };
  const timers = new Map();
  let timerID = 0;
  const board = {AbortController, TextEncoder, TextDecoder,
    setTimeout(fn, delay) { timers.set(++timerID, {fn, delay}); return timerID; },
    clearTimeout(id) { timers.delete(id); },
    async fetch(url, {method = 'GET', body} = {}) {
      if (!online) throw Error('Board disconnected');
      if (url === '/api/state' && method === 'GET') {
        return Response.json({...state, raw:raw.map(bytes => bytes.toString('hex'))});
      }
      if (url === '/api/defaults' && method === 'GET') return new Response(profile);
      if (url === '/api/profile' && method === 'GET') return new Response(boardProfile);
      if (url === '/api/profile' && method === 'POST') {
        assert.equal(body[0],9);
        assert.equal(body.length,1737);
        return new Response('OK');
      }
      if (url === '/api/profile' && method === 'PUT') {
        boardProfile = Uint8Array.from(body);
        state.motion = {hand:boardProfile[1734], samples:0, ageMs:-1, route:'none', zeroCount:0};
        writes.push(boardProfile); return new Response('OK');
      }
      throw Error('Unexpected request ' + method + ' ' + url);
    },
  };
  async function loadPage() {
    const sandbox = vm.createContext({...board, document});
    for (const [, src] of html.matchAll(/<script\b[^>]*\bsrc="\/([^"]+)"/g)) {
      await vm.runInContext(fs.readFileSync(path.join(app, src), 'utf8'), sandbox, {filename:src});
    }
  }
  await loadPage();
  const el = id => { const node = document.getElementById(id); assert.ok(node, id); return node; };
  const content = id => el(id).textContent;
  const click = id => { assert.ok(!el(id).disabled, id + ' is disabled'); el(id).click(); };
  const change = (id, value) => {
    assert.ok(!el(id).disabled, id + ' is disabled'); el(id).value = String(value);
    el(id).dispatchEvent(new EventType('change'));
  };
  const slide = (id, value) => {
    assert.ok(!el(id).disabled);
    el(id).value = String(value);
    el(id).dispatchEvent(new EventType('input'));
  };
  const check = (id, checked) => {
    assert.ok(!el(id).disabled, id + ' is disabled');
    el(id).checked = checked;
    el(id).dispatchEvent(new EventType('change'));
  };
  async function poll() {
    const next = [...timers].find(([, timer]) => timer.delay < 2500);
    assert.ok(next, 'Board polling stopped');
    timers.delete(next[0]); await next[1].fn();
  }
  function liveRow(side, kind, index) {
    const row = [...el('live-' + side + '-bindings').children].find(
      n => n.dataset.kind === kind && n.dataset.index === String(index));
    assert.ok(row, side + ' ' + kind + ' ' + index);
    return row;
  }
  function live(side, kind, index) {
    const row = liveRow(side, kind, index);
    return {output:row.children[1].textContent, state:row.dataset.state, status:row.children[2].textContent};
  }
  function tab(button, section) {
    click(button);
    for (const id of ['live','mapping','sensors','joysticks','movement']) assert.equal(!!el(id).hidden, id !== section);
    assert.equal(el(button).getAttribute('aria-pressed'), 'true');
  }
  function learn(action) {
    // Intercept the same method on every element, including the shared Cancel button.
    const prototype = document.defaultView?.Element.prototype || Object.getPrototypeOf(el('cancel'));
    const original = Object.getOwnPropertyDescriptor(prototype, 'scrollIntoView');
    let calls = 0;
    Object.defineProperty(prototype, 'scrollIntoView', {configurable:true, value:() => { ++calls; }});
    try {
      click('learn-' + action);
      assert.equal(calls, 0, 'Learn must keep the current scroll position');
    } finally {
      if (original) Object.defineProperty(prototype, 'scrollIntoView', original);
      else delete prototype.scrollIntoView;
    }
  }
  function rowFeedback(action, expected) {
    const panel = el('binding-feedback'), cell = el('action-' + action).parentElement;
    const children = [...cell.children];
    assert.equal(cell.tagName, 'TH');
    assert.equal(panel.parentElement, cell);
    assert.equal(children[children.indexOf(panel) - 1].className, 'binding-controls');
    assert.ok(!panel.hidden);
    for (const id of ['binding-message','cancel','pull-tools','pull-capture','pull-channel','pull-confirm']) {
      assert.ok(panel.contains(el(id)), id + ' must follow the selected row');
    }
    assert.match(content('binding-message'), expected);
    assert.doesNotMatch(content('message'), expected, 'Binding feedback belongs in its row');
  }
  function noRowPrompt() {
    assert.ok(el('binding-feedback').hidden || !content('binding-message').trim(), 'Stale binding prompt');
    assert.ok(el('cancel').hidden);
    assert.ok(el('pull-tools').hidden);
  }

  assert.equal(content('network'), 'Board connected');
  assert.equal(content('output-hosts'), 'USB ready');
  assert.match(content('bluetooth-status'), /output is off/);
  assert.match(content('live'), /Hold BOOT for at least 3 seconds/);
  assert.match(content('live'), /return to USB/);
  assert.match(content('live'), /FrameBridge Gamepad/);
  state.usb = 0; await poll();
  assert.equal(content('output-hosts'), 'USB · waiting for host');
  state.usb = 1; state.bluetoothMode = 1;
  for (const [mode, message] of [[1,/Pairing window open/],[0,/Pairing window closed/],[2,/reconnect/],[3,/enable gamepad/],[4,/ready/]]) {
    state.bluetooth = mode; await poll();
    assert.match(content('bluetooth-status'), message);
    assert.equal(content('output-hosts'), mode === 4 ? 'Bluetooth ready' : 'Bluetooth · waiting for host');
    assert.match(content('host-check'), /Input is routed to Bluetooth/);
  }
  state.usb = 0; await poll();
  assert.match(content('host-check'), /No USB gamepad host/);
  assert.equal(content('output-hosts'), 'Bluetooth ready');
  state.usb = 1; state.bluetoothMode = 0; await poll();
  assert.equal(content('output-hosts'), 'USB ready');
  assert.match(content('bluetooth-status'), /output is off/);
  state.bluetooth = 0; state.bluetoothOverflows = 2; await poll();
  assert.match(content('diagnostic'), /2 Bluetooth queue overflows/);
  state.bluetoothOverflows = 0;
  assert.equal(document.getElementById('touch'), null);
  assert.doesNotMatch(content('live'), /A contact/);
  assert.match(content('live'), /Right Steam.*0x19 byte 5/);
  assert.match(content('right-steam-level'), /\b81\b/);
  assert.equal(live('left','click',5).state, 'pressed');
  assert.equal(live('left','click',6).state, 'released');
  assert.deepEqual(live('left','travel',1), {output:'PL', state:'pressed', status:'Pressed'});
  assert.deepEqual(live('left','physical',0), {output:'L2 level', state:'released', status:'0 / 255'});
  assert.deepEqual(live('right','physical',1), {output:'R2 level', state:'released', status:'0 / 255'});
  assert.equal(live('right','touch',0).state, 'released');
  state.seen[0] = 1; state.up[1] = 0; await poll();
  assert.equal(live('left','travel',1).state, 'waiting');
  assert.equal(live('right','click',0).state, 'offline');
  assert.doesNotMatch(content('right-steam-level'), /\b81\b/);
  state.seen[0] = 3; state.up[1] = 1; state.masks[1] = 0; await poll();

  tab('movement-tab','movement');
  assert.equal(el('movement-hand').value,'0');
  assert.equal(el('gyro-gain').value,'100');
  assert.equal(el('gyro-reset-button').value,'0');
  const resetOption = value => [...el('gyro-reset-button').children].find(opt => opt.value === String(value));
  assert.match(resetOption(7).textContent, /left stick click/i);
  assert.match(resetOption(17).textContent, /right A/i);
  for (const [i, name] of ['Left trigger', 'Left grab', 'Left grab (full click)', 'Right trigger', 'Right grab', 'Right grab (full click)'].entries()) {
    assert.ok(resetOption(33+i).textContent.includes(name));
    assert.ok(!resetOption(33+i).disabled);
  }
  assert.match(resetOption(38).textContent, /calibrate in Bindings/);
  for (const value of [8,24]) assert.ok(resetOption(value).hidden && resetOption(value).disabled);
  assert.equal(content('movement-status'),'Off');
  assert.ok(el('movement-samples').hidden);
  change('movement-hand',1);
  assert.equal(content('movement-dirty'),'Unsaved changes');
  assert.equal(content('movement-status'),'Off', 'Status shows the saved hand while editing');
  change('movement-hand',0);
  assert.ok(el('save').disabled);
  change('movement-hand',2);
  change('gyro-reset-button',17);
  slide('gyro-gain',250);
  assert.equal(content('gyro-gain-value'),'250%');
  assert.equal(el('gyro-gain').getAttribute('aria-valuetext'),'250%');
  assert.equal(writes.length,0);

  tab('joysticks-tab','joysticks');
  state.axes = [[-32768,32767],[2800,6400]];
  await poll();
  assert.equal(content('stick-left-raw'), '-32768 / 32767');
  assert.equal(content('stick-left-output'), '0 / 0', 'Disabled stick stays centered');
  assert.equal(el('stick-left-invert-y').checked, true);
  check('stick-left',true);
  assert.equal(content('stick-left-output'), '-32768 / -32768');
  check('stick-left-invert-x',true);
  check('stick-left-invert-y',false);
  slide('stick-left-sensitivity',50);
  assert.equal(content('stick-left-output'), '16384 / 16384');
  assert.equal(el('stick-left-sensitivity').getAttribute('aria-valuetext'), '50%');
  slide('stick-left-deadzone',30);
  state.axes[0]=[9000,-9000]; await poll();
  assert.equal(content('stick-left-output'), '0 / 0');
  assert.equal(content('stick-right-raw'), '2800 / 6400');
  assert.equal(el('stick-right-revert').disabled, true, 'Hand settings are independent');
  check('stick-right',true); slide('stick-right-sensitivity',200);
  assert.equal(content('stick-right-output'), '5600 / -12800');
  click('stick-right-revert');
  assert.equal(el('stick-right').checked, false);
  assert.equal(content('stick-right-output'), '0 / 0');
  assert.equal(el('stick-left-deadzone').value,'30');
  assert.equal(content('stick-dirty'),'Unsaved changes');
  state.seen[0]=1; state.up[1]=0; await poll();
  assert.equal(el('stick-left-raw-dot').hidden,true);
  assert.equal(el('stick-left-output-dot').hidden,true);
  assert.equal(content('stick-left-output'),'—');
  assert.match(content('stick-left-status'), /waiting/i);
  assert.match(content('stick-right-status'), /offline/i);
  state.seen[0]=3; state.up[1]=1; await poll();
  assert.equal(el('stick-left-raw-dot').hidden,false);
  assert.equal(writes.length,0);

  tab('map-tab','mapping');
  const tbody = el('bindings');
  const headers = tbody.parentElement.children[0].children[0].children;
  assert.deepEqual([...headers].map(node => node.textContent), ['8BitDo Pro 3', 'Steam Frame button', 'Actual bit']);
  assert.equal(tbody.children.length, 22);
  for (const row of tbody.children) assert.deepEqual([...row.children].map(c => c.tagName), ['TH','TD','TD']);
  assert.equal(content('action-5'), 'L1');
  assert.match(content('binding-16'), /Left trigger travel/i);
  assert.equal(content('bit-16'), 'Left travel · 0x14 byte 7');
  assert.match(content('binding-20'), /Right Steam/i);
  assert.deepEqual([...el('binding-5').children].map(n => n.textContent), ['Left bumper', 'Left stick click']);
  assert.deepEqual([...el('bit-5').children].map(n => n.textContent), ['Left bit 5', 'Left bit 6']);
  assert.equal(content('bit-18'), 'Left travel · 0x14 byte 7');
  assert.equal(content('bit-21'), '—');
  learn(2); rowFeedback(2, /hold.*B/i);
  assert.ok(!el('cancel').hidden && el('pull-tools').hidden);
  state.masks[1] = 3; await poll();
  rowFeedback(2, /several bits|one button/i);

  learn(16); rowFeedback(16, /capture.*travel/i);
  assert.ok(![...el('action-2').parentElement.children].includes(el('binding-feedback')));
  assert.ok(!el('pull-tools').hidden && !el('pull-capture').hidden);
  click('pull-capture'); rowFeedback(16, /no clear travel in byte 7/i);
  // Touch and byte 6 can both change without a pull. Neither can be learned as trigger travel.
  click('calibrate-travel-16');
  state.masks[0] |= 1 << 7;
  raw[0][14] = 140; await poll();
  click('pull-capture'); rowFeedback(16, /no clear travel in byte 7/i);
  raw[0][15] = 105; await poll();
  click('pull-capture'); rowFeedback(16, /release/i);
  assert.ok(el('pull-channel').hidden && !el('pull-confirm').hidden);
  click('pull-confirm'); rowFeedback(16, /release/i);
  raw[0][14] = raw[0][15] = 0; await poll();
  click('pull-confirm'); rowFeedback(16, /travel learned/i);
  assert.ok(el('cancel').hidden && el('pull-tools').hidden);
  assert.equal(content('bit-16'), 'Left travel · 0x14 byte 7');

  learn(2); click('cancel'); rowFeedback(2, /cancel/i);
  assert.ok(el('cancel').hidden);
  learn(2); click('clear-16'); rowFeedback(16, /cleared/i);
  assert.ok(el('cancel').hidden && el('pull-tools').hidden);
  assert.equal(content('bit-16'), '—');
  state.masks[1] = 0; await poll();
  learn(21); state.masks[1] = (1 << 7) | 1; await poll();
  rowFeedback(21, /Right bit 0.*L4/);
  assert.deepEqual(['action-21','binding-21','bit-21'].map(content), ['L4','Right A','Right bit 0']);
  assert.equal(content('binding-1'), 'Right A', 'A second output must preserve the first');
  state.masks[1] = 0; await poll();
  learn(21); state.masks[1] = 2; await poll();
  assert.match(content('binding-21'), /Right A.*Right B/);
  change('binding-source',17);
  assert.equal(el('source-output-21').checked, true);
  check('source-output-4',true);
  check('source-output-21',false);
  assert.equal(content('binding-21'), 'Right A', 'Removing a link must preserve other sources');
  assert.equal(content('binding-4'), 'Right B');
  change('binding-source',16);
  assert.ok(el('source-output-1').checked && el('source-output-21').checked);
  change('source-name','Jump paddle');
  assert.match(content('binding-1'), /Jump paddle/);
  click('rename-21'); el('binding-name').value = 'Boost'; click('apply-name');
  assert.match(content('action-21'), /Boost/);
  assert.equal(content('dirty'), 'Unsaved changes');

  learn(2); rowFeedback(2, /hold.*B/i);
  tab('live-tab','live');
  state.masks[1] |= 2; await poll();
  assert.equal(live('right','click',0).output, 'A', 'Live must show the saved mapping');
  tab('map-tab','mapping'); noRowPrompt();
  assert.equal(content('bit-2'), '—', 'Leaving the tab must cancel a pending Learn');

  change('trigger-right-engage',45);
  change('trigger-right-release',150);
  click('trigger-right-apply');
  assert.equal(content('bit-17'), '—', 'Applying thresholds preserves the existing click assignment');
  change('trigger-right-engage',150);
  click('trigger-right-apply');
  assert.match(content('trigger-right-note'), /differ|distinct|equal|separat/i);
  change('trigger-right-engage',45);
  assert.ok(el('trigger-right-apply').disabled, 'Rejected thresholds must leave the staged values intact');
  for (const source of [7,23]) {
    change('binding-source',source);
    for (let action = 1; action <= 22; action++) {
      const box = el('source-output-' + action);
      assert.ok(box.disabled && !box.checked, 'Trigger contact stays unbound in the source editor');
    }
  }
  click('trigger-right-bind');
  assert.equal(content('bit-17'), 'Right travel · 0x14 byte 7');

  tab('sensors-tab','sensors');
  assert.equal(el('touch-left-list').children.length, 9);
  assert.equal(el('touch-right-list').children.length, 12);
  assert.match(content('surface-1-7-signal'), /0x19 byte 5.*uncalibrated/i);
  click('surface-1-0'); assert.equal(document.activeElement, el('sensor-heading'));
  assert.equal(content('sensor-heading'), 'Right hand · A');
  assert.equal(content('surface-1-0-output'), 'Output · B');
  raw[1][10] = 170; state.active[1] = 1; await poll();
  assert.equal(el('surface-1-0').dataset.state, 'touched');
  change('engage',140); assert.match(content('surface-1-0-signal'), /engage 140/);
  change('release',145); assert.equal(el('release').value, '20');
  click('surface-back'); assert.equal(document.activeElement, el('surface-1-0'));

  for (const [side, slot] of [[0,2], [1,5]]) {
    click('surface-' + side + '-' + slot);
    assert.ok(el('sensor-action').disabled, 'Trigger contact stays unbound when calibrated');
    assert.equal(el('sensor-action').value, '0');
    assert.equal(content('surface-' + side + '-' + slot + '-output'), 'No output assigned');
    click('surface-back');
  }

  click('surface-0-8'); assert.equal(el('surface-0-8').dataset.state, 'unlearned');
  click('sensor-learn');
  assert.equal(el('surface-1-0').disabled, true);
  for (let phase = 0; phase < 5; ++phase) {
    click('capture');
    for (let sample = 0; sample < 5; ++sample) {
      raw[0][8] = (phase % 2 ? 160 : 10) + sample; state.packets[0]++; await poll();
    }
  }
  assert.equal(el('wizard').hidden, true);
  assert.match(content('surface-0-8-signal'), /0x19 byte 8/);
  change('sensor-action', 17); assert.match(content('surface-0-8-output'), /R2/);
  assert.equal(writes.length, 0, 'Learning and editing must only stage changes');

  online = false; await poll();
  assert.equal(content('network'), 'Board disconnected');
  assert.equal(content('bluetooth-status'), 'Waiting for board');
  assert.equal(content('output-hosts'), 'Waiting for board');
  assert.equal(el('surface-0-8').dataset.state, 'offline');
  assert.equal(el('save').disabled, true);
  tab('live-tab','live'); assert.equal(live('left','physical',0).state, 'offline');
  online = true; await poll();
  tab('map-tab','mapping'); assert.equal(content('bit-21'), 'Right bit 0');
  learn(2); rowFeedback(2, /hold.*B/i);
  click('save'); await new Promise(resolve => setImmediate(resolve));
  assert.match(content('message'), /saved to board/i);
  noRowPrompt();
  assert.equal(writes.length, 1);
  assert.equal(boardProfile[1],1);
  assert.deepEqual([...boardProfile.slice(1728,1734)],[1,30,50,2,0,100]);
  assert.equal(boardProfile[1734],2);
  assert.equal(boardProfile[1735],250);
  assert.equal(boardProfile[1736],17);
  assert.equal(readMask(boardProfile,16), outputs(1,21), 'The reset button keeps its gamepad actions');
  assert.equal(readMask(boardProfile,17), outputs(4));
  assert.equal(readMask(boardProfile,7), 0);
  assert.equal(readMask(boardProfile,32), 0);
  assert.equal(boardProfile[154], 2, 'Removing a route preserves its travel calibration');
  assert.deepEqual([...boardProfile.slice(163,166)], [2,150,45]);
  assert.equal(readMask(boardProfile,35), outputs(17));
  assert.deepEqual([...boardProfile.slice(172,174)], [25,25]);
  assert.equal(readMask(boardProfile,23), 0);
  assert.equal(boardProfile[48], 0);
  assert.equal(boardProfile[123], 0);
  assert.equal(boardProfile[97], 140);
  assert.deepEqual([boardProfile[74], boardProfile[78]], [8,17]);
  assert.equal(content('dirty'), 'On board');
  timers.clear();
  document = fixtureDocument(); await loadPage();
  assert.equal(content('network'), 'Board connected');
  assert.match(live('right','click',0).output, /A.*Boost/);
  assert.match(liveRow('right','click',0).children[0].textContent, /Jump paddle/);
  assert.equal(live('right','click',1).output, 'Y');
  assert.equal(live('left','touch',8).output, 'R2');
  assert.match(liveRow('right','physical',1).children[0].textContent, /Right trigger travel · byte 7/);
  tab('movement-tab','movement');
  assert.equal(el('movement-hand').value,'2');
  assert.equal(el('gyro-gain').value,'250');
  assert.equal(el('gyro-reset-button').value,'17');
  assert.match(resetOption(17).textContent, /Jump paddle \(Right A\)/);
  assert.equal(content('gyro-reset-count'),'0');
  state.motion.zeroCount = 1; await poll(); assert.equal(content('gyro-reset-count'),'1');
  assert.match(content('movement-status'),/right hand.*waiting/i);
  assert.match(content('movement-route'),/compatible host/i);
  Object.assign(state.motion, {samples:4, ageMs:12, valid:true, accel:[-120,0,3200], gyro:[41,-80,-30]}); await poll();
  assert.equal(content('movement-status'),'Right hand');
  assert.equal(el('movement-axes').hidden,false);
  assert.equal(content('movement-accel'),'-120 / 0 / 3200');
  assert.equal(content('movement-gyro'),'41 / -80 / -30');
  slide('gyro-gain',25); await poll();
  assert.equal(content('movement-gyro'),'41 / -80 / -30', 'Sensitivity only scales outgoing data');
  assert.equal(el('movement-samples').hidden,false);
  assert.match(content('movement-samples'),/4.*12 ms/);
  state.motion.route = 'usb'; await poll(); assert.equal(content('movement-route'),'USB');
  state.motion.route = 'bluetooth'; await poll(); assert.equal(content('movement-route'),'Bluetooth');
  for (const [valid, ageMs] of [[true,250],[false,5]]) {
    Object.assign(state.motion, {valid, ageMs}); await poll();
    assert.match(content('movement-status'),/stale/i);
    assert.ok(el('movement-axes').hidden);
    assert.equal(content('movement-gyro'),'—');
    assert.equal(content('movement-accel'),'—');
  }
  Object.assign(state.motion, {valid:true, ageMs:249}); await poll();
  assert.equal(el('movement-axes').hidden,false);
  state.up[1] = 0; await poll();
  assert.match(content('movement-status'),/right hand.*offline/i);
  assert.ok(el('movement-samples').hidden);
  assert.ok(el('movement-axes').hidden);
  state.up[1] = 1;
  tab('joysticks-tab','joysticks');
  assert.equal(el('stick-left').checked,true);
  assert.equal(el('stick-left-invert-x').checked,true);
  assert.equal(el('stick-left-invert-y').checked,false);
  assert.equal(el('stick-left-sensitivity').value,'50');
  assert.equal(el('stick-left-revert').disabled,true);
  tab('map-tab','mapping');
  assert.equal(el('trigger-right-engage').value, '45');
  assert.equal(el('trigger-right-release').value, '150');
  state.usbLayout = 1;
  raw[1][14] = 62; raw[1][15] = 141; await poll();
  assert.ok(!el('trigger-settings').hidden, 'Trigger travel must also be configurable in Standard USB mode');
  assert.match(content('trigger-right-levels'), /0x14 byte 7.*141/i);
  assert.ok(el('trigger-levels').hidden, 'Standard USB has no analog trigger report');
  state.bluetoothMode = 1; state.bluetooth = 4; await poll();
  tab('live-tab','live');
  assert.equal(content('output-hosts'), 'Bluetooth ready');
  assert.match(content('trigger-levels'), /Left trigger/);
  assert.equal(live('left','physical',0).output, 'L2 level');
  assert.equal(live('right','physical',1).status, '114 / 255', 'Falling physical travel ignores the click thresholds');
  // Both touch contacts stay active throughout the independent trigger sweeps.
  state.masks[0] |= 1 << 7; state.masks[1] |= 1 << 7;
  state.active[0] |= 1 << 2; state.active[1] |= 1 << 5;
  for (let sample = 0; sample <= 255; sample++) {
    raw[0][15] = sample;
    raw[1][15] = (sample + 73) & 255;
    await poll();
    assert.equal(live('left','physical',0).status, sample + ' / 255');
    assert.equal(live('right','physical',1).status, (255 - raw[1][15]) + ' / 255');
  }
  tab('map-tab','mapping');
  change('binding-source',16);
  assert.equal(el('source-name').value, 'Jump paddle');
  assert.ok(el('source-output-1').checked && el('source-output-21').checked);
  state.up[1] = 0; await poll();
  assert.match(content('trigger-right-levels'), /offline/i);
  tab('movement-tab','movement');
  change('movement-hand',1); change('gyro-reset-button',34);
  click('save'); await new Promise(resolve => setImmediate(resolve)); await poll();
  assert.match(content('movement-status'),/left hand/i);
  assert.equal(boardProfile[1735],25);
  assert.equal(boardProfile[1736],34);
  timers.clear(); document = fixtureDocument(); await loadPage();
  tab('movement-tab','movement');
  assert.equal(el('gyro-reset-button').value,'34');
  assert.match(resetOption(34).textContent,/Left grab travel/);
  async function importProfile(profileBytes) {
    const json = JSON.stringify(require('../app/backup.js').encode(profileBytes));
    await el('backup-file').onchange({target:{files:[{size:json.length,text:async()=>json}]}});
    assert.match(content('message'),/imported/i);
  }
  const withContact = boardProfile.slice(); withContact[1736] = 24;
  await importProfile(withContact);
  assert.equal(el('gyro-reset-button').value,'24');
  assert.equal(resetOption(24).hidden,false);
  assert.match(resetOption(24).textContent,/trigger touch/i);
  const old = boardProfile.slice(0,1736); old[0] = 8;
  await importProfile(old);
  assert.ok(resetOption(24).hidden);
  assert.equal(el('gyro-reset-button').value,'0','V8 backups stage an unbound reset');
  assert.equal(el('movement-hand').value,'1');
  assert.equal(el('gyro-gain').value,'25');
  assert.equal(boardProfile[1736],34,'Import is staged until saved');
  change('gyro-reset-button',7);
  click('defaults');
  assert.equal(el('movement-hand').value,'0');
  assert.equal(el('gyro-gain').value,'100');
  assert.equal(el('gyro-reset-button').value,'0');
  assert.equal(boardProfile[1734],1, 'Restoring defaults stages the change');
  click('save'); await new Promise(resolve => setImmediate(resolve)); await poll();
  assert.equal(content('movement-status'),'Off');
  assert.equal(content('movement-route'),'Off');
  assert.equal(boardProfile[1735],100);
  assert.equal(boardProfile[1736],0);
}

module.exports = {html, testUI};
if (require.main === module) {
  testUI(fixtureDocument()).then(() => console.log('Configuration UI interactions passed.'))
    .catch(error => { console.error(error); process.exitCode = 1; });
}
