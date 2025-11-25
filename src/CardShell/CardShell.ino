#include <WiFi.h>
#include <LittleFS.h>

#include <M5Cardputer.h>
#include "libssh_esp32.h"
#include <libssh/libssh.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "handle_char.h"
#include "storage.h"

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

//==================================================
//   WiFi 配置
//==================================================

String wifi_ssid = WIFI_CONFIG_SSID;
String wifi_pass = WIFI_CONFIG_PASSWORD;



//==================================================
//   函数声明
//==================================================
int bootMenu();
void startSSHMode();
void startUSBMode();

void waitForInput(String &input, bool hideInput);
void flushKeyboard();

void connectWiFi();
void connectSSH();

void sshTask(void *pv);


//==================================================
//   MAIN
//==================================================

void setup() {

    Serial.begin(115200);
    delay(100);

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.fillScreen(BLACK);

    // ================================
    //   按键启动菜单
    // ================================
    M5.update();
    bool pressed = M5.BtnA.isPressed();

    if (pressed) {
        int mode = bootMenu();
        if (mode == 1) startUSBMode();
        else if (mode == 3) {
            g_ssh_host = "";
            g_ssh_user = "";
            g_ssh_password = "";
        }
        else if (mode == 4) {
            wifi_ssid.clear();
            wifi_pass.clear();
        }
    }

    startSSHMode();
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

        // --------------------------
        //  Create Session
        // --------------------------
        g_session = ssh_new();
        ssh_options_set(g_session, SSH_OPTIONS_HOST, g_ssh_host.c_str());
        ssh_options_set(g_session, SSH_OPTIONS_USER, g_ssh_user.c_str());

        // Connect
        termPrintln("\x1B[36mConnecting SSH...\x1B[0m");
        if (ssh_connect(g_session) != SSH_OK) {
            termPrintln("\x1B[31mSSH connect failed. Re-enter host.\x1B[0m");
            ssh_free(g_session);
            g_ssh_host = "";
            continue;
        }

        // ====================================================
        //   Try KEY AUTH First
        // ====================================================
        bool keyOK = false;
        if (LittleFS.begin(true) && LittleFS.exists("/ssh_key")) {

            termPrintln("\x1B[33mAuth Mode: Trying KEY authentication...\x1B[0m");

            File f = LittleFS.open("/ssh_key", "r");
            if (f) {
                size_t len = f.size();
                uint8_t *keyBuf = (uint8_t *)malloc(len + 1);
                f.read(keyBuf, len);
                keyBuf[len] = 0;
                f.close();

                ssh_key privKey = nullptr;

                if (ssh_pki_import_privkey_base64(
                        (const char *)keyBuf,
                        nullptr,   // no passphrase
                        nullptr, nullptr,
                        &privKey
                    ) == SSH_OK)
                {
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
        } else {
            termPrintln("\x1B[33mNo /ssh_key file, skipping key authentication.\x1B[0m");
        }

        // ====================================================
        //   If KEY auth OK → goto channel
        //   Else try PASSWORD auth
        // ====================================================
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

        // ====================================================
        //   Open SSH channel
        // ====================================================
        g_channel = ssh_channel_new(g_session);
        if (!g_channel ||
            ssh_channel_open_session(g_channel) != SSH_OK ||
            ssh_channel_request_pty(g_channel) != SSH_OK ||
            ssh_channel_request_shell(g_channel) != SSH_OK)
        {
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

        termPrintln("\x1B[36mConnecting WiFi...");
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


int bootMenu()
{
    M5Cardputer.Display.clear(BLACK);
    M5Cardputer.Display.setCursor(0, 0);

    M5Cardputer.Display.println("== CardShell Boot Menu ==");
    M5Cardputer.Display.println("1) USB Storage Mode");
    M5Cardputer.Display.println("2) SSH Shell (config)");
    M5Cardputer.Display.println("3) SSH Shell (manual)");
    M5Cardputer.Display.println("4) Reset Wi-Fi");
    M5Cardputer.Display.println();
    M5Cardputer.Display.println("Press 1, 2 or 3...");
    M5Cardputer.Display.println();

    while (true) {
        M5Cardputer.update();
        auto st = M5Cardputer.Keyboard.keysState();

        if (!st.word.empty()) {
            char c = st.word.front();
            if (c == '1') return 1;
            if (c == '2') return 2;
            if (c == '3') return 3;
            if (c == '4') return 4;
        }
        delay(20);
    }
}

void startUSBMode()
{
    storage_mountMCU();
    storage_mountPC();

    M5Cardputer.Display.clear(BLACK);
    M5Cardputer.Display.setCursor(0, 0);   // ← 光标回到左上角
    M5Cardputer.Display.println("USB Storage Mode");
    M5Cardputer.Display.println("");
    M5Cardputer.Display.println("Copy SSH key here from PC.");
    M5Cardputer.Display.println("Then eject the drive.");
    M5Cardputer.Display.println("Key will be imported to /ssh_key.");

    while (true) {
        storage_task();   // 这里会在弹出后导入 key
        delay(50);
    }
}


void startSSHMode()
{
    termPrintln("\x1B[36m== CardShell ==\x1B[0m");
    connectWiFi();

    xTaskCreatePinnedToCore(
        sshTask, "sshTask", SSH_TASK_STACK_SIZE,
        nullptr, 1, nullptr, 1
    );

}