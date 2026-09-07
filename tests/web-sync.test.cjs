// Offline synchronization checks. No network, ALSA, or audio access.
const {readFileSync}=require('node:fs');
const {join}=require('node:path');
const vm=require('node:vm');
const assert=require('node:assert/strict');
const source=readFileSync(join(__dirname,'../aegaudio/web_assets/app.js'),'utf8').split("\n$('#profile-file').onchange=")[0];
function setup(){
  const nodes=new Map();
  function node(selector){
    if(selector.includes('data-dirty'))return null;
    if(!nodes.has(selector))nodes.set(selector,{open:false,disabled:false,dataset:{},textContent:'',title:'',classList:{toggle(){}}});
    return nodes.get(selector);
  }
  const calls=[];
  const context=vm.createContext({document:{querySelector:node,activeElement:{matches:()=>false},hidden:false},calls,console});
  vm.runInContext(source,context);
  vm.runInContext(`
    DATA={card:{device_path:'ae5'},controls:[{values:['20']}],state:{direct:true},captured_at:'first'};
    CONNECTED=true;
    render=()=>calls.push('render');
    updateStatus=()=>calls.push('status');
    loadHistory=async()=>calls.push('history');
    api=async(path,body)=>{calls.push({path,body});return {card:{device_path:'ae5'},controls:[{values:['30']}],state:{direct:true},captured_at:'next'};};
  `,context);
  return {context,calls,run:code=>vm.runInContext(code,context)};
}
(async()=>{
  const v=setup();
  v.run("globalThis.dac={name:DAC_VOLUME,numid:76,iface:'MIXER',type:'INTEGER',count:2,values:['209','209'],min:0,max:255,db_min:-127.5,db_step:0.5};DATA.controls=[dac];DATA.state.direct=true;");
  assert.equal(v.run("editorValue(dac,'209')"),-23);
  assert.equal(v.run("controlValue(dac,'-20.5')"),'214');
  assert.equal(v.run("controlValue(dac,'0')"),'255');
  assert.equal(v.run("level(dac,'209')"),'-23.0 dB');
  assert(v.run('dacVolumePanel()').includes('value="-23"'));
  assert(v.run('dacVolumePanel()').includes('step="0.5"'));
  assert(v.run('dacVolumePanel()').includes('during playback'));
  v.run('DATA.controls=[]');
  assert(v.run('dacVolumePanel()').includes('does not expose'));
  let t=setup();await t.run('refresh({background:true})');
  assert(t.calls.includes('render'),'External control change should update the page');
  assert.equal(t.run('DATA.controls[0].values[0]'),'30');
  assert(t.calls.filter(c=>typeof c==='object').every(c=>c.path==='/api/state'&&c.body===undefined),'Polling must be read-only');
  t.calls.length=0;await t.run('refresh({background:true})');
  assert(!t.calls.includes('render'),'Unchanged state must not rebuild controls or lose focus');
  assert(t.calls.includes('status'));
  for(const guard of ['POINTER_ACTIVE=true','KEY_ACTIVE=true','BUSY=true',"$('#confirm').open=true"]){
    t=setup();t.run(guard);await t.run('refresh({background:true})');assert.equal(t.calls.length,0,guard+' should hold polling');
  }
  t=setup();t.run("PAGE='profiles'");await t.run('refresh({background:true})');
  assert(!t.calls.includes('render'),'Profile preview must survive background reads');
  t=setup();t.run("PAGE='history'");await t.run('refresh({background:true})');
  assert(!t.calls.includes('render'),'Listening-note draft must survive background reads');assert(t.calls.includes('history'));
  t=setup();t.run('api=()=>new Promise(resolve=>{globalThis.finishRead=resolve;})');
  let pending=t.run('refresh({background:true})');
  t.run("++REQUEST; DATA.controls[0].values=['55']; finishRead({card:{},controls:[{values:['10']}],state:{}})");await pending;
  assert.equal(t.run('DATA.controls[0].values[0]'),'55','Stale read must not replace a newer write');
  assert(!t.calls.includes('render'));
  t=setup();t.run('api=()=>new Promise(resolve=>{globalThis.finishRead=resolve;})');
  pending=t.run('refresh({background:true})');
  t.run("POINTER_ACTIVE=true;finishRead({card:{},controls:[{values:['10']}],state:{}})");await pending;
  assert.equal(t.run('DATA.controls[0].values[0]'),'20','A drag begun during a read must be preserved');
  t=setup();t.run("api=async()=>{throw new Error('offline')}");await t.run('refresh({background:true})');
  assert.equal(t.run('CONNECTED'),false);assert.equal(t.run("$('#updated').textContent"),'Offline · retrying');
  t.run('api=async()=>DATA');await t.run('refresh({background:true})');assert.equal(t.run('CONNECTED'),true);
  console.log('PASS: live readback, no writes, unchanged-state focus, edit guards, profile/note preservation, stale-read rejection, reconnect');
})().catch(error=>{console.error(error);process.exitCode=1;});
