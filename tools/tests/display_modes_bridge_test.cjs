const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const repo = path.resolve(__dirname, '../..');
const read = name => fs.readFileSync(path.join(repo, name), 'utf8');
const source = read('web/app.js');
const handler = source.slice(source.indexOf('window.onHostMessage = function(msg) {'), source.indexOf('\n};', source.indexOf('window.onHostMessage = function(msg) {')) + 3);
const start = source.indexOf('function openEditVmModal(idx) {');
const open = source.slice(start, source.indexOf('\n}\n', start) + 2);
assert(handler && open);
const elements = new Map();
const commands = [];
let updates = 0;
const initial = { id: 'vm-1', osType: 'windows11', name: 'Guest', ramMb: 4096, cpuCores: 2, networkMode: 1,
    windowedWidth: 5120, windowedHeight: 2880, fullscreenWidth: 3840, fullscreenHeight: 2160 };
const context = vm.createContext({
    DisplayResolution: require('../../web/display-resolution.js'),
    setEditDisplayResolution() {},
    editDisplayAvailable(vm) {return !context.hostBridge.isMac && vm.osType === 'windows11';},
    window: {}, hostBridge: {isMac: false}, hostDisplayCapabilities: null, editVmState: null,
    vms: [initial], rowCache: {'vm-1': {querySelector() { return {}; }}},
    sendCmd(action) { commands.push(action); },
    updateEditVmModal() { ++updates; }, setGpuSelection() {},
    document: {getElementById(id) {
        if (!elements.has(id)) elements.set(id, {value: '', classList: {add() {}}, focus() {}});
        return elements.get(id);
    }}
});
vm.runInContext(handler + '\n' + open, context);
context.openEditVmModal(0);
assert.deepEqual(commands, ['getDisplayCapabilities','getDisplaySettings']);
assert.equal(elements.get('edit-ram-size').value, 4096);
const saved = JSON.stringify(context.editVmState.initial);
const success = {type: 'displayCapabilities', ok: true, errorCode: 0,
    monitors: [{device:'display1', modes:[{width:1920,height:1080}]}], presets:[{width:1920,height:1080}]};
context.window.onHostMessage(success);
assert.equal(context.hostDisplayCapabilities, success);
const failure = {type: 'displayCapabilities', ok: false, errorCode: -1, monitors: [], presets: []};
context.window.onHostMessage(failure);
assert.equal(context.hostDisplayCapabilities, failure);
assert.equal(JSON.stringify(context.editVmState.initial), saved);
assert.equal(initial.windowedWidth, 5120); // unavailable saved size remains unchanged
assert.equal(elements.get('edit-ram-size').value, 4096);
assert.equal(updates, 3); // opening, success, failure
context.openEditVmModal(0);
assert.equal(commands.length, 4); // every opening requests a fresh snapshot
const pending = context.hostDisplayCapabilities;
assert.equal(pending.errorCode, null); // opening clears the stale snapshot while refreshing
context.hostBridge.isMac = true;
context.openEditVmModal(0);
context.window.onHostMessage(success);
assert.equal(commands.length, 4);
assert.equal(context.hostDisplayCapabilities, pending);
context.hostBridge.isMac = false;
initial.osType = 'ubuntu2604lts';
context.openEditVmModal(0);
assert.equal(commands.length, 4);
const ui = read('src/app_win/ui.c');
assert.match(ui, /wcscmp\(action, L"getDisplayCapabilities"\) == 0\) \{\s*send_display_capabilities\(\);/);
assert.match(ui, /case WM_DISPLAYCHANGE:\s*if \(webview2_is_ready\(\)\) send_display_capabilities\(\);/);
assert.match(ui, /case WM_DEVICECHANGE:\s*if \(wp != DBT_DEVNODES_CHANGED\) break;\s*if \(webview2_is_ready\(\)\) send_display_capabilities\(\);/);
assert.match(ui, /asb_get_host_display_capabilities\(&caps\)/);
assert.match(ui, /display_capabilities_json\(&caps, &json\)/);
assert.match(ui, /asb_free_host_display_capabilities\(&caps\)/);
const native = read('src/backend_win/display_modes.c');
assert.match(native, /EnumDisplaySettingsExW\(device, \(DWORD\)index, &mode, 0\)/);
assert(!/ChangeDisplaySettings/.test(native));
assert(!/display_settings_update/.test(native));
assert.match(read('AppSandboxCore.vcxproj'), /ClCompile Include="src\\backend_win\\display_modes.c"/);
assert.match(read('AppSandbox.vcxproj'), /ClCompile Include="src\\app_win\\display_capabilities_json.c"/);
const shared = read('tools/transport/asb_display_resolutions.h');
const entries = [...shared.matchAll(/X\((\d+), (\d+)\)/g)].map(m => [Number(m[1]),Number(m[2])]);
assert.deepEqual(entries, [[1920,1080],[1024,768],[1280,720],[1280,800],[1280,1024],[1366,768],
    [1440,900],[1600,900],[1680,1050],[1920,1200],[2560,1440],[2560,1600],[3440,1440],
    [3840,2160],[5120,1440],[5120,2880],[7680,4320]]);
assert.match(read('tools/vdd/vdd.h'), /ASB_COMMON_DISPLAY_RESOLUTIONS\(ASB_VDD_RESOLUTION\)/);
assert.match(native, /ASB_COMMON_DISPLAY_RESOLUTIONS\(COMMON_MODE\)/);
console.log('PASS display bridge: editor refresh, success/failure preserving inputs, platform gates, event wiring, shared driver presets');
