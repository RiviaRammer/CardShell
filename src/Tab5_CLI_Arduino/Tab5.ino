#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <vector>
#include <nvs_flash.h>

#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedKEYBOARD.h>
#include <M5HAL.hpp>
#include <M5Utility.h>

#include "libssh_esp32.h"
#include <libssh/libssh.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "handle_char.h"

using namespace m5::unit;
using namespace m5::unit::tab5_keyboard;

namespace {

constexpr int8_t TAB5_KEYBOARD_SDA = 0;
constexpr int8_t TAB5_KEYBOARD_SCL = 1;
constexpr uint32_t TAB5_KEYBOARD_I2C_CLOCK = 400000UL;
constexpr uint8_t DISPLAY_BRIGHTNESS = 180;
constexpr uint8_t TERMINAL_FONT_WIDTH = 12;
constexpr uint8_t TERMINAL_LINE_HEIGHT = 24;

constexpr gpio_num_t TAB5_WIFI_SDIO_CLK = GPIO_NUM_12;
constexpr gpio_num_t TAB5_WIFI_SDIO_CMD = GPIO_NUM_13;
constexpr gpio_num_t TAB5_WIFI_SDIO_D0  = GPIO_NUM_11;
constexpr gpio_num_t TAB5_WIFI_SDIO_D1  = GPIO_NUM_10;
constexpr gpio_num_t TAB5_WIFI_SDIO_D2  = GPIO_NUM_9;
constexpr gpio_num_t TAB5_WIFI_SDIO_D3  = GPIO_NUM_8;
constexpr gpio_num_t TAB5_WIFI_SDIO_RST = GPIO_NUM_15;

constexpr int SETTINGS_EDGE_WIDTH = 40;
constexpr int SETTINGS_SWIPE_DISTANCE = 30;
constexpr int SETTINGS_PANEL_WIDTH = 260;
constexpr int SETTINGS_HANDLE_WIDTH = 56;
constexpr int SETTINGS_HANDLE_HEIGHT = 24;
constexpr uint16_t SETTINGS_BG = 0x18E3;
constexpr uint16_t SETTINGS_HEADER = 0x03EF;
constexpr uint16_t SETTINGS_BUTTON = 0x39E7;
constexpr uint16_t SETTINGS_BUTTON_BORDER = 0x7BEF;
constexpr uint16_t SETTINGS_HANDLE_BG = 0x3186;
constexpr uint16_t SETTINGS_HANDLE_BORDER = 0x6B4D;

constexpr uint8_t HID_BACKSPACE = 0x2A;
constexpr uint8_t HID_TAB       = 0x2B;
constexpr uint8_t HID_ENTER     = 0x28;
constexpr uint8_t HID_ESCAPE    = 0x29;
constexpr uint8_t HID_SPACE     = 0x2C;
constexpr uint8_t HID_DELETE    = 0x4C;
constexpr uint8_t HID_RIGHT     = 0x4F;
constexpr uint8_t HID_LEFT      = 0x50;
constexpr uint8_t HID_DOWN      = 0x51;
constexpr uint8_t HID_UP        = 0x52;

UnitUnified Units;
UnitTab5Keyboard tab5Keyboard;
Preferences prefs;

struct TouchButton {
    int x;
    int y;
    int w;
    int h;
    const char *label;
};

struct SSHSession {
    String nickname;
    String host;
    String port;
    String user;
    String password;
    bool useKey;
};

bool g_settingsOpen = false;
bool g_touchTracking = false;
int g_touchStartX = 0;
int g_touchStartY = 0;

TouchButton settingsButtons[] = {
    {20,  70, 220, 56, "WLAN"},
    {20, 146, 220, 56, "Sessions"},
    {20, 222, 220, 56, "Keys"},
    {20, 298, 220, 56, "Reset All"},
    {20, 374, 220, 56, "Close"},
};

void bootLog(const char *msg) {
    Serial.println(msg);
    Serial.flush();
}

void setupDisplay() {
    M5.setTouchButtonHeightByRatio(100);
    M5.Display.wakeup();
    M5.Display.setBrightness(DISPLAY_BRIGHTNESS);
    M5.Display.setColorDepth(16);

    if (M5.Display.height() > M5.Display.width()) {
        M5.Display.setRotation(3);
    }

    M5.Display.fillScreen(BLACK);
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(0, 0);
    M5.Display.display();
}

struct KeyInput {
    String text;
    bool enter = false;
    bool backspace = false;
    bool ctrlPressed = false;
    bool resetSsh = false;
    bool resetWifi = false;
};

} // namespace

String g_ssh_host;
String g_ssh_port     = "22";
String g_ssh_user;
String g_ssh_password;

String wifi_ssid;
String wifi_pass;

std::vector<SSHSession> g_sessions;
int g_currentSessionIndex = -1;
volatile int g_selectedSessionIndex = -1;

ssh_session g_session = nullptr;
ssh_channel g_channel = nullptr;

const uint32_t SSH_TASK_STACK_SIZE = 51200;

void startSSHMode();
bool connectWiFi();
bool connectSSH();
void resetSSHConfig();
void sshTask(void *pv);
void waitForInput(String &input, bool hideInput);
void promptSSHPort();
void flushKeyboard();
void redrawInputTail(const String &input, int cursor, bool hideInput);
void moveInputCursorLeft(int count);
void setupTab5WiFiPins();
void loadStoredSettings();
void saveWiFiSettings();
void saveSSHSettings();
void drawSettingsDrawer();
void drawSettingsHandle();
void closeSettingsDrawer();
void handleTouchSettings();
void handleSettingsTap(int x, int y);
void promptWiFiSettings();
void promptSessionSettings();
void promptKeyImport();
void clearStoredWiFi();
void clearStoredKey();
void showSessionsPage();
void showKeysPage();
bool setupTab5Keyboard();
bool setTab5KeyboardMode(Mode mode);
bool readKeyInput(KeyInput &out);
void handleSshKeyboard();
void loadSessions();
void saveSessions();
void addSession();
void editSession(int index);
void deleteSession(int index);
int selectSession();
void connectToSession(int index);

