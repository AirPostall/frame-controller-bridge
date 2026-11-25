'use strict';
const byId = id => document.getElementById(id);
const names = ['', 'A', 'B', 'X', 'Y', 'L bumper', 'R bumper', 'Select', 'Start',
  'L stick click', 'R stick click', 'Home', 'D-pad up', 'D-pad right', 'D-pad down', 'D-pad left',
  'L trigger', 'R trigger', 'L grab', 'R grab', 'Right Steam', 'L grab full click', 'R grab full click'];
const travelActions = [16,18,21,17,19,22]; // by hand, then trigger/grab/full grab
const pro3Outputs = ['Off', 'A', 'B', 'X', 'Y', 'L1', 'R1', 'Select', 'Start',
  'L3', 'R3', 'Home', 'D-pad up', 'D-pad right', 'D-pad down', 'D-pad left',
  'L2', 'R2', 'PL', 'PR', 'Home', 'L4', 'R4'];
const pro3RowTitles = {7:'− Select', 8:'+ Start', 20:'Home (alternate binding)'};
const measuredButtons = [
  {0:'D-pad down', 1:'D-pad right', 2:'D-pad left', 3:'D-pad up', 5:'bumper', 6:'stick click', 8:'Steam', 9:'View'},
  {0:'A', 1:'B', 2:'X', 3:'Y', 5:'bumper', 6:'stick click', 9:'Menu'}
];
const usbLayouts = ['Extended', 'Standard', 'Wired Pro 3'];
const triggerSelectors = ['lt-source', 'rt-source'];
const travelOffset = index => 154 + 3*index;
let defaultProfile, pull=null;
const sides = ['Left', 'Right'];
const hats = ['Up', 'Up-right', 'Right', 'Down-right', 'Down', 'Down-left', 'Left', 'Up-left'];
let saved, draft, latest, learning = 0, previousMasks, previousUp;
let timer, busy = false, saving = false;
const label = (id, value) => { const el=byId(id); if(el.textContent!==String(value)) el.textContent=value; };
const note = value => label('message', value);
const bindingNote = value => label('binding-message', value);
const bindingIndex = (side, bit) => 2 + side * 16 + bit;
const usbLayout = state => state?.usbLayout ?? (state?.standardUsb ? 1 : 0);

function pro3Name(action) {
  return pro3Outputs[action];
}

function outputName(action, layout) {
  return layout === 2 ? pro3Name(action) : action === 0 ? 'Off' : names[action];
}

function steamButtonName(hand, bit) {
  const name = measuredButtons[hand]?.[bit];
  return name ? sides[hand] + ' ' + name : 'Unknown button';
}

function learnTarget(action) {
  if (action === 20) return 'Right Steam';
  const index = travelActions.indexOf(action);
  if (index < 0) return '';
  const control = ['trigger', 'grab', 'grab (full click)'][index % 3];
  return sides[Math.floor(index / 3)] + ' ' + control;
}

function sourceLines(id, entries) {
  const cell = byId(id);
  cell.replaceChildren();
  for (const entry of entries) {
    const line = document.createElement('span');
    line.className = 'binding-source';
    line.textContent = entry;
    cell.append(line);
  }
}

function refreshTriggerSources() {
  const enabled = usbLayout(latest) === 2 && draft?.[0] === 4;
  byId('analog-routing').hidden = !enabled;
  if (!enabled) return;
  triggerSelectors.forEach((id, output) => {
    const select = byId(id);
    select.replaceChildren();
    const add = (value, name) => {
      const option = document.createElement('option');
      option.value = String(value);
      option.textContent = name;
      select.append(option);
    };
    add(0, 'Digital press only');
    Touch.slots.forEach((slots, h) => slots.forEach((name, i) => {
      const o = Touch.offset(h, i);
      if (draft[o] !== Touch.empty) {
        const suffix = draft[o + 1] >= 3 ? ' (on/off)' : '';
        add(1 + h * 12 + i, sides[h] + ' ' + name + suffix);
      }
    }));
    select.value = String(draft[BindingBackup.triggerOffset + output]);
    select.disabled = saving;
  });
}

