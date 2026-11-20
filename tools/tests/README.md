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
