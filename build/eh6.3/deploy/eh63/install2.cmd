@echo off
rem eh6.3: swap in this branch's display watcher (original kept as *.eh63-original)
rem and restart the VDD device to clear a private-display lease orphaned by a test run.
rem Run elevated.
setlocal
pushd "%~dp0"
echo ==== %DATE% %TIME% > install2.log
set AGENT=
for /f "delims=" %%p in ('powershell -nop -c "(Get-Process appsandbox-agent -ErrorAction SilentlyContinue | Select-Object -First 1).Path"') do set "AGENT=%%p"
echo agent=%AGENT%>> install2.log
if "%AGENT%"=="" goto restart
for %%d in ("%AGENT%") do set "DIR=%%~dpd"
if not exist "%DIR%appsandbox-displays.exe.eh63-original" move "%DIR%appsandbox-displays.exe" "%DIR%appsandbox-displays.exe.eh63-original" >> install2.log 2>&1
copy /y "%~dp0appsandbox-displays.exe" "%DIR%appsandbox-displays.exe" >> install2.log 2>&1
taskkill /f /im appsandbox-displays.exe >> install2.log 2>&1
:restart
pnputil /restart-device "ROOT\DISPLAY\0000" >> install2.log 2>&1
echo done >> install2.log
type install2.log
popd
