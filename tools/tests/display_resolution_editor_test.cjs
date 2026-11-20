const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const repo = path.resolve(__dirname, '../..');
const {JSDOM} = require(path.join(repo, 'bin/tests/ui-deps/node_modules/jsdom'));
const html = fs.readFileSync(path.join(repo,'web/index.html'),'utf8');
const rules = fs.readFileSync(path.join(repo,'web/display-resolution.js'),'utf8');
const app = fs.readFileSync(path.join(repo,'web/app.js'),'utf8');
const defaults = {windowedWidth:1920,windowedHeight:1080,fullscreenOverride:false,fullscreenWidth:1920,fullscreenHeight:1080};
const caps = {type:'displayCapabilities',ok:true,errorCode:0,monitors:[{device:'hd',modes:[{width:1920,height:1080}]},
    {device:'4k',modes:[{width:3840,height:2160}]}],presets:[{width:640,height:480},{width:800,height:600},
    {width:1280,height:720},{width:1920,height:1080},{width:2560,height:1440},{width:3840,height:2160}]};
function harness(patch = {}, mac = false) {
    const commands = [];
    const dom = new JSDOM(html, {runScripts:'outside-only',url:'https://editor.test/'});
    const w = dom.window;
    w.eval = script => require("node:vm").runInContext(script,dom.getInternalVMContext());
    w.chrome = {webview:{postMessage(message) {commands.push(message);},addEventListener() {}}};
    w.setTimeout = () => 0;
    w.eval(rules); w.eval(app);
    w.eval('hostBridge.isMac = '+JSON.stringify(mac));
    const vm = Object.assign({id:'vm-1',name:'Guest',osType:'windows11',ramMb:4096,cpuCores:2,
        gpuMode:0,gpuId:'',networkMode:1,displaySettingsError:0},patch);
    w.eval('vms = '+JSON.stringify([vm])+'; rowCache = {};');
    const row = w.document.createElement('tr');
    row.innerHTML = '<td><button class="edit">Edit</button></td>';
    w.document.getElementById('vm-tbody').appendChild(row);
    w.eval('rowCache["vm-1"] = document.getElementById("vm-tbody").lastChild;');
    commands.length = 0;
    const el = id => w.document.getElementById(id);
    const event = (id,type='input') => el(id).dispatchEvent(new w.Event(type,{bubbles:true}));
    function open(capabilities=caps) { w.openEditVmModal(0); if (!mac && vm.osType === 'windows11') w.onHostMessage(capabilities); }
    function change(kind,width,height) {
        el('edit-'+kind+'-preset').value = 'custom'; event('edit-'+kind+'-preset','change');
        el('edit-'+kind+'-width').value = String(width);
        el('edit-'+kind+'-height').value = String(height);
        event('edit-'+kind+'-width');
    }
    function update(patch) {w.eval('vms[0] = Object.assign({},vms[0],'+JSON.stringify(patch)+'); updateEditVmModal();');}
    function save() {w.saveEditVm(); return commands.filter(m => m.action === 'setDisplayResolution').at(-1);}
    function enabled() {return !el('btn-save-edit-vm').disabled;}
    return {w,dom,el,event,open,change,update,save,enabled,commands,close() {dom.window.close();}};
}
let h = harness(); h.open();
assert.deepEqual(h.commands.map(m=>m.action), ['getDisplayCapabilities','getDisplaySettings']);
assert.equal(h.el('edit-windowed-preset').value,'1920x1080');
assert(h.el('edit-windowed-custom').hidden);
assert.equal(h.el('edit-fullscreen-width').value,'1920');
assert.equal(h.el('edit-fullscreen-height').value,'1080');
assert(h.el('edit-fullscreen-settings').hidden && !h.el('edit-fullscreen-override').checked);
assert.equal(h.w.document.activeElement.id,'edit-ram-size');
h.el('edit-windowed-preset').value='2560x1440'; h.event('edit-windowed-preset','change');
let result = h.save();
assert.deepEqual(JSON.parse(JSON.stringify(result.resolution)), {...defaults,windowedWidth:2560,windowedHeight:1440});
assert.deepEqual(JSON.parse(JSON.stringify(result.expected)), defaults);
assert.equal(h.commands.filter(m=>m.action==='setDisplayResolution').length,1);
assert(!h.commands.some(m=>m.action==='editVm'));
assert.equal(h.w.document.activeElement.className,'edit'); h.close();
// Saved custom and fullscreen values round-trip, including an unavailable size.
h = harness({displayResolution:{...defaults,windowedWidth:5120,windowedHeight:2880,fullscreenOverride:true,fullscreenWidth:3440,fullscreenHeight:1440}});
h.open(); assert.equal(h.el('edit-windowed-preset').value,'custom');
assert.equal(h.el('edit-windowed-width').value,'5120');
assert.equal(h.el('edit-fullscreen-preset').value,'custom'); assert(!h.el('edit-fullscreen-settings').hidden);
assert(h.el('edit-windowed-limit').textContent && h.enabled());
h.change('windowed',2048,1152); result=h.save();
assert.equal(result.resolution.windowedWidth,2048); assert.equal(result.resolution.fullscreenWidth,3440); h.close();
// Out-of-list defaults stay Custom; warnings never block Save.
h = harness(); h.open({...caps,monitors:[{modes:[{width:600,height:400}]}],presets:[]});
assert.equal(h.el('edit-windowed-preset').value,'custom'); assert.equal(h.el('edit-windowed-width').value,'1920');
h.change('windowed',640,480);
assert(h.el('edit-windowed-limit').textContent && h.el('edit-windowed-small').textContent && h.enabled());
result=h.save(); assert.equal(result.resolution.windowedWidth,640); h.close();
// Failed capability checks are separate from validation and preserve raw edits.
h = harness(); h.open(); h.change('windowed',800,600);
h.w.onHostMessage({type:'displayCapabilities',ok:false,errorCode:-1,monitors:[],presets:[]});
assert.match(h.el('edit-display-capability').textContent,/could not be checked/);
assert(!h.el('edit-windowed-limit').textContent && h.el('edit-windowed-small').textContent && h.enabled());
assert.equal(h.el('edit-windowed-width').value,'800'); h.close();
// Active invalid fields block Save; disable hides unfinished fullscreen inputs.
h=harness();h.open();
for (const [width,height] of [['',1080],[1920,''],[1920.5,1080],[639,1080],[7681,1080],[1920,479],[1920,4321],['1e3',1080]]) {
    h.change('windowed',width,height);
    assert(!h.enabled(),String(width)+'x'+height);
    assert.equal(h.el('edit-windowed-width').getAttribute('aria-invalid'),'true');
    assert(!h.save());
}
h.change('windowed',7680,4320);assert(h.enabled());
h.el('edit-fullscreen-override').checked=true;h.event('edit-fullscreen-override');
h.change('fullscreen',2560,1440); assert(h.enabled());
h.change('fullscreen','',1440); assert(!h.enabled());
h.el('edit-fullscreen-override').checked=false;h.event('edit-fullscreen-override');
assert(h.enabled() && h.el('edit-fullscreen-settings').hidden);
assert(!h.el('edit-fullscreen-error').textContent);
result=h.save(); assert.equal(result.resolution.fullscreenWidth,2560);assert.equal(result.resolution.fullscreenHeight,1440);assert(!result.resolution.fullscreenOverride);h.close();
// Toggle back on restores unfinished text and validation; a refresh cannot erase it.
h=harness();h.open();h.el('edit-fullscreen-override').checked=true;h.event('edit-fullscreen-override');
h.change('fullscreen','',720);h.el('edit-fullscreen-override').checked=false;h.event('edit-fullscreen-override');assert(h.enabled());
h.el('edit-fullscreen-override').checked=true;h.event('edit-fullscreen-override');assert(!h.enabled());
h.w.onHostMessage({...caps,presets:[{width:640,height:480}]});
assert.equal(h.el('edit-fullscreen-width').value,'');assert(!h.enabled());h.close();
// Host capabilities use pair-wise union; 4K stays available alongside 1080p.
h=harness();h.open();h.change('windowed',3840,2160);assert(!h.el('edit-windowed-limit').textContent);
h.w.onHostMessage({...caps,monitors:[{modes:[{width:3840,height:1080}]},{modes:[{width:1920,height:2160}]}]});
assert(h.el('edit-windowed-limit').textContent && h.enabled());h.close();
// Untouched external edits merge; concurrent local edits conflict as one unit.
h=harness();h.open();h.update({displayResolution:{...defaults,windowedWidth:1280,windowedHeight:720}});
assert.equal(h.el('edit-windowed-width').value,'1280');assert(h.enabled());
h.change('windowed',2560,1440);
h.update({displayResolution:{...defaults,windowedWidth:3840,windowedHeight:2160}});
assert(!h.enabled());assert.match(h.el('edit-vm-warn').textContent,/another interface/);
assert.equal(h.el('edit-windowed-width').value,'2560');assert(!h.save());
h.change('windowed',3840,2160);assert(h.enabled());h.close();
// Independent external CPU/RAM updates retain local resolution edits.
h=harness();h.open();h.change('windowed',2048,1152);h.update({cpuCores:4,ramMb:8192});
assert.equal(h.el('edit-cpu-cores').value,'4');assert.equal(h.el('edit-windowed-width').value,'2048');assert(h.enabled());h.close();
for (const flag of ['running','starting','buildingVhdx']) {
    h=harness({[flag]:true});h.w.openEditVmModal(0);assert.equal(h.w.eval('editVmState'),null);h.close();
    h=harness();h.open();h.change('windowed',2048,1152);h.update({[flag]:true});assert(!h.enabled());
    assert(h.el('edit-windowed-width').disabled && h.el('edit-fullscreen-override').disabled);assert(!h.save());h.close();
}
// Read failures cannot silently overwrite a damaged preference file.
h=harness({displaySettingsError:-1});h.open();assert(!h.enabled());assert.match(h.el('edit-vm-warn').textContent,/could not be read/);h.close();
for (const [patch,mac] of [[{osType:'ubuntu2604lts'},false],[{},true],[{osType:'macos27'},true]]) {
    h=harness(patch,mac);h.open();assert(h.el('edit-display-settings').hidden);
    assert(!h.commands.some(m=>/Display/.test(m.action)));
    h.el('edit-cpu-cores').value=4;h.event('edit-cpu-cores');assert(h.enabled());h.save();
    assert(h.commands.some(m=>m.action==='editVm' && m.field==='cpuCores'));
    assert(!h.commands.some(m=>m.action==='setDisplayResolution'));h.close();
}
// Accessible associations reference real live regions, and labels name each input.
h=harness();h.open();
for (const kind of ['windowed','fullscreen']) for (const control of ['preset','width','height']) {
    const id='edit-'+kind+'-'+control;
    assert(h.w.document.querySelector('label[for="'+id+'"]'));
    const descriptions=h.el(id).getAttribute('aria-describedby').split(' ');
    for (const description of descriptions) assert.equal(h.el(description).getAttribute('aria-live'),'polite');
}
h.close();
console.log('PASS resolution editor: full DOM, presets/custom, defaults, hidden retention, warnings, validation, conflicts, focus, lifecycle/platform gates, accessibility');
