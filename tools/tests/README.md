# Integrated display validation (gfa.9)

Run from the repository root:

```powershell
.\tools\tests\test-display-feature.ps1
# Explicit staging location, if using the prepared SDK/WDK instead of an installed WDK:
.\tools\tests\test-display-feature.ps1 -WdkRoot C:\AppSandbox\bin\wdk
```

Requires VS x64 build tools, Node, the test-only jsdom/Playwright packages described
below, installed Edge (or `ASB_TEST_BROWSER`), and either an installed WDK or the
previously staged SDK/WDK tree. No dependency download or install is performed.
The runner selects `bin/wdk` when that staged tree exists. It discovers Visual
Studio with vswhere; `-VisualStudioPath` can select another installation.
App/core checks rebuild Release x64 with `PlatformToolset=v145`, matching the
installed VS 2026 toolset; the repository's normal project toolset is unchanged.

The runner records each command, result, duration and log in
`build/display-validation/results.json`, continues after individual failures, and
fails overall if any check fails. Its checks are:

| Check | Evidence |
| --- | --- |
| Registration | All app/core/VDD C/C++ source/header paths exist and are unique; app/filter lists agree; required display sources, transport headers, shared JS assets and option IDs are registered. |
| Focused native suites | Legacy/default/atomic/concurrent settings, strict editor commands, real monitor union/ranking serializer, CPU/GPU resize protocol and actual VDD handler, windowed/fullscreen policy, overlay geometry and reminder completion/cancellation. |
| Integrated editor/native/viewer | Actual editor output crosses native parsing and atomic compare/update storage, then fresh-process viewer preference loading, real catalog maximum selection, CPU policy requests and fixed/Match fullscreen restoration. Independent pin/reminder updates survive a stale editor snapshot; stale resolution writes and forged invalid values are rejected; another VM remains isolated. |
| Browser/JS suites | Full-DOM editor validation/warnings and platform gating; Edge editor/overlay layouts, explicit menu IDs, dwell/pin/checkmarks; syntax checks. |
| Builds | Clean Release x64 app, core and actual VDD project builds. Driver signing is forced off; no submission, publishing or guest deployment. |

The integration harness reuses the focused fullscreen fixture's synthetic monitor,
window-manager and VM transport boundaries. It compiles actual viewer functions,
command parser, preference storage and mode catalog. It runs against temporary VM
directories and never starts a VM or changes host monitor modes. Its frontend
helper is shared with the existing editor suite, so both execute the same HTML,
validation and save path. All focused checks still run independently.

For just that regression: `.\tools\tests\test-display-integration.ps1` (this also
runs/generates the focused fullscreen policy harness). For only an unsigned driver
rebuild: `.\tools\tests\build-display-driver.ps1 -WdkRoot bin/wdk`. Omit `-WdkRoot`
for an installed WDK. The staged build uses the original
`WindowsUserModeDriver10.0` toolset, x64 tool paths, SDK/WDK content version
10.0.28000.0, `SignMode=Off`, and disabled Inf2Cat. The staging used here contains
WDK NuGet 10.0.28000.2526 and SDK NuGet 10.0.28000.2957. Spectre libraries are absent
in that tree, so its local build override disables Spectre mitigation. Normal
release packaging/signing must use its usual qualified toolchain.

The VDD DLL output is `tools/vdd/x64/Release/AppSandboxVDD.dll`; it is unsigned
build evidence. DrvCat reports that it has no catalog to process in this mode.
It does not establish a signed, deployable driver package. The app resource copy
step is checked by its build, and its shared web assets are copied to `bin/Release/web`.

The sections below retain historical per-child results. This integration pass
supersedes their older missing-resource and WDK-environment limitations; those
older failures are not represented as earlier successful checks. The registration
audit corrected a stale core `src/ui.h` reference, added the app's resize header,
and reconciled app filters with its source/header list.

Validation recorded on 2026-11-20: all 18 runner checks passed (zero failures),
including clean app/core/VDD rebuilds, source registration, browser checks, and the
new cross-component regression. The driver compiler used `/W4 /WX /analyze`
with Windows PREfast plugins, followed by Universal API validation. Exact commands,
durations and logs are in the ignored `build/display-validation/results.json`.
No signed deployable release package was produced. Native acceptance was not run.

Native Windows 11 host/guest acceptance stays open in `AppSandbox-gfa.10`:
physical mixed-DPI/hotplug, actual TaskDialog/Escape/focus/capture and HTTP client
disconnects, GPU sharing/CPU fallback, placement, scaling, and Fluent rendering
require separate runtime evidence. No signing submission, publishing or guest
deployment is part of this runner.

---

# Windows display preference storage tests

## Fullscreen resolution policy (gfa.6)

Run `.\tools\tests\test-fullscreen-resolution.ps1`. It compiles the actual viewer
preference loader, windowed/fullscreen evaluators, target selection, borderless
fit/transition, aspect scaling and mouse mapping with MSVC `/W4 /WX`. Synthetic
monitor APIs feed the real capability catalog builder and maximum selector.
VM, overlay and window-manager operations are substituted; no VM is started and
no host monitor modes are changed. Source checks cover monitor-move, DPI, display
configuration and device-change events, actual-frame rendering/input wiring,
project registration, and absence of host display-mode setters in the viewer.

Coverage includes maximum versus current resolution, within-cap/equal-area
ranking, smaller/larger/portrait target monitors, custom resolution retention,
letterboxing, fresh same-monitor hotplug/DPI selection, recursion guards during
window positioning, delayed CPU transport, stale in-flight transitions, rejection
and reconnect suppression, and exact windowed placement/style restoration with
Match both off and on, including maximized placement metadata. Frame/timer
notifications evaluate cached targets without enumerating monitors or refitting
the borderless window. Rejected targets leave actual frames and saved values intact.

Automatic selection refreshes the host capability snapshot on fullscreen entry,
target-monitor changes, DPI/display changes and device-node changes. Failure or
absence of an in-cap maximum is logged and falls back to the target monitor's
current desktop pixel dimensions, bounded to 640x480..7680x4320. Missing/invalid
current dimensions fall back to 1920x1080. These fallbacks never replace the
saved custom pair. Custom mode uses that pair on every monitor without capability
enumeration. Viewer bounds always use the full monitor rectangle, independently
of guest resolution and fallback limits. Windows fullscreen requests use fixed
guest mode while preserving the session Match choice for exit.

Validation on 2026-11-20: fullscreen and windowed policy suites, mode catalog,
resize protocol and actual VDD-handler regressions passed. Release x64 app/core
builds passed with the installed v145 override, including app resource copying.
These establish policy, API wiring and build behavior; physical mixed-DPI/hotplug,
native window placement/composition and real guest GPU/CPU resizing remain
separate Windows 11 host/guest acceptance in `AppSandbox-gfa.10`.

