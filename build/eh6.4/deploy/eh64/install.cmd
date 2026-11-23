@echo off
rem eh6.4: install this branch's guest service (supervises the Remote Windows helper) and
rem appsandbox-remote-windows.exe and the updated display watcher next to it. The original service binary is kept as
rem *.eh64-original. Run elevated. Then, unelevated: remote-windows-share-native-test.exe --guest
setlocal
pushd "%~dp0"
echo ==== %DATE% %TIME% > install.log
set AGENT=
for /f "delims=" %%p in ('powershell -nop -c "(Get-CimInstance Win32_Service -Filter \"Name='AppSandboxAgent'\").PathName.Trim([char]34)"') do set "AGENT=%%p"
echo agent=%AGENT%>> install.log
if "%AGENT%"=="" (echo AppSandboxAgent service not found>> install.log & goto end)
for %%d in ("%AGENT%") do set "DIR=%%~dpd"
net stop AppSandboxAgent >> install.log 2>&1
taskkill /f /im appsandbox-displays.exe >> install.log 2>&1
taskkill /f /im appsandbox-remote-windows.exe >> install.log 2>&1
timeout /t 2 /nobreak >nul
if not exist "%AGENT%.eh64-original" copy /y "%AGENT%" "%AGENT%.eh64-original" >> install.log 2>&1
copy /y "%~dp0appsandbox-agent.exe" "%AGENT%" >> install.log 2>&1
if not exist "%DIR%appsandbox-displays.exe.eh64-original" copy /y "%DIR%appsandbox-displays.exe" "%DIR%appsandbox-displays.exe.eh64-original" >> install.log 2>&1
copy /y "%~dp0appsandbox-displays.exe" "%DIR%appsandbox-displays.exe" >> install.log 2>&1
copy /y "%~dp0appsandbox-remote-windows.exe" "%DIR%appsandbox-remote-windows.exe" >> install.log 2>&1
net start AppSandboxAgent >> install.log 2>&1
:end
echo done >> install.log
type install.log
popd
