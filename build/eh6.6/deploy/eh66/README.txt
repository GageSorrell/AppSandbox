AppSandbox eh6.6 guest check (Remote Windows native host proxies)

1. In the guest, elevated:  powershell -ExecutionPolicy Bypass -File install.ps1
   (swaps only appsandbox-remote-windows.exe; the previous one is kept as *.eh65-original;
   restarting the service revokes viewer input control - click "Return control").
2. In the guest, unelevated: remote-windows-proxy-test-app.exe (a standard and a custom-chrome
   window), plus Notepad / Explorer / a browser as wanted.
3. On the host, unelevated:
   bin\tests\remote-windows-proxy\remote-windows-proxy-vm-check.exe --vm <runtime-id>
       --select "eh6.6 standard" --select "eh6.6 custom" --select Notepad --seconds 300 --log <file>
   then move/resize/Snap/minimize/maximize the proxies, open menus and dialogs, Close with
   unsaved changes and cancel. Ctrl+C (or the timeout) ends sharing; the guest restores.
Rollback: copy *.eh65-original back over appsandbox-remote-windows.exe (service stopped).
