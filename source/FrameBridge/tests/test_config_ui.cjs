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
    setAttribute(name, value) { this.attributes[name] = String(value); }
    getAttribute(name) { return this.attributes[name] ?? null; }
    append(...nodes) { for (const node of nodes) { node.parentElement = this; this.children.push(node); } }
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
  const profile = new Uint8Array(174);
  profile[0] = 4;
  for (let offset = 34; offset < 154; offset += 5) profile[offset] = 255;
  profile[2 + 5] = profile[2 + 6] = 5;   // Two left buttons assigned to L1.
  profile[18] = 1;                       // Right bit 0 (A) assigned to A.
  profile.set([2,30,70], 157);           // Left grab on press travel byte 7.
  profile.set([10,0,20,120,2], 94);      // Right A touch assigned to B and L2.
  profile[172] = 13;
  let boardProfile = profile.slice(), online = true;
  const writes = [], raw = [Buffer.alloc(18), Buffer.alloc(18)];
  raw[1][10] = 70;
  const state = {
    up:[1,1], epoch:[1,1], seen:[3,3], masks:[32,1], travel:[2,0], active:[0,0],
    packets:[50,60], axes:[[0,0],[0,0]], usb:1, usbLayout:2, standardUsb:0,
    touch:70, buttons:1 | 1<<4 | 1<<13, triggers:[128,0], hat:8, bad:0, overflows:0,
  };
  const timers = new Map();
  let timerID = 0;
  const sandbox = vm.createContext({document, AbortController,
    setTimeout(fn, delay) { timers.set(++timerID, {fn, delay}); return timerID; },
    clearTimeout(id) { timers.delete(id); },
    async fetch(url, {method = 'GET', body} = {}) {
      if (!online) throw Error('Board disconnected');
      if (url === '/api/state' && method === 'GET') {
        return Response.json({...state, raw:raw.map(bytes => bytes.toString('hex'))});
      }
      if (url === '/api/defaults' && method === 'GET') return new Response(profile);
      if (url === '/api/profile' && method === 'GET') return new Response(boardProfile);
      if (url === '/api/profile' && method === 'PUT') {
        boardProfile = Uint8Array.from(body); writes.push(boardProfile); return new Response('OK');
      }
      throw Error('Unexpected request ' + method + ' ' + url);
    },
  });
  for (const [, src] of html.matchAll(/<script\b[^>]*\bsrc="\/([^"]+)"/g)) {
    await vm.runInContext(fs.readFileSync(path.join(app, src), 'utf8'), sandbox, {filename:src});
  }
  const el = id => { const node = document.getElementById(id); assert.ok(node, id); return node; };
  const content = id => el(id).textContent;
  const click = id => { assert.ok(!el(id).disabled, id + ' is disabled'); el(id).click(); };
  const change = (id, value) => {
    assert.ok(!el(id).disabled, id + ' is disabled'); el(id).value = String(value);
    el(id).dispatchEvent(new EventType('change'));
  };
  async function poll() {
    const next = [...timers].find(([, timer]) => timer.delay < 2500);
    assert.ok(next, 'Board polling stopped');
    timers.delete(next[0]); await next[1].fn();
  }
  function live(side, kind, index) {
    const row = [...el('live-' + side + '-bindings').children].find(
      n => n.dataset.kind === kind && n.dataset.index === String(index));
    assert.ok(row, side + ' ' + kind + ' ' + index);
    return {output:row.children[1].textContent, state:row.dataset.state, status:row.children[2].textContent};
  }
  function tab(button, section) {
    click(button);
    for (const id of ['live','mapping','sensors']) assert.equal(!!el(id).hidden, id !== section);
    assert.equal(el(button).getAttribute('aria-pressed'), 'true');
  }

  assert.equal(content('network'), 'Board connected');
  assert.equal(live('left','click',5).state, 'pressed');
  assert.equal(live('left','click',6).state, 'released');
  assert.deepEqual(live('left','travel',1), {output:'PL', state:'pressed', status:'Pressed'});
  assert.deepEqual(live('right','analog',0), {output:'L2 level', state:'active', status:'128 / 255'});
  assert.equal(live('right','touch',0).state, 'released');
  state.seen[0] = 1; state.up[1] = 0; await poll();
  assert.equal(live('left','travel',1).state, 'waiting');
  assert.equal(live('right','click',0).state, 'offline');
  state.seen[0] = 3; state.up[1] = 1; state.masks[1] = 0; await poll();

  tab('map-tab','mapping');
  const tbody = el('bindings');
  const headers = tbody.parentElement.children[0].children[0].children;
  assert.deepEqual([...headers].map(node => node.textContent), ['8BitDo Pro 3', 'Steam Frame button', 'Actual bit']);
  assert.equal(tbody.children.length, 22);
  for (const row of tbody.children) assert.deepEqual([...row.children].map(c => c.tagName), ['TH','TD','TD']);
  assert.equal(content('action-5'), 'L1');
  assert.deepEqual([...el('binding-5').children].map(n => n.textContent), ['Left bumper', 'Left stick click']);
  assert.deepEqual([...el('bit-5').children].map(n => n.textContent), ['Left bit 5', 'Left bit 6']);
  assert.equal(content('bit-18'), 'Left travel · 0x14 byte 7');
  assert.equal(content('bit-21'), '—');
  click('learn-21'); state.masks[1] = 1; await poll();
  assert.deepEqual(['action-21','binding-21','bit-21'].map(content), ['L4','Right A','Right bit 0']);
  assert.equal(content('binding-1'), 'Unbound');
  assert.equal(content('dirty'), 'Unsaved changes');

  tab('live-tab','live');
  assert.equal(live('right','click',0).output, 'A', 'Live must show the saved mapping');

  tab('sensors-tab','sensors');
  assert.equal(el('touch-left-list').children.length, 9);
  assert.equal(el('touch-right-list').children.length, 12);
  click('surface-1-0'); assert.equal(document.activeElement, el('sensor-heading'));
  assert.equal(content('sensor-heading'), 'Right hand · A');
  assert.match(content('surface-1-0-output'), /B.*L2 \(analog\)/);
  raw[1][10] = 170; state.active[1] = 1; state.triggers[0] = 255; await poll();
  assert.equal(el('surface-1-0').dataset.state, 'touched');
  change('engage',140); assert.match(content('surface-1-0-signal'), /engage 140/);
  change('release',145); assert.equal(el('release').value, '20');
  click('surface-back'); assert.equal(document.activeElement, el('surface-1-0'));

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
  assert.equal(el('surface-0-8').dataset.state, 'offline');
  assert.equal(el('save').disabled, true);
  tab('live-tab','live'); assert.equal(live('right','analog',0).state, 'offline');
  online = true; await poll();
  tab('map-tab','mapping'); assert.equal(content('bit-21'), 'Right bit 0');
  click('save'); await new Promise(resolve => setImmediate(resolve));
  assert.equal(writes.length, 1);
  assert.equal(boardProfile[18], 21);
  assert.equal(boardProfile[97], 140);
  assert.deepEqual([boardProfile[74], boardProfile[78]], [8,17]);
  assert.equal(content('dirty'), 'On board');
  tab('live-tab','live'); assert.equal(live('right','click',0).output, 'L4');
  assert.equal(live('left','touch',8).output, 'R2');
}

module.exports = {html, testUI};
if (require.main === module) {
  testUI(fixtureDocument()).then(() => console.log('Configuration UI interactions passed.'))
    .catch(error => { console.error(error); process.exitCode = 1; });
}