async function request(path, options={}) {
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 2500);
  try {
    const response = await fetch(path, {...options, cache:'no-store', signal:controller.signal});
    if (!response.ok) throw new Error('Board returned '+response.status);
    return path === '/api/state' ? await response.json() :
      options.method ? await response.text() : new Uint8Array(await response.arrayBuffer());
  } finally { clearTimeout(timeout); }
}
function redrawBindings() {
  const changed = !!draft && draft.some((v,i)=>v!==saved[i]);
  label('dirty',changed ? 'Unsaved changes' : 'On board');
  byId('save').disabled = !changed || saving || !latest;
  byId('defaults').disabled = !draft || saving;
  byId('export').disabled = !draft || saving;
  byId('import').disabled = !draft || !latest || saving;
  byId('mapping').inert = saving;
  byId('sensors').inert = saving;
  refreshSensorEditor();
  refreshTriggerSources();
  byId('cancel').hidden = !learning;
  const layout = usbLayout(latest);
  for (let action = 1; action < names.length; action++) {
    const physical = [], bits = [];
    if (draft) {
      for (let hand = 0; hand < 2; hand++) {
        for (let bit = 0; bit < 16; bit++) {
          if (draft[bindingIndex(hand,bit)] !== action) continue;
          physical.push(steamButtonName(hand, bit));
          bits.push(sides[hand] + ' bit ' + bit);
        }
      }
      const index = travelActions.indexOf(action);
      if (index >= 0 && draft[travelOffset(index)]) {
        const channel = draft[travelOffset(index)];
        physical.push(learnTarget(action));
        bits.push(sides[Math.floor(index / 3)] + ' travel · 0x14 byte ' + (5 + channel));
      }
    }
    const target = learnTarget(action);
    const empty = draft ? (target ? 'Unbound · learn ' + target : 'Unbound') : 'Waiting for board';
    sourceLines('binding-' + action, physical.length ? physical : [empty]);
    sourceLines('bit-' + action, bits.length ? bits : ['—']);
    const output = outputName(action, layout);
    const alternate = byId('layout-name-' + action);
    alternate.hidden = !latest || layout === 2;
    if (!alternate.hidden) alternate.textContent = usbLayouts[layout] + ' · ' + output;
    const labelName = action === 20 && layout === 2 ? pro3RowTitles[20] : output;
    byId('learn-' + action).setAttribute('aria-label', 'Learn ' + labelName);
    byId('clear-' + action).setAttribute('aria-label', 'Clear ' + labelName);
    label('touch-action-' + action, labelName);
    label('learn-' + action, learning === action ? 'Listening…' : 'Learn');
    byId('learn-' + action).disabled = !draft || !latest || saving;
    byId('clear-' + action).disabled = !bits.length || saving;
  }
  byId('pull-tools').hidden=!learning || !pull;
  byId('pull-confirm').hidden=!pull?.candidates;
  byId('pull-capture').hidden=!!pull?.candidates;
  byId('pull-channel').hidden=!(pull?.candidates?.length>1);
  ['left','right'].forEach((side,h)=>{
    const el=byId('stick-'+side); el.disabled=!draft || saving;
    el.checked=!!(draft && (draft[1] & (1<<h)));
  });
}
function clearAction(action) {
  for(let i=2;i<34;i++) if(draft[i]===action) draft[i]=0;
  const index=travelActions.indexOf(action);
  if(index>=0) draft.fill(0,travelOffset(index),travelOffset(index)+3);
}
function showBindingFeedback(cell) {
  const panel = byId('binding-feedback');
  if (panel.parentElement !== cell) cell.append(panel);
  panel.hidden = false;
  note('');
}
function endBinding(message = '') {
  const panel = byId('binding-feedback');
  if (learning && message && !document.hidden && panel.contains(document.activeElement)) {
    byId('learn-' + learning).focus({preventScroll:true});
  }
  learning = 0;
  pull = null;
  bindingNote(message);
  panel.hidden = !message;
}
function learnFrom(state) {
  if(pull && (!state.up[pull.hand] || state.epoch[pull.hand]!==pull.epoch)) {
    endBinding('Controller disconnected. Start learning again.');redrawBindings();return;
  }
  if(pull?.candidates) return;
  // A reconnect or a button already held when learning starts is not a new press.
  const edges=[];
  for(let h=0;h<2;h++) if(state.up[h] && previousUp[h]) {
    const rising=state.masks[h] & ~previousMasks[h];
    for(let b=0;b<16;b++) if(rising & (1<<b)) edges.push([h,b]);
  }
  previousMasks=state.masks.slice(); previousUp=state.up.slice();
  if(edges.length>1){bindingNote('Several bits changed. Release them and press one button.');return;}
  if(edges.length!==1) return;
  const [h,b]=edges[0], index=bindingIndex(h,b), old=draft[index], action=learning;
  clearAction(action); draft[index]=action;
  const layout = usbLayout(state);
  endBinding(sides[h]+' bit '+b+' → '+outputName(action, layout)+(old && old!==action ? ' (previously '+outputName(old, layout)+')' : '')+'. Save to apply.');
  redrawBindings();
}
const liveRows = [];
let liveProfile, liveLayout;
const liveMasks = {click:'masks', travel:'travel', touch:'active'};

