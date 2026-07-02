#pragma once

#define WIFI_CONFIG_SSID      "YOUR_WIFI_SSID"
#define WIFI_CONFIG_PASSWORD  "YOUR_WIFI_PASSWORD"

#define VNC_CONFIG_HOST       "192.168.1.100"
#define VNC_CONFIG_PORT       5900
#define VNC_CONFIG_PASSWORD   ""
#define VNC_CONFIG_WIDTH      1280
#define VNC_CONFIG_HEIGHT     720

// There is intentionally no VNC_CONFIG_USER. This PoC only supports classic
// VNC password authentication, not username/password login prompts.
