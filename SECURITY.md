# Security

WheelBri installs a Windows service that runs as SYSTEM (the WheelBri Helper), two drivers and its own code-signing
certificate in the PC's trusted stores, and on Linux a udev rule for `/dev/uhid`. A flaw in any of them can matter
beyond WheelBri, so please report it privately.

## Reporting

Use GitHub's private vulnerability reporting: the repository's **Security** tab > **Report a vulnerability**
(<https://github.com/grusnac/WheelBri/security/advisories/new>). Please do not open a public issue.

Say which version (Settings > Apps, or `VERSION.txt` in the package), which Windows or Linux, and how to reproduce it.
The aim is an answer within a week, and a fix in a release whose changelog credits you, unless you would rather not be
named.

## Supported versions

Only the latest release gets security fixes.

## In scope

- The WheelBri Helper and its pipe: any way for a standard user to make it do something they could not do themselves.
- The drivers (`wheelbri-wheel`, `wheelbri-hide`), the installer and uninstaller, the signing certificate's handling.
- The UDP links on 127.0.0.1 (41329-41331): another program steering the virtual wheel or sending forces.
- Force feedback reaching the wheel around the safety limits.
- The shim (`dinput8.dll`) in a game's folder, `install-udev-rules.sh` and `wheelbri.sh`.

Logitech's software, G HUB and games are out of scope: report those to their makers.
