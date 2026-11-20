const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'../..');
function project(name) {
 const text=fs.readFileSync(path.join(root,name),'utf8'),map=new Map();
 for(const [,kind,file] of text.matchAll(/<(ClCompile|ClInclude) Include="([^"]+)"/g)) {
  const key=kind+':'+file.toLowerCase();assert(!map.has(key),`${name}: duplicate ${file}`);map.set(key,file);
  assert(fs.existsSync(path.resolve(root,path.dirname(name),file)),`${name}: missing ${file}`);
 }
 return {text,map};
}
function registered(p,kind,file) {assert(p.map.has(kind+':'+file.toLowerCase()),`unregistered ${kind} ${file}`);}
const app=project('AppSandbox.vcxproj'),core=project('AppSandboxCore.vcxproj'),driver=project('tools/vdd/AppSandboxVDD.vcxproj');
for(const file of ['display_capabilities_json','display_resolution_command']) {
 registered(app,'ClCompile',`src\\app_win\\${file}.c`);registered(app,'ClInclude',`src\\app_win\\${file}.h`);
}
for(const file of ['display_settings','display_modes']) {
 registered(core,'ClCompile',`src\\backend_win\\${file}.c`);registered(core,'ClInclude',`src\\backend_win\\${file}.h`);
}
for(const file of ['windowed_resolution','fullscreen_resolution','fullscreen_request','agent_overlay'])
 registered(app,'ClInclude',`src\\backend_win\\${file}.h`);
registered(app,'ClInclude','tools\\transport\\asb_display_resize.h');
registered(core,'ClInclude','tools\\transport\\asb_display_resolutions.h');
for(const file of ['asb_display_resize','asb_display_resolutions'])registered(driver,'ClInclude',`..\\transport\\${file}.h`);
const filters=project('AppSandbox.vcxproj.filters');
assert.deepEqual([...filters.map.keys()].sort(),[...app.map.keys()].sort(),'App project/filter registrations differ');
const defined=new Set([...filters.text.matchAll(/<Filter Include="([^"]+)"/g)].map(m=>m[1]));
for(const [,filter]of filters.text.matchAll(/<Filter>([^<]+)<\/Filter>/g))assert(defined.has(filter),`Undefined filter ${filter}`);
const html=fs.readFileSync(path.join(root,'web/index.html'),'utf8');
assert.match(html,/<script src="display-resolution\.js"><\/script>/);
assert(html.indexOf('display-resolution.js')<html.indexOf('src="app.js"'));
const overlay=fs.readFileSync(path.join(root,'web/agent-overlay.html'),'utf8');
const ids=[...overlay.matchAll(/data-option="(\d+)"/g)].map(m=>Number(m[1]));
assert.deepEqual(ids,[5,0,1,2,3,4]);assert.equal(new Set(ids).size,ids.length);
assert.match(overlay,/s\.windowsControls===true/);
const header=fs.readFileSync(path.join(root,'src/backend_win/agent_overlay.h'),'utf8');
assert.match(header,/AGENT_VIEWER_OPTION_PIN = 5, AGENT_VIEWER_OPTIONS_QUERY = 6/);
const viewer=fs.readFileSync(path.join(root,'src/backend_win/vm_display_idd.c'),'utf8');
assert.match(viewer,/#include "fullscreen_request\.h"/);assert(!viewer.includes('SendMessageTimeoutW'));
const mac=fs.readFileSync(path.join(root,'src/backend_mac/agent_overlay.m'),'utf8');
assert(!mac.includes('windowsControls') && !mac.includes('fullscreenPinned'));
console.log('PASS display source registration: app/core/VDD files, complete app filters, shared HTML assets, stable option IDs and platform capability boundary');
