eh6.7 guest update (x64)
  appsandbox-remote-windows.exe  Remote Windows helper 1.2.0: ACTIVATE_REQUEST/ACK, foreground release,
                                 foreground loss/gain notifications (host input arbitration)
  appsandbox-input.exe           input injector: mouse version 3 (INPUT_MOUSE_VIRTUAL, guest
                                 virtual-screen pixels incl. the private display)
Install: run install.ps1 elevated. It stops AppSandboxAgent (shared windows are restored), keeps
*.eh66-original copies once, copies both files beside appsandbox-agent.exe and restarts the service.
Rollback: copy the *.eh66-original files back over the new ones (elevated, service stopped).