void setup() {
    Serial.begin(115200);
    delay(100);
    bootLog("[BOOT] Serial ready");

    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.internal_mic = false;
    cfg.internal_spk = false;
    bootLog("[BOOT] before M5.begin");
    M5.begin(cfg);
    bootLog("[BOOT] after M5.begin");
    setupTab5WiFiPins();
    Serial.printf("[BOOT] board=%d display=%dx%d psram=%u free=%u\n",
                  (int)M5.getBoard(), M5.Display.width(), M5.Display.height(),
                  (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    Serial.flush();

    esp_err_t nvsErr = nvs_flash_init();
    Serial.printf("[BOOT] nvs_flash_init: %s (%d)\n", nvsErr == ESP_OK ? "OK" : "FAIL", nvsErr);
    if (nvsErr != ESP_OK) {
        nvs_flash_erase();
        nvs_flash_init();
        Serial.println("[BOOT] NVS erased and re-initialized");
    }

    setupDisplay();
    LittleFS.begin(true);
    loadStoredSettings();

    if (!setupTab5Keyboard()) {
        termPrintln("\x1B[31mTab5 Keyboard init failed.\x1B[0m");
        termPrintln("Check Tab5 Keyboard connection: SDA G0, SCL G1, INT G50.");
        while (true) delay(1000);
    }

    termPrintln("\x1B[36m== M5Term Tab5 ==\x1B[0m");
    termPrintln("Swipe from top-right tab for settings.");
    drawSettingsHandle();

    if (!setTab5KeyboardMode(Mode::HID)) {
        termPrintln("\x1B[31mFailed to switch keyboard to HID mode.\x1B[0m");
        while (true) delay(1000);
    }

    startSSHMode();
}

void loop() {
    M5.update();
    Units.update();
    handleTouchSettings();

    if (!g_settingsOpen) {
        KeyInput key;
        if (readKeyInput(key)) {
            if (key.text.indexOf('\x1B') >= 0) {
                Serial.println("[ESC] pressed, returning to home");
                if (g_channel && !ssh_channel_is_eof(g_channel)) {
                    ssh_channel_close(g_channel);
                    ssh_channel_free(g_channel);
                    ssh_disconnect(g_session);
                    ssh_free(g_session);
                    g_channel = nullptr;
                    g_session = nullptr;
                }
                termClear();
                if (g_sessions.empty()) {
                    termPrintln("\x1B[33mNo sessions configured.\x1B[0m");
                    termPrintln("Open settings to add a session.");
                } else {
                    termPrintln("\x1B[36m== M5Term Tab5 ==\x1B[0m");
                    for (int i = 0; i < (int)g_sessions.size(); i++) {
                        String line = String(i + 1) + ". " + g_sessions[i].nickname;
                        if (g_sessions[i].useKey) line += " [Key]";
                        termPrintln(line);
                    }
                    termPrintln("");
                    termPrintln("Open settings to connect.");
                }
                drawSettingsHandle();
            }
        }
    }

    vTaskDelay(50 / portTICK_PERIOD_MS);
}

bool setupTab5Keyboard() {
    bootLog("[BOOT] before Tab5 keyboard init");
    auto cfg = tab5Keyboard.config();
    cfg.mode = Mode::Normal;
    cfg.start_periodic = true;
    cfg.irq_pin = 50;
    tab5Keyboard.config(cfg);

    Wire.end();
    bootLog("[BOOT] Wire.end done");
    Wire.begin(TAB5_KEYBOARD_SDA, TAB5_KEYBOARD_SCL, TAB5_KEYBOARD_I2C_CLOCK);
    bootLog("[BOOT] Wire.begin keyboard bus done");

    if (!Units.add(tab5Keyboard, Wire) || !Units.begin()) {
        bootLog("[BOOT] Units.begin failed");
        return false;
    }

    Serial.printf("Tab5 Keyboard firmware: %02X\n", tab5Keyboard.firmwareVersion());
    Serial.flush();
    bootLog("[BOOT] Tab5 keyboard init OK");
    return true;
}

bool setTab5KeyboardMode(Mode mode) {
    if (!tab5Keyboard.writeMode(mode)) {
        return false;
    }
    tab5Keyboard.flush();
    return true;
}

void setupTab5WiFiPins() {
#if defined(BOARD_SDIO_ESP_HOSTED_CLK) && defined(BOARD_SDIO_ESP_HOSTED_CMD) && \
    defined(BOARD_SDIO_ESP_HOSTED_D0) && defined(BOARD_SDIO_ESP_HOSTED_D1) && \
    defined(BOARD_SDIO_ESP_HOSTED_D2) && defined(BOARD_SDIO_ESP_HOSTED_D3) && \
    defined(BOARD_SDIO_ESP_HOSTED_RESET)
    WiFi.setPins(BOARD_SDIO_ESP_HOSTED_CLK, BOARD_SDIO_ESP_HOSTED_CMD,
                 BOARD_SDIO_ESP_HOSTED_D0, BOARD_SDIO_ESP_HOSTED_D1,
                 BOARD_SDIO_ESP_HOSTED_D2, BOARD_SDIO_ESP_HOSTED_D3,
                 BOARD_SDIO_ESP_HOSTED_RESET);
    Serial.println("[WiFi] SDIO pins configured from board defaults");
#else
    WiFi.setPins(TAB5_WIFI_SDIO_CLK, TAB5_WIFI_SDIO_CMD,
                 TAB5_WIFI_SDIO_D0, TAB5_WIFI_SDIO_D1,
                 TAB5_WIFI_SDIO_D2, TAB5_WIFI_SDIO_D3,
                 TAB5_WIFI_SDIO_RST);
    Serial.println("[WiFi] SDIO pins configured from Tab5 defaults");
#endif
}

bool readKeyInput(KeyInput &out) {
    bool got = false;

    while (!tab5Keyboard.empty()) {
        const auto evt = tab5Keyboard.oldest();
        tab5Keyboard.discard();

        const uint8_t keycode = evt.hid.keycode;
        const uint8_t modifier = evt.modifier;
        const bool ctrl = evt.isCtrl();
        const bool alt = evt.isAlt();

        if (ctrl) {
            out.ctrlPressed = true;
            got = true;
        }

        if (keycode == 0) {
            continue;
        }

        got = true;
        const char c = hidUsageToChar(keycode, modifier);

        if (ctrl && alt && (c == 's' || c == 'S')) {
            out.resetSsh = true;
            continue;
        }
        if (ctrl && alt && (c == 'w' || c == 'W')) {
            out.resetWifi = true;
            continue;
        }

        switch (keycode) {
        case HID_ENTER:     out.enter = true; break;
        case HID_TAB:       out.text += '\t'; break;
        case HID_BACKSPACE: out.backspace = true; break;
        case HID_DELETE:    out.text += "\x1B[3~"; break;
        case HID_LEFT:      out.text += "\x1B[D"; break;
        case HID_RIGHT:     out.text += "\x1B[C"; break;
        case HID_UP:        out.text += "\x1B[A"; break;
        case HID_DOWN:      out.text += "\x1B[B"; break;
        case HID_ESCAPE:    out.text += '\x1B'; break;
        case HID_SPACE:     out.text += ' '; break;
        default:
            if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
                char cc = (c >= 'a' && c <= 'z') ? c - 'a' + 1 : c - 'A' + 1;
                out.text += cc;
            } else if (c >= 0x20 && c <= 0x7E) {
                out.text += c;
            }
            break;
        }
    }

    return got;
}

void loadStoredSettings() {
    Serial.println("[Settings] Loading stored settings from NVS");
    if (!prefs.begin("m5term", true)) {
        Serial.println("[Settings] Failed to open prefs");
        return;
    }

    String storedWifiSsid = prefs.getString("wifi_ssid", "");
    String storedWifiPass = prefs.getString("wifi_pass", "");
    String storedHost = prefs.getString("ssh_host", "");
    String storedPort = prefs.getString("ssh_port", "");
    String storedUser = prefs.getString("ssh_user", "");
    String storedPassword = prefs.getString("ssh_pass", "");
    prefs.end();

    if (!storedWifiSsid.isEmpty()) wifi_ssid = storedWifiSsid;
    if (!storedWifiPass.isEmpty()) wifi_pass = storedWifiPass;
    if (!storedHost.isEmpty()) g_ssh_host = storedHost;
    if (!storedPort.isEmpty()) g_ssh_port = storedPort;
    if (!storedUser.isEmpty()) g_ssh_user = storedUser;
    if (!storedPassword.isEmpty()) g_ssh_password = storedPassword;
    
    Serial.printf("[Settings] WiFi SSID: %s\n", wifi_ssid.isEmpty() ? "(empty)" : wifi_ssid.c_str());
    Serial.printf("[Settings] SSH Host: %s\n", g_ssh_host.isEmpty() ? "(empty)" : g_ssh_host.c_str());
    
    loadSessions();
}

void saveWiFiSettings() {
    if (!prefs.begin("m5term", false)) return;
    prefs.putString("wifi_ssid", wifi_ssid);
    prefs.putString("wifi_pass", wifi_pass);
    prefs.end();
}

void saveSSHSettings() {
    if (!prefs.begin("m5term", false)) return;
    prefs.putString("ssh_host", g_ssh_host);
    prefs.putString("ssh_port", g_ssh_port);
    prefs.putString("ssh_user", g_ssh_user);
    prefs.putString("ssh_pass", g_ssh_password);
    prefs.end();
}

void loadSessions() {
    if (!prefs.begin("m5term", true)) {
        Serial.println("[Sessions] Failed to open prefs for reading");
        return;
    }
    
    int count = prefs.getInt("session_count", 0);
    g_sessions.clear();
    Serial.printf("[Sessions] Loading %d sessions from NVS\n", count);
    
    for (int i = 0; i < count; i++) {
        String prefix = "session_" + String(i) + "_";
        SSHSession session;
        session.nickname = prefs.getString((prefix + "nick").c_str(), "");
        session.host = prefs.getString((prefix + "host").c_str(), "");
        session.port = prefs.getString((prefix + "port").c_str(), "22");
        session.user = prefs.getString((prefix + "user").c_str(), "");
        session.password = prefs.getString((prefix + "pass").c_str(), "");
        session.useKey = prefs.getBool((prefix + "usekey").c_str(), false);
        
        if (!session.host.isEmpty()) {
            g_sessions.push_back(session);
            Serial.printf("[Sessions] Loaded: %s -> %s@%s:%s\n", 
                session.nickname.c_str(), session.user.c_str(), session.host.c_str(), session.port.c_str());
        }
    }
    
    prefs.end();
    Serial.printf("[Sessions] Total loaded: %d\n", (int)g_sessions.size());
}

void saveSessions() {
    if (!prefs.begin("m5term", false)) return;
    
    prefs.putInt("session_count", g_sessions.size());
    
    for (size_t i = 0; i < g_sessions.size(); i++) {
        String prefix = "session_" + String(i) + "_";
        prefs.putString((prefix + "nick").c_str(), g_sessions[i].nickname);
        prefs.putString((prefix + "host").c_str(), g_sessions[i].host);
        prefs.putString((prefix + "port").c_str(), g_sessions[i].port);
        prefs.putString((prefix + "user").c_str(), g_sessions[i].user);
        prefs.putString((prefix + "pass").c_str(), g_sessions[i].password);
        prefs.putBool((prefix + "usekey").c_str(), g_sessions[i].useKey);
    }
    
    prefs.end();
}

void addSession() {
    g_settingsOpen = false;
    termClear();
    termPrint("\x1B[?25l");

    SSHSession session;
    session.port = "22";
    session.useKey = false;

    constexpr int FX = 200, FY = 80, FW = 700, FH = 52;
    constexpr int LBL_H = 40;
    constexpr int GAP = LBL_H + FH + 16;

    auto drawField = [&](int idx, const char *label, const String &val, bool hide) {
        int y = FY + idx * GAP;
        M5.Display.setTextColor(0xA555, BLACK);
        M5.Display.setTextSize(2);
        M5.Display.drawString(label, FX, y);
        int iy = y + LBL_H;
        M5.Display.fillRoundRect(FX, iy, FW, FH, 6, 0x18E3);
        M5.Display.drawRoundRect(FX, iy, FW, FH, 6, 0x7BEF);
        M5.Display.setTextColor(WHITE, 0x18E3);
        M5.Display.setTextSize(2);
        String disp = val;
        if (hide && !val.isEmpty()) { disp = ""; for (unsigned i = 0; i < val.length(); i++) disp += '*'; }
        if (disp.isEmpty()) disp = " ";
        M5.Display.drawString(disp, FX + 10, iy + 14);
    };

    auto drawForm = [&]() {
        M5.Display.fillScreen(BLACK);
        M5.Display.setTextColor(0x07FF, BLACK);
        M5.Display.setTextSize(3);
        M5.Display.drawString("Add Session", FX, 20);
        drawField(0, "Nickname", session.nickname, false);
        drawField(1, "Host", session.host, false);
        drawField(2, "Port", session.port, false);
        drawField(3, "User", session.user, false);
        drawField(4, "Password", session.password, true);
        int cbY = FY + 5 * GAP + 32;
        M5.Display.fillRoundRect(FX, cbY, 24, 24, 4, session.useKey ? 0x07E0 : 0x4208);
        M5.Display.drawRoundRect(FX, cbY, 24, 24, 4, 0x7BEF);
        M5.Display.setTextColor(0xA555, BLACK);
        M5.Display.setTextSize(2);
        M5.Display.drawString("Use Key", FX + 32, cbY + 2);
        int by = FY + 6 * GAP + 10;
        M5.Display.fillRoundRect(FX, by, 240, 48, 6, 0x0560);
        M5.Display.setTextColor(WHITE, 0x0560);
        M5.Display.setTextSize(2);
        M5.Display.drawString("Save", FX + 90, by + 12);
        M5.Display.fillRoundRect(FX + 400, by, 240, 48, 6, 0xA800);
        M5.Display.setTextColor(WHITE, 0xA800);
        M5.Display.drawString("Cancel", FX + 460, by + 12);
    };

    auto promptField = [&](int idx, const char *label, String &target, bool hide) {
        String input = target;
        int cursor = input.length();
        flushKeyboard();
        int iy = FY + idx * GAP + LBL_H;
        M5.Display.fillRoundRect(FX, iy + FH + 4, FW, 36, 4, 0x03EF);
        M5.Display.setTextColor(WHITE, 0x03EF);
        M5.Display.setTextSize(2);
        M5.Display.drawString("Type, then press Enter", FX + 10, iy + FH + 10);

        while (true) {
            M5.update(); Units.update();
            KeyInput key;
            if (readKeyInput(key)) {
                if (key.backspace && cursor > 0) { input.remove(--cursor, 1); }
                for (int i = 0; i < (int)key.text.length(); i++) {
                    char ch = key.text[i];
                    if (ch == '\x1B' && i + 2 < (int)key.text.length() && key.text[i + 1] == '[') {
                        char cmd = key.text[i + 2];
                        if (cmd == 'D' && cursor > 0) { --cursor; i += 2; continue; }
                        if (cmd == 'C' && cursor < (int)input.length()) { ++cursor; i += 2; continue; }
                        if (cmd == '3' && i + 3 < (int)key.text.length() && key.text[i + 3] == '~') {
                            if (cursor < (int)input.length()) input.remove(cursor, 1);
                            i += 3; continue;
                        }
                    }
                    if (ch >= 0x20 && ch <= 0x7E) { input = input.substring(0, cursor) + String(ch) + input.substring(cursor); ++cursor; }
                }
                if (key.enter) { target = input; return; }
                M5.Display.fillRoundRect(FX, iy, FW, FH, 6, 0x18E3);
                M5.Display.drawRoundRect(FX, iy, FW, FH, 6, 0x07FF);
                M5.Display.setTextColor(WHITE, 0x18E3);
                M5.Display.setTextSize(2);
                String disp = input;
                if (hide && !input.isEmpty()) { disp = ""; for (unsigned j = 0; j < input.length(); j++) disp += '*'; }
                if (disp.isEmpty()) disp = " ";
                M5.Display.drawString(disp, FX + 10, iy + 14);
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    };

    drawForm();
    bool done = false;
    while (!done) {
        M5.update();
        Units.update();

        KeyInput key;
        if (readKeyInput(key) && key.text.indexOf('\x1B') >= 0) { Serial.println("[ESC] AddSession"); break; }

        auto touch = M5.Touch.getDetail();
        if (!touch.wasReleased()) { vTaskDelay(10 / portTICK_PERIOD_MS); continue; }
        int tx = touch.x, ty = touch.y;
        Serial.printf("[Form] Tap at (%d,%d)\n", tx, ty);
        if (tx >= FX && tx <= FX + FW) {
            for (int i = 0; i < 5; i++) {
                int iy = FY + i * GAP + LBL_H;
                if (ty >= iy && ty <= iy + FH) {
                    const char *labels[] = {"Nickname", "Host", "Port", "User", "Password"};
                    String *vals[] = {&session.nickname, &session.host, &session.port, &session.user, &session.password};
                    Serial.printf("[Form] Field %d tapped\n", i);
                    promptField(i, labels[i], *vals[i], i == 4);
                    drawForm();
                    break;
                }
            }
            int cbY = FY + 5 * GAP + LBL_H;
            if (ty >= cbY && ty <= cbY + 28) { session.useKey = !session.useKey; drawForm(); }
            int by = FY + 6 * GAP + 10;
            if (ty >= by && ty <= by + 48) {
                if (tx < FX + 400 && !session.nickname.isEmpty() && !session.host.isEmpty()) {
                    g_sessions.push_back(session);
                    saveSessions();
                    Serial.printf("[Session] Added: %s\n", session.nickname.c_str());
                    done = true;
                } else if (tx >= FX + 400) { done = true; }
            }
        }
    }
    termRedraw();
    drawSettingsHandle();
}

void editSession(int index) {
    if (index < 0 || index >= (int)g_sessions.size()) return;
    g_settingsOpen = false;
    termClear();
    termPrint("\x1B[?25l");

    SSHSession &session = g_sessions[index];

    constexpr int FX = 200, FY = 80, FW = 700, FH = 52;
    constexpr int LBL_H = 40;
    constexpr int GAP = LBL_H + FH + 16;

    auto drawField = [&](int idx, const char *label, const String &val, bool hide) {
        int y = FY + idx * GAP;
        M5.Display.setTextColor(0xA555, BLACK);
        M5.Display.setTextSize(2);
        M5.Display.drawString(label, FX, y);
        int iy = y + LBL_H;
        M5.Display.fillRoundRect(FX, iy, FW, FH, 6, 0x18E3);
        M5.Display.drawRoundRect(FX, iy, FW, FH, 6, 0x7BEF);
        M5.Display.setTextColor(WHITE, 0x18E3);
        M5.Display.setTextSize(2);
        String disp = val;
        if (hide && !val.isEmpty()) { disp = ""; for (unsigned i = 0; i < val.length(); i++) disp += '*'; }
        if (disp.isEmpty()) disp = " ";
        M5.Display.drawString(disp, FX + 10, iy + 14);
    };

    auto drawForm = [&]() {
        M5.Display.fillScreen(BLACK);
        M5.Display.setTextColor(0x07FF, BLACK);
        M5.Display.setTextSize(3);
        M5.Display.drawString("Edit Session", FX, 20);
        drawField(0, "Nickname", session.nickname, false);
        drawField(1, "Host", session.host, false);
        drawField(2, "Port", session.port, false);
        drawField(3, "User", session.user, false);
        drawField(4, "Password", session.password, true);
        int cbY = FY + 5 * GAP + LBL_H;
        M5.Display.fillRoundRect(FX, cbY, 24, 24, 4, session.useKey ? 0x07E0 : 0x4208);
        M5.Display.drawRoundRect(FX, cbY, 24, 24, 4, 0x7BEF);
        M5.Display.setTextColor(0xA555, BLACK);
        M5.Display.setTextSize(2);
        M5.Display.drawString("Use Key", FX + 32, cbY + 2);
        int by = FY + 6 * GAP + 10;
        M5.Display.fillRoundRect(FX, by, 240, 48, 6, 0x0560);
        M5.Display.setTextColor(WHITE, 0x0560);
        M5.Display.setTextSize(2);
        M5.Display.drawString("Save", FX + 90, by + 12);
        M5.Display.fillRoundRect(FX + 400, by, 240, 48, 6, 0xA800);
        M5.Display.setTextColor(WHITE, 0xA800);
        M5.Display.drawString("Cancel", FX + 460, by + 12);
    };

    auto promptField = [&](int idx, const char *label, String &target, bool hide) {
        String input = target;
        int cursor = input.length();
        flushKeyboard();
        int iy = FY + idx * GAP + LBL_H;
        M5.Display.fillRoundRect(FX, iy + FH + 4, FW, 36, 4, 0x03EF);
        M5.Display.setTextColor(WHITE, 0x03EF);
        M5.Display.setTextSize(2);
        M5.Display.drawString("Type, then press Enter", FX + 10, iy + FH + 10);

        while (true) {
            M5.update(); Units.update();
            KeyInput key;
            if (readKeyInput(key)) {
                if (key.backspace && cursor > 0) { input.remove(--cursor, 1); }
                for (int i = 0; i < (int)key.text.length(); i++) {
                    char ch = key.text[i];
                    if (ch == '\x1B' && i + 2 < (int)key.text.length() && key.text[i + 1] == '[') {
                        char cmd = key.text[i + 2];
                        if (cmd == 'D' && cursor > 0) { --cursor; i += 2; continue; }
                        if (cmd == 'C' && cursor < (int)input.length()) { ++cursor; i += 2; continue; }
                        if (cmd == '3' && i + 3 < (int)key.text.length() && key.text[i + 3] == '~') {
                            if (cursor < (int)input.length()) input.remove(cursor, 1);
                            i += 3; continue;
                        }
                    }
                    if (ch >= 0x20 && ch <= 0x7E) { input = input.substring(0, cursor) + String(ch) + input.substring(cursor); ++cursor; }
                }
                if (key.enter) { target = input; return; }
                M5.Display.fillRoundRect(FX, iy, FW, FH, 6, 0x18E3);
                M5.Display.drawRoundRect(FX, iy, FW, FH, 6, 0x07FF);
                M5.Display.setTextColor(WHITE, 0x18E3);
                M5.Display.setTextSize(2);
                String disp = input;
                if (hide && !input.isEmpty()) { disp = ""; for (unsigned j = 0; j < input.length(); j++) disp += '*'; }
                if (disp.isEmpty()) disp = " ";
                M5.Display.drawString(disp, FX + 10, iy + 14);
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    };

    drawForm();
    bool done = false;
    while (!done) {
        M5.update();
        Units.update();

        KeyInput key;
        if (readKeyInput(key) && key.text.indexOf('\x1B') >= 0) { Serial.println("[ESC] EditSession"); break; }

        auto touch = M5.Touch.getDetail();
        if (!touch.wasReleased()) { vTaskDelay(10 / portTICK_PERIOD_MS); continue; }
        int tx = touch.x, ty = touch.y;
        Serial.printf("[Form] Edit tap at (%d,%d)\n", tx, ty);
        if (tx >= FX && tx <= FX + FW) {
            for (int i = 0; i < 5; i++) {
                int iy = FY + i * GAP + LBL_H;
                if (ty >= iy && ty <= iy + FH) {
                    const char *labels[] = {"Nickname", "Host", "Port", "User", "Password"};
                    String *vals[] = {&session.nickname, &session.host, &session.port, &session.user, &session.password};
                    promptField(i, labels[i], *vals[i], i == 4);
                    drawForm();
                    break;
                }
            }
            int cbY = FY + 5 * GAP + LBL_H;
            if (ty >= cbY && ty <= cbY + 28) { session.useKey = !session.useKey; drawForm(); }
            int by = FY + 6 * GAP + 10;
            if (ty >= by && ty <= by + 48) {
                if (tx < FX + 400 && !session.nickname.isEmpty() && !session.host.isEmpty()) {
                    saveSessions();
                    Serial.printf("[Session] Updated: %s\n", session.nickname.c_str());
                    done = true;
                } else if (tx >= FX + 400) { done = true; }
            }
        }
    }
    termRedraw();
    drawSettingsHandle();
}

void deleteSession(int index) {
    if (index < 0 || index >= (int)g_sessions.size()) return;
    
    g_sessions.erase(g_sessions.begin() + index);
    saveSessions();
    
    termPrintln("\x1B[32mSession deleted.\x1B[0m");
    delay(600);
}

int selectSession() {
    Serial.printf("[Session] selectSession: %d sessions available\n", (int)g_sessions.size());
    termClear();
    termPrintln("\x1B[36m== Select Session ==\x1B[0m");
    
    if (g_sessions.empty()) {
        termPrintln("No sessions configured.");
        termPrintln("Add sessions in settings.");
        drawSettingsHandle();
        delay(1000);
        return -1;
    }
    
    for (size_t i = 0; i < g_sessions.size(); i++) {
        String label = String(i + 1) + ". " + g_sessions[i].nickname;
        if (g_sessions[i].useKey) {
            label += " [Key]";
        }
        termPrintln(label);
    }
    
    termPrintln("0. Cancel");
    termPrint("Select (1-" + String(g_sessions.size()) + "):");
    
    String input;
    waitForInput(input, false);
    
    int choice = input.toInt();
    if (choice > 0 && choice <= (int)g_sessions.size()) {
        Serial.printf("[Session] User selected %d: %s\n", choice, g_sessions[choice-1].nickname.c_str());
        return choice - 1;
    }
    
    Serial.println("[Session] User cancelled");
    return -1;
}

void connectToSession(int index) {
    if (index < 0 || index >= (int)g_sessions.size()) return;
    
    g_currentSessionIndex = index;
    const SSHSession &session = g_sessions[index];
    
    g_ssh_host = session.host;
    g_ssh_port = session.port;
    g_ssh_user = session.user;
    g_ssh_password = session.password;
    
    termPrintln("\x1B[36mConnecting to " + session.nickname + "...\x1B[0m");
}

bool pointInButton(const TouchButton &button, int x, int y) {
    int baseX = M5.Display.width() - min(SETTINGS_PANEL_WIDTH, (int)M5.Display.width());
    return x >= baseX + button.x && x < baseX + button.x + button.w &&
           y >= button.y && y < button.y + button.h;
}

void drawSettingsButton(const TouchButton &button) {
    int baseX = M5.Display.width() - min(SETTINGS_PANEL_WIDTH, (int)M5.Display.width());
    int x = baseX + button.x;
    M5.Display.fillRoundRect(x, button.y, button.w, button.h, 6, SETTINGS_BUTTON);
    M5.Display.drawRoundRect(x, button.y, button.w, button.h, 6, SETTINGS_BUTTON_BORDER);
    M5.Display.setTextColor(WHITE, SETTINGS_BUTTON);
    M5.Display.setTextSize(2);
    M5.Display.setFont(&fonts::efontCN_24);
    M5.Display.drawString(button.label, x + 20, button.y + 14);
}

void drawSettingsHandle() {
    if (g_settingsOpen) return;

    int x = M5.Display.width() - SETTINGS_HANDLE_WIDTH - 4;
    int y = 2;
    Serial.printf("[UI] Drawing CFG handle at (%d,%d)\n", x, y);
    M5.Display.fillRoundRect(x, y, SETTINGS_HANDLE_WIDTH, SETTINGS_HANDLE_HEIGHT, 4, SETTINGS_HANDLE_BG);
    M5.Display.drawRoundRect(x, y, SETTINGS_HANDLE_WIDTH, SETTINGS_HANDLE_HEIGHT, 4, SETTINGS_HANDLE_BORDER);
    M5.Display.setTextColor(0xE71C, SETTINGS_HANDLE_BG);
    M5.Display.setTextSize(1);
    M5.Display.setFont(&fonts::efontCN_24);
    M5.Display.drawString("CFG", x + 9, y + 1);
    M5.Display.display();
}

void drawSettingsDrawer() {
    g_settingsOpen = true;
    int panelW = SETTINGS_PANEL_WIDTH;
    if (panelW > (int)M5.Display.width()) {
        panelW = M5.Display.width();
    }
    int panelX = M5.Display.width() - panelW;
    M5.Display.fillRect(panelX, 0, panelW, M5.Display.height(), SETTINGS_BG);
    M5.Display.fillRect(panelX, 0, panelW, 56, SETTINGS_HEADER);
    M5.Display.setTextColor(WHITE, SETTINGS_HEADER);
    M5.Display.setTextSize(2);
    M5.Display.setFont(&fonts::efontCN_24);
    M5.Display.drawString("Settings", panelX + 20, 14);

    for (auto &button : settingsButtons) {
        if (button.y + button.h <= M5.Display.height()) {
            drawSettingsButton(button);
        }
    }
    M5.Display.display();
}

void closeSettingsDrawer() {
    g_settingsOpen = false;
    termPrint("\x1B[?25h");
    termRedraw();
    drawSettingsHandle();
}

void handleTouchSettings() {
    auto touch = M5.Touch.getDetail();

    if (g_settingsOpen) {
        if (touch.wasReleased()) {
            Serial.printf("[Touch] Tap at (%d,%d)\n", touch.x, touch.y);
            handleSettingsTap(touch.x, touch.y);
        }
        return;
    }

    if (touch.wasPressed()) {
        int handleX = M5.Display.width() - SETTINGS_HANDLE_WIDTH - 4;
        bool onRightEdge = touch.x >= M5.Display.width() - SETTINGS_EDGE_WIDTH;
        bool onHandle = touch.x >= handleX && touch.y >= 2 && touch.y <= SETTINGS_HANDLE_HEIGHT + 2;
        g_touchTracking = onRightEdge || onHandle;
        g_touchStartX = touch.x;
        g_touchStartY = touch.y;
        if (g_touchTracking) Serial.printf("[Touch] Tracking start at (%d,%d) edge=%d handle=%d\n", touch.x, touch.y, onRightEdge, onHandle);
        return;
    }

    if (g_touchTracking && touch.wasReleased()) {
        int dx = g_touchStartX - touch.x;
        int dy = abs(touch.y - g_touchStartY);
        g_touchTracking = false;
        Serial.printf("[Touch] Swipe dx=%d dy=%d\n", dx, dy);
        if (dx >= SETTINGS_SWIPE_DISTANCE && dy < 100) {
            Serial.println("[Touch] Opening settings drawer");
            drawSettingsDrawer();
        }
    }
}

void handleSettingsTap(int x, int y) {
    if (pointInButton(settingsButtons[0], x, y)) {
        promptWiFiSettings();
        drawSettingsDrawer();
    } else if (pointInButton(settingsButtons[1], x, y)) {
        showSessionsPage();
    } else if (pointInButton(settingsButtons[2], x, y)) {
        showKeysPage();
    } else if (pointInButton(settingsButtons[3], x, y)) {
        clearStoredWiFi();
        clearStoredKey();
        g_sessions.clear();
        saveSessions();
        Serial.println("[Reset] All settings cleared");
        drawSettingsDrawer();
    } else if (pointInButton(settingsButtons[4], x, y)) {
        closeSettingsDrawer();
    }
}

void showSessionsPage() {
    g_settingsOpen = false;

    constexpr int FX = 100, FY = 60, FW = 500, FH = 52;
    constexpr int ITEM_H = 56, LBL_H = 40;
    int selected = -1;

    auto drawPage = [&]() {
        M5.Display.fillScreen(BLACK);
        M5.Display.setTextColor(0x07FF, BLACK);
        M5.Display.setTextSize(3);
        M5.Display.drawString("Sessions", FX, 10);

        if (g_sessions.empty()) {
            M5.Display.setTextColor(0xA555, BLACK);
            M5.Display.setTextSize(2);
            M5.Display.drawString("No sessions. Tap + to add.", FX, FY + 20);
        } else {
            for (int i = 0; i < (int)g_sessions.size(); i++) {
                int y = FY + i * ITEM_H;
                uint16_t bg = (i == selected) ? 0x04A0 : 0x18E3;
                uint16_t border = (i == selected) ? 0x07FF : 0x7BEF;
                M5.Display.fillRoundRect(FX, y, FW, ITEM_H - 6, 6, bg);
                M5.Display.drawRoundRect(FX, y, FW, ITEM_H - 6, 6, border);
                M5.Display.setTextColor(WHITE, bg);
                M5.Display.setTextSize(2);
                M5.Display.drawString(g_sessions[i].nickname, FX + 10, y + 8);
                M5.Display.setTextColor(0xA555, bg);
                M5.Display.setTextSize(1);
                M5.Display.drawString(g_sessions[i].host + ":" + g_sessions[i].port, FX + 10, y + 30);
            }
        }

        int btnY = M5.Display.height() - 60;
        M5.Display.fillRoundRect(FX, btnY, 100, 48, 6, 0x0560);
        M5.Display.setTextColor(WHITE, 0x0560);
        M5.Display.setTextSize(2);
        M5.Display.drawString("+", FX + 36, btnY + 10);
        M5.Display.fillRoundRect(FX + 120, btnY, 100, 48, 6, 0xA800);
        M5.Display.setTextColor(WHITE, 0xA800);
        M5.Display.drawString("Del", FX + 146, btnY + 10);
        M5.Display.fillRoundRect(FX + 240, btnY, 140, 48, 6, 0x04A0);
        M5.Display.setTextColor(WHITE, 0x04A0);
        M5.Display.drawString("Connect", FX + 256, btnY + 10);
        M5.Display.fillRoundRect(FX + 400, btnY, 100, 48, 6, 0x4208);
        M5.Display.setTextColor(WHITE, 0x4208);
        M5.Display.drawString("Edit", FX + 426, btnY + 10);
        M5.Display.fillRoundRect(FX + 520, btnY, 100, 48, 6, SETTINGS_BUTTON);
        M5.Display.setTextColor(WHITE, SETTINGS_BUTTON);
        M5.Display.drawString("Back", FX + 546, btnY + 10);
    };

    drawPage();
    bool done = false;
    while (!done) {
        M5.update();
        Units.update();

        KeyInput key;
        if (readKeyInput(key) && key.text.indexOf('\x1B') >= 0) { Serial.println("[ESC] Sessions"); break; }

        auto touch = M5.Touch.getDetail();
        if (touch.wasPressed() && touch.x >= M5.Display.width() - SETTINGS_EDGE_WIDTH) {
            g_touchTracking = true;
            g_touchStartX = touch.x;
            continue;
        }
        if (g_touchTracking && touch.wasReleased()) {
            g_touchTracking = false;
            if (g_touchStartX - touch.x >= SETTINGS_SWIPE_DISTANCE) break;
            continue;
        }
        if (!touch.wasReleased()) { vTaskDelay(10 / portTICK_PERIOD_MS); continue; }
        int tx = touch.x, ty = touch.y;

        for (int i = 0; i < (int)g_sessions.size(); i++) {
            int y = FY + i * ITEM_H;
            if (tx >= FX && tx <= FX + FW && ty >= y && ty <= y + ITEM_H - 6) {
                selected = i;
                drawPage();
                break;
            }
        }

        int btnY = M5.Display.height() - 60;
        if (ty >= btnY && ty <= btnY + 48) {
            if (tx >= FX && tx < FX + 100) {
                addSession();
                drawPage();
            } else if (tx >= FX + 120 && tx < FX + 220 && selected >= 0) {
                deleteSession(selected);
                selected = -1;
                drawPage();
            } else if (tx >= FX + 240 && tx < FX + 380 && selected >= 0) {
                g_selectedSessionIndex = selected;
                Serial.printf("[UI] Connecting session %d\n", selected);
                done = true;
            } else if (tx >= FX + 400 && tx < FX + 500 && selected >= 0) {
                editSession(selected);
                drawPage();
            } else if (tx >= FX + 520 && tx < FX + 620) {
                done = true;
            }
        }
    }
    termRedraw();
    drawSettingsHandle();
}

void showKeysPage() {
    g_settingsOpen = false;

    constexpr int FX = 200, FY = 200, FW = 700, FH = 52;
    constexpr int GAP = 100;
    bool hasKey = LittleFS.begin(true) && LittleFS.exists("/ssh_key");

    auto drawPage = [&]() {
        M5.Display.fillScreen(BLACK);
        M5.Display.setTextColor(0x07FF, BLACK);
        M5.Display.setTextSize(3);
        M5.Display.drawString("SSH Keys", FX, 140);

        M5.Display.setTextColor(hasKey ? 0x07E0 : 0xA555, BLACK);
        M5.Display.setTextSize(2);
        M5.Display.drawString(hasKey ? "Private key: loaded" : "Private key: none", FX, FY);

        int by = FY + GAP;
        M5.Display.fillRoundRect(FX, by, 240, 48, 6, 0x0560);
        M5.Display.setTextColor(WHITE, 0x0560);
        M5.Display.setTextSize(2);
        M5.Display.drawString("Import", FX + 70, by + 12);
        M5.Display.fillRoundRect(FX + 300, by, 240, 48, 6, 0xA800);
        M5.Display.setTextColor(WHITE, 0xA800);
        M5.Display.drawString("Clear", FX + 370, by + 12);
        M5.Display.fillRoundRect(FX + 600, by, 160, 48, 6, SETTINGS_BUTTON);
        M5.Display.setTextColor(WHITE, SETTINGS_BUTTON);
        M5.Display.drawString("Back", FX + 640, by + 12);
    };

    drawPage();
    bool done = false;
    while (!done) {
        M5.update();
        Units.update();

        KeyInput key;
        if (readKeyInput(key) && key.text.indexOf('\x1B') >= 0) { Serial.println("[ESC] Keys"); break; }

        auto touch = M5.Touch.getDetail();
        if (touch.wasPressed() && touch.x >= M5.Display.width() - SETTINGS_EDGE_WIDTH) {
            g_touchTracking = true;
            g_touchStartX = touch.x;
            continue;
        }
        if (g_touchTracking && touch.wasReleased()) {
            g_touchTracking = false;
            if (g_touchStartX - touch.x >= SETTINGS_SWIPE_DISTANCE) break;
            continue;
        }
        if (!touch.wasReleased()) { vTaskDelay(10 / portTICK_PERIOD_MS); continue; }
        int tx = touch.x, ty = touch.y;
        int by = FY + GAP;
        if (ty >= by && ty <= by + 48) {
            if (tx >= FX && tx < FX + 240) {
                promptKeyImport();
                hasKey = LittleFS.begin(true) && LittleFS.exists("/ssh_key");
                drawPage();
            } else if (tx >= FX + 300 && tx < FX + 540) {
                clearStoredKey();
                hasKey = false;
                drawPage();
            } else if (tx >= FX + 600 && tx < FX + 760) {
                done = true;
            }
        }
    }
    termRedraw();
    drawSettingsHandle();
}

void promptWiFiSettings() {
    g_settingsOpen = false;
    Serial.println("[WiFi] Opening settings page");
    termClear();
    termPrint("\x1B[?25l");

    constexpr int FX = 200, FY = 200, FW = 700, FH = 52;
    constexpr int LBL_H = 40;
    constexpr int GAP = LBL_H + FH + 16;

    auto drawField = [&](int idx, const char *label, const String &val, bool hide) {
        int y = FY + idx * GAP;
        M5.Display.setTextColor(0xA555, BLACK);
        M5.Display.setTextSize(2);
        M5.Display.drawString(label, FX, y);
        int iy = y + LBL_H;
        M5.Display.fillRoundRect(FX, iy, FW, FH, 6, 0x18E3);
        M5.Display.drawRoundRect(FX, iy, FW, FH, 6, 0x7BEF);
        M5.Display.setTextColor(WHITE, 0x18E3);
        M5.Display.setTextSize(2);
        String disp = val;
        if (hide && !val.isEmpty()) { disp = ""; for (unsigned i = 0; i < val.length(); i++) disp += '*'; }
        if (disp.isEmpty()) disp = " ";
        M5.Display.drawString(disp, FX + 10, iy + 14);
    };

    auto drawForm = [&]() {
        M5.Display.fillScreen(BLACK);
        M5.Display.setTextColor(0x07FF, BLACK);
        M5.Display.setTextSize(3);
        M5.Display.drawString("WiFi Settings", FX, 140);
        drawField(0, "SSID", wifi_ssid, false);
        drawField(1, "Password", wifi_pass, true);
        int by = FY + 2 * GAP + 20;
        M5.Display.fillRoundRect(FX, by, 240, 48, 6, 0x0560);
        M5.Display.setTextColor(WHITE, 0x0560);
        M5.Display.setTextSize(2);
        M5.Display.drawString("Save", FX + 90, by + 12);
        M5.Display.fillRoundRect(FX + 400, by, 240, 48, 6, 0xA800);
        M5.Display.setTextColor(WHITE, 0xA800);
        M5.Display.drawString("Cancel", FX + 460, by + 12);
        M5.Display.display();
    };

    auto promptField = [&](int idx, const char *label, String &target, bool hide) {
        String input = target;
        int cursor = input.length();
        flushKeyboard();
        int iy = FY + idx * GAP + LBL_H;
        M5.Display.fillRoundRect(FX, iy + FH + 4, FW, 36, 4, 0x03EF);
        M5.Display.setTextColor(WHITE, 0x03EF);
        M5.Display.setTextSize(2);
        M5.Display.drawString("Type, then press Enter", FX + 10, iy + FH + 10);

        while (true) {
            M5.update(); Units.update();
            KeyInput key;
            if (readKeyInput(key)) {
                if (key.backspace && cursor > 0) { input.remove(--cursor, 1); }
                for (int i = 0; i < (int)key.text.length(); i++) {
                    char ch = key.text[i];
                    if (ch == '\x1B' && i + 2 < (int)key.text.length() && key.text[i + 1] == '[') {
                        char cmd = key.text[i + 2];
                        if (cmd == 'D' && cursor > 0) { --cursor; i += 2; continue; }
                        if (cmd == 'C' && cursor < (int)input.length()) { ++cursor; i += 2; continue; }
                        if (cmd == '3' && i + 3 < (int)key.text.length() && key.text[i + 3] == '~') {
                            if (cursor < (int)input.length()) input.remove(cursor, 1);
                            i += 3; continue;
                        }
                    }
                    if (ch >= 0x20 && ch <= 0x7E) { input = input.substring(0, cursor) + String(ch) + input.substring(cursor); ++cursor; }
                }
                if (key.enter) { target = input; return; }
                M5.Display.fillRoundRect(FX, iy, FW, FH, 6, 0x18E3);
                M5.Display.drawRoundRect(FX, iy, FW, FH, 6, 0x07FF);
                M5.Display.setTextColor(WHITE, 0x18E3);
                M5.Display.setTextSize(2);
                String disp = input;
                if (hide && !input.isEmpty()) { disp = ""; for (unsigned j = 0; j < input.length(); j++) disp += '*'; }
                if (disp.isEmpty()) disp = " ";
                M5.Display.drawString(disp, FX + 10, iy + 14);
                M5.Display.display();
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }
    };

    drawForm();
    bool done = false;
    while (!done) {
        M5.update();
        Units.update();

        KeyInput key;
        if (readKeyInput(key) && key.text.indexOf('\x1B') >= 0) { Serial.println("[ESC] WiFi"); break; }

        auto touch = M5.Touch.getDetail();
        if (!touch.wasReleased()) { vTaskDelay(10 / portTICK_PERIOD_MS); continue; }
        int tx = touch.x, ty = touch.y;
        Serial.printf("[WiFi] Form tap at (%d,%d)\n", tx, ty);
        if (tx >= FX && tx <= FX + FW) {
            for (int i = 0; i < 2; i++) {
                int iy = FY + i * GAP + LBL_H;
                if (ty >= iy && ty <= iy + FH) {
                    const char *labels[] = {"SSID", "Password"};
                    String *vals[] = {&wifi_ssid, &wifi_pass};
                    promptField(i, labels[i], *vals[i], i == 1);
                    drawForm();
                    break;
                }
            }
            int by = FY + 2 * GAP + 20;
            if (ty >= by && ty <= by + 48) {
                if (tx < FX + 400 && !wifi_ssid.isEmpty()) {
                    saveWiFiSettings();
                    Serial.printf("[WiFi] Saved SSID: %s\n", wifi_ssid.c_str());
                    done = true;
                } else if (tx >= FX + 400) { done = true; }
            }
        }
    }
    termRedraw();
    drawSettingsHandle();
}

void promptSessionSettings() {
    g_settingsOpen = false;
    termClear();
    termPrintln("\x1B[36m== Session Settings ==\x1B[0m");
    g_ssh_host = "";
    g_ssh_port = "";
    g_ssh_user = "";
    g_ssh_password = "";

    termPrint("SSH Host:");
    waitForInput(g_ssh_host, false);
    promptSSHPort();
    termPrint("SSH User:");
    waitForInput(g_ssh_user, false);
    termPrint("SSH Password(empty to skip save):");
    waitForInput(g_ssh_password, true);
    saveSSHSettings();
    termPrintln("\x1B[32mSession saved.\x1B[0m");
    delay(600);
}

void promptKeyImport() {
    g_settingsOpen = false;
    termClear();
    termPrintln("\x1B[36m== Paste Private Key ==\x1B[0m");
    termPrintln("Paste PEM body/base64 as one line.");
    termPrintln("Leave empty to cancel.");
    termPrint("Key:");

    String keyBody;
    waitForInput(keyBody, true);
    if (keyBody.isEmpty()) {
        termPrintln("\x1B[33mCanceled.\x1B[0m");
        delay(600);
        return;
    }

    File f = LittleFS.open("/ssh_key", "w");
    if (!f) {
        termPrintln("\x1B[31mOpen /ssh_key failed.\x1B[0m");
        delay(800);
        return;
    }

    f.print(keyBody);
    f.close();
    termPrintln("\x1B[32mKey saved to /ssh_key.\x1B[0m");
    delay(600);
}

void clearStoredWiFi() {
    wifi_ssid.clear();
    wifi_pass.clear();
    if (prefs.begin("m5term", false)) {
        prefs.remove("wifi_ssid");
        prefs.remove("wifi_pass");
        prefs.end();
    }
}

void clearStoredKey() {
    LittleFS.remove("/ssh_key");
}

void writeSsh(const String &s) {
    for (int i = 0; i < s.length(); ++i) {
        char c = s[i];
        ssh_channel_write(g_channel, &c, 1);
    }
}

void handleSshKeyboard() {
    KeyInput key;
    if (!readKeyInput(key)) return;

    if (key.enter) {
        char c = '\r';
        ssh_channel_write(g_channel, &c, 1);
    }
    if (key.backspace) {
        char c = 0x7F;
        ssh_channel_write(g_channel, &c, 1);
    }
    if (key.text.length()) {
        writeSsh(key.text);
    }
}

void flushKeyboard() {
    uint32_t idleSince = millis();
    while (millis() - idleSince < 80) {
        M5.update();
        Units.update();
        KeyInput key;
        if (readKeyInput(key)) {
            idleSince = millis();
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void moveInputCursorLeft(int count) {
    while (count-- > 0) {
        termPrint("\x1B[D");
    }
}

void redrawInputTail(const String &input, int cursor, bool hideInput) {
    termPrint("\x1B[K");

    for (int i = cursor; i < input.length(); ++i) {
        termPrint(hideInput ? "*" : String(input[i]));
    }

    moveInputCursorLeft(input.length() - cursor);
}

void waitForInput(String &input, bool hideInput) {
    flushKeyboard();
    input = "";
    int cursor = 0;

    while (true) {
        M5.update();
        Units.update();

        KeyInput key;
        if (readKeyInput(key)) {
            if (key.backspace && cursor > 0) {
                --cursor;
                input.remove(cursor, 1);
                termPrint("\x1B[D");
                redrawInputTail(input, cursor, hideInput);
            }

            if (key.text.length()) {
                for (int i = 0; i < key.text.length(); ++i) {
                    char ch = key.text[i];
                    if (ch == '\x1B' && i + 2 < key.text.length() && key.text[i + 1] == '[') {
                        char cmd = key.text[i + 2];
                        if (cmd == 'D') {
                            if (cursor > 0) {
                                --cursor;
                                termPrint("\x1B[D");
                            }
                            i += 2;
                            continue;
                        }
                        if (cmd == 'C') {
                            if (cursor < input.length()) {
                                ++cursor;
                                termPrint("\x1B[C");
                            }
                            i += 2;
                            continue;
                        }
                        if (cmd == '3' && i + 3 < key.text.length() && key.text[i + 3] == '~') {
                            if (cursor < input.length()) {
                                input.remove(cursor, 1);
                                redrawInputTail(input, cursor, hideInput);
                            }
                            i += 3;
                            continue;
                        }
                    }

                    if (ch < 0x20 || ch > 0x7E) continue;
                    input = input.substring(0, cursor) + String(ch) + input.substring(cursor);
                    ++cursor;
                    termPrint(hideInput ? "*" : String(ch));
                    redrawInputTail(input, cursor, hideInput);
                }
            }

            if (key.enter) {
                termPrintln("");
                flushKeyboard();
                return;
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

bool parseSSHPort(int &port) {
    if (g_ssh_port.isEmpty()) return false;

    for (int i = 0; i < g_ssh_port.length(); i++) {
        char ch = g_ssh_port[i];
        if (ch < '0' || ch > '9') return false;
    }

    port = g_ssh_port.toInt();
    return port > 0 && port <= 65535;
}

void promptSSHPort() {
    while (true) {
        if (g_ssh_port.isEmpty()) {
            termPrint("SSH Port(22):");
            waitForInput(g_ssh_port, false);
            if (g_ssh_port.isEmpty()) {
                g_ssh_port = "22";
            }
        }

        int ssh_port = 0;
        if (parseSSHPort(ssh_port)) return;

        termPrintln("\x1B[31mInvalid SSH port. Enter again.\x1B[0m");
        g_ssh_port = "";
    }
}

void resetSSHConfig() {
    termClear();
    termPrintln("\x1B[36m== Reset SSH Config ==\x1B[0m");

    g_ssh_host = "";
    g_ssh_port = "";
    g_ssh_user = "";
    g_ssh_password = "";

    termPrint("SSH Host:");
    waitForInput(g_ssh_host, false);

    promptSSHPort();

    termPrint("SSH User:");
    waitForInput(g_ssh_user, false);

    termPrint("SSH Password:");
    waitForInput(g_ssh_password, true);
}

bool connectSSH() {
    Serial.printf("[SSH] connectSSH: host=%s port=%s user=%s\n", 
        g_ssh_host.c_str(), g_ssh_port.c_str(), g_ssh_user.c_str());
    
    if (g_ssh_host.isEmpty()) {
        Serial.println("[SSH] No host configured");
        termPrintln("\x1B[31mNo SSH host configured.\x1B[0m");
        return false;
    }

    int ssh_port = 0;
    if (!parseSSHPort(ssh_port)) {
        Serial.printf("[SSH] Invalid port: %s\n", g_ssh_port.c_str());
        termPrintln("\x1B[31mInvalid SSH port.\x1B[0m");
        return false;
    }

    Serial.printf("[SSH] Creating session to %s:%d as %s\n", g_ssh_host.c_str(), ssh_port, g_ssh_user.c_str());
    g_session = ssh_new();
    ssh_options_set(g_session, SSH_OPTIONS_HOST, g_ssh_host.c_str());
    ssh_options_set(g_session, SSH_OPTIONS_PORT, &ssh_port);
    ssh_options_set(g_session, SSH_OPTIONS_USER, g_ssh_user.c_str());

    termPrintln("\x1B[36mConnecting SSH...\x1B[0m");
    if (ssh_connect(g_session) != SSH_OK) {
        Serial.printf("[SSH] ssh_connect failed: %s\n", ssh_get_error(g_session));
        termPrintln("\x1B[31mSSH connect failed.\x1B[0m");
        ssh_free(g_session);
        return false;
    }

    bool keyOK = false;
    if (g_currentSessionIndex >= 0 && g_currentSessionIndex < (int)g_sessions.size() && 
        g_sessions[g_currentSessionIndex].useKey && LittleFS.begin(true) && LittleFS.exists("/ssh_key")) {
        termPrintln("\x1B[33mAuth Mode: Trying KEY authentication...\x1B[0m");

        File f = LittleFS.open("/ssh_key", "r");
        if (f) {
            size_t len = f.size();
            uint8_t *keyBuf = (uint8_t *)malloc(len + 1);
            if (keyBuf) {
                f.read(keyBuf, len);
                keyBuf[len] = 0;

                ssh_key privKey = nullptr;
                if (ssh_pki_import_privkey_base64((const char *)keyBuf, nullptr, nullptr, nullptr, &privKey) == SSH_OK) {
                    if (ssh_userauth_publickey(g_session, nullptr, privKey) == SSH_AUTH_SUCCESS) {
                        termPrintln("\x1B[32mKEY authentication success!\x1B[0m");
                        keyOK = true;
                    } else {
                        termPrintln("\x1B[31mKEY authentication failed.\x1B[0m");
                    }
                } else {
                    termPrintln("\x1B[31mFailed to import private key.\x1B[0m");
                }

                if (privKey) ssh_key_free(privKey);
                free(keyBuf);
            }
            f.close();
        }
    } else {
        termPrintln("\x1B[33mNo /ssh_key file, skipping key authentication.\x1B[0m");
    }

    if (!keyOK) {
        if (g_ssh_password.isEmpty()) {
            termPrint("SSH Password:");
            waitForInput(g_ssh_password, true);
        }

        termPrintln("\x1B[33mAuth Mode: Trying PASSWORD authentication...\x1B[0m");
        if (ssh_userauth_password(g_session, nullptr, g_ssh_password.c_str()) != SSH_AUTH_SUCCESS) {
            termPrintln("\x1B[31mPassword authentication failed.\x1B[0m");
            ssh_disconnect(g_session);
            ssh_free(g_session);
            return false;
        }

        termPrintln("\x1B[32mPassword authentication success!\x1B[0m");
    }

    g_channel = ssh_channel_new(g_session);
    if (!g_channel ||
        ssh_channel_open_session(g_channel) != SSH_OK ||
        ssh_channel_request_pty_size(g_channel, "xterm",
                                     M5.Display.width() / TERMINAL_FONT_WIDTH,
                                     M5.Display.height() / TERMINAL_LINE_HEIGHT) != SSH_OK ||
        ssh_channel_request_shell(g_channel) != SSH_OK)
    {
        Serial.println("[SSH] PTY/Shell request failed");
        termPrintln("\x1B[31mSSH PTY/Shell failed.\x1B[0m");

        ssh_disconnect(g_session);
        ssh_free(g_session);
        return false;
    }

    termPrintln("\x1B[32mSSH Connected!\x1B[0m");
    Serial.println("[SSH] Connection established successfully");
    return true;
}

bool connectWiFi() {
    if (wifi_ssid.isEmpty()) {
        Serial.println("[WiFi] No SSID configured");
        termPrintln("\x1B[33mWiFi not configured. Use settings to configure.\x1B[0m");
        return false;
    }

    Serial.printf("[WiFi] Connecting to SSID: %s\n", wifi_ssid.c_str());
    termPrintln("\x1B[36mConnecting WiFi...\x1B[0m");
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 30) {
        M5.update();
        handleTouchSettings();
        delay(500);
        retry++;
        if (retry % 5 == 0) Serial.printf("[WiFi] retry %d/30 status=%d\n", retry, (int)WiFi.status());
    }

    if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        Serial.printf("[WiFi] Connected! IP: %s\n", ip.toString().c_str());
        termPrint("IP: ");
        termPrintln(ip.toString());
        return true;
    }

    Serial.printf("[WiFi] Failed after %d retries, status=%d\n", retry, (int)WiFi.status());
    termPrintln("\x1B[31mWiFi connection failed.\x1B[0m");
    return false;
}

void sshTask(void *pv) {
    Serial.println("[sshTask] Started, waiting for session selection via settings");
    termPrintln("\x1B[33mOpen settings and select a session to connect.\x1B[0m");
    drawSettingsHandle();

    while (true) {
        if (g_selectedSessionIndex < 0 || g_selectedSessionIndex >= (int)g_sessions.size()) {
            vTaskDelay(200 / portTICK_PERIOD_MS);
            continue;
        }

        int idx = g_selectedSessionIndex;
        g_selectedSessionIndex = -1;
        g_currentSessionIndex = idx;

        Serial.printf("[sshTask] Connecting to session %d: %s\n", idx, g_sessions[idx].nickname.c_str());
        termPrintln("");
        connectToSession(idx);

        if (!connectSSH()) {
            Serial.println("[sshTask] SSH connection failed");
            termPrintln("\x1B[31mSSH connection failed.\x1B[0m");
            termPrintln("\x1B[33mSelect another session from settings.\x1B[0m");
            drawSettingsHandle();
            delay(1000);
            continue;
        }

        Serial.println("[sshTask] SSH connected, entering main loop");
        char buf[256];

        while (!ssh_channel_is_eof(g_channel)) {
            M5.update();
            Units.update();
            handleTouchSettings();

            if (!g_settingsOpen) {
                handleSshKeyboard();
            }

            if (!g_settingsOpen) {
                int n = ssh_channel_read_nonblocking(g_channel, buf, sizeof(buf), 0);
                if (n > 0) {
                    for (int i = 0; i < n; i++) {
                        handleSshChar(buf[i]);
                    }
                    termFlush();
                    drawSettingsHandle();
                }
            }

            vTaskDelay(5 / portTICK_PERIOD_MS);
        }

        Serial.println("[sshTask] SSH channel EOF");
        ssh_channel_close(g_channel);
        ssh_channel_free(g_channel);
        ssh_disconnect(g_session);
        ssh_free(g_session);

        termPrintln("\x1B[33mSSH disconnected. Select another session.\x1B[0m");
        drawSettingsHandle();
        delay(1000);
    }
}

void startSSHMode() {
    Serial.println("[SSH] startSSHMode");
    termPrintln("\x1B[36mStarting SSH mode...\x1B[0m");
    if (!connectWiFi()) {
        Serial.println("[SSH] WiFi not connected, waiting for config via settings");
        termPrintln("\x1B[33mWiFi not connected. Configure WiFi in settings.\x1B[0m");
        drawSettingsHandle();
        return;
    }

    Serial.println("[SSH] Creating sshTask");
    xTaskCreatePinnedToCore(
        sshTask, "sshTask", SSH_TASK_STACK_SIZE,
        nullptr, 1, nullptr, 1
    );
}
