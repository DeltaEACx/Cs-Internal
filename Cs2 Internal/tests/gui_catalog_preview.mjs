// Isolated static-Rust-in-DLL proof. This never opens or injects a game process.
import { spawn } from 'node:child_process';
import { createRequire } from 'node:module';
import { writeFile, readdir } from 'node:fs/promises';
import { resolve } from 'node:path';
import assert from 'node:assert/strict';
import net from 'node:net';

const require=createRequire(new URL('../gui/package.json',import.meta.url));
const {chromium}=require('playwright-core');
const catalogArguments=process.argv.slice(2).filter(argument=>argument!=='--current-css');
assert(catalogArguments[0], 'Provide the isolated host executable as the first argument.');
const executable=resolve(catalogArguments[0]);
const dll=resolve(catalogArguments[1]||'bin/x64/Release/tests/embedded/gui_embedded_probe.dll');
const report={source:'Standalone host -> LoadLibrary probe DLL -> statically linked Tauri/Wry -> direct C ABI -> production C++ DrawPage on WARP',tests:[]};
const delay=ms=>new Promise(resolve=>setTimeout(resolve,ms));
const freePort=()=>new Promise((resolve,reject)=>{const server=net.createServer();server.once('error',reject);server.listen(0,'127.0.0.1',()=>{const port=server.address().port;server.close(()=>resolve(port));});});
const flatten=list=>(list||[]).flatMap(control=>[control,...flatten(control.children)]);

