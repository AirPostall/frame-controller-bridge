// Fields are normalized to 0..255 so the same profile works for bytes, nibbles and bits.
const Touch = (() => {
  const slots = [
    ['D-pad','Grab','Trigger','Joystick','Steam','View','Bumper','Grip · ring','Grip · little'],
    ['A','B','X','Y','Grab','Trigger','Joystick','Steam','Menu','Bumper','Grip · ring','Grip · little']
  ];
  const size = 174, empty = 255;
  const offset = (hand, slot) => 34 + (hand * 12 + slot) * 5;
  const mask = mode => [255, 15, 240][mode] ?? (1 << (mode - 3));
  function value(raw, mode) {
    if (mode === 0) return raw;
    if (mode === 1) return (raw & 15) * 17;
    if (mode === 2) return (raw >> 4) * 17;
    return raw & mask(mode) ? 255 : 0;
  }
  function source(channel, mode) {
    const block=channel<14?'0x19':'0x14';
    const byte=channel<14?channel:[6,7,0,1][channel-14];
    const part=mode===0?'':mode===1?' low nibble':mode===2?' high nibble':' bit '+(mode-3);
    return block+' byte '+byte+part;
  }
  function overlap(a,b) { return a.channel===b.channel && !!(mask(a.mode)&mask(b.mode)); }

  // Each phase has several observations from a steady pose. Use the worst separation
  // across both repetitions so a one-off movement cannot win the calibration.
  function candidate(phases, channel, mode) {
    const fields=phases.map(samples=>samples.map(s=>value(s.raw[channel],mode)));
    const lo=fields.filter((_,i)=>i%2===0).flat(), hi=fields.filter((_,i)=>i%2===1).flat();
    const a=Math.min(...lo), b=Math.max(...lo), c=Math.min(...hi), d=Math.max(...hi);
    const increasing=c>b, gap=increasing?c-b:a-d;
    const spread=Math.max(b-a,d-c);
    if(gap<8 || gap<spread*.7) return null;
    const start=increasing?b:a, end=increasing?c:d;
    // Favor continuous channels when their readings are reliable. A single changing
    // bit on an otherwise fixed byte is handled directly below.
    const score=(gap/(gap+spread*.55+4)) * (0.65+0.35*Math.min(gap/64,1)) *
      (mode===0?1:mode<=2 ? .96 : .72);
    return {channel,mode,score,release:Math.round(start+.30*(end-start)),
      engage:Math.round(start+.60*(end-start))};
  }
  function learn(phases, hand, slot, profile) {
    const candidates=[];
    for(let channel=0;channel<18;channel++) {
      if(phases.some(phase=>!phase.length || phase.some(s=>!(s.seen & (channel<14?1:2))))) continue;
      const all=phases.flat().map(s=>s.raw[channel]);
      const variation=all.reduce((m,v)=>m|(v^all[0]),0);
      if(!variation) continue;
      const singleBit=(variation & (variation-1))===0;
      for(let mode=0;mode<=10;mode++) {
        if(!(mask(mode)&variation)) continue;
        if(singleBit && mode<3) continue;
        if(!singleBit && mode===0 && (!(variation & 15) || !(variation & 240))) continue;
        // The button mask also contains click bits. Exclude known clicks and
        // every bit already assigned in the user's digital profile.
        if(channel>=16) {
          if(mode<3) continue;
          const bit=8*(channel-16)+mode-3;
          if(defaultClick(hand,bit) || profile[2+hand*16+bit]) continue;
        }
        const item=candidate(phases,channel,mode);
        if(item) candidates.push(item);
      }
    }
    candidates.sort((a,b)=>b.score-a.score);
    const best=candidates[0];
    if(!best || best.score<.43) return {error:'No repeatable signal. Try again with the controller still. This surface may not report touch.'};
    const nearby=candidates.filter(c=>c.score>=best.score*.88);
    const matches=c=> {
      for(let i=0;i<slots[hand].length;i++) {
        const o=offset(hand,i);
        if(i!==slot && overlap(c,{channel:profile[o],mode:profile[o+1]})) return slots[hand][i];
      }
      return '';
    };
    // A previous calibration (including right A) can disambiguate a mirrored field.
    const current=offset(hand,slot);
    const existing=nearby.find(c=>c.channel===profile[current] && c.mode===profile[current+1]);
    if(existing && !matches(existing)) return existing;
    const distinct=[];
    for(const c of nearby) if(!distinct.some(d=>overlap(c,d))) distinct.push(c);
    if(distinct.length>1) {
      const choices=distinct.filter(c=>!matches(c));
      return {error:'Multiple signals followed that gesture. Retry with other fingers still, or preview a candidate and choose it explicitly.',choices};
    }
    const used=matches(best);
    if(used) return {error:'That signal is already assigned to '+used+'. Isolate the two controls and try again.'};
    return best;
  }
  function defaultClick(hand, bit) {
    return bit<=3 || bit===5 || bit===6 || bit===9 || bit===8;
  }
  function latch(raw, mode, off, on, active) {
    const v=value(raw,mode);
    if(on>off) return v>=on || (active && v>off);
    return v<=on || (active && v<off);
  }
  return {slots,size,empty,offset,mask,value,source,learn,latch};
})();
