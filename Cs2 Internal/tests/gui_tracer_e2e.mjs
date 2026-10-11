// Embedded WebView + real native menu/request queue. No game process or hooks.
import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createRequire} from 'node:module';
import {createServer} from 'node:net';
import {mkdir, writeFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import {pages as menuPages, groups as menuGroups} from '../gui/src/pages.js';
const require = createRequire(new URL('../gui/package.json', import.meta.url));
const {chromium} = require('playwright-core');
const root = fileURLToPath(new URL('../', import.meta.url));
const artifacts = process.env.DELTAEAC_TEST_ARTIFACTS || path.join(root, 'tests/artifacts/tokyo');
await mkdir(artifacts, {recursive:true});
const port = await new Promise(resolve => {
  const server = createServer(); server.listen(0, '127.0.0.1', () => { const port=server.address().port; server.close(() => resolve(port)); });
});
const hostStartedAt=performance.now();
const host = spawn(path.join(root, 'bin/x64/Release/tests/embedded/gui_tracer_host.exe'),
  [path.join(root, 'bin/x64/Release/tests/embedded/gui_embedded_probe.dll')], {
  cwd:root, windowsHide:true, env:{...process.env,
    WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS:`--remote-debugging-port=${port} --remote-debugging-address=127.0.0.1`,
    WEBVIEW2_USER_DATA_FOLDER:path.join(artifacts, `webview-profile-${Date.now()}`)},
});
let output='', stderr='', nextId=0, finished, ready;
const requests=new Map(), errors=[];
const readyPromise=new Promise(resolve => ready=resolve);
const exitPromise=new Promise(resolve => host.once('exit',(code,signal)=>resolve({code,signal})));
host.stderr.on('data', data => stderr+=data);
host.stdout.on('data', data => {
  output+=data;
  for(let index; (index=output.indexOf('\n'))>=0;) {
    const line=output.slice(0,index); output=output.slice(index+1);
    let value; try {value=JSON.parse(line);} catch {continue;}
    if(value.event==='ready')ready(value);
    if(value.event==='finished')finished=value.state;
    if(value.error)errors.push(value.error);
    if(value.id&&requests.has(value.id)){requests.get(value.id)(value.state);requests.delete(value.id);}
  }
});
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const command=(op='state',values={})=>new Promise((resolve,reject)=>{
  const id=++nextId, timer=setTimeout(()=>{requests.delete(id);reject(new Error(`Host command timed out: ${op}`));},5000);
  requests.set(id,value=>{clearTimeout(timer);resolve(value);});host.stdin.write(JSON.stringify({id,op,...values})+'\n');
});
async function until(probe,label,timeout=15000){
  const deadline=Date.now()+timeout;let value;
  while(Date.now()<deadline){value=await probe();if(value)return value;await delay(60);}
  throw new Error(`Timed out: ${label}`);
}
function find(node,label,type){
  if(!node||typeof node!=='object')return undefined;
  if(node.label===label&&node.type===type)return node;
  for(const value of Object.values(node)){const found=find(value,label,type);if(found)return found;}
}
const checks=[], report={implementation:'Embedded Tauri WebView + production native request queue/DrawPage on WARP',checks};
let browser,page;
try {
  await Promise.race([readyPromise,exitPromise.then(value=>{throw new Error(`Host exited early: ${JSON.stringify(value)} ${stderr}`);})]);
  await until(async()=>{try{browser=await chromium.connectOverCDP(`http://127.0.0.1:${port}`);return true;}catch{return false;}},'WebView CDP');
  page=await until(async()=>browser.contexts().flatMap(context=>context.pages()).find(page=>page.url().includes('tauri.localhost')||page.url().startsWith('tauri://')),'Tauri page');
  page.on('pageerror',error=>errors.push(String(error)));
  page.setDefaultTimeout(10000);
  const readyState=await until(async()=>{const state=await command();return state.guiReady&&state.window&&state.running===1&&state;},'GUI-ready handshake');
  if(!readyState.visible)await command('toggle');
  const initial=await until(async()=>{const state=await command();return state.visible&&state.sessionVisible&&state;},'show GUI');
  const window=initial.window; assert.equal(initial.rawInputPreserved,true);
  await page.locator('#startup-loading').waitFor({state:'visible'});
  await command('features_stage',{progress:40,message:'Loading native feature details'});
  await page.waitForFunction(()=>document.querySelector('#startup-track').getAttribute('aria-valuenow')==='40');
  const loader=await page.evaluate(()=>{
    const brand=document.querySelector('.startup-brand'),track=getComputedStyle(document.querySelector('.startup-track'));
    return {title:document.querySelector('.startup-title').textContent,messageHidden:document.querySelector('#startup-message').classList.contains('sr-only'),brand:brand.textContent,animation:getComputedStyle(brand).animationName,trackHeight:track.height,blocked:document.querySelector('#content').inert,contentVisibility:getComputedStyle(document.querySelector('#content')).visibility};
  });
  assert.deepEqual(loader,{title:'Loading',messageHidden:true,brand:'DeltaEAC',animation:'none',trackHeight:'2px',blocked:true,contentVisibility:'hidden'});
  await delay(400);
  await page.screenshot({path:path.join(artifacts,'loading-clean.png')});
  checks.push('Clean loading screen: static DeltaEAC branding, one Loading label, thin native progress bar; controls blocked until features ready');
  const featuresReadyStartedAt=performance.now();
  await command('features_ready');
  await page.locator('#startup-loading').waitFor({state:'hidden'});
  await page.waitForFunction(()=>performance.getEntriesByName('deltaeac:first-settings','mark').length>0);
  report.startup={hostToFirstSettingsObservedMs:performance.now()-hostStartedAt,
    featuresReadyToSettingsObservedMs:performance.now()-featuresReadyStartedAt,
    ...await page.evaluate(()=>({hidden:document.hidden,stagesMs:Object.fromEntries(
      ['loading-frame','gui-ready','native-ready','first-settings'].map(name=>
        [name,performance.getEntriesByName(`deltaeac:${name}`,'mark')[0]?.startTime??null]))}))};
  assert(report.startup.featuresReadyToSettingsObservedMs<600,
    `Native readiness took ${report.startup.featuresReadyToSettingsObservedMs.toFixed(1)}ms to reach usable settings`);
  checks.push('Native features-ready reaches usable settings promptly; packaged startup stage timings recorded');
  const invoke=request=>page.evaluate(request=>window.__TAURI__.core.invoke('bridge_request',{request}),request);
  const navigate=async id=>{
    const destination=menuPages.find(entry=>entry.id===id);assert(destination,`Missing visible page ${id}`);
    await page.locator('#main-nav').getByRole('button',{name:menuGroups[destination.group],exact:true}).click();
    await page.locator('#sub-nav').getByRole('button',{name:destination.label,exact:true}).click();
  };
  const navigation=await invoke({op:'state',page:23});assert.equal(navigation.ok,true,navigation.error);
  assert.deepEqual(navigation.pages,menuPages);assert.deepEqual(navigation.groups,menuGroups.map((label,id)=>({id,label})));
  assert(!navigation.pages.some(entry=>entry.id===29));
  const flatten=list=>(list||[]).flatMap(control=>[control,...flatten(control.children)]);
  for(const id of [23,24,25]){
    const state=await invoke({op:'state',page:id});assert.equal(state.ok,true,state.error);
    assert(!flatten(state.page.controls).some(control=>control.type==='text'&&control.disabled),`Diagnostics appeared on page ${id}`);
  }
  for(const [id,masterLabel,settingLabel] of [[23,'Enable ragebot','Automatic fire'],[25,'Enable movement assists','Directional steering']]){
    await navigate(id);
    const state=await invoke({op:'state',page:id}),master=find(state,masterLabel,'bool'),setting=find(state,settingLabel,'bool');
    assert(master&&setting,`Missing feature controls on page ${id}`);
    const toggle=page.getByRole('switch',{name:masterLabel,exact:true});await toggle.waitFor();
    if(master.value)await toggle.click();
    await until(async()=>find(await invoke({op:'state',page:id}),masterLabel,'bool')?.value===false,`${masterLabel} disabled`);
    for(let poll=0;poll<4;poll++)assert.equal(find(await invoke({op:'state',page:id}),masterLabel,'bool')?.value,false,'Native state poll re-enabled a feature');
    await page.getByRole('switch',{name:settingLabel,exact:true}).click();
    await until(async()=>find(await invoke({op:'state',page:id}),settingLabel,'bool')?.value===!setting.value,`${settingLabel} saved while disabled`);
    assert.equal(find(await invoke({op:'state',page:id}),masterLabel,'bool')?.value,false,'Editing another setting re-enabled a feature');
    await delay(1000);assert.equal(await toggle.getAttribute('aria-checked'),'false','Frontend polling re-enabled a feature');
    for(const control of [setting,master])assert.equal((await invoke({op:'set',page:id,id:control.id,value:control.value})).ok,true);
  }
  checks.push('Visible navigation matches native metadata and excludes unavailable Legit; rage/movement enable switches stay off across native/frontend polls and setting edits; diagnostics absent and original settings restored');
  await navigate(11);
  const tracer=page.getByRole('switch',{name:'Bullet Tracers',exact:true});
  await tracer.waitFor();assert.equal(await tracer.getAttribute('aria-checked'),'false');
  await page.getByRole('button',{name:'Bullet Tracers options',exact:true}).click();
  const popup=page.getByRole('dialog',{name:'Bullet Tracers options',exact:true});
  await popup.waitFor();
  await popup.getByRole('combobox',{name:'Style',exact:true}).selectOption({label:'Dashed'});
  await popup.getByRole('spinbutton',{name:'Width value',exact:true}).fill('4.5');
  await popup.getByRole('spinbutton',{name:'Width value',exact:true}).press('Enter');
  await popup.getByRole('spinbutton',{name:'Duration value',exact:true}).fill('2.5');
  await popup.getByRole('spinbutton',{name:'Duration value',exact:true}).press('Enter');
  await popup.getByRole('switch',{name:'Fade',exact:true}).click();
  const nativeState=()=>page.evaluate(()=>window.__TAURI__.core.invoke('bridge_request',{request:{op:'state',page:11}}));
  await until(async()=>{const state=await nativeState();const options=find(state,'Bullet Tracers','popup'),style=find(options,'Style','select');return options&&find(options,'Width','number')?.value===4.5&&find(options,'Duration','number')?.value===2.5&&style?.value===style?.options.find(option=>option.label==='Dashed')?.value&&find(options,'Fade','bool')?.value===false;},'popup settings reach native renderer');
  await popup.getByRole('button',{name:'Color color',exact:true}).click();
  const colorPopup=page.getByRole('dialog',{name:'Color options',exact:true});
  for(const [channel,value] of [['red','64'],['green','128'],['blue','192'],['alpha','128']]){
    const field=colorPopup.getByRole('spinbutton',{name:`Color ${channel}`,exact:true});await field.fill(value);await field.press('Tab');
  }
  await until(async()=>{const color=find(find(await nativeState(),'Bullet Tracers','popup'),'Color','color')?.value;return color&&color.every((value,index)=>Math.abs(value-[64,128,192,128][index]/255)<.00001);},'color channels reach native renderer');
  await colorPopup.getByRole('button',{name:'Close options',exact:true}).click();
  await page.getByRole('button',{name:'Bullet Tracers options',exact:true}).click();
  await delay(250);
  await page.screenshot({path:path.join(artifacts,'bullet-tracers.png')});
  await popup.getByRole('button',{name:'Close options',exact:true}).click();
  await tracer.click();
  await until(async()=>find(await nativeState(),'Bullet Tracers','bool')?.value===true,'tracer enabled');
  checks.push('Bullet Tracers three-dot settings work while disabled; color, style, width, duration and fade persist through native bridge; feature toggle enables tracers');
  assert.equal(await page.locator('.window-actions button').count(),1);
  assert.equal(await page.locator('#hide-window').count(),0);
  async function minimized(label){
    const state=await until(async()=>{const state=await command();return state.minimized&&!state.sessionVisible&&state;},label);
    assert.equal(state.window,window);assert.equal(state.running,1);assert.equal(state.rawInputPreserved,true);
  }
  async function restore(){
    await command('toggle');const state=await until(async()=>{const state=await command();return state.visible&&state.sessionVisible&&state;},'restore same GUI');
    assert.equal(state.window,window);assert.equal(state.rawInputPreserved,true);
  }
  await page.getByRole('button',{name:'Minimize window',exact:true}).click();await minimized('minimize button');await restore();
  await page.evaluate(()=>window.__TAURI__.window.getCurrentWindow().close());await minimized('Tauri close request');await restore();
  await command('close');await minimized('native WM_CLOSE');await restore();
  await command('toggle');await minimized('Insert-style toggle');await restore();
  checks.push('Only minimize button exists; button, Tauri close, WM_CLOSE and Insert-style toggle preserve same window/runtime; restore succeeds and input lease follows visibility');
  assert.deepEqual(errors,[]); report.ok=true;report.liveGameplayTested=false;
} catch(error){report.ok=false;report.error=String(error);report.errors=errors;report.stderr=stderr;report.targets=browser?.contexts().flatMap(context=>context.pages()).map(target=>target.url())||[];report.nativeState=await command().catch(error=>({error:String(error)}));}
finally {
  if(page&&report.ok===false)await page.screenshot({path:path.join(artifacts,'tracer-e2e-failure.png')}).catch(()=>{});
  await command('quit').catch(()=>{});host.stdin.end();
  const exit=await Promise.race([exitPromise,delay(10000).then(()=>({timeout:true}))]);report.hostExit=exit;report.finished=finished;
  if(browser)await browser.close().catch(()=>{});
  if(exit.timeout){host.kill();report.ok=false;report.error||='Isolated host did not stop';}
  if(exit.code!==0){report.ok=false;report.error||='Isolated host exited with an error';}
}
await writeFile(path.join(artifacts,'tracer-gui-checks.json'),JSON.stringify(report,null,2)+'\n');
console.log(JSON.stringify(report,null,2));process.exitCode=report.ok?0:1;