function buildLiveBindings(profile, layout) {
  liveRows.length = 0;
  for (let h = 0; h < 2; ++h) {
    const side = sides[h].toLowerCase();
    const list = byId('live-' + side + '-bindings');
    const empty = byId('live-' + side + '-empty');
    list.replaceChildren();
    function add(kind, index, name, action, offset) {
      const row = document.createElement('li');
      row.className = 'live-binding';
      row.dataset.kind = kind;
      row.dataset.index = String(index);
      const source = document.createElement('span');
      source.className = 'live-source';
      source.textContent = name;
      const target = document.createElement('span');
      target.className = 'live-target';
      target.textContent = outputName(action, layout) + (kind === 'analog' ? ' level' : '');
      const status = document.createElement('span');
      status.className = 'live-status';
      row.append(source, target, status);
      list.append(row);
      const block = offset !== undefined && profile[offset] < 14 ? 1 : 2;
      liveRows.push({row, status, h, kind, index, offset, block});
    }
    for (let bit = 0; bit < 16; ++bit) {
      const action = profile[bindingIndex(h, bit)];
      if (!action) continue;
      const name = steamButtonName(h, bit);
      add('click', bit, name === 'Unknown button' ? name + ' · bit ' + bit : name, action);
    }
    for (let i = 0; i < 3; ++i) {
      const n = h * 3 + i, channel = profile[travelOffset(n)];
      if (channel) {
        const control = ['Trigger travel', 'Grab travel', 'Full grab travel'][i];
        add('travel', i, control + ' · byte ' + (5 + channel), travelActions[n]);
      }
    }
    Touch.slots[h].forEach((surface, slot) => {
      const offset = Touch.offset(h, slot), action = profile[offset + 4];
      if (profile[offset] === Touch.empty) return;
      if (action) add('touch', slot, surface + ' touch', action, offset);
      if (layout !== 2 || profile[0] !== 4) return;
      for (let output = 0; output < 2; ++output) {
        if (profile[BindingBackup.triggerOffset + output] === 1 + h * 12 + slot)
          add('analog', output, surface + ' touch', 16 + output, offset);
      }
    });
    empty.hidden = list.children.length > 0;
    empty.textContent = 'No saved bindings for this hand.';
  }
}

function updateLiveBindings(state) {
  if (!saved || currentView !== 'live') return;
  const layout = state ? usbLayout(state) : liveLayout;
  if (saved !== liveProfile || layout !== liveLayout) {
    buildLiveBindings(saved, layout);
    liveProfile = saved;
    liveLayout = layout;
  }
  for (const {row, status, h, kind, index, offset, block} of liveRows) {
    let style = 'offline', value = 'Offline';
    if (state?.up[h]) {
      style = 'waiting';
      value = 'Waiting for input';
      if (state.seen[h] & block) {
        if (kind === 'analog') {
          // Display only this touch source. A separate digital press can override the USB level.
          const channel = saved[offset];
          const byte = parseInt(state.raw[h].slice(2 * channel, 2 * channel + 2), 16);
          const sample = Touch.value(byte, saved[offset + 1]);
          const release = saved[offset + 2], engage = saved[offset + 3];
          const level = Math.max(0, Math.min(255, Math.round(255 * (sample - release) / (engage - release))));
          style = level ? 'active' : 'released';
          value = level + ' / 255';
        } else {
          const mask = state[liveMasks[kind]]?.[h];
          if (mask !== undefined) {
            const pressed = mask & (1 << index);
            style = pressed ? 'pressed' : 'released';
            value = pressed ? 'Pressed' : 'Released';
          }
        }
      }
    }
    if (row.dataset.state !== style) row.dataset.state = style;
    if (status.textContent !== value) status.textContent = value;
  }
}

