@echo off
rem Builds the eh6.1 feasibility spike binaries (x64) into tools\remote-windows-spike\bin.
rem Requires VS 2022 C++ tools and Windows SDK 10.0.26100 (C++/WinRT headers).
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist bin mkdir bin
set FLAGS=/nologo /EHsc /std:c++20 /O2 /W3 /DUNICODE /D_UNICODE /permissive- /Zc:__cplusplus /utf-8
cl %FLAGS% rw_spike_guest.cpp /Fe:bin\rw_spike_guest.exe /Fo:bin\ /link /SUBSYSTEM:WINDOWS || exit /b 1
cl %FLAGS% rw_spike_host.cpp /Fe:bin\rw_spike_host.exe /Fo:bin\ /link /SUBSYSTEM:CONSOLE || exit /b 1
echo built bin\rw_spike_guest.exe bin\rw_spike_host.exe
