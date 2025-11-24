#include <WiFi.h>
#include <M5Cardputer.h>
#include "libssh_esp32.h"
#include <libssh/libssh.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "handle_char.h"

//==================================================
//   SSH 配置
//==================================================
String g_ssh_host     = SSH_CONFIG_HOST;
String g_ssh_user     = SSH_CONFIG_USER;
String g_ssh_password = SSH_CONFIG_PASSWORD;

// SSH session / channel（handle_char.cpp 会使用）
ssh_session g_session = nullptr;
ssh_channel g_channel = nullptr;

const uint32_t SSH_TASK_STACK_SIZE = 51200;

void connectSSH() {

    while (true) {

        if (g_ssh_host.isEmpty()) {
            termPrint("SSH Host:");
            waitForInput(g_ssh_host, false);
        }

        if (g_ssh_user.isEmpty()) {
            termPrint("SSH User:");
            waitForInput(g_ssh_user, false);
        }

        if (g_ssh_password.isEmpty()) {
            termPrint("SSH Password:");
            waitForInput(g_ssh_password, true);
        }

        // 创建 session
        g_session = ssh_new();
        ssh_options_set(g_session, SSH_OPTIONS_HOST, g_ssh_host.c_str());
        ssh_options_set(g_session, SSH_OPTIONS_USER, g_ssh_user.c_str());

        // 尝试连接
        if (ssh_connect(g_session) != SSH_OK) {
            termPrintln("\x1B[31mSSH connect failed. Please re-enter host.\x1B[0m");
            ssh_free(g_session);
            g_ssh_host = "";
            continue;
        }

        // 认证
        if (ssh_userauth_password(g_session, nullptr, g_ssh_password.c_str()) != SSH_AUTH_SUCCESS) {
            termPrintln("\x1B[31mSSH authentication failed. Enter password again.\x1B[0m");
            ssh_disconnect(g_session);
            ssh_free(g_session);
            g_ssh_password = "";
            continue;
        }

        // SSH channel
        g_channel = ssh_channel_new(g_session);
        if (!g_channel ||
            ssh_channel_open_session(g_channel) != SSH_OK ||
            ssh_channel_request_pty(g_channel) != SSH_OK ||
            ssh_channel_request_shell(g_channel) != SSH_OK) {

            termPrintln("\x1B[31mSSH PTY/Shell failed. Re-enter info.\x1B[0m");
            ssh_disconnect(g_session);
            ssh_free(g_session);

            g_ssh_host = "";
            g_ssh_user = "";
            g_ssh_password = "";
            continue;
        }

        termPrintln("\x1B[32mSSH Connected!\x1B[0m");
        return;
    }
}

//==================================================
//   WiFi 配置
//==================================================
void connectWiFi() {

    while (true) {

        String wifi_ssid = WIFI_CONFIG_SSID;
        String wifi_pass = WIFI_CONFIG_PASSWORD;

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
            termPrint(ip.toString());
            termPrintln("\x1B[0m");
            return;
        }

        termPrintln("\x1B[31mWiFi Failed! Please try again.\x1B[0m");
    }
}



//==================================================
//   函数声明
//==================================================
void connectWiFi();
void connectSSH();

void sshTask(void *pv);

void waitForInput(String &input, bool hideInput);
void flushKeyboard();


void setup() {
    Serial.begin(115200);
    delay(100);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);

    termClear();
    termPrintln("\x1B[36m=== M5Cardputer SSH Shell ===\x1B[0m");

    connectWiFi();

    xTaskCreatePinnedToCore(
        sshTask, "sshTask", SSH_TASK_STACK_SIZE,
        nullptr, 1, nullptr, 1
    );
}

void loop() {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}

//==================================================
//                   SSH Task
//==================================================
void sshTask(void *pv) {

    termPrintln("");

    connectSSH();

    char buf[256];

    while (!ssh_channel_is_eof(g_channel)) {

        M5Cardputer.update();
        auto st = M5Cardputer.Keyboard.keysState();

        handleKbdChar(st);

        int n = ssh_channel_read_nonblocking(g_channel, buf, sizeof(buf), 0);
        if (n > 0) {
            for (int i = 0; i < n; i++)
                handleSshChar(buf[i]);
        }

        vTaskDelay(5 / portTICK_PERIOD_MS);
    }

    ssh_channel_close(g_channel);
    ssh_channel_free(g_channel);
    ssh_disconnect(g_session);
    ssh_free(g_session);

    vTaskDelete(nullptr);
}



void flushKeyboard() {
    while (true) {
        M5Cardputer.update();
        auto st = M5Cardputer.Keyboard.keysState();
        bool any = st.enter || st.del || !st.word.empty();
        if (!any) break;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void waitForInput(String &input, bool hideInput) {
    flushKeyboard();
    input = "";

    bool lastDel = false;
    unsigned long lastKeyMillis = 0;
    const unsigned long debounceDelay = 150;

    while (true) {
        M5Cardputer.update();
        auto st = M5Cardputer.Keyboard.keysState();

        if (st.del && !lastDel && input.length() > 0) {
            input.remove(input.length() - 1);
            termPrint("\b \b");   // 终端退格
        }
        lastDel = st.del;

        if (!st.word.empty()) {
            if (millis() - lastKeyMillis >= debounceDelay) {
                for (char ch : st.word) {
                    input += ch;
                    termPrint(hideInput ? "*" : String(ch));
                }
                lastKeyMillis = millis();
            }
        }

        if (st.enter) {
            termPrintln("");
            flushKeyboard();
            return;
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}
