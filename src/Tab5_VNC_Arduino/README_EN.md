# Tab5 VNC Arduino

[中文](README.md)

An experimental Arduino VNC client for M5Stack Tab5. It uses the Tab5 display, touch screen, and Tab5 Keyboard to connect to a compatible VNC server over Wi-Fi.

## Status

- Supports TigerVNC classic `VncAuth` password authentication.
- Supports Tab5 1280x720 RGB565 display output.
- Supports basic Tab5 Keyboard input.
- Supports touch short press as left click and long press as right click.
- Does not support modern encrypted VNC authentication, username login, clipboard, audio, or USB keyboard/mouse.

## Configuration

Copy the template:

```bash
cp config_example.h config.h
```

Edit `config.h`:

```cpp
#define WIFI_CONFIG_SSID      "YOUR_WIFI_SSID"
#define WIFI_CONFIG_PASSWORD  "YOUR_WIFI_PASSWORD"

#define VNC_CONFIG_HOST       "192.168.1.100"
#define VNC_CONFIG_PORT       5901
#define VNC_CONFIG_PASSWORD   "YOUR_VNC_PASSWORD"
#define VNC_CONFIG_WIDTH      1280
#define VNC_CONFIG_HEIGHT     720
```

`config.h` is ignored by `.gitignore` and should not be committed.

## TigerVNC Example

On Raspberry Pi, a TigerVNC virtual desktop can be started with:

```bash
tigervncserver -kill :1
tigervncserver :1 -SecurityTypes VncAuth -localhost no -geometry 1280x720 -depth 16
```

Display `:1` maps to port `5901`.

## Build

Arduino CLI example:

```powershell
& 'D:\Softwares\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe' compile --fqbn m5stack:esp32:m5stack_tab5 .\src\Tab5_VNC_Arduino
```

## Credits And Notice

This project vendors and modifies the core source code from [Links2004/arduinoVNC](https://github.com/Links2004/arduinoVNC) for VNC protocol handling. The display backend is replaced with a Tab5/M5GFX adapter. Thanks to Markus Sattler and the arduinoVNC project.

arduinoVNC is licensed under GPL-2.0. Follow the original license terms when distributing this project or derived versions.
