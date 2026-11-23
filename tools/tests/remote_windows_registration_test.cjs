// Remote Windows (eh6.2): source registration, Windows-only service wiring and no public API.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'../..');
const read=f=>fs.readFileSync(path.join(root,f),'utf8');
const core=read('AppSandboxCore.vcxproj');
for(const [kind,file] of [['ClCompile','src\\backend_win\\hcs_services.c'],['ClInclude','src\\backend_win\\hcs_services.h'],
  ['ClCompile','tools\\transport\\asb_remote_windows.c'],['ClInclude','tools\\transport\\asb_remote_windows.h']]) {
 assert(core.includes(`<${kind} Include="${file}" />`),`AppSandboxCore.vcxproj must register ${file}`);
 assert(fs.existsSync(path.join(root,file)),`missing ${file}`);
}
// The VM document builds its ServiceTable through the tested builder, with room for channels 9-11.
const vm=read('src/backend_win/hcs_vm.c');
assert.match(vm,/hcs_service_table_json\(config->os_type, service_table, ARRAYSIZE\(service_table\)\)/);
assert.match(vm,/wchar_t service_table\[4096\];/);
assert(!/void hcs_service_guid\(/.test(vm),'GUID helpers live in hcs_services.c only');
assert.match(read('src/backend_win/hcs_vm.h'),/#include "hcs_services.h"/);
const services=read('src/backend_win/hcs_services.c');
assert.match(services,/ASB_CH_RW_CONTROL, ASB_CH_RW_FRAMES, ASB_CH_RW_FILES/);
// Channel numbers: existing ones unchanged, Remote Windows reserved 9/10/11.
const transport=read('tools/transport/asb_transport.h');
for(const [name,n] of [['AGENT',1],['DISPLAY',2],['INPUT',3],['AUDIO',4],['CLIPBOARD',5],['CLIPBOARD_READER',6],['SSH',7]])
 assert.match(transport,new RegExp(`#define ASB_CH_${name}\\s+${n}\\b`));
assert.match(read('tools/transport/asb_automation.h'),/#define ASB_CH_SCREENSHOT 8\b/);
const proto=read('tools/transport/asb_remote_windows.h');
assert.match(proto,/#define ASB_CH_RW_CONTROL 9\b/);assert.match(proto,/#define ASB_CH_RW_FRAMES\s+10\b/);assert.match(proto,/#define ASB_CH_RW_FILES\s+11\b/);
// Windows-host feature: the macOS ivshmem layout does not grow Remote Windows regions.
assert(!/ASB_CH_RW_|asb_remote_windows/.test(read('tools/transport/asb_shm_layout.h')));
// Internal only: nothing reaches HTTP, MCP, the operation catalogue or the Python SDK.
for(const file of ['src/app_win/headless.c','src/core/mcp_server.c','src/core/api_contract.c','src/core/api_operations.c',
  'tools/headless-api/asb.py','tools/headless-api/README.md'])
 assert(!/remote[_ ]?windows|asb_rw_|ASB_RW_|ASB_CH_RW_/i.test(read(file)),`${file} must not expose Remote Windows`);
// eh6.4: the standard-user helper builds from tested sources, is in the solution, and the service supervises it.
const helper=read('tools/agent/appsandbox-remote-windows.vcxproj');
for(const file of ['appsandbox-remote-windows.c','remote_windows_helper.c','remote_windows_share.c','remote_windows_share_win32.c',
  'remote_windows_inventory.c','remote_windows_display.c','..\\transport\\asb_remote_windows.c','..\\transport\\asb_transport.c']) {
 assert(helper.includes(`<ClCompile Include="${file}" />`),`helper project must compile ${file}`);
 assert(fs.existsSync(path.join(root,'tools/agent',file)),`missing ${file}`);
}
const sln=read('AppSandbox.sln'),guid=/<ProjectGuid>(\{[^}]+\})<\/ProjectGuid>/.exec(helper)[1];
assert(sln.includes(`"appsandbox-remote-windows", "tools\\agent\\appsandbox-remote-windows.vcxproj", "${guid}"`));
assert.equal(sln.split(`${guid} = ${guid}`).length-1,2,'AppSandbox and AppSandboxPackage depend on the helper');
for(const c of ['Debug|ARM64','Debug|x64','Release|ARM64','Release|x64']) assert(sln.includes(`${guid}.${c}.Build.0 = ${c}`));
const agent=read('tools/agent/agent.c');
assert(agent.includes('L"appsandbox-remote-windows.exe"'));
assert.match(agent,/start_rw_monitor\(\);/);assert.match(agent,/stop_rw_monitor\(\);/);
assert.match(agent,/L"\\"%s\\" --recover"/,'service runs independent recovery');
assert.match(agent,/PROC_THREAD_ATTRIBUTE_HANDLE_LIST/,'helper inherits only the lease pipe and stop event');
// eh6.5: capture engine + frame flow in the helper, host receiver in the app (with the D3D display code).
assert(helper.includes('<ClCompile Include="..\\transport\\asb_rw_frames.c" />'),'helper compiles the frame flow core');
assert.match(helper,/<ClCompile Include="remote_windows_capture.cpp">[\s\S]*?<LanguageStandard>stdcpp20<\/LanguageStandard>[\s\S]*?<\/ClCompile>/,
 'helper compiles the C++/WinRT capture engine as C++20');
assert.equal(helper.split('d3d11.lib;dxgi.lib;windowsapp.lib').length-1,4,'every helper configuration links D3D and WinRT');
const app=read('AppSandbox.vcxproj'),filters=read('AppSandbox.vcxproj.filters');
for(const [kind,file] of [['ClCompile','src\\backend_win\\remote_windows_frames_host.c'],['ClCompile','tools\\transport\\asb_rw_frames.c'],
  ['ClCompile','tools\\transport\\asb_remote_windows.c'],['ClInclude','src\\backend_win\\remote_windows_frames_host.h'],
  ['ClInclude','tools\\transport\\asb_rw_frames.h']]) {
 assert(app.includes(`<${kind} Include="${file}" />`),`AppSandbox.vcxproj must register ${file}`);
 assert(filters.includes(`<${kind} Include="${file}">`),`AppSandbox.vcxproj.filters must list ${file}`);
 assert(fs.existsSync(path.join(root,file)),`missing ${file}`);
}
const rwmain=read('tools/agent/appsandbox-remote-windows.c');
assert.match(rwmain,/asb_listen\(ASB_CH_RW_FRAMES\)/,'helper serves frame channel 10');
assert.match(rwmain,/asb_rw_capture_session\(capture, tag\)/,'frame channel bound to the control session');
const framesHost=read('src/backend_win/remote_windows_frames_host.c');
assert.match(framesHost,/D3DKMTDuplicateHandle/,'GPU pools reuse the Windows-guest texture duplication path');
assert.match(framesHost,/D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX/);
// eh6.6: guest window control in the helper; host session model + C++ proxy manager in the app.
assert(helper.includes('<ClCompile Include="remote_windows_control_win32.c" />'),'helper compiles the window-control ops');
assert(helper.includes('<ClInclude Include="remote_windows_control_win32.h" />'));
assert.match(rwmain,/asb_rw_ctl_ops\(&ops\)/,'helper wires geometry/close/hit-test/state/icon ops');
assert.match(rwmain,/ASB_RW_CAP_GEOMETRY/,'helper advertises proxy support with its capture engine');
for(const [kind,file] of [['ClCompile','src\\backend_win\\remote_windows_session.c'],['ClInclude','src\\backend_win\\remote_windows_session.h'],
  ['ClInclude','src\\backend_win\\remote_windows_proxy.h']]) {
 assert(app.includes(`<${kind} Include="${file}" />`),`AppSandbox.vcxproj must register ${file}`);
 assert(filters.includes(`<${kind} Include="${file}">`),`AppSandbox.vcxproj.filters must list ${file}`);
 assert(fs.existsSync(path.join(root,file)),`missing ${file}`);
}
assert.match(app,/<ClCompile Include="src\\backend_win\\remote_windows_proxy\.cpp">[\s\S]*?<CompileAs>CompileAsCpp<\/CompileAs>[\s\S]*?<LanguageStandard>stdcpp20<\/LanguageStandard>[\s\S]*?<\/ClCompile>/,
 'the proxy manager compiles as C++20 inside the C app project');
assert(filters.includes('<ClCompile Include="src\\backend_win\\remote_windows_proxy.cpp">'));
// Managers are owned by the app shell, per running Windows VM, independent of the desktop viewer.
const ui=read('src/app_win/ui.c');
assert.match(ui,/#include "remote_windows_host\.h"/,'eh6.7: each manager is wrapped by the per-VM host (input, clipboard, audio)');
assert.match(ui,/asb_rw_host_start\(&config\)/);
assert.match(ui,/case WM_CORE_CHANGED:[\s\S]*?reconcile_remote_windows\(\);/,'managers follow VM state');
assert.match(ui,/case WM_DESTROY:[\s\S]*?stop_remote_windows\(\);[\s\S]*?asb_cleanup\(\);/,'app exit ends sharing');
assert.match(ui,/hcs_guest_has_remote_windows\(vm->os_type\)/,'Windows guests only');
for(const viewer of ['src/backend_win/vm_display_idd.c','src/backend_win/vm_display.c'])
 assert(!/asb_rw_manager|remote_windows_proxy/.test(read(viewer)),`${viewer} must not own sharing (viewer-independent)`);
assert.match(read('src/backend_win/hcs_services.h'),/ASB_API BOOL hcs_guest_has_remote_windows/);
// eh6.7: one input arbitration, one clipboard/audio owner per VM; the viewer is a client of both.
for(const [kind,file] of [['ClCompile','src\\backend_win\\remote_windows_host.c'],['ClInclude','src\\backend_win\\remote_windows_host.h'],
  ['ClCompile','src\\backend_win\\vm_integration.c'],['ClInclude','src\\backend_win\\vm_integration.h']]) {
 assert(app.includes(`<${kind} Include="${file}" />`),`AppSandbox.vcxproj must register ${file}`);
 assert(filters.includes(`<${kind} Include="${file}">`),`AppSandbox.vcxproj.filters must list ${file}`);
 assert(fs.existsSync(path.join(root,file)),`missing ${file}`);
}
const idd=read('src/backend_win/vm_display_idd.c');
assert(!/vm_clipboard_create|audio_recv_thread_proc|AddClipboardFormatListener/.test(idd),'the viewer no longer owns clipboard connections or audio');
assert.match(idd,/vm_integration_acquire\(/,'the viewer holds the VM integration services');
assert.match(idd,/asb_input_focus\(d->input, ASB_INPUT_SOURCE_VIEWER/,'the viewer claims the user input source on activation');
assert.match(read('src/backend_win/remote_windows_host.c'),/asb_input_send_user\(h->input, ASB_INPUT_SOURCE_WINDOWS/,'proxies inject through the shared router');
assert.match(read('tools/agent/appsandbox-input.c'),/INPUT_MOUSE_VIRTUAL/,'the injector maps virtual-desktop moves');
// eh6.8: one shared flyout implementation in the app and the helper; guest tray + exit handshake.
assert(app.includes('<ClCompile Include="tools\\agent\\remote_windows_flyout.c" />'),'the app compiles the shared flyout');
assert(filters.includes('<ClCompile Include="tools\\agent\\remote_windows_flyout.c">'));
for(const file of ['remote_windows_flyout.c','remote_windows_tray.c']) assert(helper.includes(`<ClCompile Include="${file}" />`),`helper compiles ${file}`);
assert(helper.includes('<ResourceCompile Include="appsandbox-remote-windows.rc" />'),'the guest tray has the App Sandbox icon');
assert.match(rwmain,/asb_rw_guest_tray_start\(&tc\)/);assert.match(rwmain,/ASB_RW_CAP_GUEST_SELECTOR/);
assert.match(rwmain,/#define EXIT_USER\s+4/);assert.match(agent,/#define RW_EXIT_USER\s+4/,'service and helper agree on the user-exit code');
assert.match(agent,/current != user_exit_session/,'the service does not restart a helper the user exited');
// Host tray: right-click menu contents and dispatch and double-click Show are unchanged; left click opens the selector.
const menu=/static void tray_show_menu\(HWND hwnd\)\n\{([\s\S]*?)\n\}/.exec(ui)[1];
const items=[...menu.matchAll(/tray_append\(menu,\s*([^,]+),\s*([^,]+),/g)].map(m=>`${m[1].trim()}|${m[2].trim()}`);
assert.deepEqual(items,['TRAY_CMD_SHOW|L"Show App Sandbox"','0|NULL','0|v->name','TRAY_CMD_CONNECT_BASE + i|L"\\U0001F4FA  Connect"',
 'TRAY_CMD_SHUTDOWN_BASE + i|L"\\u23FB  Shutdown"','TRAY_CMD_STOP_BASE + i|L"\\u2715  Force Stop"','0|NULL','TRAY_CMD_EXIT|L"Exit"'],'right-click menu items unchanged');
for(const c of ['cmd == TRAY_CMD_SHOW','cmd == TRAY_CMD_EXIT','cmd >= TRAY_CMD_STOP_BASE','cmd >= TRAY_CMD_SHUTDOWN_BASE','cmd >= TRAY_CMD_CONNECT_BASE'])
 assert(menu.includes(c),`right-click dispatch keeps ${c}`);
const trayCase=/case WM_TRAYICON:([\s\S]*?)return 0;\n\n    case WM_TIMER:/.exec(ui)[1];
assert.match(trayCase,/case ASB_RW_TRAY_DOUBLE:[\s\S]*?ShowWindow\(hwnd, SW_RESTORE\);\s*SetForegroundWindow\(hwnd\);/,'double click shows App Sandbox');
assert.match(trayCase,/case ASB_RW_TRAY_MENU:[\s\S]*?tray_show_menu\(hwnd\);/,'right click shows the same menu');
assert.match(ui,/asb_rw_tray_timer\(&g_tray_clicks[\s\S]*?rw_selector_open\(\);/,'a deferred single left click opens the selector');
assert.match(ui,/msg == g_taskbar_created\)[\s\S]*?tray_add\(hwnd\);/,'the host icon is recreated when Explorer restarts');
assert.match(ui,/ChangeWindowMessageFilterEx\(hwnd, g_taskbar_created, MSGFLT_ALLOW/,'the elevated app accepts TaskbarCreated');
console.log('PASS remote windows registration: core sources, tested Windows-only service table, unchanged channels, no public API, supervised helper, eh6.5 capture/frame sources, eh6.6 proxy manager/session/window-control sources and viewer-independent ownership, eh6.7 shared input/clipboard/audio, eh6.8 shared flyout, guest tray exit handshake and unchanged host tray menu');
// eh6.9: two-way copy-only OLE drag and drop. Guest bridge in the helper; a medium-integrity host broker.
for(const file of ['..\\transport\\asb_rw_files.c','..\\transport\\asb_clipboard_temp.c'])
 assert(helper.includes(`<ClCompile Include="${file}" />`),`helper compiles ${file}`);
for(const file of ['remote_windows_files.cpp','remote_windows_files_win32.cpp'])
 assert.match(helper,new RegExp(`<ClCompile Include="${file.replace('.','\\.')}">[\\s\\S]*?<LanguageStandard>stdcpp20</LanguageStandard>[\\s\\S]*?</ClCompile>`),`helper compiles ${file} as C++20`);
for(const file of ['remote_windows_files.h','remote_windows_files_win32.h','..\\transport\\asb_rw_files.h'])
 assert(helper.includes(`<ClInclude Include="${file}" />`),`helper lists ${file}`);
assert.equal(helper.split('shlwapi.lib;wtsapi32.lib').length-1,4,'every helper configuration links the staging dependencies');
assert.match(rwmain,/asb_listen\(ASB_CH_RW_FILES\)/,'helper serves file channel 11');
assert.match(rwmain,/ASB_RW_CAP_FILES_TO_GUEST \| ASB_RW_CAP_FILES_FROM_GUEST/,'helper advertises both directions with the bridge');
assert.match(rwmain,/asb_rw_files_session\(g_files, tag\)/,'file sessions are bound to the control session');
assert.match(rwmain,/asb_rw_files_session\(g_files, 0\)/,'file sessions end with the control session');
const broker=read('AppSandboxDrag.vcxproj'),bguid=/<ProjectGuid>(\{[^}]+\})<\/ProjectGuid>/.exec(broker)[1];
for(const file of ['src\\app_win\\appsandbox_drag.c','tools\\transport\\asb_remote_windows.c','tools\\transport\\asb_rw_files.c',
  'tools\\transport\\asb_clipboard_temp.c']) {
 assert(broker.includes(`<ClCompile Include="${file}" />`),`broker compiles ${file}`);
 assert(fs.existsSync(path.join(root,file)),`missing ${file}`);
}
for(const file of ['src\\backend_win\\remote_windows_drag_host.cpp','tools\\agent\\remote_windows_files_win32.cpp'])
 assert(broker.includes(`<ClCompile Include="${file}">`),`broker compiles ${file}`);
assert.match(broker,/<UACExecutionLevel>AsInvoker<\/UACExecutionLevel>/,'the broker never asks for elevation');
assert.match(broker,/<OutDir>\$\(SolutionDir\)bin\\\$\(Configuration\)\\<\/OutDir>/,'the broker is built beside AppSandbox.exe');
assert(sln.includes(`"AppSandboxDrag", "AppSandboxDrag.vcxproj", "${bguid}"`),'the broker is in the solution');
assert.equal(sln.split(`${bguid} = ${bguid}`).length-1,2,'AppSandbox and AppSandboxPackage depend on the broker');
for(const c of ['Debug|ARM64','Debug|x64','Release|ARM64','Release|x64']) assert(sln.includes(`${bguid}.${c}.Build.0 = ${c}`));
const rwhost=read('src/backend_win/remote_windows_host.c');
assert.match(rwhost,/L"AppSandboxDrag\.exe"/);
assert.match(rwhost,/GetShellWindow\(\)[\s\S]*?PROC_THREAD_ATTRIBUTE_PARENT_PROCESS/,'an elevated app starts the broker with the shell\'s medium token');
assert.match(rwhost,/mc\.drag_start = drag_start;/);
const proxy=read('src/backend_win/remote_windows_proxy.cpp');
assert.match(proxy,/asb_rw_session_drag_probe\(/,'proxies probe for guest drags when the pointer leaves with a button held');
assert.match(proxy,/tag_drag\(m, w, hwnd\)/,'proxy windows carry the broker cookie and surface');
// Real OLE on both sides, copy only, and the clipboard is never used to fake a drag.
const dragFiles=['tools/agent/remote_windows_files.cpp','tools/agent/remote_windows_files_win32.cpp','src/backend_win/remote_windows_drag_host.cpp'];
const dragSrc=dragFiles.map(read).join('\n');
assert(!/OpenClipboard|SetClipboardData|EmptyClipboard|OleSetClipboard|OleFlushClipboard/.test(dragSrc),'drag and drop never touches the clipboard');
assert.equal((dragSrc.match(/(SH)?DoDragDrop\((h->overlay, )?data, src, DROPEFFECT_COPY, &effect\)/g)||[]).length,2,'both drag loops are real (SH)DoDragDrop, copy only');
assert(!/DROPEFFECT_MOVE/.test(dragSrc),'no move effect is ever offered or reported');
assert.match(read('tools/agent/remote_windows_files.cpp'),/RegisterDragDrop\(f->catcher/,'the guest catcher is a registered IDropTarget');
assert.match(read('src/backend_win/remote_windows_drag_host.cpp'),/RegisterDragDrop\(h->overlay/,'the host overlay is a registered IDropTarget');
console.log('PASS remote windows registration (eh6.9): helper file bridge sources, broker project/solution/launch, real copy-only OLE, no clipboard use');
