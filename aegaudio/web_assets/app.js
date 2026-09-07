'use strict';
const $ = selector => document.querySelector(selector);
const esc = value => String(value ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
let DATA = null, PAGE = 'sbx', BUSY = false, REQUEST = 0, PROFILE = null;
let READING = 0, POINTER_ACTIVE = false, KEY_ACTIVE = false, CONNECTED = false, SYNC_TIMER = null;
function editingControl() {
  return POINTER_ACTIVE || KEY_ACTIVE || $('#confirm').open || !!document.querySelector('[data-control][data-dirty="true"]') ||
    !!document.activeElement?.matches('#page input[type="number"], #page select');
}
function stateSignature(data) { return JSON.stringify([data.card,data.controls,data.state]); }
const titles = {playback:'Playback',sbx:'SBX profile',equalizer:'Equalizer',recording:'Recording',mixer:'Mixer',profiles:'Saved profiles',history:'Listening history',status:'Device status',features:'Feature support'};
const intros = {
  playback:'Your output, listening level, and DAC settings.',
  sbx:'Tune the AE-5 acoustic engine using the controls exposed by its Linux driver.',
  equalizer:'Shape your sound with the driver’s ten-band equalizer and built-in presets.',
  recording:'AE-5 input selection, capture levels, and voice processing.',
  mixer:'Independent channel levels and switches on the AE-5.',
  profiles:'Keep your own settings. Review changes before applying a profile.',
  history:'Driver readbacks and your listening observations, kept together.',
  status:'Read the active PCM format and the hardware amplifier mute bits.',
  features:'What this independent Linux interface can control today.'
};
const descriptions = {
  'Surround':'Adjust the width of the virtual sound field.',
  'Crystalizer':'Adjust the processing applied to transients and detail.',
  'X-Bass':'Set bass processing strength and crossover.',
  'Smart Volume':'Manage differences between quiet and loud passages.',
  'Dialog Plus':'Adjust voice emphasis in playback.'
};
function message(text, error=false) { const node=$('#message');node.textContent=text;node.hidden=!text;node.classList.toggle('error',error); }
async function api(path, body) {
  const options = body === undefined ? {} : {method:'POST',headers:{'Content-Type':'application/json','X-AegAudio-Token':DATA.csrf},body:JSON.stringify({...body,device_path:DATA.card.device_path})};
  const response = await fetch(path, {...options,signal:AbortSignal.timeout(25000)});
  const data = await response.json();
  if (!response.ok) throw new Error(data.error || `Request failed (${response.status})`);
  return data;
}
function find(name) { return DATA.controls.find(c => c.name === name); }
const DAC_VOLUME='AE-5: Direct DAC Playback Volume';
function editorValue(c,value) {
  return c.name===DAC_VOLUME ? c.db_min+(Number(value)-c.min)*c.db_step : value;
}
function controlValue(c,value) {
  return c.name===DAC_VOLUME ? String((Number(value)-c.db_min)/c.db_step+c.min) : value;
}
function valueLabel(c) {
  if (!c) return 'Unavailable';
  if (c.type === 'ENUMERATED') return c.values.map(v => c.items[Number(v)] ?? v).join(' / ');
  if (c.type === 'BOOLEAN') return c.values.every(v => v === 'on') ? 'On' : c.values.every(v => v === 'off') ? 'Off' : 'Mixed';
  return c.values.join(' / ');
}
function pretty(c) {
  return c.name.replace(/^AE-5: /,'').replace(/^FX: /,'').replace(/ Playback (Volume|Switch)$/,'').replace(/ Capture (Volume|Switch)$/,' capture').replace('Enable OutFX','Acoustic engine').replace('Enable InFX capture','Voice processing').replace('X-Bass','Bass').replace('Dialog Plus','Dialog+');
}
function level(c,v) {
  if(c.name===DAC_VOLUME)return `${Number(editorValue(c,v)).toFixed(1)} dB`;
  const db = c.db_min === undefined ? '' : ` · ${(c.db_min+(Number(v)-c.min)*c.db_step).toFixed(1)} dB`;
  return `${v}${db}`;
}
function control(c, {compact=false}={}) {
  if (!c) return '';
  const disabled = c.locked ? 'disabled' : '';
  const editors = c.values.map((v,i) => {
    const id=`c${c.numid}-${compact?'small':'full'}-${i}`;
    const channel=c.count === 2 ? ['L','R'][i] : c.count > 1 ? String(i+1) : '';
    const label=`${c.name}${channel?' '+channel:''}`;
    if(c.type === 'BOOLEAN') return `<div class="channel"><label class="check-label" for="${id}"><input id="${id}" type="checkbox" data-i="${i}" ${v==='on'?'checked':''} ${disabled}><span>${channel?`${channel} · `:''}Enabled</span></label></div>`;
    if(c.type === 'ENUMERATED') return `<div class="channel"><span class="channel-label">${channel}</span><label class="sr-only" for="${id}">${esc(label)}</label><select id="${id}" data-i="${i}" ${disabled}>${c.items.map((item,n)=>`<option value="${n}" ${String(n)===v?'selected':''}>${esc(item)}</option>`).join('')}</select></div>`;
    if(c.type === 'INTEGER' && c.iface==='MIXER') {
      const outside=Number(v)<c.min||Number(v)>c.max;
      const lo=editorValue(c,c.min),hi=editorValue(c,c.max),shown=editorValue(c,v),step=c.name===DAC_VOLUME?c.db_step:(c.step||1);
      return `<div class="channel"><span class="channel-label">${channel}</span><label class="sr-only" for="${id}">${esc(label)}</label><input id="${id}" type="range" min="${lo}" max="${hi}" step="${step}" value="${shown}" data-i="${i}" ${disabled || (outside?'disabled':'')}><input type="number" aria-label="${esc(label)} ${c.name===DAC_VOLUME?'dB':'value'}" min="${lo}" max="${hi}" step="${step}" value="${shown}" data-number="${i}" ${disabled}><output class="readback" data-output="${i}">${esc(level(c,v))}</output></div>${outside?`<p class="hint">Driver readback ${esc(v)} is outside its advertised ${c.min}–${c.max} range. No correction is applied automatically.</p>`:''}`;
    }
    return `<div class="readback">${esc(v)}</div>`;
  }).join('');
  return `<form class="control" data-control="${c.numid}"><div class="control-head"><label>${esc(pretty(c))}</label><small>${esc(c.name)}</small></div><div class="control-body"><div class="editors">${editors}</div>${c.locked?'':`<button type="submit" disabled hidden>Apply</button>`}</div>${c.locked?`<p class="locked-note">${esc(c.locked)}</p>`:''}${c.protected?'<p class="hint">Hardware setting. Changing this requires a separate confirmation.</p>':''}</form>`;
}
function panel(heading,controls) {return `<section class="panel"><h2>${esc(heading)}</h2>${controls.length?controls.map(c=>control(c)).join(''):'<p class="muted">This driver does not expose a control for this section.</p>'}</section>`;}
function heading(extra='') {return `<div class="page-heading"><div><h1>${titles[PAGE]}</h1><p>${intros[PAGE]}</p></div>${extra}</div>`;}
function directNotice() {return DATA.state.direct?'<div class="notice">Direct playback is active. SBX and equalizer processing are bypassed. Stored effect settings are shown below; this panel leaves them unchanged while Direct playback is active.</div>':'';}
function dacVolumePanel() {
  const dac=find(DAC_VOLUME);
  if(!dac)return '<section class="panel"><h2>Direct DAC volume</h2><p class="hint">The loaded driver does not expose adjustable DAC volume.</p></section>';
  const status=find('AE-5: Direct DAC Status');
  return `<section class="panel"><h2>Direct DAC volume</h2>${status?`<p class="readback">${esc(valueLabel(status))}</p>`:''}<p class="hint">${DATA.state.direct?'Adjust the output level during playback.':'Choose the level for the next Direct playback.'} Headphone amplifier gain stays separate. Levels are saved independently from effect profiles.</p>${control(dac)}</section>`;
}
function quality() {
  const active=Object.values(DATA.state.streams).find(s=>s.includes('format:') && s.includes('rate:'));
  const rate=active?.match(/rate:\s*(\d+)/)?.[1];
  const format=active?.match(/format:\s*(\S+)/)?.[1];
  return {rate:rate?`${Number(rate)/1000} kHz`:'Idle',format:format||'No active PCM',bits:format?.match(/(?:S|U|FLOAT)(\d+)/)?.[1]};
}
function headphones() {return '<svg class="headphones" viewBox="0 0 220 210" aria-hidden="true"><path d="M39 118v-22a71 71 0 0 1 142 0v22" fill="none" stroke="currentColor" stroke-width="12"/><path d="M54 93a56 56 0 0 1 112 0" fill="none" stroke="#656a74" stroke-width="5"/><path d="M29 111v56q0 17 20 17h12v-89H49q-20 0-20 16m162 0v56q0 17-20 17h-12v-89h12q20 0 20 16" fill="#4c505a" stroke="#a6aab5" stroke-width="3"/><rect x="52" y="103" width="21" height="81" rx="8" fill="#292c32" stroke="#ee655d" stroke-width="2"/><rect x="147" y="103" width="21" height="81" rx="8" fill="#292c32" stroke="#ee655d" stroke-width="2"/><path d="M166 183v11q0 8-10 8h-17" fill="none" stroke="#777c88" stroke-width="3"/></svg>';}
function toggle(c,label) {
  if(!c)return '';
  const on=c.values.every(v=>v==='on');
  return `<button class="command-toggle ${on?'enabled':''}" role="switch" aria-checked="${on}" aria-label="${esc(label||pretty(c))}" data-toggle="${c.numid}" ${c.locked?'disabled':''}><span></span></button>`;
}
function dial(c,label,switchControl) {
  if(!c)return '';
  const value=Number(c.values[0]),ratio=(value-c.min)/(c.max-c.min);
  return `<div class="dial-unit"><div class="dial ${c.locked?'locked':''}" role="slider" tabindex="${c.locked?'-1':'0'}" aria-disabled="${!!c.locked}" aria-label="${esc(label)}" aria-valuemin="${c.min}" aria-valuemax="${c.max}" aria-valuenow="${value}" data-dial="${c.numid}"><svg viewBox="0 0 110 110" aria-hidden="true"><circle class="dial-track" cx="55" cy="55" r="39" pathLength="100" stroke-dasharray="75 100" transform="rotate(135 55 55)"/><circle class="dial-value" cx="55" cy="55" r="39" pathLength="100" stroke-dasharray="${ratio*75} 100" transform="rotate(135 55 55)"/><text x="55" y="63" text-anchor="middle">${value}</text></svg></div>${toggle(switchControl,label+' enabled')}<span>${esc(label)}</span><small>${c.locked?'Direct bypass':'Drag to adjust'}</small></div>`;
}
function renderPlayback() {
  const q=quality(),output=find('Output Select'),filter=find('AE-5: Sound Filter');
  return heading()+`<div class="command-tabs"><button class="selected" data-local-tab="analog">Speakers/Headphones</button><button data-local-tab="digital">Digital</button></div><div id="analog-view" class="command-playback"><div class="playback-device">Playback device: <strong>Sound Blaster AE-5</strong></div><div class="output-grid"><div><h2>Output</h2>${output?output.items.map((name,i)=>`<label class="output-option"><input type="radio" name="output" value="${i}" data-output-select="${output.numid}" ${Number(output.values[0])===i?'checked':''} ${output.locked?'disabled':''}><span class="output-symbol">${i?'♧':'▣'}</span><span>${i?'Headphones':'Speakers'}<small>${i?esc(valueLabel(find('AE-5: Headphone Gain'))):esc(valueLabel(find('Surround Channel Config')))}</small></span></label>`).join(''):''}</div><div class="output-diagram">${headphones()}<span>${esc(valueLabel(output))}</span></div></div><section class="direct-setting"><h2>Direct Mode</h2><button class="command-toggle ${DATA.state.direct?'enabled':''}" disabled aria-label="Direct mode is read only" role="switch" aria-checked="${DATA.state.direct}"><span></span></button> ${DATA.state.direct?'On':'Off'}<p class="hint">Current mode is read-only here. Driver service transitions are not enabled in this preview.</p></section>${dacVolumePanel()}<div class="two-col format-controls"><div><h2>Audio Quality</h2><select disabled aria-label="Active audio quality"><option>${esc(q.bits?`${q.bits} bit, ${q.rate}`:q.rate)}</option></select><p class="hint">Live PCM format · ${esc(q.format)}</p></div><div><h2>Filters</h2>${filter?`<select aria-label="DAC filter" data-select="${filter.numid}" ${filter.locked?'disabled':''}>${filter.items.map((name,i)=>`<option value="${i}" ${String(i)===filter.values[0]?'selected':''}>${esc(name)}</option>`).join('')}</select>`:''}</div></div><details class="advanced"><summary>Headphone / speaker configuration and channel controls</summary>${DATA.controls.filter(c=>c.section==='playback'&&c!==output&&c!==filter&&c.name!==DAC_VOLUME&&c.writable).map(c=>control(c)).join('')}${['Master Playback Volume','Master Playback Switch','Front Playback Volume','Front Playback Switch'].map(find).filter(Boolean).map(c=>control(c)).join('')}</details></div><div id="digital-view" hidden>${panel('Digital output',DATA.controls.filter(c=>c.name.startsWith('IEC958')))}</div>`;
}
function renderSbx() {
  const effects=[['Surround','Surround'],['Crystalizer','Crystalizer'],['X-Bass','Bass'],['Smart Volume','Smart Vol'],['Dialog Plus','Dialog+']];
  const controls=effects.flatMap(([name])=>[find('FX: '+name+' Playback Volume'),find('FX: '+name+' Playback Switch')]).filter(Boolean);
  const master=find('Enable OutFX Playback Switch');
  const more=DATA.controls.filter(c=>c.section==='sbx'&&c!==master&&!controls.includes(c));
  return `<div class="command-page-title">${toggle(master,'SBX profile enabled')}<h1>SBX Profile</h1><select id="profile-shortcut" aria-label="Profile selection"><option>Current settings</option><option>My saved profiles…</option></select><button class="icon-button" id="quick-save" aria-label="Save current profile">+</button></div><section class="profile-carousel"><div class="profile-backdrop"></div><div class="profile-caption"><strong>SBX</strong><span>Your personal sound profile</span></div><div class="profile-tiles"><button class="profile-tile current" id="current-profile"><span class="scene scene-game"></span><span class="tile-title">Current settings<small>Live AE-5 readback</small></span></button><button class="profile-tile" id="save-profile-tile"><span class="scene scene-cinema"></span><span class="tile-title">Save a profile<small>Keep your own settings</small></span></button><button class="profile-tile" id="import-profile-tile"><span class="scene scene-concert"></span><span class="tile-title">Load a profile<small>Review before applying</small></span></button></div></section><section class="acoustic-engine"><h2>Acoustic Engine</h2><div class="dial-row">${effects.map(([name,label])=>dial(find('FX: '+name+' Playback Volume'),label,find('FX: '+name+' Playback Switch'))).join('')}</div>${DATA.state.direct?'<p class="bypass-note">DIRECT · Effects are bypassed. The dials show stored values.</p>':''}${more.length?`<details class="advanced"><summary>Bass crossover and Smart Volume mode</summary>${more.map(c=>control(c)).join('')}</details>`:''}</section>`;
}
function renderEq() {
  const bands=DATA.controls.filter(c=>c.name.startsWith('EQ Band')).sort((a,b)=>Number(a.name.match(/Band(\d+)/)[1])-Number(b.name.match(/Band(\d+)/)[1]));
  const points=bands.map((c,i)=>`${40+i*720/Math.max(1,bands.length-1)},${25+150*(1-(Number(c.values[0])-c.min)/(c.max-c.min))}`);
  const preset=find('FX: Equalizer Preset Switch');
  return `<div class="command-page-title">${toggle(find('FX: Equalizer Playback Switch'),'Equalizer enabled')}<h1>Equalizer</h1></div><section class="eq-command"><div class="eq-toolbar">${preset?`<select aria-label="Equalizer preset" data-select="${preset.numid}" ${preset.locked?'disabled':''}>${preset.items.map((n,i)=>`<option value="${i}" ${String(i)===preset.values[0]?'selected':''}>${esc(n)}</option>`).join('')}</select>`:''}<button class="icon-button" id="quick-save" aria-label="Save equalizer profile">+</button><span id="eq-value">${DATA.state.direct?'Direct bypass':'Drag a point to change a band'}</span></div><svg id="interactive-eq" class="eq-graph" viewBox="0 0 800 200" preserveAspectRatio="none" role="group" aria-label="Ten-band graphic equalizer">${bands.map((c,i)=>`<path class="eq-grid" d="M${points[i].split(',')[0]} 15V175"/><text class="eq-label" x="${points[i].split(',')[0]}" y="194" text-anchor="middle">Band ${i}</text>`).join('')}<path class="eq-zero" d="M40 100H760"/><text class="eq-label" x="8" y="20">+12</text><text class="eq-label" x="16" y="104">0</text><text class="eq-label" x="8" y="177">−12</text><polyline class="eq-line" points="${points.join(' ')}"/>${points.map((p,i)=>`<circle class="eq-dot" cx="${p.split(',')[0]}" cy="${p.split(',')[1]}" r="6" data-eq="${bands[i].numid}" role="slider" tabindex="${bands[i].locked?-1:0}" aria-label="Equalizer band ${i}" aria-valuemin="${bands[i].min}" aria-valuemax="${bands[i].max}" aria-valuenow="${bands[i].values[0]}" aria-disabled="${!!bands[i].locked}"/>`).join('')}</svg><p class="eq-caption">dB · Linux driver band indices 0–9. The line shows settings, not a measured frequency response.</p>${DATA.state.direct?'<p class="bypass-note">DIRECT · Equalizer processing is bypassed.</p>':''}<details class="advanced"><summary>Fine-tune band values</summary>${bands.map(c=>control(c)).join('')}</details></section>`;
}
function renderRecording() {
  const cs=DATA.controls.filter(c=>c.section==='recording');
  const voice=c=>c.name.startsWith('FX:')||c.name.startsWith('Enable InFX')||c.name.startsWith('VoiceFX')||/Wedge Angle|SVM Level/.test(c.name);
  return heading()+panel('Input and recording levels',cs.filter(c=>!voice(c)))+directNotice()+panel('Voice processing',cs.filter(voice));
}
function renderMixer() {
  return heading('<label class="sr-only" for="search">Find a mixer control</label><input class="search" type="text" id="search" placeholder="Find a control…">')+panel('Playback and digital output',DATA.controls.filter(c=>c.section==='mixer'))+'<p class="hint">Changes apply when you release a slider, choose an option, or finish entering a number. L and R remain independent. Settings sync automatically with the AE-5.</p>';
}
function renderProfiles() {
  return heading()+`<section class="panel"><h2>Save the current AE-5 settings</h2><p class="muted">Export a JSON profile to this computer. DAC volume, headphone gain, output selection, and speaker topology are excluded. Exporting does not change your sound.</p><div class="actions"><button id="export" class="primary">Export current profile</button><button id="import">Import and review…</button></div></section><section class="panel"><h2>Profile review</h2><div id="profile-review" class="muted">Choose a profile to see its exact changes before applying it.</div></section>`;
}
function renderHistory() {
  return heading()+`<section class="panel"><h2>Record what you hear</h2><form id="note-form" class="note-grid"><label for="observation">Result</label><select id="observation"><option>Heard as expected</option><option>No audible change</option><option>Silent</option><option>Distorted</option><option>Other</option></select><label for="note">What did you change, and what happened?</label><textarea id="note" rows="3" maxlength="4000" required placeholder="For example: changed the DAC filter, music continued without interruption."></textarea><div><button class="primary" type="submit">Save observation</button></div></form></section><section class="panel"><h2>Recent activity</h2><p class="hint">Control entries confirm ALSA readback. Only your listening notes report what you heard.</p><div id="history-items">Loading history…</div><div class="actions"><button id="export-history">Export recent history</button></div></section>`;
}
function renderStatus() {
  const rows=DATA.state.dacs.map(d=>`<div class="support-row"><strong>DAC ${esc(d.node)}</strong><p>${d.muted.some(Boolean)?'At least one channel muted':'Mute bits clear'} · ${esc(d.amps.join(' / '))}</p></div>`).join('');
  return heading()+`<section class="panel"><h2>AE-5 identity</h2><p class="muted">Codec ${esc(DATA.card.vendor)} · subsystem ${esc(DATA.card.subsystem)} · ALSA card ${DATA.card.index}</p><p class="hint">${esc(DATA.card.device_path)}</p><p class="hint">${DATA.controls.length} controls read. This interface rejects requests for other cards.</p></section><section class="panel"><h2>Hardware mute readback</h2>${rows||'<p class="muted">No DAC amplifier registers reported.</p>'}<p class="hint">Clear mute bits and an active PCM stream do not establish audible output at the headphones.</p></section><section class="panel"><h2>Active streams</h2><pre class="raw-state">${esc(JSON.stringify(DATA.state.streams,null,2))}</pre></section>`+panel('Jack detection and driver information',DATA.controls.filter(c=>c.section==='status'));
}
function renderFeatures() {
  const rows=[
    ['Playback and mixer','AE-5 output controls, channel levels, mutes, headphone gain preset, and DAC filter. Changes apply on release or selection; hardware output and gain changes require confirmation.'],
    ['SBX and equalizer','Linux acoustic engine controls, ten EQ bands, and the driver’s preset list. Direct playback bypasses these effects; stored values are displayed without changing them.'],
    ['Recording','AE-5 inputs, capture levels, mic boost, What U Hear, and available voice processing controls.'],
    ['Profiles and history','Profile export, a change preview before import, ALSA readback history, and your own listening notes.'],
    ['Direct mode and format','The current format is displayed. This web preview does not restart audio services or change the Direct lifecycle.'],
    ['Aurora lighting','No supported lighting control in the captured Linux driver. No RGB controls are implemented here.'],
    ['Scout Mode and encoders','Scout Mode, Dolby Digital Live, and DTS Connect are not implemented. Digital mixer controls are not surround encoders.'],
    ['Creative services','Creative account sync, proprietary profile downloads, vendor updates, and Windows calibration features are not implemented.']
  ];
  return heading()+`<section class="panel">${rows.map(([name,detail])=>`<div class="support-row"><strong>${name}</strong><p>${detail}</p></div>`).join('')}<p class="support-link">Independent AegAudio interface inspired by <a href="https://download.creative.com/manualdn/Manuals/TSD/14190/qfWrGlGToW/Sound%20Blaster%20Command%20Software%20Guide.pdf" target="_blank" rel="noreferrer">Creative’s Sound Blaster Command layout</a>. It is not Creative’s Windows application.</p></section>`;
}
function updateStatus(force=false) {
  const q=quality();$('#connection').textContent=`Connected · ALSA card ${DATA.card.index}`;
  $('#mode').textContent=DATA.state.direct?'Direct mode active':'Standard playback';
  $('#output-status').textContent='Output · '+valueLabel(find('Output Select'));
  $('#front-status').textContent='Front · '+valueLabel(find('Front Playback Switch'));
  $('#format-status').textContent=q.rate+' · '+q.format;
  $('#updated').textContent='Live · '+new Date(DATA.captured_at).toLocaleTimeString();
  $('#footer-engine').textContent=DATA.state.direct?'DIRECT':'SBX';
  const volume=find('Master Playback Volume'),mute=find('Master Playback Switch');
  const footerSignature=JSON.stringify([volume,mute]);
  if(force || $('#footer-volume').dataset.signature!==footerSignature) {
  $('#footer-volume').innerHTML=volume?`<button id="master-mute" aria-label="${mute?.values.includes('off')?'Unmute':'Mute'} master">${mute?.values.includes('off')?'◖×':'◖))'}</button><label class="sr-only" for="master-volume">Master volume</label><input id="master-volume" type="range" min="${volume.min}" max="${volume.max}" value="${volume.values[0]}"><output id="master-percent">${Math.round(Number(volume.values[0])/volume.max*100)}%</output>`:'';
  if(volume){$('#master-volume').oninput=e=>$('#master-percent').textContent=Math.round(Number(e.target.value)/volume.max*100)+'%';$('#master-volume').onchange=e=>writeControl(volume,[e.target.value]);}
  if(mute)$('#master-mute').onclick=()=>writeControl(mute,mute.values.map(v=>v==='on'?'off':'on'));
  $('#footer-volume').dataset.signature=footerSignature;
  }
  const master=find('Master Playback Switch'),front=find('Front Playback Switch');
  const muted=(master?.values.includes('off')||front?.values.includes('off')||DATA.state.dacs.find(d=>d.node==='0x02')?.muted.some(Boolean));
  $('#mute-warning').hidden=!muted;$('#mute-warning').textContent='Master, Front, or the Front DAC reports a muted channel. Review Listening level and Device status. No automatic unmute is performed.';
}
function render() {
  if(!DATA)return;
  const focused=document.activeElement?.id;
  const openDetails=[...document.querySelectorAll('#page details')].map(d=>d.open);
  const search=$('#search')?.value;
  const digital=$('[data-local-tab="digital"]')?.classList.contains('selected');
  document.querySelectorAll('[data-page]').forEach(b=>{b.classList.toggle('active',b.dataset.page===PAGE);b.setAttribute('aria-current',b.dataset.page===PAGE?'page':'false');});
  const renderers={playback:renderPlayback,sbx:renderSbx,equalizer:renderEq,recording:renderRecording,mixer:renderMixer,profiles:renderProfiles,history:renderHistory,status:renderStatus,features:renderFeatures};
  $('#page').innerHTML=renderers[PAGE]();bind();updateStatus(true);
  document.querySelectorAll('#page details').forEach((d,i)=>d.open=!!openDetails[i]);
  if(search && $('#search')) {$('#search').value=search;$('#search').dispatchEvent(new Event('input'));}
  if(digital && $('#digital-view')) {$('#analog-view').hidden=true;$('#digital-view').hidden=false;document.querySelectorAll('[data-local-tab]').forEach(b=>b.classList.toggle('selected',b.dataset.localTab==='digital'));}
  if(focused)document.getElementById(focused)?.focus({preventScroll:true});
}
async function confirmChange(title,text,detail='') {
  $('#confirm-title').textContent=title;$('#confirm-text').textContent=text;$('#confirm-detail').textContent=detail;
  const dialog=$('#confirm');dialog.returnValue='cancel';dialog.showModal();
  return new Promise(resolve=>dialog.addEventListener('close',()=>resolve(dialog.returnValue==='apply'),{once:true}));
}
async function writeControl(c,values) {
  if(BUSY||c.locked)return;
  if(!CONNECTED){message('AE-5 connection is unavailable. Waiting to reconnect.',true);return;}
  if(c.protected&&!await confirmChange('Change '+pretty(c)+'?','This changes a hardware output or amplifier setting.',`${valueLabel(c)} → ${c.type==='ENUMERATED'?c.items[Number(values[0])]:values.join(' / ')}`)){render();return;}
  BUSY=true;++REQUEST;
  try{await api('/api/set',{key:c.key,expected:c.values,values,confirmed:!!c.protected});message(`${pretty(c)} applied. ALSA readback matched.`);}
  catch(error){message(error.message,true);}
  finally{BUSY=false;await refresh();}
}
async function refresh({background=false}={}) {
  if(BUSY || (background && (READING || document.hidden || editingControl())))return;
  const request=++REQUEST;READING++;if(!background)$('#refresh').disabled=true;
  try {
    const data=await api('/api/state');
    if(request!==REQUEST || BUSY || (background && editingControl()))return;
    const changed=!DATA || stateSignature(data)!==stateSignature(DATA);
    const recovered=!CONNECTED;CONNECTED=true;DATA=data;
    if(!background || (changed && !['profiles','history'].includes(PAGE)))render();
    else updateStatus();
    $('#refresh').title='Live sync every second. Click to reload all settings.';
    if(recovered && $('#message').dataset.connectionError==='true'){message('AE-5 connection restored.');delete $('#message').dataset.connectionError;}
    if(background && PAGE==='history')await loadHistory();
  }
  catch(error){
    if(request!==REQUEST)return;
    CONNECTED=false;message('Live sync unavailable: '+error.message,true);$('#message').dataset.connectionError='true';
    $('#connection').textContent='Device unavailable';$('#mode').textContent='State unavailable';$('#updated').textContent='Offline · retrying';
  }
  finally{READING--;if(!background)$('#refresh').disabled=false;}
}
function download(name,data) {const url=URL.createObjectURL(new Blob([JSON.stringify(data,null,2)],{type:'application/json'}));const a=document.createElement('a');a.href=url;a.download=name;a.click();setTimeout(()=>URL.revokeObjectURL(url),1000);}
async function loadHistory() {
  try {const entries=await api('/api/history');if(PAGE!=='history')return;$('#history-items').innerHTML=entries.length?entries.reverse().map(e=>`<article class="history-item"><time>${esc(new Date(e.time).toLocaleString())}</time><strong>${esc(e.control||e.result||e.kind)}</strong>${e.before?`<p>${esc(e.before.join(' / '))} → ${esc(e.after.join(' / '))}</p>`:''}${e.note?`<p>${esc(e.note)}</p>`:''}${e.detail?`<p>${esc(e.detail)}</p>`:''}${e.changes?`<pre>${esc(JSON.stringify(e.changes,null,2))}</pre>`:''}</article>`).join(''):'<p class="hint">No changes or listening notes recorded yet.</p>';}
  catch(error){message(error.message,true);}
}
function bind() {
  document.querySelectorAll('[data-toggle]').forEach(button=>button.onclick=()=>{
    const c=DATA.controls.find(c=>c.numid===Number(button.dataset.toggle));
    writeControl(c,c.values.map(()=>c.values.every(v=>v==='on')?'off':'on'));
  });
  document.querySelectorAll('[data-select]').forEach(select=>select.onchange=()=>{
    const c=DATA.controls.find(c=>c.numid===Number(select.dataset.select));writeControl(c,[select.value]);
  });
  document.querySelectorAll('[data-output-select]').forEach(radio=>radio.onchange=()=>{
    const c=DATA.controls.find(c=>c.numid===Number(radio.dataset.outputSelect));writeControl(c,[radio.value]);
  });
  document.querySelectorAll('[data-local-tab]').forEach(button=>button.onclick=()=>{
    $('#analog-view').hidden=button.dataset.localTab!=='analog';$('#digital-view').hidden=button.dataset.localTab!=='digital';
    document.querySelectorAll('[data-local-tab]').forEach(b=>b.classList.toggle('selected',b===button));
  });
  document.querySelectorAll('[data-dial]').forEach(node=>{
    const c=DATA.controls.find(c=>c.numid===Number(node.dataset.dial));let start=null,value=Number(c.values[0]);
    const draw=()=>{node.querySelector('text').textContent=value;node.querySelector('.dial-value').setAttribute('stroke-dasharray',`${(value-c.min)/(c.max-c.min)*75} 100`);node.setAttribute('aria-valuenow',value);};
    node.onpointerdown=e=>{if(c.locked||BUSY)return;start={y:e.clientY,value};node.setPointerCapture(e.pointerId);};
    node.onpointermove=e=>{if(!start)return;value=Math.round(Math.min(c.max,Math.max(c.min,start.value+(start.y-e.clientY)*(c.max-c.min)/160)));draw();};
    node.onpointerup=e=>{if(!start)return;start=null;node.releasePointerCapture(e.pointerId);if(String(value)!==c.values[0])writeControl(c,[String(value)]);};
    node.onpointercancel=()=>{start=null;value=Number(c.values[0]);draw();};
    node.onkeydown=e=>{if(c.locked||BUSY||!['ArrowUp','ArrowDown','ArrowLeft','ArrowRight','Home','End'].includes(e.key))return;e.preventDefault();value=e.key==='Home'?c.min:e.key==='End'?c.max:Math.min(c.max,Math.max(c.min,value+(['ArrowUp','ArrowRight'].includes(e.key)?1:-1)));draw();};
    node.onkeyup=e=>{if(['ArrowUp','ArrowDown','ArrowLeft','ArrowRight','Home','End'].includes(e.key)&&String(value)!==c.values[0])writeControl(c,[String(value)]);};
  });
  if($('#interactive-eq')) {
    const svg=$('#interactive-eq');let active=null,value=null;
    const drawEq=(c,v)=>{const dot=svg.querySelector(`[data-eq="${c.numid}"]`);dot.setAttribute('cy',25+150*(1-(v-c.min)/(c.max-c.min)));dot.setAttribute('aria-valuenow',v);svg.querySelector('polyline').setAttribute('points',[...svg.querySelectorAll('[data-eq]')].map(d=>d.getAttribute('cx')+','+d.getAttribute('cy')).join(' '));$('#eq-value').textContent=pretty(c)+' · '+level(c,v);};
    svg.querySelectorAll('[data-eq]').forEach(dot=>{
      const c=DATA.controls.find(c=>c.numid===Number(dot.dataset.eq));
      dot.onpointerdown=e=>{if(c.locked||BUSY)return;active=c;value=Number(c.values[0]);svg.setPointerCapture(e.pointerId);e.preventDefault();};
      dot.onkeydown=e=>{if(c.locked||BUSY||!['ArrowUp','ArrowDown'].includes(e.key))return;e.preventDefault();const v=Math.min(c.max,Math.max(c.min,Number(dot.getAttribute('aria-valuenow'))+(e.key==='ArrowUp'?1:-1)));drawEq(c,v);};
      dot.onkeyup=e=>{if(['ArrowUp','ArrowDown'].includes(e.key)&&dot.getAttribute('aria-valuenow')!==c.values[0])writeControl(c,[dot.getAttribute('aria-valuenow')]);};
    });
    svg.onpointermove=e=>{if(!active)return;const r=svg.getBoundingClientRect(),y=(e.clientY-r.top)/r.height*200;value=Math.round(Math.min(active.max,Math.max(active.min,active.min+(1-(y-25)/150)*(active.max-active.min))));drawEq(active,value);};
    svg.onpointerup=e=>{if(!active)return;const c=active;active=null;svg.releasePointerCapture(e.pointerId);if(String(value)!==c.values[0])writeControl(c,[String(value)]);};
    svg.onpointercancel=()=>{if(active)drawEq(active,Number(active.values[0]));active=null;};
  }
  const exportQuick=async()=>{try{download('ae5-profile-'+new Date().toISOString().slice(0,10)+'.json',await api('/api/profile'));message('Your profile was exported without changing audio.');}catch(error){message(error.message,true);}};
  if($('#quick-save'))$('#quick-save').onclick=exportQuick;
  if($('#save-profile-tile'))$('#save-profile-tile').onclick=exportQuick;
  if($('#current-profile'))$('#current-profile').onclick=()=>message('These are the current AE-5 settings. No preset was applied.');
  if($('#import-profile-tile'))$('#import-profile-tile').onclick=()=>{PAGE='profiles';render();$('#profile-file').value='';$('#profile-file').click();};
  if($('#profile-shortcut'))$('#profile-shortcut').onchange=e=>{if(e.target.selectedIndex){PAGE='profiles';render();}};
  document.querySelectorAll('[data-control]').forEach(form=>{
    const c=DATA.controls.find(c=>c.numid===Number(form.dataset.control));
    const button=form.querySelector('button[type=submit]');
    const values=()=>c.values.map((v,i)=>c.type==='BOOLEAN'?(form.querySelector(`[data-i="${i}"]`).checked?'on':'off'):c.type==='INTEGER'?controlValue(c,form.querySelector(`[data-number="${i}"]`).value):form.querySelector(`[data-i="${i}"]`).value);
    form.addEventListener('input',event=>{
      if(!button)return;
      const i=event.target.dataset.i??event.target.dataset.number;
      if(c.type==='INTEGER'&&i!==undefined){
        const value=event.target.value;form.querySelector(`[data-i="${i}"]`).value=value;form.querySelector(`[data-number="${i}"]`).value=value;form.querySelector(`[data-output="${i}"]`).textContent=level(c,controlValue(c,value));
      }
      button.disabled=JSON.stringify(values())===JSON.stringify(c.values);
      form.dataset.dirty=String(!button.disabled);
    });
    form.addEventListener('change',()=>{if(button && !button.disabled && form.checkValidity())form.requestSubmit(button);});
    form.addEventListener('submit',async event=>{
      event.preventDefault();if(BUSY||c.locked)return;
      if(!CONNECTED){message('AE-5 connection is unavailable. Waiting to reconnect.',true);return;}
      if(!form.reportValidity())return;
      const requested=values();
      if(c.protected && !await confirmChange('Change '+pretty(c)+'?',`This changes a hardware output or amplifier setting.`,`${valueLabel(c)} → ${c.type==='ENUMERATED'?c.items[Number(requested[0])]:requested.join(' / ')}`)){render();return;}
      BUSY=true;++REQUEST;button.disabled=true;form.dataset.dirty='false';
      try{await api('/api/set',{key:c.key,expected:c.values,values:requested,confirmed:c.protected});message(`${pretty(c)} applied. ALSA readback matched.`);}
      catch(error){message(error.message,true);}
      finally{BUSY=false;await refresh();}
    });
  });
  if($('#search'))$('#search').oninput=event=>document.querySelectorAll('[data-control]').forEach(form=>{form.hidden=!form.textContent.toLowerCase().includes(event.target.value.toLowerCase());});
  if($('#export'))$('#export').onclick=async()=>{try{const profile=await api('/api/profile');download('ae5-profile-'+new Date().toISOString().slice(0,10)+'.json',profile);message('Profile exported. Audio settings were not changed.');}catch(error){message(error.message,true);}};
  if($('#import'))$('#import').onclick=()=>{$('#profile-file').value='';$('#profile-file').click();};
  if($('#note-form')) {
    loadHistory();
    $('#note-form').onsubmit=async event=>{event.preventDefault();const button=event.target.querySelector('button');button.disabled=true;try{await api('/api/note',{note:$('#note').value,result:$('#observation').value});$('#note').value='';message('Listening observation saved.');await loadHistory();}catch(error){message(error.message,true);}finally{button.disabled=false;}};
    $('#export-history').onclick=async()=>{try{download('ae5-listening-history.json',await api('/api/history'));}catch(error){message(error.message,true);}};
  }
}
$('#profile-file').onchange=async event=>{
  const file=event.target.files[0];if(!file)return;
  try{
    if(file.size>1000000)throw new Error('Profile exceeds 1 MB.');
    const profile=JSON.parse(await file.text());const preview=await api('/api/profile/preview',{profile});PROFILE={profile,reviewed:preview.changes};
    if(PAGE!=='profiles')return;
    $('#profile-review').innerHTML=preview.changes.length?`<p>${preview.changes.length} changes. Gain and output topology are excluded.</p><pre>${esc(preview.changes.map(c=>`${c.name}: ${c.before.join(' / ')} → ${c.after.join(' / ')}`).join('\n'))}</pre><button id="apply-profile" class="primary" ${DATA.state.direct?'disabled':''}>Apply reviewed profile</button>${DATA.state.direct?'<p class="hint">Applying processing profiles is unavailable during Direct playback.</p>':''}`:'No changes. This profile already matches the current settings.';
    if($('#apply-profile'))$('#apply-profile').onclick=async()=>{if(BUSY||!PROFILE)return;if(!await confirmChange('Apply profile?','This will apply the reviewed control changes. Volumes and mutes may change.'))return;BUSY=true;try{await api('/api/profile/apply',PROFILE);message('Profile applied; ALSA readbacks matched.');PROFILE=null;}catch(error){message(error.message,true);}finally{BUSY=false;await refresh();}};
  }catch(error){message(error.message,true);}
};
document.querySelectorAll('[data-page]').forEach(button=>button.onclick=()=>{if(BUSY)return;PAGE=button.dataset.page;PROFILE=null;render();});
$('#refresh').onclick=()=>refresh();
document.addEventListener('pointerdown',event=>{if(event.target.closest('[data-control],[data-dial],#interactive-eq,#footer-volume'))POINTER_ACTIVE=true;});
document.addEventListener('pointerup',()=>{POINTER_ACTIVE=false;});
document.addEventListener('pointercancel',()=>{POINTER_ACTIVE=false;});
document.addEventListener('keydown',event=>{if(['ArrowUp','ArrowDown','ArrowLeft','ArrowRight','Home','End'].includes(event.key)&&event.target.closest('[role="slider"],#footer-volume'))KEY_ACTIVE=true;});
document.addEventListener('keyup',()=>{KEY_ACTIVE=false;});
window.addEventListener('blur',()=>{POINTER_ACTIVE=false;KEY_ACTIVE=false;});
async function syncLoop(){try{await refresh({background:true});}finally{clearTimeout(SYNC_TIMER);SYNC_TIMER=setTimeout(syncLoop,1000);}}
refresh();
SYNC_TIMER=setTimeout(syncLoop,1000);