function showState(state) {
  label('network','Board connected'); label('usb',state.usb?'USB · connected':'USB · waiting');
  const layout = usbLayouts[usbLayout(state)] || 'Unknown USB layout';
  label('host-check', layout + (state.usb
    ? ' · Host configured the USB gamepad. Check for input in the game or controller settings.'
    : ' · Waiting for an active USB host. Unlock the host and reconnect the native USB/OTG port with a data cable.'));
  for(let h=0;h<2;h++) {
    const key=sides[h].toLowerCase();
    label(key+'-link',state.up[h]?'Connected':'Searching');
    byId(key+'-link').classList.toggle('active',!!state.up[h]);
    label(key+'-mask',state.up[h]?'0x'+state.masks[h].toString(16).padStart(4,'0'):'—');
    label(key+'-count',state.packets[h]);
    label(key+'-xy',state.up[h]?state.axes[h].join(' / '):'—');
  }
  updateSensorState(state);
  updateLiveBindings(state);
  const hasContactLevel = state.up[1] && state.touch >= 0;
  byId('touch').value = hasContactLevel ? state.touch : 0;
  label('touch-text', hasContactLevel ? state.touch+' / 255' : '—');
  const pro3 = usbLayout(state) === 2;
  const outputs=[];
  for(let i=0;i<18;i++) if(state.buttons & (1<<i)) {
    const action = i<11?i+1:i+5;
    const output = outputName(action, usbLayout(state));
    if (!outputs.includes(output)) outputs.push(output);
  }
  if(state.hat<8) outputs.push('D-pad '+hats[state.hat].toLowerCase());
  const levels = pro3 && state.triggers;
  byId('trigger-levels').hidden = !levels;
  if (levels) label('trigger-levels','Left trigger '+levels[0]+' / 255 · Right trigger '+levels[1]+' / 255');
  label('output', outputs.join(' + ') || (levels && levels.some(Boolean) ? 'Analog trigger active' : 'Neutral'));
  byId('diagnostic').hidden=!(state.bad||state.overflows);
  label('diagnostic',state.bad+' malformed packets · '+state.overflows+' USB queue overflows');
}
async function poll() {
  clearTimeout(timer);
  if(document.hidden || busy) return;
  busy=true;
  try {
    if(!latest){
      const previousSaved = saved;
      const p=await request('/api/profile');
      if (!defaultProfile || p[0] !== defaultProfile[0]) defaultProfile=await request('/api/defaults');
      if(!BindingBackup.size(p[0]) || p[0]!==defaultProfile[0] ||
         [p,defaultProfile].some(x=>x.length!==BindingBackup.size(x[0]))) throw Error('Unsupported profile');
      if (saved === previousSaved && (!saved || p.length !== saved.length || p.some((v,i) => v !== saved[i]))) {
        const keepDraft = draft?.length === p.length && draft.some((v,i) => v !== saved[i]);
        saved = p;
        if (!keepDraft) draft = p.slice();
      }
    }
    const state=await request('/api/state'), reconnect=!latest || usbLayout(latest)!==usbLayout(state);
    latest=state;
    if(learning) learnFrom(state);
    showState(state);
    if(reconnect) redrawBindings();
  } catch(e) {
    if(wizard || alternatives.length) stopWizard('Board disconnected. Reconnect and start the capture again.');
    if(learning) endBinding('Board disconnected. Reconnect and choose Learn again.');
    latest=null; previews=[0,0]; previousEpoch=null;
    label('sensor-value','—'); byId('sensor-meter').value=0;
    label('touch-left','Left · offline'); label('touch-right','Right · offline'); label('network','Board disconnected'); label('usb','USB · unknown');
    for(const side of ['left','right']){
      for(const field of ['mask','count','xy','link']) label(side+'-'+field,'—');
      byId(side+'-link').classList.remove('active');
    }
    byId('touch').value=0; label('touch-text','—'); label('output','Waiting for board'); label('host-check','Waiting for board');
    byId('trigger-levels').hidden = true;
    updateLiveBindings(null);
    byId('diagnostic').hidden=true; redrawBindings();
  } finally {
    busy=false;
    if(!document.hidden) timer=setTimeout(poll,latest?150:1200);
  }
}
function openView(view) {
  currentView=view;
  endBinding();
  stopWizard();
  for(const [tab,section] of [['live-tab','live'],['map-tab','mapping'],['sensors-tab','sensors']]){
    byId(section).hidden=view!==section;
    byId(tab).setAttribute('aria-pressed',String(view===section));
  }
  byId('editor').hidden=view==='live';
  updateLiveBindings(latest);
  redrawBindings();
}
names.slice(1).forEach((_, i) => {
  const a = i + 1, row = document.createElement('tr');
  row.dataset.action = String(a);
  const title = document.createElement('th');
  title.scope = 'row';
  const name = document.createElement('strong');
  name.id = 'action-' + a;
  title.setAttribute('aria-labelledby', name.id);
  name.textContent = pro3RowTitles[a] || pro3Name(a);
  const alternate = document.createElement('small');
  alternate.id = 'layout-name-' + a;
  alternate.className = 'binding-layout';
  alternate.hidden = true;
  const controls = document.createElement('div');
  controls.className = 'binding-controls';
  const learn = document.createElement('button'), clear = document.createElement('button');
  learn.id = 'learn-' + a;
  clear.id = 'clear-' + a;
  learn.textContent = 'Learn';
  clear.textContent = 'Clear';
  controls.append(learn, clear);
  title.append(name, alternate, controls);
  const physical = document.createElement('td'), bit = document.createElement('td');
  physical.id = 'binding-' + a;
  bit.id = 'bit-' + a;
  row.append(title, physical, bit);
  learn.onclick=()=>{
    if(!latest) return;
    showBindingFeedback(title);
    learning=a; pull=null;
    previousMasks=latest.masks.slice(); previousUp=latest.up.slice();
    const index=travelActions.indexOf(a), h=Math.floor(index/3);
    if(index>=0 && latest.up[h] && (latest.seen[h]&2)) {
      pull={hand:h,epoch:latest.epoch[h],rest:travelBytes(latest,h)};
    }
    const output = outputName(a, usbLayout(latest));
    const prompt=pull ? 'Hold the '+learnTarget(a)+' press. If no click appears, choose Capture held travel.' :
      'Release other controls, then hold the button for '+output+'.';
    bindingNote(prompt);
    redrawBindings();
  };
  clear.onclick=()=>{
    showBindingFeedback(title);
    clearAction(a);
    const focused = document.activeElement;
    if (!learn.disabled && (focused===clear || byId('binding-feedback').contains(focused))) {
      learn.focus({preventScroll:true});
    }
    endBinding('Binding cleared. Save to apply.');
    redrawBindings();
  };
  byId('bindings').append(row);
});
function travelBytes(state,h) {
  return [14,15].map(i=>parseInt(state.raw[h].slice(i*2,i*2+2),16));
}
byId('pull-capture').onclick=()=>{
  if(!learning || !pull || !latest) return;
  const held=travelBytes(latest,pull.hand);
  const candidates=[];
  held.forEach((v,i)=>{
    const delta=v-pull.rest[i];
    if(Math.abs(delta)>=12) candidates.push({channel:i+1,rest:pull.rest[i],delta});
  });
  if(!candidates.length) {bindingNote('No clear press travel in either field. Hold the physical press and retry.');return;}
  pull.candidates=candidates;
  const select=byId('pull-channel');
  select.replaceChildren();
  candidates.forEach((v,i)=>{
    const opt=document.createElement('option');
    opt.value=String(i);opt.textContent='Byte '+(5+v.channel)+' · '+v.rest+' → '+(v.rest+v.delta);
    select.append(opt);
  });
  select.value='0';
  bindingNote((candidates.length>1?'Two press fields changed. Choose the field you intend to use. ':'')+
    'Release the control, then confirm.');
  const advanceFocus = document.activeElement===byId('pull-capture');
  redrawBindings();
  if(advanceFocus) (candidates.length>1 ? select : byId('pull-confirm')).focus({preventScroll:true});
};
byId('pull-confirm').onclick=()=>{
  if(!learning || !pull?.candidates || !latest) return;
  const c=pull.candidates[Number(byId('pull-channel').value)];
  const current=travelBytes(latest,pull.hand)[c.channel-1];
  if((current-c.rest)*Math.sign(c.delta)>Math.max(4,Math.abs(c.delta)*.2)) {
    bindingNote('Release to the starting position, then confirm.');return;
  }
  clearAction(learning);
  const o=travelOffset(travelActions.indexOf(learning));
  draft.set([c.channel,Math.round(c.rest+.35*c.delta),Math.round(c.rest+.65*c.delta)],o);
  endBinding('Press travel learned. Save to apply.');redrawBindings();
};
names.forEach((name,i)=>{
  const option=document.createElement('option');
  option.id='touch-action-'+i;
  option.value=String(i); option.textContent=name || 'Off';
  byId('sensor-action').append(option);
});
triggerSelectors.forEach((id, side) => {
  byId(id).onchange = e => {
    if (saving || usbLayout(latest) !== 2 || draft?.[0] !== 4) return;
    draft[BindingBackup.triggerOffset + side] = Number(e.target.value);
    note('Trigger source changed. Save to apply.');
    redrawBindings();
  };
});
byId('cancel').onclick=()=>{endBinding('Learning canceled.');redrawBindings();};
byId('defaults').onclick=()=>{draft=defaultProfile.slice();previews=[0,0];stopWizard('Defaults staged. Save to apply.');
  endBinding();note('Defaults restored in this view. Save to apply.');redrawBindings();};
