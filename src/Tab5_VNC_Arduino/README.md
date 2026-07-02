# Tab5 VNC Arduino

[English](README_EN.md)

这是一个面向 M5Stack Tab5 的 Arduino VNC 客户端实验项目。它使用 Tab5 屏幕、触摸屏和 Tab5 Keyboard，通过 Wi-Fi 连接兼容的 VNC 服务端。

## 功能状态

- 支持 TigerVNC `VncAuth` 经典密码认证。
- 支持 Tab5 1280x720 RGB565 显示。
- 支持 Tab5 Keyboard 基础按键输入。
- 支持触摸短按左键、长按右键。
- 暂不支持现代 VNC 加密认证、用户名登录、剪贴板、音频、USB 键鼠。

## 配置

复制配置模板：

```bash
cp config_example.h config.h
```

修改 `config.h`：

```cpp
#define WIFI_CONFIG_SSID      "YOUR_WIFI_SSID"
#define WIFI_CONFIG_PASSWORD  "YOUR_WIFI_PASSWORD"

#define VNC_CONFIG_HOST       "192.168.1.100"
#define VNC_CONFIG_PORT       5901
#define VNC_CONFIG_PASSWORD   "YOUR_VNC_PASSWORD"
#define VNC_CONFIG_WIDTH      1280
#define VNC_CONFIG_HEIGHT     720
```

`config.h` 已被 `.gitignore` 忽略，不应提交到仓库。

## TigerVNC 示例

树莓派上可使用 TigerVNC 虚拟桌面：

```bash
tigervncserver -kill :1
tigervncserver :1 -SecurityTypes VncAuth -localhost no -geometry 1280x720 -depth 16
```

`:1` 对应端口 `5901`。

## 编译

Arduino CLI 示例：

```powershell
& 'D:\Softwares\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe' compile --fqbn m5stack:esp32:m5stack_tab5 .\src\Tab5_VNC_Arduino
```

## 致谢与声明

本项目 vendored 并修改了 [Links2004/arduinoVNC](https://github.com/Links2004/arduinoVNC) 的核心源码，用于 VNC 协议处理；显示后端改为 Tab5/M5GFX 适配。感谢 Markus Sattler 和 arduinoVNC 项目。

arduinoVNC 使用 GPL-2.0 许可证。请在分发本项目或派生版本时遵守原项目许可证要求。