async function host({unconfigured=false,failRuntime=false,failProfile=false,earlyStop=false}={}){
  const startedAt=performance.now();
  const port=await freePort();const tag=`${process.pid}-${Date.now()}-${port}`;
  const env={...process.env,WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS:`--remote-debugging-address=127.0.0.1 --remote-debugging-port=${port}`,WEBVIEW2_USER_DATA_FOLDER:resolve(`.asset-build/embedded-webview-${tag}`)};
  if(failRuntime)env.WEBVIEW2_BROWSER_EXECUTABLE_FOLDER=resolve('.asset-build/nonexistent-webview-runtime');
  if(failProfile){
    const invalidProfile=resolve(`.asset-build/embedded-invalid-profile-${tag}.txt`);
    await writeFile(invalidProfile,'Isolated WebView profile-path failure fixture.\n',{flag:'wx'});
    env.WEBVIEW2_USER_DATA_FOLDER=invalidProfile;
  }
  const child=spawn(executable,[dll,...unconfigured?['--unconfigured-dpi']:[],...earlyStop?['--early-stop']:[]],{cwd:process.cwd(),windowsHide:true,env,stdio:['pipe','pipe','pipe']});
  let id=0,buffer='',stderr='',exited=false,exitCode=null;const pending=new Map();const events=[];
  let readyResolve,readyReject;const ready=new Promise((resolve,reject)=>{readyResolve=resolve;readyReject=reject});
  child.stdout.on('data',bytes=>{buffer+=bytes;for(;;){const end=buffer.indexOf('\n');if(end<0)break;const line=buffer.slice(0,end);buffer=buffer.slice(end+1);let event;try{event=JSON.parse(line)}catch{continue}events.push(event);
    if(event.event==='ready')readyResolve(event);if(event.event==='error')readyReject(new Error(event.error));if(event.event==='response'&&pending.has(event.id)){const request=pending.get(event.id);pending.delete(event.id);clearTimeout(request.timer);request.resolve(event);}
  }});
  child.stderr.on('data',bytes=>stderr+=bytes);
  const exit=new Promise(resolve=>child.on('exit',(code,signal)=>{exited=true;exitCode=code;const error=new Error(`Embedded host exited ${code}/${signal}: ${stderr.slice(-2000)}`);readyReject(error);for(const request of pending.values()){clearTimeout(request.timer);request.reject(error)}pending.clear();resolve({code,signal})}));
  child.on('error',readyReject);
  const command=(op,extra={})=>new Promise((resolve,reject)=>{if(exited)return reject(new Error(`Host exited ${exitCode}`));const sequence=++id;const timer=setTimeout(()=>{pending.delete(sequence);reject(new Error(`Host ${op} timed out: ${stderr.slice(-1000)}`))},15000);pending.set(sequence,{resolve,reject,timer});child.stdin.write(JSON.stringify({id:sequence,op,...extra})+'\n')});
  const baseline=await Promise.race([ready,delay(10000).then(()=>{throw new Error('Host startup timed out')})]);
  const poll=async(predicate,timeout=45000)=>{const start=Date.now();let state;while(Date.now()-start<timeout){state=(await command('state')).state;if(predicate(state))return state;await delay(80)}throw new Error(`Embedded state condition timed out: ${JSON.stringify(state)} ${stderr.slice(-1500)}`)};
  const connect=async()=>{const start=Date.now();while(Date.now()-start<45000){try{const browser=await chromium.connectOverCDP(`http://127.0.0.1:${port}`);let page;for(let attempt=0;attempt<50;attempt++){page=browser.contexts().flatMap(context=>context.pages()).find(candidate=>/tauri\.localhost|tauri:\/\//.test(candidate.url()));if(page)break;await delay(100)}if(page)return {browser,page};await browser.close()}catch{}await delay(100)}throw new Error(`WebView CDP unavailable: ${stderr.slice(-1500)}`)};
  const quit=async()=>{if(exited)return;await command('quit');child.stdin.end();const result=await Promise.race([exit,delay(10000).then(()=>{throw new Error('Host did not return normally')})]);assert.equal(result.code,0,stderr)};
  return {baseline,command,poll,connect,quit,exit,child,events,startedAt,get stderr(){return stderr},get exited(){return exited}};
}

function preserved(state,stage){assert.equal(state.rawInputPreserved,true,`${stage}: host raw input registrations changed`);assert.equal(state.dpiPreserved,true,`${stage}: host DPI context changed`);assert.equal(state.timeouts,0,`${stage}: native callback timed out`);assert.equal(state.afterStop,0,`${stage}: native callback invoked after run returned`)}



const instance=await host();let browser;
try {
 await instance.poll(state=>state.running&&state.window);
 const connected=await instance.connect();browser=connected.browser;const page=connected.page;page.setDefaultTimeout(15000);
 const errors=[];page.on('pageerror',error=>errors.push(error.message));
 await page.waitForFunction(()=>typeof window.__TAURI__?.core?.invoke==='function');
 await page.waitForFunction(()=>performance.getEntriesByName('deltaeac:first-settings','mark').length>0);
 report.startup={hostToFirstSettingsObservedMs:performance.now()-instance.startedAt,
  ...await page.evaluate(()=>({hidden:document.hidden,stagesMs:Object.fromEntries(
   ['loading-frame','gui-ready','native-ready','first-settings'].map(name=>[name,performance.getEntriesByName(`deltaeac:${name}`,'mark')[0]?.startTime??null]))}))};
 if(process.argv.includes('--current-css')){const css=(await readdir('gui/dist/assets')).find(name=>name.endsWith('.css'));await page.addStyleTag({path:resolve('gui/dist/assets',css)});}
 await page.getByRole('button',{name:'Skins',exact:true}).click();
 await page.getByRole('button',{name:'Weapons',exact:true}).click();
 await page.locator('.skin-gallery .catalog-card').first().waitFor();
 assert.equal(await page.locator('.sidebar kbd').count(),0);
 assert.equal(await page.locator('.nav-item svg').count(),0);
 const tiles=await page.locator('.skin-gallery .catalog-card').count();assert(tiles>10);
 const geometry=await page.locator('.skin-gallery .catalog-card img').first().boundingBox();assert(geometry.height>=90);
 await page.screenshot({path:'tests/artifacts/tokyo/catalog-after.png'});
 const search=page.getByRole('searchbox',{name:'Search collection...',exact:true});
 await search.fill('AK-47');await search.press('Enter');
 await page.waitForFunction(()=>[...document.querySelectorAll('.skin-gallery .catalog-card')].every(card=>card.textContent.includes('AK-47')));
 await page.getByRole('button',{name:'AK-47',exact:true}).click();
 await page.getByRole('button',{name:'Back to items',exact:true}).waitFor();
 await page.waitForFunction(()=>document.querySelector('.skin-gallery .catalog-card')?.getAttribute('aria-label')==='AK-74 Wings');
 await page.getByRole('button',{name:'AK-47 | Red Laminate',exact:true}).click();
 await page.locator('.skin-editor').waitFor();
 await page.getByRole('slider',{name:'Wear',exact:true}).waitFor();
 const positions=await page.evaluate(()=>({editor:document.querySelector('.skin-editor').getBoundingClientRect().top,gallery:document.querySelector('.skin-gallery').getBoundingClientRect().top,footer:document.querySelector('.skin-pager').getBoundingClientRect().bottom,overflow:document.documentElement.scrollWidth>innerWidth}));
 assert(Math.abs(positions.editor-positions.gallery)<2);assert(!positions.overflow);assert(positions.footer<=720);
 const invoke=request=>page.evaluate(request=>window.__TAURI__.core.invoke('bridge_request',{request}),request);
 const before=flatten((await invoke({op:'state',page:0})).page.controls).find(control=>control.label==='Wear');
 const wear=page.getByRole('spinbutton',{name:'Wear value',exact:true});await wear.fill('0.25');await wear.press('Enter');
 await page.waitForFunction(()=>document.querySelector('.skin-editor input[type=number]')?.value==='0.2500');
 await delay(150);assert.equal(flatten((await invoke({op:'state',page:0})).page.controls).find(control=>control.id===before.id).value,.25);
 await invoke({op:'set',page:0,id:before.id,value:before.value});
 await page.screenshot({path:'tests/artifacts/tokyo/catalog-editor-after.png'});
 for(const [width,height] of [[880,640],[640,480]]){
  await page.setViewportSize({width,height});
  const fit=await page.evaluate(()=>({overflow:document.documentElement.scrollWidth>innerWidth,preview:document.querySelector('#character-preview').getBoundingClientRect().left,workspace:document.querySelector('.workspace').getBoundingClientRect().right,body:document.querySelector('.skin-body').clientHeight,pager:document.querySelector('.skin-pager').getBoundingClientRect().bottom}));
  assert(!fit.overflow);assert(fit.preview>=fit.workspace-1);assert(fit.body>100);assert(fit.pager<=height);
  await page.screenshot({path:`tests/artifacts/tokyo/catalog-${width}.png`});
 }
 await page.setViewportSize({width:1080,height:720});

 await page.getByRole('button',{name:'Close',exact:true}).click();await page.locator('.skin-editor').waitFor({state:'detached'});
 await page.getByRole('button',{name:'Back to items',exact:true}).click();
 await page.getByRole('button',{name:'Next',exact:true}).click();await delay(150);await page.getByRole('button',{name:'Previous',exact:true}).click();
 for(const tab of ['Knives','Gloves','Agents','Custom Agents']){await page.getByRole('button',{name:tab,exact:true}).click();await page.locator('.skin-browser').waitFor();assert(await page.locator('#character-preview').isVisible());}
 assert.deepEqual(errors,[]);
 console.log('PASS: real catalog grid, search, item/finish selection, docked editor, wear roundtrip, close/back/paging and all five skin tabs; no INS or navigation icons.');
} finally {if(browser)await browser.close();await instance.quit();}