byId('export').onclick = () => {
  const json = JSON.stringify(BindingBackup.encode(draft), null, 2);
  const url = URL.createObjectURL(new Blob([json], {type:'application/json'}));
  const link = document.createElement('a');
  link.href = url;
  link.download = 'FrameBridge-bindings.json';
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url), 30000);
};
byId('import').onclick = () => byId('backup-file').click();
byId('backup-file').onchange = async event => {
  const file = event.target.files[0];
  event.target.value = '';
  if (!file || saving) return;
  saving = true;
  endBinding();
  stopWizard();
  redrawBindings();
  try {
    if (file.size > 65536) throw Error('This file is too large for a bindings backup.');
    const imported = BindingBackup.decode(JSON.parse(await file.text()), saved[0]);
    await request('/api/profile', {method:'POST', headers:{'Content-Type':'application/octet-stream'}, body:imported});
    draft = imported;
    previews = [0,0];
    note('Bindings imported. Choose Save to board to apply them.');
  } catch (e) {
    note('Import failed. ' + e.message);
  } finally {
    saving = false;
    redrawBindings();
  }
};
byId('save').onclick=async()=>{
  saving=true;endBinding();stopWizard();redrawBindings();
  const submitted=draft.slice();
  try{await request('/api/profile',{method:'PUT',headers:{'Content-Type':'application/octet-stream'},body:submitted});
    saved=submitted;updateLiveBindings(latest);note('Saved to board.');}
  catch(e){note('Save could not be confirmed. Reconnect and retry.');}
  finally{saving=false;redrawBindings();}
};
['left','right'].forEach((s,h)=>{byId('stick-'+s).onchange=e=>{
  draft[1]=(draft[1]&~(1<<h))|(Number(e.target.checked)<<h);redrawBindings();};});
