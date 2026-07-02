# Installation

- **Board:** M5Stack  
- **Libraries:** LibSSH-ESP32, M5Unified, M5UnitUnified, M5Unit-KEYBOARD (and all required dependencies)

# Compile

- Use `m5stack:esp32:m5stack_tab5`.
- Do not add the `ChipVariant` option unless your installed M5Stack board package explicitly exposes it.

# Mode

- SSH mode starts directly.
- Swipe left from the top-right `CFG` tab or the right edge to open the touch settings drawer.
- Settings can edit WiFi, SSH session fields, paste a private-key body, clear WiFi, and clear the stored key.
- WiFi and session fields are stored in NVS. The private key is stored as `/ssh_key` in LittleFS.

# Notice
- Only support PEM Private Key
- Stored passwords and keys are not secure against physical flash extraction unless the device firmware enables flash encryption / secure boot.
```bash
-----BEGIN RSA PRIVATE KEY-----
-----END RSA PRIVATE KEY-----
```
