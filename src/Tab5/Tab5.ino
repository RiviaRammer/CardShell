#include <WiFi.h>
#include <LittleFS.h>
#include <Wire.h>

#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedKEYBOARD.h>
#include <M5HAL.hpp>
#include <M5Utility.h>

#include "libssh_esp32.h"
#include <libssh/libssh.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if __has_include("config.h")
#include "config.h"
#elif __has_include("../Cardputer/config.h")
#include "../Cardputer/config.h"
#else
#include "config_example.h"
#endif

#include "handle_char.h"

using namespace m5::unit;
using namespace m5::unit::tab5_keyboard;

namespace {

constexpr int8_t TAB5_KEYBOARD_SDA = 0;
constexpr int8_t TAB5_KEYBOARD_SCL = 1;
constexpr uint32_t TAB5_KEYBOARD_I2C_CLOCK = 400000UL;
constexpr uint8_t DISPLAY_BRIGHTNESS = 180;
constexpr uint8_t TERMINAL_FONT_WIDTH = 12;
constexpr uint8_t TERMINAL_LINE_HEIGHT = 16;

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

String g_ssh_host     = SSH_CONFIG_HOST;
String g_ssh_port     = String(SSH_CONFIG_PORT);
String g_ssh_user     = SSH_CONFIG_USER;
String g_ssh_password = SSH_CONFIG_PASSWORD;

String wifi_ssid = WIFI_CONFIG_SSID;
String wifi_pass = WIFI_CONFIG_PASSWORD;

ssh_session g_session = nullptr;
ssh_channel g_channel = nullptr;

const uint32_t SSH_TASK_STACK_SIZE = 51200;

void startSSHMode();
void connectWiFi();
void connectSSH();
void resetSSHConfig();
void sshTask(void *pv);
void waitForInput(String &input, bool hideInput);
void flushKeyboard();
void redrawInputTail(const String &input, int cursor, bool hideInput);
void moveInputCursorLeft(int count);
bool setupTab5Keyboard();
bool setTab5KeyboardMode(Mode mode);
bool checkBootKeyboardReset(bool &resetWifi);
bool readKeyInput(KeyInput &out);
void handleSshKeyboard();

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
    Serial.printf("[BOOT] board=%d display=%dx%d psram=%u free=%u\n",
                  (int)M5.getBoard(), M5.Display.width(), M5.Display.height(),
                  (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
    Serial.flush();

    setupDisplay();

    if (!setupTab5Keyboard()) {
        termPrintln("\x1B[31mTab5 Keyboard init failed.\x1B[0m");
        termPrintln("Check Tab5 Keyboard connection: SDA G0, SCL G1, INT G50.");
        while (true) delay(1000);
    }

    termPrintln("\x1B[36m== CardShell Tab5 ==\x1B[0m");
    termPrintln("Hold Ctrl while reset to configure SSH.");
    termPrintln("Press 4 to clear WiFi, or wait to start.");

    bool resetWifi = false;
    bool resetSsh = checkBootKeyboardReset(resetWifi);
    if (!setTab5KeyboardMode(Mode::HID)) {
        termPrintln("\x1B[31mFailed to switch keyboard to HID mode.\x1B[0m");
        while (true) delay(1000);
    }

    if (resetSsh) {
        resetSSHConfig();
    }
    if (resetWifi) {
        wifi_ssid.clear();
        wifi_pass.clear();
        termPrintln("\x1B[33mWiFi config cleared.\x1B[0m");
    }

    startSSHMode();
}

void loop() {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
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

bool checkBootKeyboardReset(bool &resetWifi) {
    resetWifi = false;
    uint32_t deadline = millis() + 3000;

    while (millis() < deadline) {
        M5.update();
        Units.update();
        tab5Keyboard.update(true);

        if (tab5Keyboard.isCtrl()) {
            return true;
        }

        const auto &pressed = tab5Keyboard.pressedBits();
        for (uint8_t i = 0; i < KEY_COUNT; ++i) {
            if (pressed.test(i) && tab5Keyboard.keyMatrixToChar(i) == '4') {
                resetWifi = true;
                return false;
            }
        }

        delay(10);
    }

    return false;
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

void connectSSH() {
    while (true) {
        if (g_ssh_host.isEmpty()) {
            termPrint("SSH Host:");
            waitForInput(g_ssh_host, false);
        }

        promptSSHPort();

        if (g_ssh_user.isEmpty()) {
            termPrint("SSH User:");
            waitForInput(g_ssh_user, false);
        }

        int ssh_port = 0;
        parseSSHPort(ssh_port);

        g_session = ssh_new();
        ssh_options_set(g_session, SSH_OPTIONS_HOST, g_ssh_host.c_str());
        ssh_options_set(g_session, SSH_OPTIONS_PORT, &ssh_port);
        ssh_options_set(g_session, SSH_OPTIONS_USER, g_ssh_user.c_str());

        termPrintln("\x1B[36mConnecting SSH...\x1B[0m");
        if (ssh_connect(g_session) != SSH_OK) {
            termPrintln("\x1B[31mSSH connect failed. Re-enter host.\x1B[0m");
            ssh_free(g_session);
            g_ssh_host = "";
            g_ssh_port = "";
            continue;
        }

        bool keyOK = false;
        if (LittleFS.begin(true) && LittleFS.exists("/ssh_key")) {
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
                termPrintln("\x1B[31mPassword authentication failed. Enter password again.\x1B[0m");
                ssh_disconnect(g_session);
                ssh_free(g_session);
                g_ssh_password = "";
                continue;
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
            termPrintln("\x1B[31mSSH PTY/Shell failed. Re-enter info.\x1B[0m");

            ssh_disconnect(g_session);
            ssh_free(g_session);

            g_ssh_host = "";
            g_ssh_port = "";
            g_ssh_user = "";
            g_ssh_password = "";
            continue;
        }

        termPrintln("\x1B[32mSSH Connected!\x1B[0m");
        return;
    }
}

void connectWiFi() {
    while (true) {
        if (wifi_ssid.isEmpty()) {
            termPrint("Enter WiFi SSID:");
            waitForInput(wifi_ssid, false);
        }

        if (wifi_pass.isEmpty()) {
            termPrint("Enter WiFi Password:");
            waitForInput(wifi_pass, true);
        }

        termPrintln("\x1B[36mConnecting WiFi...\x1B[0m");
        WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

        int retry = 0;
        while (WiFi.status() != WL_CONNECTED && retry < 30) {
            delay(500);
            retry++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            IPAddress ip = WiFi.localIP();
            termPrint("IP: ");
            termPrintln(ip.toString());
            return;
        }

        termPrintln("\x1B[31mWiFi Failed! Please try again.\x1B[0m");
        wifi_ssid = "";
        wifi_pass = "";
    }
}

void sshTask(void *pv) {
    termPrintln("");
    connectSSH();

    char buf[256];

    while (!ssh_channel_is_eof(g_channel)) {
        M5.update();
        Units.update();

        handleSshKeyboard();

        int n = ssh_channel_read_nonblocking(g_channel, buf, sizeof(buf), 0);
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                handleSshChar(buf[i]);
            }
            termFlush();
        }

        vTaskDelay(5 / portTICK_PERIOD_MS);
    }

    ssh_channel_close(g_channel);
    ssh_channel_free(g_channel);
    ssh_disconnect(g_session);
    ssh_free(g_session);

    vTaskDelete(nullptr);
}

void startSSHMode() {
    termPrintln("\x1B[36mStarting SSH mode...\x1B[0m");
    connectWiFi();

    xTaskCreatePinnedToCore(
        sshTask, "sshTask", SSH_TASK_STACK_SIZE,
        nullptr, 1, nullptr, 1
    );
}
