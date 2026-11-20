const assert=require('node:assert/strict');
const fs=require('node:fs'), path=require('node:path');
const {pathToFileURL}=require('node:url');
const repo=path.resolve(__dirname,'../..');
const {chromium}=require(path.join(repo,'bin/tests/ui-deps/node_modules/playwright-core'));
(async()=>{
 const browser=await chromium.launch({headless:true,executablePath:process.env.ASB_TEST_BROWSER||'C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe'});
 try {
  const page=await browser.newPage({viewport:{width:1024,height:768}});
  const errors=[];page.on('pageerror',e=>errors.push(e.message));
  await page.addInitScript(()=>{
   window.messages=[];window.deliver=null;window.chrome=window.chrome||{};
   window.chrome.webview={postMessage(m){messages.push(m)},addEventListener(t,f){window.deliver=s=>f({data:s})}};
  });
  const load=()=>page.goto(pathToFileURL(path.join(repo,'web/agent-overlay.html')).href);
  const send=s=>page.evaluate(s=>deliver(s),s);
  const state=(pin=false,full=true,windows=true,revision=1)=>send({fullscreen:full,windowsControls:windows,fullscreenPinned:pin,layoutRevision:revision});
  const visible=()=>page.locator('#fullscreen-controls').isVisible();
  const layout=()=>page.evaluate(()=>JSON.parse(messages.filter(m=>m.startsWith('layout:')).at(-1).slice(7)));
  const click=async selector=>{
   const r=await page.locator(selector).boundingBox();assert(r,selector);
   for(const kind of ['down','up']) await send({type:'input',kind,button:0,x:r.x+r.width/2,y:r.y+r.height/2});
  };
  await load();await state();assert(!await visible());
  assert.equal(await page.locator('#exit-fullscreen').getAttribute('aria-label'),'Exit fullscreen');
  assert.equal(await page.locator('#exit-fullscreen .fluent').textContent(),'\uE73F');
  assert.equal(await page.locator('#exit-fullscreen .fluent').evaluate(e=>getComputedStyle(e).fontFamily),'"Segoe Fluent Icons"');
  await send({type:'fullscreen_pointer',zone:1});await page.waitForTimeout(550);assert(!await visible());
  await send({type:'fullscreen_pointer',zone:2});await page.waitForTimeout(250);assert(!await visible());
  await send({type:'fullscreen_pointer',zone:0});await page.waitForTimeout(300);assert(!await visible());
  await send({type:'fullscreen_pointer',zone:2});await page.waitForTimeout(550);assert(await visible());
  await click('#viewer-options');await send({type:'viewer_options',enabled:63,checked:4});
  assert.equal(await page.locator('#viewer-menu button:visible').first().textContent(),'Pin fullscreen controls');
  assert(await page.locator('[data-option="2"]').evaluate(e=>e.classList.contains('checked')));
  assert(!await page.locator('[data-option="5"]').evaluate(e=>e.classList.contains('checked')));
  await click('[data-option="5"]');assert(await page.evaluate(()=>messages.includes('viewer_option:5')));
  await state(true);await send({type:'fullscreen_pointer',zone:0});assert(await visible());
  await click('#viewer-options');await send({type:'viewer_options',enabled:63,checked:32});
  assert(await page.locator('[data-option="5"]').evaluate(e=>e.classList.contains('checked')));
  for(let i=0;i<5;i++) {
   if(i) await click('#viewer-options');
   await click('[data-option="'+i+'"]');
   assert(await page.evaluate(i=>messages.includes('viewer_option:'+i),i));
  }
  await click('#viewer-options');await send({type:'input',kind:'cancel',layoutRevision:2});
  assert(await visible());assert(!await page.locator('#viewer-menu').isVisible());assert.equal((await layout()).layoutRevision,2);
  await page.setViewportSize({width:800,height:600});assert(await visible());
  const l=await layout();assert.equal(l.fw,36);assert.equal(l.bw,36);assert.equal(l.ow,0);
  // Host forwards only visible rectangles. Gap, hidden menu, and guest content cannot activate commands.
  const before=await page.evaluate(()=>messages.filter(m=>m.startsWith('viewer_option:')).length);
  for(const kind of ['down','up']) await send({type:'input',kind,button:0,x:400,y:26});
  assert.equal(await page.evaluate(()=>messages.filter(m=>m.startsWith('viewer_option:')).length),before);
  await state(false);assert(!await visible());assert.equal((await layout()).fw,0);
  await send({type:'fullscreen_pointer',zone:2});await page.waitForTimeout(550);assert(await visible());
  await send({type:'input',kind:'cancel',layoutRevision:3});assert(!await visible());
  await state(true,false);assert(!await visible());await state(true,true);assert(await visible());
  await click('#exit-fullscreen');assert(await page.evaluate(()=>messages.includes('exit_fullscreen')));
  await page.screenshot({path:path.join(repo,'bin/tests/fullscreen-controls.png')});
  // A fresh shared macOS page has no Windows capability and keeps the original glyph/menu.
  await load();await state(true,true,false);assert(!await page.locator('[data-option="5"]').isVisible());
  assert.equal(await page.locator('#exit-fullscreen svg').evaluate(e=>getComputedStyle(e).display),'block');
  assert.equal(await page.locator('#exit-fullscreen .fluent').evaluate(e=>getComputedStyle(e).display),'none');
  assert(!await visible());assert.deepEqual(errors,[]);
  const native=fs.readFileSync(path.join(repo,'src/backend_win/vm_display_idd.c'),'utf8');
  assert.match(native,/g_viewer_commands\[\]=\{IDM_SEND_SAS,IDM_MATCH_WINDOW,IDM_SHARE_CLIPBOARD,IDM_XMIT_HOTKEYS,IDM_AUDIO_MUTE\}/);
  assert.match(native,/fullscreen_pinned = settings.fullscreen_pinned/);
  assert.match(native,/asb_vm_set_fullscreen_pinned/);
  const overlay=fs.readFileSync(path.join(repo,'src/backend_win/agent_overlay.c'),'utf8');
  assert.match(overlay,/s->options_message,AGENT_VIEWER_OPTIONS_QUERY,0/);
  assert.match(overlay,/text\[14\]<=L'0'\+AGENT_VIEWER_OPTION_PIN/);
  console.log('PASS fullscreen controls: dwell, pin, checkmark, stable routing, cancellation, resize, transitions, layout, platform gating');
 } finally {await browser.close()}
})().catch(e=>{console.error(e);process.exitCode=1});
