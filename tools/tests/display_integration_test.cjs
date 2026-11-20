const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),os=require('node:os');
const {spawnSync}=require('node:child_process');
const {harness,defaults}=require('./display_resolution_editor_test.cjs');
const root=path.resolve(__dirname,'../..');
const exe=path.join(root,'bin/tests/display-integration/display-integration-test.exe');
const directory=fs.mkdtempSync(path.join(os.tmpdir(),'asb-display-integration-'));
const disk=path.join(directory,'disk.vhdx'),settings=path.join(directory,'display_settings.json');
let sequence=0;
function native(action='read',command,vmDisk=disk) {
 const args=[action,vmDisk];
 if(command) {const file=path.join(directory,'command-'+(++sequence)+'.json');fs.writeFileSync(file,JSON.stringify(command));args.push(file);}
 const result=spawnSync(exe,args,{encoding:'utf8',timeout:20000});
 assert.equal(result.status,0,result.stderr||result.error?.message);return JSON.parse(result.stdout);
}
function editor(snapshot) {const h=harness({displayResolution:snapshot.resolution});h.open();return h;}
const windows=[];
try {
 // Legacy independent options survive resolution editing, pinning and reminder updates.
 fs.writeFileSync(settings,JSON.stringify({transmitKeyboardHotkeys:true,shareClipboard:false,future:{text:'preserve me'}}));
 let saved=native();assert.deepEqual(saved.resolution,defaults);assert(!saved.pin && !saved.reminder);
 assert.deepEqual(saved.windowed,[1920,1080]);assert.deepEqual(saved.fullscreen,[3840,2160]);
 const h=editor(saved);windows.push(h);
 h.change('windowed',640,480);assert(h.el('edit-windowed-small').textContent && h.enabled());
 h.el('edit-fullscreen-override').checked=true;h.event('edit-fullscreen-override','change');
 h.change('fullscreen',3440,1440);
 // Independent preference updates while editor's expected resolution remains unchanged.
 assert(native('preferences').pin);
 let command=h.save();saved=native('edit',command);
 assert(saved.ok && saved.pin && saved.reminder && saved.hotkeys && !saved.clipboard);
 assert.deepEqual(saved.windowed,[640,480]);assert.deepEqual(saved.fullscreen,[3440,1440]);
 assert.deepEqual(JSON.parse(fs.readFileSync(settings,'utf8')).future,{text:'preserve me'});
 // Fresh DOM + native process reload the exact saved custom dimensions.
 const reopened=editor(native());windows.push(reopened);
 assert.equal(reopened.el('edit-windowed-width').value,'640');
 assert.equal(reopened.el('edit-fullscreen-width').value,'3440');
 const stale=editor(saved);windows.push(stale);stale.change('windowed',1280,720);
 reopened.el('edit-fullscreen-override').checked=false;reopened.event('edit-fullscreen-override','change');
 saved=native('edit',reopened.save());assert(saved.ok && !saved.resolution.fullscreenOverride);
 assert.equal(saved.resolution.fullscreenWidth,3440);assert.deepEqual(saved.fullscreen,[3840,2160]);
 const bytes=fs.readFileSync(settings,'utf8');
 assert(!native('edit',stale.save()).ok);assert.equal(fs.readFileSync(settings,'utf8'),bytes);
 // Frontend blocks invalid edits; native validation also rejects a forged command.
 const invalid=editor(saved);windows.push(invalid);invalid.change('windowed','',480);
 assert(!invalid.enabled());assert.equal(invalid.save(),undefined);
 assert(!native('edit',{resolution:{...saved.resolution,windowedWidth:640.5},expected:saved.resolution}).ok);
 assert.equal(fs.readFileSync(settings,'utf8'),bytes);
 const other=path.join(directory,'other');fs.mkdirSync(other);
 const isolated=native('read',null,path.join(other,'disk.vhdx'));
 assert.deepEqual(isolated.resolution,defaults);assert(!isolated.pin && !isolated.reminder);
 assert(!fs.existsSync(path.join(other,'display_settings.json')));
 console.log('PASS integrated editor -> native command -> atomic storage -> fresh viewer/catalog/CPU policy, fullscreen restoration, stale edits and VM isolation');
} finally {
 for(const h of windows)h.close();
 const resolved=fs.realpathSync(directory);
 assert.equal(path.dirname(resolved),fs.realpathSync(os.tmpdir()));
 assert(path.basename(resolved).startsWith('asb-display-integration-'));
 fs.rmSync(resolved,{recursive:true,force:true});
}
