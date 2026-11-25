'use strict';
const byId = id => document.getElementById(id);
const names = ['', 'A', 'B', 'X', 'Y', 'L bumper', 'R bumper', 'Select', 'Start',
  'L stick click', 'R stick click', 'Home', 'D-pad up', 'D-pad right', 'D-pad down', 'D-pad left',
  'L trigger', 'R trigger', 'L grab', 'R grab', 'Right Steam', 'L grab full click', 'R grab full click'];
const travelActions = [16,18,21,17,19,22]; // by hand, then trigger/grab/full grab
const pro3Output = {18:'PL', 19:'PR', 20:'Home', 21:'L4', 22:'R4'};
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
const bindingIndex = (side, bit) => 2 + side * 16 + bit;
const usbLayout = state => state?.usbLayout ?? (state?.standardUsb ? 1 : 0);

function bindingName(action) {
  const mapped = usbLayout(latest) === 2 && pro3Output[action];
  return mapped ? names[action] + ' → ' + mapped : names[action];
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
  for(let action=1; action<names.length; action++) {
    const bits=[];
    if(draft) for(let h=0;h<2;h++) for(let b=0;b<16;b++)
      if(draft[bindingIndex(h,b)]===action) bits.push(physicalName(h,b));
    const index=travelActions.indexOf(action);
    if(draft && index>=0){
      const o=travelOffset(index);
      if(draft[o]) bits.push(sides[Math.floor(index/3)]+' travel · byte '+(5+draft[o]));
    }
    label('binding-'+action,bits.join(', ') || 'Unbound');
    const name = bindingName(action);
    label('action-'+action,name);
    byId('learn-'+action).setAttribute('aria-label','Learn '+name);
    byId('clear-'+action).setAttribute('aria-label','Clear '+name);
    label('touch-action-'+action,name);
    label('learn-'+action,learning===action ? 'Listening…' : 'Learn');
    byId('learn-'+action).disabled = !draft || !latest || saving;
    byId('clear-'+action).disabled = !bits.length || saving;
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
function learnFrom(state) {
  if(pull && (!state.up[pull.hand] || state.epoch[pull.hand]!==pull.epoch)) {
    pull=null; learning=0;note('Controller disconnected. Start learning again.');redrawBindings();return;
  }
  if(pull?.candidates) return;
  // A reconnect or a button already held when learning starts is not a new press.
  const edges=[];
  for(let h=0;h<2;h++) if(state.up[h] && previousUp[h]) {
    const rising=state.masks[h] & ~previousMasks[h];
    for(let b=0;b<16;b++) if(rising & (1<<b)) edges.push([h,b]);
  }
  previousMasks=state.masks.slice(); previousUp=state.up.slice();
  if(edges.length>1){note('Several bits changed. Release them and press one button.');return;}
  if(edges.length!==1) return;
  const [h,b]=edges[0], index=bindingIndex(h,b), old=draft[index], action=learning;
  clearAction(action); draft[index]=action; learning=0;
  note(sides[h]+' bit '+b+' → '+names[action]+(old && old!==action ? ' (previously '+names[old]+')' : '')+'. Save to apply.');
  redrawBindings();
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
  const hasContactLevel = state.up[1] && state.touch >= 0;
  byId('touch').value = hasContactLevel ? state.touch : 0;
  label('touch-text', hasContactLevel ? state.touch+' / 255' : '—');
  const pro3 = usbLayout(state) === 2;
  const outputs=[];
  for(let i=0;i<18;i++) if(state.buttons & (1<<i)) {
    const action = i<11?i+1:i+5;
    const output = pro3 && pro3Output[action] || names[action];
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
    if(!draft){
      const p=await request('/api/profile');
      defaultProfile=await request('/api/defaults');
      if(!BindingBackup.size(p[0]) || p[0]!==defaultProfile[0] ||
         [p,defaultProfile].some(x=>x.length!==BindingBackup.size(x[0]))) throw Error('Unsupported profile');
      saved=p; draft=p.slice();
    }
    const state=await request('/api/state'), reconnect=!latest || usbLayout(latest)!==usbLayout(state);
    latest=state;
    if(learning) learnFrom(state);
    showState(state);
    if(reconnect) redrawBindings();
  } catch(e) {
    if(wizard || alternatives.length) stopWizard('Board disconnected. Reconnect and start the capture again.');
    latest=null; previews=[0,0]; previousEpoch=null; learning=0;
    label('sensor-value','—'); byId('sensor-meter').value=0;
    label('touch-left','Left · offline'); label('touch-right','Right · offline'); label('network','Board disconnected'); label('usb','USB · unknown');
    for(const side of ['left','right']){
      for(const field of ['mask','count','xy','link']) label(side+'-'+field,'—');
      byId(side+'-link').classList.remove('active');
    }
    byId('touch').value=0; label('touch-text','—'); label('output','Waiting for board'); label('host-check','Waiting for board');
    byId('trigger-levels').hidden = true;
    byId('diagnostic').hidden=true; redrawBindings();
  } finally {
    busy=false;
    if(!document.hidden) timer=setTimeout(poll,latest?150:1200);
  }
}
function openView(view) {
  currentView=view;
  learning=0;
  stopWizard();
  for(const [tab,section] of [['live-tab','live'],['map-tab','mapping'],['sensors-tab','sensors']]){
    byId(section).hidden=view!==section;
    byId(tab).setAttribute('aria-pressed',String(view===section));
  }
  byId('editor').hidden=view==='live';
  redrawBindings();
}
function physicalName(hand,bit) {
  const special=bit===9?(hand===0?'View':'Menu'):bit===8 && hand===0?'Steam':'';
  return sides[hand]+(special?' '+special+' ·':'')+' bit '+bit;
}
names.slice(1).forEach((name,i)=>{
  const a=i+1,row=document.createElement('div');
  row.innerHTML='<strong></strong><code></code><button>Learn</button><button>Clear</button>';
  const [title, value, learn, clear]=row.children;
  title.id='action-'+a; title.textContent=name; value.id='binding-'+a; learn.id='learn-'+a; clear.id='clear-'+a;
  learn.setAttribute('aria-label','Learn '+name); clear.setAttribute('aria-label','Clear '+name);
  learn.onclick=()=>{
    if(!latest) return;
    learning=a; pull=null;
    previousMasks=latest.masks.slice(); previousUp=latest.up.slice();
    const index=travelActions.indexOf(a), h=Math.floor(index/3);
    if(index>=0 && latest.up[h] && (latest.seen[h]&2)) {
      pull={hand:h,epoch:latest.epoch[h],rest:travelBytes(latest,h)};
    }
    const prompt=index>=0 ? 'Hold the '+name+' press. If no click appears, capture the held travel below.' :
      'Release other controls, then hold the button for '+name+'.';
    note(prompt);
    redrawBindings();
    byId('cancel').scrollIntoView?.({block:'nearest'});
  };
  clear.onclick=()=>{clearAction(a);learning=0;note('Binding cleared. Save to apply.');redrawBindings();};
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
  if(!candidates.length) {note('No clear press travel in either field. Hold the physical press and retry.');return;}
  pull.candidates=candidates;
  const select=byId('pull-channel');
  select.replaceChildren();
  candidates.forEach((v,i)=>{
    const opt=document.createElement('option');
    opt.value=String(i);opt.textContent='Byte '+(5+v.channel)+' · '+v.rest+' → '+(v.rest+v.delta);
    select.append(opt);
  });
  select.value='0';
  note((candidates.length>1?'Two press fields changed. Choose the field you intend to use. ':'')+
    'Release the control, then confirm.');
  redrawBindings();
};
byId('pull-confirm').onclick=()=>{
  if(!learning || !pull?.candidates || !latest) return;
  const c=pull.candidates[Number(byId('pull-channel').value)];
  const current=travelBytes(latest,pull.hand)[c.channel-1];
  if((current-c.rest)*Math.sign(c.delta)>Math.max(4,Math.abs(c.delta)*.2)) {
    note('Release to the starting position, then confirm.');return;
  }
  clearAction(learning);
  const o=travelOffset(travelActions.indexOf(learning));
  draft.set([c.channel,Math.round(c.rest+.35*c.delta),Math.round(c.rest+.65*c.delta)],o);
  learning=0;pull=null;note('Press travel learned. Save to apply.');redrawBindings();
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
byId('cancel').onclick=()=>{learning=0;note('Learning canceled.');redrawBindings();};
byId('defaults').onclick=()=>{draft=defaultProfile.slice();previews=[0,0];stopWizard('Defaults staged. Save to apply.');
  learning=0;note('Defaults restored in this view. Save to apply.');redrawBindings();};
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
  learning = 0;
  pull = null;
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
  saving=true;learning=0;stopWizard();redrawBindings();
  const submitted=draft.slice();
  try{await request('/api/profile',{method:'PUT',headers:{'Content-Type':'application/octet-stream'},body:submitted});
    saved=submitted;note('Saved to board.');}
  catch(e){note('Save could not be confirmed. Reconnect and retry.');}
  finally{saving=false;redrawBindings();}
};
['left','right'].forEach((s,h)=>{byId('stick-'+s).onchange=e=>{
  draft[1]=(draft[1]&~(1<<h))|(Number(e.target.checked)<<h);redrawBindings();};});
byId('live-tab').onclick=()=>openView('live');
byId('map-tab').onclick=()=>openView('mapping');
byId('sensors-tab').onclick=()=>openView('sensors');
document.addEventListener('visibilitychange',()=>{
  clearTimeout(timer);learning=0;
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
function fillSlots() {
  const select=byId('sensor-slot');
  select.replaceChildren();
  Touch.slots[hand].forEach((name,i)=>{
    const option=document.createElement('option');
    option.value=String(i); option.textContent=name; select.append(option);
  });
  select.value=String(slot);
}
function refreshSensorEditor() {
  const o=bindOffset(), has=!!draft && draft[o]!==Touch.empty;
  const locked=!draft || saving || !!wizard;
  byId('sensor-hand').disabled=byId('sensor-slot').disabled=saving || !!wizard;
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
    const active=[];
    for(let i=0;i<Touch.slots[h].length;i++) {
      const o=Touch.offset(h,i), c=draft[o];
      const available=!!state.up[h] && c!==Touch.empty && (frames[h].seen & (c<14?1:2));
      const sameCalibration=available && draft[o]===saved[o] && draft[o+1]===saved[o+1] &&
        draft[o+2]===saved[o+2] && draft[o+3]===saved[o+3];
      const touched=available && (sameCalibration ? state.active[h] & (1<<i) :
        Touch.latch(frames[h].raw[c],draft[o+1],draft[o+2],draft[o+3],previews[h] & (1<<i)));
      if(touched){
        previews[h]|=1<<i; active.push(Touch.slots[h][i]);
      }else previews[h]&=~(1<<i);
      if(h===hand && i===slot) {
        const v=available ? Touch.value(frames[h].raw[c],draft[o+1]) : 0;
        byId('sensor-meter').value=v;
        label('sensor-value',available?v+' / 255 · '+(previews[h]&(1<<i)?'Touch':'Released'):'—');
      }
    }
    label('touch-'+sides[h].toLowerCase(),sides[h]+' · '+(state.up[h] ? active.join(', ') || 'No contact' : 'Offline'));
  }
  const linkChanged=previousEpoch?.[hand]!==state.epoch[hand];
  previousEpoch=state.epoch.slice();
  if(linkChanged) refreshSensorEditor();
}
function selectSensor() {
  alternatives=[]; sensorNote(''); redrawBindings();
  if(latest) updateSensorState(latest);
}
byId('sensor-hand').onchange=e=>{hand=Number(e.target.value);slot=0;fillSlots();selectSensor();};
byId('sensor-slot').onchange=e=>{slot=Number(e.target.value);selectSensor();};
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
fillSlots();redrawBindings();poll();
