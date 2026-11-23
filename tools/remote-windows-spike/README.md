# Remote Windows feasibility spike (AppSandbox-eh6.1)

Throwaway prototype that proves the riskiest parts of seamless guest windows
(epic AppSandbox-eh6) on a real Windows 11 host and guest before product work
begins. It is not product code. eh6.2 replaces the wire protocol and
eh6.3–eh6.9 replace these components.

## Result

**PASS.** Every capability in the gate was demonstrated natively on
2026-11-20, using the existing HCS / Hyper-V socket backend:

| Capability | Result |
|---|---|
| Private second display | `AppSandboxVDD` adds a second IddCx monitor (`DISPLAY\ASB0002`, 2560x1440, 120 DPI) on demand and removes it cleanly. The original monitor stays the guest desktop, and the viewer/screenshot/automation path keeps using it (API screenshots stayed 1920x1080 throughout). |
| Capture of parked roots | Notepad and File Explorer were parked on the private display (normal show state, never minimized or hidden). Windows Graphics Capture `CreateForWindow` delivered frames continuously (`IsSupported=1`, capture hr `S_OK`). |
| Guest shell disappearance | While shared, the window was absent from the visible guest desktop, the guest taskbar and guest Alt+Tab (screenshots). |
| Restoration | Unselect, host disconnect and helper exit restored the journaled placement and style exactly. For example, Explorer returned to `(200,120,980,720)`, `ex=0x100`, owner `0`. After restoration it was back on the taskbar and in Alt+Tab. |
| Host window | Native medium-integrity host window with its own taskbar entry. It took real host mouse and keyboard input: a click moved the guest caret, Ctrl+N opened a guest tab, and typed text appeared in the guest. Host Esc dismissed a guest dialog. |
| Popups outside root bounds | A WinUI context menu (`Microsoft.UI.Content.PopupWindowSiteBridge`), an owned modal `#32770` Save As dialog wider than the root, and tooltips were each captured separately and shown as owned no-activate host windows at their guest offsets, extending beyond the root. |
| OLE host → guest | Dragging a real host Explorer folder onto the proxy ran a genuine guest `DoDragDrop` into guest Explorer (`DRAGDROP_S_DROP`, `DROPEFFECT_COPY`). The recursive Unicode folder arrived, a 2 MiB file's SHA-256 matched, and the host source was untouched. |
| OLE guest → host | A drag started in guest Explorer inside the proxy was caught by the guest helper's `IDropTarget` catcher on the private display after the pointer left the proxy. The `CF_HDROP` files were shipped and a host `DoDragDrop` dropped them into host Explorer (`DROPEFFECT_COPY`). The guest drag then ended with `DROPEFFECT_NONE`, so the guest source was preserved. The recursive Unicode folder arrived and a 3 MiB file's SHA-256 matched. |

No RemoteApp hosting, process injection, minimizing/hiding of capture sources
or host security-policy change was used. The host process ran unelevated.

## Prerequisites observed

- Host: Windows 11 Pro 10.0.26200, AppSandbox 0.1.13 at `C:\Application\AppSandbox` (unchanged, still running elevated). The spike host process runs at **medium integrity**.
- Guest: Windows 11 **Home** 26H2 build 26300.9818, x64, one VM ("Main", HCS RuntimeId `52fb82b7-1d7e-54a4-80f4-3578cf3b4507`), standard user session, `testsigning` already on.
- Guest driver: this branch's `AppSandboxVDD` (oem9.inf, DriverVer `11/20/2026,0.1.13.1`), catalog signed with a new local `CN=AppSandbox VDD Development Test` certificate (thumbprint `E888DD0F7AEAC7910998E1785C810925D2A3EB7E`) that was imported into the guest's LocalMachine Root and TrustedPublisher stores. The previous package (oem8.inf, 0.1.13.0) was exported to `C:\eh61\backup` in the guest.
- APIs: Windows Graphics Capture (C++/WinRT, SDK 10.0.26100), `IGraphicsCaptureItemInterop::CreateForWindow`, `ITaskbarList::DeleteTab/AddTab`, OLE `DoDragDrop`/`RegisterDragDrop`, Shell `BHID_DataObject`, `SendInput`, `AF_HYPERV`.
- Transport: the spike uses **channel 7** (`a5b0cafe-0007-…`). It is the only spare service already in the running VM's HvSocket `ServiceTable` (Everyone connect SDDL) and is idle because SSH is off for this VM. Channels 9/10/11 need `hcs_vm.c` ServiceTable entries plus a VM restart (eh6.2/eh6.3).

## Components

