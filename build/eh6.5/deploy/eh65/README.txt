eh6.5 guest check (Windows 11 guest "Main")

1. Elevated:   install.cmd (runs install.ps1: service + watcher + helper with capture engine; log install.log)
2. Unelevated: remote-windows-capture-test-app.exe
   Shows "eh6.5 capture test" (animated, magenta marker), an owned alpha popup past its right
   edge, opens an overflowing menu at 8-14 s of every 30 s cycle and signals a capture-engine
   device loss at 20 s of every cycle. Runs 300 s. Log: %TEMP%\eh6.5-capture-test-app.log
3. The host runs remote-windows-frames-vm-check.exe (selects the window, receives frames over
   channel 10 with shared GPU pools, then forced CPU, then unselects).

Helper log: %LOCALAPPDATA%\AppSandbox\RemoteWindows\helper.log ("capture:" lines)
Roll back: copy the *.eh64-original files over the installed ones (service stopped).