byId('live-tab').onclick=()=>openView('live');
byId('map-tab').onclick=()=>openView('mapping');
byId('sensors-tab').onclick=()=>openView('sensors');
document.addEventListener('visibilitychange',()=>{
  clearTimeout(timer);
  if(document.hidden && learning) endBinding('Learning paused. Choose Learn to try again.');
  if(document.hidden && wizard) stopWizard('Capture paused. Start again when ready.');
  redrawBindings();if(!document.hidden) poll();
});
let hand=1, slot=0, wizard=null, alternatives=[], previews=[0,0], previousEpoch=null, currentView='live';
const instructions=[
  'Lift off the selected surface or straighten just this finger.',
  'Rest the selected finger or touch the surface lightly.',
  'Lift off again and keep the other fingers still.',
  'Touch the same surface a second time.',
  'Lift off once more.'
];
const bindOffset=()=>Touch.offset(hand,slot);
const sensorNote=m=>label('sensor-note',m);
const surfaceId=(side,index)=>'surface-'+side+'-'+index;
const contactStates={waiting:'Waiting for input',offline:'Offline',unlearned:'',released:'Released',touched:'Touched'};
let lastTouchLayout;

function createSurfaceLists() {
  Touch.slots.forEach((slots,side)=>{
    const list=byId('touch-'+sides[side].toLowerCase()+'-list');
    slots.forEach((name,index)=>{
      const item=document.createElement('li'), button=document.createElement('button');
      button.id=surfaceId(side,index);
      button.type='button';
      button.className='touch-row';
      button.setAttribute('aria-controls','sensor-editor');
      for(const field of ['name','state','output','signal']){
        const detail=document.createElement(field==='name'?'strong':'span');
        detail.className='touch-'+field;
        detail.id=button.id+'-'+field;
        if(field==='name') detail.textContent=name;
        button.append(detail);
      }
      button.onclick=()=>{
        hand=side; slot=index;
        alternatives=[]; sensorNote('');
        redrawBindings();
        if(latest) updateSensorState(latest);
        byId('sensor-heading').focus();
      };
      item.append(button); list.append(item);
    });
  });
}

function setSurfaceState(id,status) {
  const row=byId(id);
  if(row.dataset.state===status) return;
  row.dataset.state=status;
  label(id+'-state',contactStates[status]);
}

