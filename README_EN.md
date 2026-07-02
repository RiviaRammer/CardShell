# M5Term

[中文](README.md)

M5Term is a set of lightweight remote terminal and remote desktop experiments for M5Stack devices. The goal is to bring common remote access workflows to small embedded screens.

![Example](images/screen.jpg)

## Projects

| Board | SSH | VNC | RDP |
| --- | --- | --- | --- |
| M5Stack Cardputer | Supported | Not supported yet | Not supported yet |
| M5Stack Tab5 | Supported | Experimental, simple VNC/VncAuth only | Not supported yet |

## Folders

- `src/Cardputer_CLI_Arduino`: Cardputer SSH terminal.
- `src/Tab5_CLI_Arduino`: Tab5 SSH terminal.
- `src/Tab5_VNC_Arduino`: Tab5 VNC client experiment.

## Notes

The VNC client currently targets servers such as TigerVNC configured with classic `VncAuth`. Modern encrypted authentication, username login, clipboard, audio, and RDP are not supported yet.
