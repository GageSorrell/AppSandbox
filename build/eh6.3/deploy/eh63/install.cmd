@echo off
rem eh6.3: install this branch's test-signed AppSandboxVDD (run elevated).
pushd "%~dp0"
echo ==== %DATE% %TIME% > install.log
pnputil /add-driver "%~dp0vdd\AppSandboxVDD.inf" /install >> install.log 2>&1
echo pnputil exit %ERRORLEVEL% >> install.log
pnputil /enum-devices /class Display /drivers >> install.log 2>&1
type install.log
popd