## Saved windowed viewer policy (gfa.5)

Run `.\tools\tests\test-windowed-resolution.ps1`. The suite compiles the actual
viewer preference loader, policy evaluator, Match command helpers, DPI content
sizing and letterboxed input mapping with MSVC `/W4 /WX`. VM access and window
operations are substituted; nonclient calculations use the real Windows
`AdjustWindowRectExForDpi` at 96, 144 and 192 DPI. Integration checks cover
the timer, frame-message, acknowledgment, command and project wiring.

Coverage includes default/custom targets, read-error defaults, reopening in fixed
mode, resizing a fixed viewer without changing the guest, Match on/off, restoring
saved content size, delayed CPU transport readiness without shared GPU textures,
ACK versus matching frames, old frames, obsolete in-flight Match acknowledgments,
unsent request replacement, disconnect/reconnect, rejection/timeout suppression,
preference preservation, fullscreen/minimized/interactive-sizing suspension,
legacy capability failure reporting, inclusive bounds, and actual-frame mouse
coordinates after a rejected target. Match can be turned off during a disconnect.

Windows viewers load the saved pair on each opening and size physical client
pixels with the actual window DPI before showing the window. Fixed session targets
remain separate from received frame dimensions and acknowledged guest size mode.
Frame changes never resize a fixed viewer or replace its saved target. A timer
evaluates deferred policy after transport negotiation; existing bounded resize
state prevents duplicate requests and repeated failed-target attempts. Errors
appear in the viewer log without writing preferences. Windows Match requires the
updated VDD CPU-resize capability, applies only while windowed, and is never saved.
Other guest viewers retain their existing sizing path. Fullscreen target selection
and restoration belong to gfa.6.

Validation on 2026-11-20: this suite, resize protocol/actual-driver-handler tests,
storage tests (including 500 concurrent updates), and native editor-command tests
passed. Release x64 app and core built with the installed v145 toolset override;
the app resource/WebView copy step also completed. No driver code changed here.
These checks establish policy, arithmetic, protocol and compilation behavior;
they do not establish physical mixed-DPI transitions, actual window-manager
placement, or real guest GPU/CPU resizing. Native Windows 11 host/guest acceptance
remains tracked by `AppSandbox-gfa.10`.

## Windows resize transport (gfa.4)

Run `.\tools\tests\test-display-resize.ps1`. The C suite checks the unchanged
68-byte/version-3 packet layout, inclusive 640x480 and 7680x4320 bounds,
1280x720, 1920x1080, custom 1365x911, invalid dimensions/refresh/session/version,
independent CPU resize capability negotiation, old GPU-only capability behavior,
rejected requests, ACK versus observed-frame completion, timeout, and suppression
of the same failed target after reconnect. A different target remains available.

The C++ harness compiles the actual `VddHandleResize` function extracted from
`tools/vdd/vdd.cpp`, with WDF enqueue and transport replies stubbed. It exercises
both protocol size modes, bounds/custom sizes, unavailable work items, stopping,
old sessions/versions, rejection without changing the pending driver mode, and
the unchanged IVSHMEM minimum. This harness does not emulate IddCx or DWM.

The driver advertises `DISPLAY_GPU_CAP_CPU_RESIZE` alongside the existing resize
bit. New Windows hosts query control even with GPU sharing disabled; CPU polling
dispatches resize independently of texture offers. Both endpoints use the shared
Windows bounds helper. Legacy GPU-only Match Window behavior retains its old
minimum; explicit selected modes require the new capability. An incompatible
protocol disables further queries for that viewer session and keeps CPU frames.
Failures are reported through the viewer log and the selected-mode API HRESULT.

The driver retains Windows modes when sharing/connectivity is lost, restores
the previous dimensions on monitor creation failure, and makes a bounded rollback
attempt if a requested mode produces no frame within 15 seconds. The host waits
for a matching mode frame after ACK, retains actual frame dimensions until then,
and prevents repeating the same failed request across reconnects.

Validation on 2026-11-20: resize suites passed with MSVC `/W4 /WX`; storage,
catalog and strict editor-command regressions passed. Release x64 app and core
built with the installed v145 override. The actual Release x64 VDD project built
with `WindowsUserModeDriver10.0`, MSVC 14.51, WDK NuGet 10.0.28000.2526 and SDK
NuGet 10.0.28000.2957, including PREfast compilation and Universal API validation.
The WDK build integration and SDK/WDK files were staged locally under ignored
`bin/wdk`, without installing a guest driver. Local build overrides selected
x64 tool paths, `SignMode=Off`, `Driver_SpectreMitigation=false` (the Spectre
libraries are absent), and `EnableInf2cat=false` (NuGet supplies DrvCat instead).
That DLL is build evidence, not a signed deployable release package; catalog
generation, signing and native host/guest runtime acceptance remain unverified.
Use the normal WDK package/signing flow for a deployable release. See
`VM-UPGRADE.md` for the existing guest update prerequisite and packaging route.

## Preference storage

Run from the repository root:

```powershell
.\tools\tests\test-display-settings.ps1
```

Requires MSVC x64 C++ build tools and a Windows SDK. The script discovers Visual
Studio with `vswhere`, initializes its CLI environment, and builds with `/W4 /WX`.
Outputs go to ignored `bin/tests/display-settings`. Tests use a private temporary
directory, do not start a VM, and remove their fixtures on success.

Coverage includes missing and legacy files, defaults, malformed dimension pairs,
custom resolution round trips and inclusive bounds, disabled fullscreen override
retaining dimensions, unrelated UTF-8/nested values, invalid arguments, failed
atomic replacement preserving the previous file, stale snapshot updates, and
500 updates from five concurrent processes.

The public backend interface is in `src/backend_win/asb_core.h`. Resolution updates
include both dimension pairs and the fullscreen override in one operation; the
backend rejects them while a VM is running, starting, or building. Individual
hotkey, clipboard, pin, and reminder setters remain available during sessions.
The viewer uses these accessors for its existing hotkey and clipboard preferences.

Storage remains `display_settings.json` beside the VM disk. New JSON keys are
`windowedWidth`, `windowedHeight`, `fullscreenOverride`, `fullscreenWidth`,
`fullscreenHeight`, `fullscreenPinned`, and `suppressFullscreenReminder`.
Missing fields default without forcing a write. Either malformed member causes
the entire dimension pair to default to 1920x1080. Malformed JSON, invalid UTF-8,
oversized files (over 1 MiB), and I/O errors fail updates without replacing the
file. Independent fields and unknown JSON values survive updates. A named mutex
serializes reload/merge/flush/atomic replacement across app/core processes.