function refreshSurfaceLists() {
  if(latest) lastTouchLayout=usbLayout(latest);
  Touch.slots.forEach((slots,side)=>slots.forEach((name,index)=>{
    const id=surfaceId(side,index), row=byId(id), offset=Touch.offset(side,index);
    const calibrated=!!draft && draft[offset]!==Touch.empty;
    row.disabled=saving || !!wizard;
    row.setAttribute('aria-pressed',String(side===hand && index===slot));
    if(!draft){
      label(id+'-output','');
      label(id+'-signal','Waiting for profile');
    }else{
      const action=draft[offset+4], outputs=[];
      if(action) outputs.push(lastTouchLayout===undefined ? names[action] : outputName(action,lastTouchLayout));
      if(draft[0]===4){
        const source=1+side*12+index;
        for(let trigger=0;trigger<2;trigger++){
          if(draft[BindingBackup.triggerOffset+trigger]===source)
            outputs.push(outputName(16+trigger,2)+(lastTouchLayout===2?' (analog)':' (Pro 3 analog)'));
        }
      }
      label(id+'-output', outputs.length ? 'Output · '+outputs.join(' + ') : calibrated ? 'No output assigned' : '');
      const signal=calibrated ? Touch.source(draft[offset],draft[offset+1])+
        (draft[offset+1]>=3 ? ' · on/off' : ' · engage '+draft[offset+3]+' / release '+draft[offset+2])
        : 'Uncalibrated';
      label(id+'-signal',signal);
    }
    if(!latest) setSurfaceState(id,draft?'offline':'waiting');
    else if(!latest.up[side]) setSurfaceState(id,'offline');
    else if(!calibrated) setSurfaceState(id,'unlearned');
    else if(!(latest.seen[side] & (draft[offset]<14?1:2))) setSurfaceState(id,'waiting');
    else setSurfaceState(id,previews[side] & (1<<index)?'touched':'released');
  }));
}
function refreshSensorEditor() {
  const o=bindOffset(), has=!!draft && draft[o]!==Touch.empty;
  const locked=!draft || saving || !!wizard;
  refreshSurfaceLists();
  label('sensor-heading',sides[hand]+' hand · '+Touch.slots[hand][slot]);
  byId('surface-back').disabled=saving || !!wizard;
  byId('sensor-learn').disabled=locked || !latest?.up[hand];
  byId('sensor-clear').disabled=locked || !has;
  byId('sensor-action').disabled=locked || !has;
  byId('sensor-action').value=String(draft?.[o+4] || 0);
  for(const [id,index] of [['release',2],['engage',3]]){
    const el=byId(id);
    el.disabled=locked || !has || draft[o+1]>=3;
    if(document.activeElement!==el) el.value=has?String(draft[o+index]):'';
  }
  byId('dpad-hint').hidden=hand!==0 || slot!==0;
  label('sensor-source',has ? Touch.source(draft[o],draft[o+1])+
    (draft[o+1]>=3?' · on/off signal':' · adjustable sensitivity') : 'No learned signal');
  byId('wizard').hidden=!wizard;
  byId('candidates').hidden=!alternatives.length;
  byId('use-candidate').disabled=locked || !latest?.up[hand];
  byId('save').disabled ||= !!wizard;
  byId('defaults').disabled ||= !!wizard;
  if(!wizard) return;
  const n=wizard.phases.length;
  label('wizard-prompt',(n+1)+' of 5 · '+instructions[n]+(wizard.samples?' Hold still.':' Choose Capture when ready.'));
  byId('capture').disabled=!!wizard.samples;
}
function stopWizard(message) {
  wizard=null; alternatives=[];
  if(message) sensorNote(message);
}
function currentSamples(state,h) {
  const hex=state.raw[h];
  return {seen:state.seen[h],raw:Array.from({length:18},(_,i)=>parseInt(hex.slice(2*i,2*i+2),16))};
}
function sampleGesture(state,observation) {
  if(!wizard) return;
  if(!state.up[hand] || state.epoch[hand]!==wizard.epoch) {
    stopWizard('Controller reconnected or went offline. Start the capture again.');
    redrawBindings();return;
  }
  if(!wizard.samples) return;
  if(Date.now()>wizard.deadline) {
    stopWizard('No fresh controller data. Wake the controller and retry.');
    redrawBindings();return;
  }
  if(state.packets[hand]===wizard.packet) return;
  wizard.packet=state.packets[hand];
  wizard.samples.push(observation);
  if(wizard.samples.length<5) return;
  wizard.phases.push(wizard.samples);
  wizard.samples=null;
  if(wizard.phases.length===instructions.length) {
    const found=Touch.learn(wizard.phases,hand,slot,draft);
    stopWizard();
    if(found.error) {
      sensorNote(found.error);
      alternatives=found.choices || [];
      const list=byId('candidate-list');
      list.replaceChildren();
      alternatives.forEach((v,i)=>{
        const option=document.createElement('option');
        option.value=String(i);option.textContent=Touch.source(v.channel,v.mode);list.append(option);
      });
      list.value='0';
    } else stageSensor(found);
  }
  redrawBindings();
}
function stageSensor(found) {
  draft.set([found.channel,found.mode,found.release,found.engage],bindOffset());
  previews[hand]&=~(1<<slot); alternatives=[];
  sensorNote('Touch learned. Try touching and lifting, then save to the board.');
  note('Touch calibration has unsaved changes.');
}
byId('use-candidate').onclick=()=>{
  const chosen=alternatives[Number(byId('candidate-list').value)];
  if(chosen && latest?.up[hand]) {stageSensor(chosen);redrawBindings();}
};
function updateSensorState(state) {
  if(!draft || currentView!=='sensors') return;
  const frames=[currentSamples(state,0),currentSamples(state,1)];
  if(alternatives.length && (!state.up[hand] || previousEpoch?.[hand]!==state.epoch[hand])) {
    stopWizard('Controller connection changed. Start the capture again.');
    redrawBindings();
  }
  sampleGesture(state,frames[hand]);
  const candidate=alternatives[Number(byId('candidate-list').value)];
  if(candidate) label('candidate-value','Live level · '+Touch.value(frames[hand].raw[candidate.channel],candidate.mode)+' / 255');
  for(let h=0;h<2;h++) {
    if(!state.up[h] || previousEpoch?.[h]!==state.epoch[h]) previews[h]=0;
    let activeCount=0;
    for(let i=0;i<Touch.slots[h].length;i++) {
      const o=Touch.offset(h,i), c=draft[o];
      const available=!!state.up[h] && c!==Touch.empty && (frames[h].seen & (c<14?1:2));
      const sameCalibration=available && draft[o]===saved[o] && draft[o+1]===saved[o+1] &&
        draft[o+2]===saved[o+2] && draft[o+3]===saved[o+3];
      const touched=available && (sameCalibration ? state.active[h] & (1<<i) :
        Touch.latch(frames[h].raw[c],draft[o+1],draft[o+2],draft[o+3],previews[h] & (1<<i)));
      if(touched){
        previews[h]|=1<<i; activeCount++;
      }else previews[h]&=~(1<<i);
      const status=!state.up[h]?'offline':c===Touch.empty?'unlearned':
        !available?'waiting':touched?'touched':'released';
      setSurfaceState(surfaceId(h,i),status);
      if(h===hand && i===slot) {
        const v=available ? Touch.value(frames[h].raw[c],draft[o+1]) : 0;
        byId('sensor-meter').value=v;
        label('sensor-value',available?v+' / 255 · '+(previews[h]&(1<<i)?'Touch':'Released'):'—');
      }
    }
    label('touch-'+sides[h].toLowerCase(),state.up[h] ? activeCount+' touched' : 'Offline');
  }
  const linkChanged=previousEpoch?.[hand]!==state.epoch[hand];
  previousEpoch=state.epoch.slice();
  if(linkChanged) refreshSensorEditor();
}
byId('surface-back').onclick=()=>byId(surfaceId(hand,slot)).focus();
byId('sensor-learn').onclick=()=>{
  if(!latest?.up[hand]) return;
  alternatives=[]; wizard={phases:[],samples:null,epoch:latest.epoch[hand]};
  sensorNote('');redrawBindings();
};
byId('capture').onclick=()=>{
  if(!wizard || !latest || wizard.samples) return;
  wizard.packet=latest.packets[hand];wizard.deadline=Date.now()+6000;wizard.samples=[];
  refreshSensorEditor();
};
byId('stop-capture').onclick=()=>{stopWizard('Capture canceled.');redrawBindings();};
byId('sensor-clear').onclick=()=>{
  alternatives=[]; draft.set([Touch.empty,0,0,0,0],bindOffset());previews[hand]&=~(1<<slot);
  const removed = [];
  if (draft[0] === 4) {
    const source = 1 + hand * 12 + slot;
    triggerSelectors.forEach((_, side) => {
      const offset = BindingBackup.triggerOffset + side;
      if (draft[offset] === source) {
        draft[offset] = 0;
        removed.push(sides[side].toLowerCase()+' trigger');
      }
    });
  }
  const message = removed.length ? ' Cleared touch routing to '+removed.join(' and ')+'.' : '';
  sensorNote('Calibration cleared.'+message+' Save to apply.');redrawBindings();
};
for(const [id,index] of [['release',2],['engage',3]]) {
  byId(id).onchange=e=>{
    const v=e.target.valueAsNumber, o=bindOffset(), other=draft[o+(index===2?3:2)];
    const rising=draft[o+3]>draft[o+2];
    if(!Number.isInteger(v) || v<0 || v>255 || (index===2 ? (rising?v>=other:v<=other) : (rising?v<=other:v>=other))) {
      e.target.value=String(draft[o+index]);sensorNote('Keep thresholds between 0 and 255 and preserve the learned direction.');return;
    }
    draft[o+index]=v;previews[hand]&=~(1<<slot);
    sensorNote('Sensitivity changed. Save to apply.');redrawBindings();
  };
}
byId('sensor-action').onchange=e=>{draft[bindOffset()+4]=Number(e.target.value);redrawBindings();};
createSurfaceLists();redrawBindings();poll();