| File | Role |
|---|---|
| `tools/vdd/vdd.cpp`, `vdd.h` | Private monitor: `MaxMonitorsSupported = 2`. A periodic WDF timer queues a work item that creates or departs the monitor according to `HKLM\SOFTWARE\AppSandbox\VDD!PrivateDisplay`. Its swap chain is only drained, on its own cached D3D device, and never touches the desktop frame or screenshot channels. Same EDID name (so the guest display watcher keeps it), distinct PnP ID `ASB0002`. |
| `rw_spike_guest.cpp` | Standard-user helper: AF_HYPERV listener, eligible-root inventory (elevated processes excluded), journal, parking, suppression, per-window WGC, popup tracking, `SendInput`, and both OLE bridges. |
| `rw_spike_host.cpp` | Medium-integrity host proxy: AF_HYPERV client, GDI-rendered root and popup windows, input forwarding, OLE drop target and drag source. |
| `rw_spike_common.h`, `rw_spike_proto.h` | Framing, copy-only file transfer with traversal/reparse rejection and cleanup of failed staging, and the shell data object. |

## Commands

```bat
tools\remote-windows-spike\build.cmd
rem guest (signed-in user, limited token):  rw_spike_guest.exe
rem host (unelevated):
tools\remote-windows-spike\bin\rw_spike_host.exe 52fb82b7-1d7e-54a4-80f4-3578cf3b4507 "eh61 - File Explorer" 3480,100 owner
```

The VDD was built with VS 2022 MSBuild using `/p:Driver_SpectreMitigation=false` (the Spectre libraries are not installed here; the default script picks VS 18, which lacks a matching WDK task DLL). The catalog was then made with `Inf2Cat` and `signtool`, as `tools/MakePackageDev.ps1` does. The guest install was `pnputil /add-driver AppSandboxVDD.inf /install`. The private display was toggled with `PrivateDisplay` = 1/0.

Logs: the guest writes `%LOCALAPPDATA%\AppSandbox\RemoteWindowsSpike\{helper,journal}.log`, the host writes `%TEMP%\AppSandboxRemoteWindowsSpike\host.log`, and the driver writes `C:\ProgramData\AppSandboxVDD.log` (`Private:` lines).

## Findings that constrain the plan

1. **Owner-based suppression is preferred over `WS_EX_TOOLWINDOW`.** Adding the tool-window bit removed Notepad's caption min/max buttons, a visible chrome change captured in the frame. Re-parenting the root to a hidden helper owner (`GWLP_HWNDPARENT`), clearing `WS_EX_APPWINDOW` and calling `DeleteTab` removed taskbar and Alt+Tab presence with the frame untouched. Cross-process ownership attaches input queues, so the helper's UI thread must never block (eh6.4).
2. **Mixed-DPI restore needs two placements.** Restoring from the 120-DPI private display to the 96-DPI desktop with one `SetWindowPlacement` shrank the window by 0.8 on every cycle. Applying the journaled placement twice (once to cross the DPI change, once at the final DPI) restores it exactly.
3. **The host must be medium integrity.** AppSandbox.exe runs elevated, and UIPI blocks drops from a medium-integrity Explorer into elevated windows. The per-VM host manager and proxies (eh6.6) need a medium-integrity process that brokers to the elevated core. An unelevated process can connect to the VM's AF_HYPERV services because their connect SDDL is `D:P(A;;FA;;;WD)`.
4. **OLE drag from injected input** needs the dragging thread to have retrieved the injected button-down message, because OLE reads `GetKeyState` before `DoDragDrop`. Without that, the drop happens instantly with no effect.
5. **Buttons are logical.** The host has `SM_SWAPBUTTON=1`. The proxy forwards logical buttons, and the guest maps them to physical `SendInput` flags using the guest's own swap setting. Synthesized host keys may carry a zero scan code or a spurious extended bit, so normalize them from the virtual key.
6. **The guest foreground is shared.** Keys typed in the full-desktop viewer went to the parked, foreground Notepad. eh6.7 must arbitrate the viewer against shared windows, as the epic requires.
7. **Popup surfaces need alpha.** Tooltip and `SysShadow` windows render as opaque black when blitted. Use per-pixel-alpha layered host windows (or D3D/DirectComposition) and skip shadow-only windows. WGC window frames can also be smaller than `GetWindowRect` (shadow margins), so size popups from the frame.
8. **Parking does not move the guest cursor.** Injected input moves the single guest cursor onto the private display. The private display sits next to the desktop, so a user moving the guest cursor past the right edge can reach it (eh6.3/eh6.7).
9. **The private display is controlled by an admin-only registry value** in this spike. Product work needs a negotiated control path that a standard user can drive (eh6.3).
10. **The driver reinstall reset the desktop mode.** The guest desktop went from the viewer-matched 3440x1334 to 1920x1080 and stays there until the viewer renegotiates its size. Existing-guest upgrade guidance (eh6.11) should mention this.
11. Bulk file data shares the control stream in the spike. The product must use channel 11, as planned.

## Evidence

Screenshots are in `build/eh6.1-evidence/` (not committed; they show private desktop content). Logs are listed above.