Local build validation with the installed VS 2026 compiler:

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild AppSandboxCore.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145
& $msbuild AppSandbox.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v145
```

The override leaves the repository's v143 configuration intact. These are storage
and compilation checks. They do not qualify real guest display behavior or the
fullscreen/editor features assigned to later issues in `AppSandbox-gfa`.

Both binaries built successfully for gfa.1. The existing app post-build copy step
reports `File not found - resources` because that directory is absent in this
checkout; successful compilation/linking does not prove runtime packaging.


# Windows host resolution catalog tests

```powershell
.\tools\tests\test-display-modes.ps1
node tools/tests/display_modes_bridge_test.cjs
node --check web/app.js
```

The native suite compiles the actual catalog builder and UI JSON serializer with
`/W4 /WX`. Synthetic monitor sources test a 1080p/4K union, independent width and
height maxima (including multiple modes on one monitor), portrait and ultrawide
sizes, duplicate pairs from refresh-rate variants, equal-area ranking, inclusive
bounds, automatic maxima drawn from supported in-cap modes, no in-cap maximum,
hotplug refresh, empty/failed enumeration, and complete JSON messages. A read-only
live enumeration smoke check found two desktop monitors and 11 presets on the
implementation host. The JavaScript harness executes the actual editor-opening
function and host-message handler, checking refresh requests, failure handling,
Windows guest gating, and preservation of saved/unavailable dimensions. Source
registration checks cover the native message/event wiring and shared presets.

The core API exposes a fresh `AsbDisplayCapabilities` snapshot through
`asb_get_host_display_capabilities`; release it with
`asb_free_host_display_capabilities` before reusing the output. Do not free its
arrays in the app: allocation and release belong to the core DLL.
`asb_host_display_accommodates` checks whether any single supported pair on any
monitor is at least as large in both dimensions. `asb_get_host_monitor_maximum`
resolves an HMONITOR's GDI name into this snapshot's highest supported in-cap mode.
A missing target or missing in-cap maximum is an error, with no current-mode or
clamped fallback. Ranking is pixel count, then width, then height.

The internal Windows bridge accepts `getDisplayCapabilities` and emits:

```json
{"type":"displayCapabilities","ok":true,"errorCode":0,"monitors":[{"device":"\\\\.\\DISPLAY1","maximum":{"width":1920,"height":1080},"modes":[{"width":1920,"height":1080}]}],"presets":[{"width":1920,"height":1080}]}
```

Failure has `ok:false`, a failing HRESULT in `errorCode`, and empty arrays.
A monitor with modes but no supported mode within 640x480..7680x4320 has a null
`maximum`. Raw supported pairs remain available for capability checks, including
host modes larger than app limits; presets and automatic selections stay in cap.
No saved settings are loaded, clamped, or written by this service. The frontend
retains the latest snapshot in `hostDisplayCapabilities`. Opening a Windows 11 VM
editor, `WM_DISPLAYCHANGE`, and `DBT_DEVNODES_CHANGED` request fresh enumeration.
Each refresh starts native mode enumeration at zero. Resolution controls and
fullscreen policy execution remain assigned to later issues.

The native implementation uses attached, non-mirroring desktop monitors and
`EnumDisplaySettingsExW` with zero flags. It never calls a display-mode setter.
API behavior was checked against Microsoft's documentation for
[EnumDisplaySettingsExW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaysettingsexw)
and [EnumDisplayDevicesW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumdisplaydevicesw).
The driver and host now expand the same 17-entry common preset list from
`tools/transport/asb_display_resolutions.h`. Existing driver order, default mode,
and refresh defaults are preserved; the five additional host presets do not
change driver execution.

The Release x64 app and core compile/link with the existing v145 override shown
above. The app still reports the missing `resources` post-build directory.
A full driver build was attempted but fails at MSB8020 because
`WindowsUserModeDriver10.0` is not installed. The WDK driver build and real guest,
physical hotplug, mixed-DPI, and fullscreen acceptance remain separate gates.


# Windows VM resolution editor tests

```powershell
# Test-only packages; installed into ignored output, not app runtime dependencies.
npm install --prefix bin/tests/ui-deps --no-save --no-audit --no-fund jsdom@31.0.1 playwright-core@1.65.1
.\tools\tests\test-display-resolution-command.ps1
node tools/tests/display_resolution_editor_test.cjs
node tools/tests/display_resolution_browser_test.cjs
node tools/tests/display_modes_bridge_test.cjs
node --check web/app.js
node --check web/display-resolution.js
```

The browser test uses the installed Microsoft Edge in headless mode. Set
`ASB_TEST_BROWSER` to another Chromium executable if necessary. It starts no VM.
Screenshots go to ignored `bin/tests/resolution-editor.png` and
`bin/tests/resolution-editor-small.png`; both were visually reviewed at 1024x900
and 640x420. The smaller viewport keeps the editor body scrollable and Save/Cancel
visible. The full-DOM suite executes the actual HTML, value rules, and `app.js`;
it does not replace editor validation or conflict handling with test stubs.

Coverage includes new/legacy defaults, available presets and custom round trips,
unavailable saved/default sizes remaining Custom, windowed/fullscreen warnings,
simultaneous monitor-limit and sub-720p warnings, advisory-only Save behavior,
capability failure, empty/fractional/exponent/out-of-range inputs, hidden fullscreen
retention, hotplug refresh preserving unfinished text, external updates/conflicts,
independent CPU/RAM changes, running/starting/building transitions, platform/guest
gating, associated live regions, initial focus, and restored pencil-button focus.

The C suite compiles the real native command parser and storage implementation
with `/W4 /WX`. It rejects coerced types, fractional/exponent JSON numbers,
missing/duplicate fields, invalid bounds, malformed expected snapshots, and
running/starting/building writes. It verifies legacy and unrelated preferences,
independent preference changes, rejection of stale saves without modifying the
file, and two competing editors with the same original snapshot: exactly one
commits. It uses temporary files and no host VM configuration.

Windows VM list messages now include `displayResolution` (all five values) and
`displaySettingsError`. Opening the pencil editor requests `getDisplaySettings`
through the existing VM-list refresh path. Resolution updates notify core
subscribers so open editors reconcile external changes. The editor compares
resolution values as one unit, following the existing merge/conflict pattern.
Read errors block saving rather than replacing unreadable preferences.

The internal `setDisplayResolution` command carries a `resolution` object and an
`expected` object, each with integer `windowedWidth`, `windowedHeight`,
`fullscreenWidth`, `fullscreenHeight`, and boolean `fullscreenOverride`. Native
parsing uses strict JSON tokens rather than numeric coercion. The new backend
`asb_vm_set_display_resolution_if_unchanged` checks VM identity and Windows guest
type, delegates lifecycle/value guards to storage, and compares the expected
resolution under the same cross-process mutex as the atomic update. A stale
resolution returns `ERROR_REVISION_MISMATCH`; clipboard/hotkey/pin/reminder updates
are independent and do not create false resolution conflicts.

While fullscreen override is disabled, Save retains the last valid fullscreen
pair. Unfinished hidden inputs remain available if the user re-enables the
override, but are not submitted. All submitted resolution values therefore pass
native validation together. Monitor refreshes never clamp dimensions or erase
unfinished custom inputs. Resolution controls remain hidden for macOS hosts and
non-Windows guests, with their existing CPU/RAM/etc. editor behavior preserved.

The Release x64 app and core compile/link using the installed v145 override.
The existing missing `resources` copy warning remains. These tests establish
editor/storage/interface behavior; they do not apply a guest display mode,
qualify fullscreen policy, or satisfy real Windows host/guest acceptance. Driver
build availability remains as recorded for gfa.2. No gfa.4+ execution is included.

# Windows fullscreen controls (gfa.7)

```powershell
.\tools\tests\test-fullscreen-controls.ps1
.\tools\tests\test-display-settings.ps1
node tools/tests/fullscreen_controls_browser_test.cjs
```

The browser suite uses the same test-only Playwright installation and headless
Edge setup documented above. It executes the shared overlay HTML and its real
input bridge. Coverage includes the first visible pin item, explicit command IDs
0 through 4 retaining their meanings, pin ID 5, checkmarks, 500ms edge dwell and
cancellation, pinned visibility, unpin auto-hide, menu closing, resize,
fullscreen transitions, visible layout rectangles, button-gap input, and Windows
capability gating. Windows selects Segoe Fluent Icons BackToWindow U+E73F while
retaining the Exit fullscreen accessible label. The source of the glyph is
[Microsoft's Segoe Fluent Icons catalog](https://learn.microsoft.com/en-us/windows/apps/design/iconography/segoe-fluent-icons-font).
The shared page keeps the original SVG and hides pinning without the Windows
capability; the macOS backend is unchanged.

The native suite extracts and compiles the actual overlay layout, hit-test,
cancel, pin and fullscreen functions with MSVC `/W4 /WX`. WebView posting and
unrelated rendering operations are stubbed. It checks DPI-scaled visible button
and menu rectangles, passage through the gap and guest area, cancellation,
rejection of delayed layout messages, and fullscreen transitions. Layout revisions
invalidate native rectangles before resize, cancellation or state changes;
only a matching WebView layout may restore them. Storage tests additionally
verify pin reload in a fresh process, independent VM directories, and unpin
persistence through the shared atomic preference path. Failed pin writes leave
the current viewer setting unchanged and report an error in its log.

Validation on 2026-11-20: these suites passed; Release x64 AppSandbox and
AppSandboxCore compiled and linked with the installed `PlatformToolset=v145`
override. The full-solution build attempted with that override failed in the
VDD project at missing `IddCx.h`; it is not a successful driver build. No driver
changes are part of gfa.7. A headless screenshot was reviewed at
`bin/tests/fullscreen-controls.png`. These checks do not establish native
WebView composition/focus behavior, physical DPI changes, macOS runtime behavior,
or real Windows 11 host/guest acceptance; those remain qualification work in
`AppSandbox-gfa.10`. Prerequisites gfa.5/gfa.6 were still open during this work.

# Windows native fullscreen reminder and completion (gfa.8)

```powershell
.\tools\tests\test-fullscreen-entry.ps1
.\tools\tests\test-display-settings.ps1
.\tools\tests\test-fullscreen-resolution.ps1
.\tools\tests\test-fullscreen-controls.ps1
node tools/tests/fullscreen_controls_browser_test.cjs
```

The Windows viewer owns a native `TaskDialogIndirect` reminder titled Before
entering fullscreen. It explains the half-second top-center reveal and pinning,
indicates a currently pinned VM in its footer, and offers Enter fullscreen,
Cancel, and an initially unchecked per-VM suppression checkbox. Escape, Alt-F4
and title-bar close use the native Cancel result. A checked checkbox is saved
through the shared preference setter only after confirmed, successful borderless
entry. A failed save is logged and leaves the reminder enabled for a later entry.
The preference is freshly read at every entry; read failures show the reminder.
Exiting or requesting the existing fullscreen state does not prompt.

Both the system-menu action and programmatic entry use the same gate. The gate
prevents reentrant requests, cancels overlay input, releases held guest keys and
mouse buttons/capture, removes the keyboard hook, and disables guest input routing
until it finishes. Relative capture and keyboard routing are reevaluated afterward.
Closing/stopping the viewer during the reminder cancels entry and defers owner
teardown until the modal has returned.

The previous one-second `SendMessageTimeoutW` call is replaced by reference-counted
requests with completion events. A caller may supply a cancellation event through
`vm_display_idd_set_fullscreen_cancellable`. Cancellation competes atomically with
committing the transition; cancelled queued requests retain their viewer reference
until discarded and cannot enter later. Running requests unwind their modal before
reporting cancellation; callers wait for committed transitions and receive their
actual result. `WM_QUIT` cancels waiting work and is preserved for the caller's
outer loop. Viewer shutdown drains remaining request references. API calls watch
HTTP connection disconnects and pass that event through the GUI/headless bridge;
the overlapped watcher is cancelled and joined before its storage is released.

The entry suite compiles the actual gate and input-send guard extracted from the
viewer, plus its actual request helper, with MSVC `/W4 /WX`. It exercises real
Win32 events, viewer/caller threads, message queues, and reference cleanup, while
stubbing the TaskDialog response and VM/window transition boundaries. Coverage
includes repeated entry/no-op/exit, native Cancel result mapping, checked cancel,
failed entry, failed dialog/save/read, successful suppression, per-VM isolation,
pinned explanation, pre-modal input release, blocked guest input with release
packets permitted, duplicate requests, a response after more than one second,
cancellation during a prompt, queued abandonment, cancellation after commit,
`WM_QUIT`, and stable handle counts across 100 calls. A separate harness compiles
the actual HTTP watcher with mocked HTTP I/O and checks disconnect/setup failure
and cancellation-completion cleanup. Storage tests also reload suppression in a
fresh process and verify an independent VM remains unsuppressed.

Validation on 2026-11-20: entry/HTTP/storage and fullscreen policy/control
regressions passed; Release x64 app and core built with `PlatformToolset=v145`.
These are source, simulated-boundary and build checks. Actual TaskDialog rendering,
physical Escape/focus/capture behavior, live HTTP disconnects and Windows 11
guest behavior remain native qualification work in `AppSandbox-gfa.10`.

Native API references:
[TaskDialogIndirect](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/nf-commctrl-taskdialogindirect),
[TASKDIALOGCONFIG](https://learn.microsoft.com/en-us/windows/win32/api/commctrl/ns-commctrl-taskdialogconfig),
[HttpWaitForDisconnect](https://learn.microsoft.com/en-us/windows/win32/api/http/nf-http-httpwaitfordisconnect),
[CancelIoEx](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex).

# Remote Windows protocol and session contracts (eh6.2)

```powershell
.\tools\tests\test-remote-windows-protocol.ps1
node .\tools\tests\remote_windows_registration_test.cjs
```

The native suite builds `tools/tests/remote_windows_protocol_test.c` with
`/W4 /WX` against the shared codec (`tools/transport/asb_remote_windows.c`), the
guest window collector (`tools/agent/remote_windows_inventory.c`) and the HvSocket
service table (`src/backend_win/hcs_services.c`). It covers:

| Area | Evidence |
| --- | --- |
| Channels | Channels 1-8 and the input/display/screenshot packet contracts are unchanged. Windows guests get services 1-11; Linux and legacy empty `os_type` keep exactly 1-6 and 8. |
| Codec | Round trips for every message type at their size limits, including CPU/GPU frames and file chunks. Every strict prefix is "need more". Bad magic, wire version, type, channel, oversize lengths (rejected from the header), trailing bytes, inverted rectangles, invalid enums, frame shapes and unsafe file paths are rejected. A 200,000-iteration mutation fuzz checks that every accepted message re-encodes byte-for-byte. |
| Negotiation | Both endpoints compute identical results. Old (v0), newer-only and invalid version ranges are refused. An older guest without the required capabilities gets `upgrade_required` rather than reduced sharing. Unknown capability bits are ignored and limits take the minimum of both sides. |
| Identity | IDs are stable per window. HWND reuse by another process, PID reuse, destroyed windows, forged IDs and IDs from a previous connection (including a repeated session tag) are `stale_id`. The table is bounded. |
| Inventory | Visible or minimized standard-user roots in the active session are included. Hidden, cloaked, owned, tool, no-caption, untitled, shell, helper, elevated and unknown-elevation windows are excluded. Selected roots stay listed. Association is by ownership only; same-process windows and shadows are never associated. Add/change/remove events and generations are tested, event overflow is bounded, and duplicate titles are labelled with the application name and an ordinal. |
| Native windows | Real off-screen windows in the test process are enumerated (normal, duplicate, tool, owned, hidden, minimized, DWM-cloaked, popup, custom chrome, untitled), along with the real taskbar. A destroyed window's ID becomes stale immediately. |

The registration test checks core project registration, the tested ServiceTable wiring in
`hcs_vm.c`, the unchanged channel numbers, and that no HTTP/MCP/Python surface mentions the feature.

# Remote Windows private display (eh6.3)

```powershell
.\tools\tests\test-remote-windows-display.ps1
```

The script builds and runs two focused suites with `/W4 /WX` and builds a guest-only
native check:

| Suite | Evidence |
| --- | --- |
| Driver (`remote_windows_display_driver_test.cpp`) | Extracts the real `VddCreatePrivateMonitor`/`VddRemovePrivateMonitor`, `VddTargetResolutions`, the EDID builder/parser and the `VDD_EDID` template from `tools/vdd`, and compiles `private_control.inl` against a mocked IddCx boundary. Covers the private identity (EDID `ASB0002`, serial 2, connector 1, own container ID, same `AppSandboxVDD` name), modes independent of desktop GPU/CPU mode negotiation, 20 arrival/departure cycles with alternating modes, failed create/arrival with no leaked monitor or lease, failed departure retry, lease token enforcement, REMOVE refused until RESTORE, guest mode limits (640x480 to 7680x4320) and IVSHMEM refusal. |
| Guest (`remote_windows_display_guest_test.c`) | Runs the real `tools/agent/remote_windows_display.c` and `appsandbox-displays.c` against a stateful CCD model. Covers identity by PnP device path (never friendly name or enumeration order), the compact two-path topology that leaves the desktop's own modes untouched, verifying before applying, desktop resize while sharing, re-placing a misplaced private output, refusing foreign/cloned outputs and topologies Windows rejects, bounded retries, removal only after a successful source restoration, and no `SDC_SAVE_TO_DATABASE` while the private output exists. The watcher keeps the desktop primary at (0,0), puts the private output beyond its right edge, recovers in two passes after desktop re-arrival, never re-enables a removed private output and keeps its existing single-desktop behaviour. |
| Manual helper (`remote_windows_display_toggle.c`) | Built only, not part of the product. `asb-private-display.exe start [width height]`, `stop` and `status` turn the private output on and off by hand in a guest, run as a standard user. `start` saves its lease token to `%LOCALAPPDATA%\AppSandbox\private-display.lease` before activating and leaves the output in place when it exits. `stop` restores and removes the output using that token. It parks nothing. |
| Native check (`remote_windows_display_native_check.c`) | Built only. Copy `bin\tests\remote-windows-display\remote-windows-display-native-check.exe` into a Windows 11 guest running this branch's VDD and run it unelevated, optionally with a watch period in seconds (`... 90`) for resizing the viewer and taking API screenshots. It checks the standard-user control path, that the desktop stays primary and remains the `SM_CXSCREEN` input target, private placement, refusals, parking a real window, and that the window is restored before removal. Results go to `%LOCALAPPDATA%\AppSandbox\eh6.3-native-check.log`. |

Later additions:

- `remote_windows_display_keep_test.cpp` covers the VDD keeping the host frame connection across a desktop swap-chain reassignment. Private-output arrival and departure re-commit modes, and IddCx reassigns the desktop swap chain. Only CPU-path connections are kept; connections with GPU texture state reconnect as before.
- The guest suite also covers the display watcher. It retries refused topologies for about 5 s instead of exiting. It also returns unmarked windows from the private output to the desktop, after every display pass and once a second while a private output exists. Windows land there when Windows migrates them while the desktop output is re-created during a viewer resize. A window stays on the private output only if it, or a window that owns it, carries the `AppSandbox.PrivateDisplay.Parked` property (`ASB_PRIVATE_PARKED_PROPERTY`), or if it is a caption-less popup from a process that has a marked root.

The private output is controlled through the VDD's device interface
`{66d5c6a1-78f7-44af-91b3-3304f165c528}`. Its INF security descriptor grants
read/write to interactive users, and every operation except QUERY requires a lease
token. Nothing writes registry preferences, and closing the handle never removes the
output.

# Remote Windows guest helper, window parking and restoration journal (eh6.4)

```powershell
.\tools\tests\test-remote-windows-share.ps1            # add -SkipNative to skip the real-window suite
```

The script builds everything with `/W4 /WX`, runs two suites and the registration test, and
builds the helper (`appsandbox-remote-windows.exe`):

| Suite | Evidence |
| --- | --- |
| Model (`remote_windows_share_test.c`) | Runs the real parking/journal core (`tools/agent/remote_windows_share.c`) and helper control session (`remote_windows_helper.c`) against a stateful window model. The model covers placement and show state (with the mixed-DPI drift of a single placement), styles, owners, the parked property, taskbar tabs, responsiveness, handle reuse, the private-display lease and the journal file. Every modifying call asserts that the journal already holds the window's record. Covered: select/unselect of normal, minimized, maximized and minimized-from-maximized originals (renderable, never activated, exact restore); rollback after a failure at each step (mark, show, move, verify, style, owner), a journal write failure and a display failure; owned dialogs (direct and chained) travelling with the root while popups and independent same-process roots stay untouched; concurrent application style and owner changes surviving restoration; destroyed and reused handles dropped untouched; hung windows (select refused, restoration pending and retried, display kept); recovery after helper death (identities validated, never re-parked, PARKING records, journaled lease, hung survivors, corrupt journal); display drift; the table limit; and the protocol (HELLO/version gate, snapshot and events, PENDING/SHARED and RESTORING/NONE acknowledgements, stale and elevated, RESTORE_ALL, UPGRADE_REQUIRED, malformed input, restoring on close). Three mutations of the core (one placement on restore, an unconditional owner reset, no journal write before parking) each fail it. |
| Native (`remote_windows_share_native_test.c`) | Uses real windows owned by a separate application process and the real Win32 ops (`remote_windows_share_win32.c`). In host mode the "private display" is a region far to the right of every monitor. The original windows hang 8 px below the lowest monitor's bottom edge, because `SetWindowPlacement` moves windows that are completely off-screen. Expect off-screen taskbar buttons for a few seconds. The suite parks normal and minimized roots, checks the owned dialog follows and the independent root is untouched, survives application style changes and migration, and restores exactly. A second helper process then parks a window and is killed: Windows unlinks the window (it is not destroyed) and recovery restores it from the journal. A shared window the application closes is dropped. The application process survives every step. |

Guest check: copy `bin\tests\remote-windows-share\remote-windows-share-native-test.exe` into a
Windows 11 guest running this branch's VDD and display watcher, and run it unelevated with
`--guest`. It uses the real private display and adds a maximized original. It logs to
`%LOCALAPPDATA%\AppSandbox\eh6.4-native-check.log`.

The helper runs in the active console session as the signed-in user. The guest service
(`agent.c`) starts it with the private-display lease on stdin and an inherited stop event (no
other handles are inherited). The service keeps the lease across helper restarts and stops the
helper gracefully (it restores every window first). If the helper dies or has to be killed, the
service runs `appsandbox-remote-windows.exe --recover` in that session. After a logoff, when no
window survives, the service removes the private display itself. The journal is
`%LOCALAPPDATA%\AppSandbox\RemoteWindows\journal.bin`. It records the originals and the lease,
never a selection to re-establish. The log is `helper.log` in the same folder.
# Remote Windows per-window capture and frame delivery (eh6.5)

```powershell
.\tools\tests\test-remote-windows-frames.ps1           # -SkipNative skips the real-capture suite
.\tools\tests\test-remote-windows-frames.ps1 -Evidence C:\path\to\evidence   # BMPs + log
```

The script builds everything with `/W4 /WX` (the capture engine is C++/WinRT, C++20), runs two
suites and builds the helper with its capture engine. The eh6.4 share script also exercises the
helper's surface tracking.

| Suite | Evidence |
| --- | --- |
| Model (`remote_windows_frames_test.c`) | The real frame-channel codec and the guest (`AsbRwTx`) and host (`AsbRwRx`) flow state machines (`tools/transport/asb_rw_frames.c`), connected through the codec. Covered: size/length validation of every channel-10 message (stride/data length, generation 0, slots, pool sizes, handle/status pairing, root/kind consistency, z, flags, rectangles, truncation, trailing bytes, oversize headers rejected before allocation, wrong channel, ERROR on any channel); FRAMES_HELLO negotiation (stale session tag refused, GPU only for matching non-Basic-Render adapters, forced CPU); queue and memory bounds with a consumer that never reads (slots in flight, one pending frame, superseded frames released, CPU byte budget across surfaces); stale generations (resize, held frame retired, old frames released never shown, unannounced generation refused, generations only grow); GPU pool lifetimes (no slot before POOL_STATUS, slot reuse before release, content larger than the pool, shrink/grow, stale pool results, host- and guest-side pool failure falling back to CPU, recovery re-enabling GPU, releases for an invalidated generation are stale); teardown (SURFACE_END frees host state, late releases ignored, frames after the end refused, disconnect) and table limits; popup association. |
| Helper surfaces (`remote_windows_share_test.c`, `test_helper_surfaces`) | The helper sends SURFACES for a parked root with owned tooltip/menu and unowned same-process popup bridge, bottom-to-top, with rects outside the root, but not an independent same-process root, another process's popup or a shadow; IDs are stable session IDs; unchanged sets are not resent; a closed popup's ID is released; unselect and session close send count 0 / empty capture sets. |
| Native (`remote_windows_frames_native_test.c`) | Real Windows Graphics Capture through the real capture engine (`tools/agent/remote_windows_capture.cpp`) and the real host receiver (`src/backend_win/remote_windows_frames_host.c`) over loopback TCP (GPU pools shared with `DuplicateHandle` instead of `D3DKMTDuplicateHandle`). Surfaces come from the real scan and `asb_rw_collect_surfaces`. For shared GPU pools and then forced CPU frames: live root content (changing frames, magenta marker), an owned per-pixel-alpha popup past the root's right edge (premultiplied alpha preserved, z above the root), an overflowing popup menu as its own surface and its SURFACE_END on close, resize generations, guest device loss and recovery (new generation, frames continue), host device loss (everything freed) and reconnect, a frame channel for another control session refused, bounds with a slow consumer, teardown with no texture or CPU buffer left. |

Guest check (Windows 11 guest, branch host so the VM has services 9-11): copy
`build\eh6.5\deploy\eh65` into the guest, run `install.cmd` elevated, then
`remote-windows-capture-test-app.exe` unelevated (animated root, alpha popup, an overflowing
menu at 8-14 s of every 30 s cycle, a capture-engine device loss at 20 s of every cycle; its log
is `%TEMP%\eh6.5-capture-test-app.log`). On the host, unelevated:
`bin\tests\remote-windows-frames\remote-windows-frames-vm-check.exe --evidence <dir>` (built by the
script). It selects the window over channel 9, receives
channel 10 frames with shared GPU pools (including a host device loss and reconnect) and then
forced CPU frames, and finally unselects and checks that every surface ended.

Frame channel rules (see `asb_remote_windows.h`, "Frame channel"): every root and associated
popup is a separate capture with its own generation; the guest keeps at most the agreed slots in
flight per surface and only the newest capture while they are busy; the host keeps one pending
and one displayed frame per surface and releases every frame exactly once. GPU pools are BGRA
keyed-mutex textures created by the host per surface generation and duplicated into the helper.

# Remote Windows native host proxies (eh6.6)

```powershell
.\tools\tests\test-remote-windows-proxy.ps1            # model suite, builds, native suite (GPU, then forced CPU)
.\tools\tests\test-remote-windows-proxy.ps1 -SkipNative
.\tools\tests\test-remote-windows-proxy.ps1 -Evidence C:\path\to\evidence   # screenshots (BMP) + logs
.\tools\tests\test-remote-windows-proxy.ps1 -Snap      # also Win+Left on a proxy (takes the foreground briefly)
```

Everything builds with `/W4 /WX` (the manager is C++20; everything else C). The native suite puts
real windows on the primary monitor for about a minute (shown without activation; no input is
injected unless `-Snap`).

| Suite | Evidence |
| --- | --- |
| Model (`remote_windows_proxy_test.c`) | Codecs of HIT_TEST / WINDOW_STATE / WINDOW_ICON (round trips, request without code, flag/request/limit/icon-size validation, payload length, control channel only). Host session (`src/backend_win/remote_windows_session.c`) against recorded proxy operations: negotiation (an eh6.5 guest keeps the inventory but selection answers `upgrade_required` without asking the guest); proxy identities and lifetimes (one proxy per SHARED root, duplicates/foreign-session IDs ignored, FAILED keeps the error without a proxy, host unselect destroys at once and never sends a close, NONE+STALE_ID, RESTORE_ALL and session end destroy everything, a proxy that cannot be created undoes the selection); acknowledged geometry (one command in flight, newest request coalesced, stale/unknown serials ignored, application minimum adopted, not during a host size loop but when it ends, BUSY retried by the timer, unanswered commands resent, host minimize reported without resizing, maximize never adopted back, application-initiated sizes followed only when nothing is pending); state feedback (styles/limits/modal-disabled forwarded, first state is a baseline, application maximize/minimize replayed, maximize toggles when the proxy is maximized, icons, titles); modal ownership and Close (disabled root: Close not forwarded, the owned window is brought forward; owned windows closed with their own ID; BUSY brings the dialog forward; one Close in flight; prompt + cancellation keeps the proxy; a later Close is forwarded again; the real close removes it); owned-window moves (root-relative, overlaid on older SURFACES until acknowledged); the hit-test cache (one query per surface, newest point queued, stale serials ignored, radius/near answers, expiry, resize forgets, retries, far points HTNOWHERE); protocol violations. Guest helper (`remote_windows_helper.c`) against a window model: in-place root resize, kept on the private display, application minimum acknowledged, host minimization leaves the source untouched, BUSY when not responding, owned windows moved (size kept when not resizable), menus not geometry targets, stale/foreign IDs refused, Close as SC_CLOSE and BUSY while modal-disabled, hit tests in visible-frame coordinates, WINDOW_STATE only on change with paced limit queries, icons on change, application min/max normalized and reported with a new serial, owned windows not normalized, no proxy messages towards an older host. Six deliberate mutations of these rules (close cancellation, modal refusal, coalescing, stale acks, host minimization, normalization) were each caught. |
| Native (`remote_windows_proxy_native_test.c`) | The real guest side (helper session, parking, window control, capture engine) in this process against `remote-windows-proxy-test-app.exe` (its own process), over loopback TCP to the real host manager (`src/backend_win/remote_windows_proxy.cpp`), which creates real proxies on this desktop. The fake private display is the right half of the primary work area. Two roots shared at once (standard frame; custom chrome without a system caption); shell entries (unowned, `WS_EX_APPWINDOW`, guest title and icon, per-application AUMID, sources owned by the helper); no host caption or border strip (client top = visible top, client = guest visible frame, the DWM border is drawn transparent; guest marker/title band at the client origin); guest hit testing through the proxy (custom caption, minimize/maximize/close boxes, top edge; standard caption and the application's own maximize button for Snap Layouts; host side border = native resize); acknowledged resize (900x600 reaches the source and the frames), the application minimum adopted and mirrored in WM_GETMINMAXINFO; host maximize fills the work area with the source sized but never maximized, restore, host minimize leaves the source untouched; application maximize/minimize replayed on the proxy; an overflowing menu as an owned non-activating popup outside the root; a modal dialog as an owned framed proxy with the root disabled, host Close refused, the dialog moved by +40,+30 through the host; Close with unsaved changes shows the prompt, cancellation keeps window and proxy; unselect restores exactly without closing; a real Close removes the proxy; stopping the manager (app exit) restores the source; no proxy windows left. `-Snap`: Win+Left snaps a proxy to the left half and the guest follows the snapped size. Runs with shared GPU pools and again with forced CPU frames. |

Guest check (Windows 11 guest with the eh6.6 helper, branch host so the VM has services 9-11):
`bin\tests\remote-windows-proxy\remote-windows-proxy-vm-check.exe --vm <runtime-id> --select <title part> [--select ...] [--seconds N] [--cpu] [--log <file>] [--evidence <dir>]`
uses the real manager over AF_HYPERV, prints the guest inventory, shares the matching windows
and keeps the proxies for interactive qualification (move, resize, Snap, minimize/maximize, menus,
dialogs, Close with unsaved changes). It checks shell entry, frames, no host caption and client =
guest frame per proxy; Ctrl+C or the timeout ends sharing (the guest restores its windows).
Exit 2 means the guest helper is older (upgrade required) or not reachable.

Proxy rules (see `asb_remote_windows.h`, "Proxy windows", and `remote_windows_proxy.h`): one
guest pixel per host pixel; the proxy client area is the guest window's visible frame, so the
application's own title bar is the only one; Close goes to the application (SC_CLOSE) and the
proxy stays until the window really closes; a modal owned window disables the root proxy;
host minimization never minimizes the source.

# Remote Windows host and guest tray selectors (eh6.8)

```powershell
.\tools\tests\test-remote-windows-selector.ps1            # model suite + native UI suite
.\tools\tests\test-remote-windows-selector.ps1 -SkipNative
.\tools\tests\test-remote-windows-selector.ps1 -Evidence C:\path\to\evidence   # flyout screenshots (BMP)
```

| Suite | Covers |
| --- | --- |
| Model (`remote_windows_selector_test.c`) | Flyout placement on bottom/top/left/right taskbars, the overflow area, clamping at screen edges, negative-coordinate monitors and oversized flyouts; tray click sequences (a single click deferred by the double-click time, double click only shows App Sandbox, right click cancels a pending click, the click that dismissed the flyout does nothing); row and status texts for every state (requesting, sharing, shared, restoring, failed with reason, stale windows silent; connecting, not connected, upgrade, no guest, several guests, empty); duplicate titles distinguished by application. Host session: a guest SELECT_REQUEST runs through the host's own selection path (SELECT_REQUEST back to the guest, proxy on SHARED, unselect restores), refusals answered with ERROR carrying the guest's request ID (stale ID, upgrade required), a guest without the capability is a protocol violation, inventory icons only for listed IDs. Guest helper: a toggle sends SELECT_REQUEST and the row is pending, one request per row, a host ERROR shows the reason, the host acting on the window clears it, share states map to shared/sharing/restoring, unanswered requests fail after 5 s, inventory icons sent once and again only when they change, Restore all reaches the host, a host without the selector shows the upgrade state and refuses toggles locally. |
| Native (`remote_windows_selector_native_test.c`) | Real flyout windows (dark and light): rows, toggles, statuses and placement above the taskbar; Space/Enter only request (pending row, a second toggle refused) until the model acknowledges; a rejected request shows its reason; Home/End/Down navigation; a new window and a new title keep the focus on the same window; MSAA exposes each row as a named check button with its checked state; Esc and deactivation close (deactivation is reported so the dismissing tray click is ignored); upgrade state disables the rows; empty and several-guests states. At 150 % DPI: rows and width scale, 8 rows visible and the rest scrolls, top-taskbar placement and side anchors on the anchor's monitor. The tray icon is re-added on `TaskbarCreated` (Explorer restart). The guest tray thread: published models drive its flyout, a toggle becomes a selection action for the control thread, `TaskbarCreated` re-adds the guest icon. |

Host: tray left click opens the selector for the sole running Windows guest (right-click menu and
double-click Show App Sandbox unchanged). Guest: the helper's tray icon opens the same flyout;
right click offers Restore all windows and Exit (everything is restored first; the service does
not start the helper again in that session).

# Remote Windows two-way copy-only OLE file drag and drop (eh6.9)

```powershell
.\tools\tests\test-remote-windows-files.ps1               # model suite + native suite
.\tools\tests\test-remote-windows-files.ps1 -SkipNative
.\tools\tests\test-remote-windows-files.ps1 -Verbose      # native suite with bridge/broker logs
.\tools\tests\test-remote-windows-protocol.ps1            # includes the drag/file codecs
.\tools\tests\test-remote-windows-proxy.ps1 -SkipNative   # includes the session/helper drag arbitration
node tools\tests\remote_windows_registration_test.cjs
```

| Suite | Covers |
| --- | --- |
| Codecs (`remote_windows_protocol_test.c`) | DRAG_PROBE / DRAG_STATE / DRAG_ACK on the control channel only; FILES_HELLO / FILE_POINTER / FILE_FEEDBACK / FILE_STREAM on the file channel; field validation (probe needs a surface and serial, ACTIVE needs a session, state/ack values per type, effects only NONE or COPY, chunk limits, pointer bounds, top-level and entry limits found before allocation, host -> guest BEGIN needs a target); hostile entry names patched onto the wire (traversal, roots, drives, streams, devices, trailing dots/spaces, empty segments, `\\?\`) never decode. |
| Model (`remote_windows_files_test.c`) | The receive rules of `asb_rw_files.c`: a valid nested Unicode stream; index gaps, wrong session, entries before BEGIN, STREAM too early / too small / too large; hostile names; top-level single segments; orphans, entries under files, late top-level items; duplicates and case aliases (ASCII and accented); directory sizes; data before STREAM, for directories, past the size, at a wrong offset, interleaved, restarted, empty, unknown index; a nested file's data must follow its entry; END with missing entries/bytes, wrong totals or an error status; 20 000 entries. |
| Session (`remote_windows_proxy_test.c`, "drag") | Host pointer arbitration: probe only for a button this source pressed in the active proxy; while probing nothing is injected (moves, Escape); stale probe answers refused; FROM_GUEST releases the proxy's capture and forgets the held button (no button-up injected even when host focus moves to the broker or the session ends); NONE resumes normal forwarding; SINK answers the host button-up with DRAG_ACK RELEASE; unanswered probes time out; TO_GUEST claims a fresh input epoch before DRAG_ACK ALLOW, a second drag is refused, a viewer takeover cancels once, automation ownership refuses; capability gating on both sides; the guest helper routes DRAG_PROBE / DRAG_ACK only when files were negotiated and publishes the surface table. |
| Native (`remote_windows_files_native_test.cpp`) | Real trees (nested Unicode folders, empty files/folders, hidden files, 9-64 MB files). Sources: order and totals; junctions inside and at the top, symbolic links, offline items, missing paths, devices, `GLOBALROOT`, drive roots, empty and duplicate names rejected; a file truncated or replaced after the walk fails the stream. Staging: byte-for-byte equality, delivered staging kept, truncated and hostile streams removed. Bridge: the guest bridge and the host broker in one process over loopback TCP with real shell data objects, real OLE drop targets/sources and real `IFileOperation` copies (only the two `DoDragDrop` loops and the guest pointer are simulated, because a real drag would follow this desktop's mouse). Host -> guest: feedback only after the guest target reports, the guest pointer mapped to the surface and released at the drop point, a 64 MB + tree copy, a second drop renamed by the destination's own conflict handling, a refusing target, leave, Escape in the guest loop, unsupported data, a junction found by the background walk, a source deleted after the drag began. Guest -> host: probe, offer, deferred drop until staged, Escape in the host loop, unsupported data parked until RELEASE, no drag. Disconnect mid-transfer: nothing delivered, the pointer released, incomplete staging removed. Sources intact, the clipboard sequence number unchanged. |

Native qualification (real host and guest; needs the eh6.9 host install, `AppSandboxDrag.exe` beside
`AppSandbox.exe`, and the 1.4.0 helper in the guest): drag files and a nested Unicode folder from host
Explorer onto a shared guest Explorer window (and the reverse, out of a shared window onto the host
desktop or an Explorer folder). Expect the copy cursor only where the guest target accepts, the
destination's own conflict prompt, progress/Cancel in the host broker window for large copies, the
sources untouched, and nothing on the clipboard. The broker logs to
`%LOCALAPPDATA%\AppSandbox\RemoteWindows\drag-broker.log`; the helper to its `helper.log`.
