/**
 * M5Cardputer SSH Shell
 *
 * - todo：Backspace / 左右方向键导致显示错乱
 * - todo: 本地缓存
 * - todo: ctrl+c
 */

#include <WiFi.h>
#include <M5Cardputer.h>

#include "libssh_esp32.h"
#include <libssh/libssh.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"

//================= 网络配置 =================//
const char* WIFI_SSID = WIFI_CONFIG_SSID;
const char* WIFI_PASS = WIFI_CONFIG_PASSWORD;

// 使用 secrets.h 的默认值初始化 String
String g_ssh_host     = SSH_CONFIG_HOST;
String g_ssh_user     = SSH_CONFIG_USER;
String g_ssh_password = SSH_CONFIG_PASSWORD;

//================= 任务与栈 =================//
const uint32_t SSH_TASK_STACK_SIZE = 51200;  // 50KB

//================= 终端显示相关 =================//
int cursorY          = 0;
const int lineHeight = 16;   // 行高
int fontW            = 6;    // 字宽估值
int fontH            = 16;   // 行高估值

// ANSI 状态机
bool ansiInEsc       = false;
bool ansiInCsi       = false;
bool ansiInOsc       = false;  // ESC ] ... BEL
String ansiBuf       = "";

//================= SSH 全局对象 =================//
ssh_session g_session = nullptr;
ssh_channel g_channel = nullptr;

//================= 函数声明 =================//
void sshTask(void* pv);
void waitForInput(String& input, bool hideInput = false);
void handleSshChar(char c);
void terminalPutChar(char c);
void terminalNewLine();
void ensureScroll();
void flushKeyboard();
int ssh_auth_with_password(ssh_session session, const String& password);

//---------------------------------------------
// 可选：键盘调试（串口打印按键情况）
//---------------------------------------------
void debugKeyboard() {
    M5Cardputer.update();
    auto st = M5Cardputer.Keyboard.keysState();

    if (st.word.empty() && !st.enter && !st.del && !st.fn)
        return;

    Serial.println("------ KEY EVENT ------");

    if (st.fn) {
        Serial.println("FN pressed = YES");
    }

    if (!st.word.empty()) {
        String key = String(st.word.data(), st.word.size());
        Serial.print("word = \"");
        Serial.print(key);
        Serial.println("\"");
    } else {
        Serial.println("word = (empty)");
    }

    if (st.enter)
        Serial.println("ENTER pressed");

    if (st.del)
        Serial.println("DEL pressed");

    Serial.println("------------------------");
}

//---------------------------------------------
// 清理键盘缓冲
//---------------------------------------------
void flushKeyboard() {
    while (true) {
        M5Cardputer.update();
        auto st = M5Cardputer.Keyboard.keysState();
        bool any = st.enter || st.del || !st.word.empty();
        if (!any) break;
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

//---------------------------------------------
// 等待用户输入（带退格）
//---------------------------------------------
void waitForInput(String& input, bool hideInput) {
    flushKeyboard();
    input = "";

    bool lastDel = false;
    unsigned long lastKeyMillis = 0;
    const unsigned long debounceDelay = 150;

    while (true) {
        M5Cardputer.update();
        auto st = M5Cardputer.Keyboard.keysState();

        // --- 退格（按一次删一个） ---
        if (st.del && !lastDel && input.length() > 0) {
            input.remove(input.length() - 1);

            int cx = M5Cardputer.Display.getCursorX();
            int cy = M5Cardputer.Display.getCursorY();

            cx = max(0, cx - fontW);
            M5Cardputer.Display.setCursor(cx, cy);
            M5Cardputer.Display.print(" ");
            M5Cardputer.Display.setCursor(cx, cy);

            cursorY = cy;
        }
        lastDel = st.del;

        // --- 普通字符 ---
        if (!st.word.empty()) {
            if (millis() - lastKeyMillis >= debounceDelay) {
                for (char ch : st.word) {
                    input += ch;
                    M5Cardputer.Display.print(hideInput ? '*' : ch);
                }
                cursorY = M5Cardputer.Display.getCursorY();
                lastKeyMillis = millis();
            }
        }

        // --- 回车 ---
        if (st.enter) {
            M5Cardputer.Display.println();
            cursorY = M5Cardputer.Display.getCursorY();
            flushKeyboard();
            break;
        }

        ensureScroll();
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

//---------------------------------------------
// 密码认证
//---------------------------------------------
int ssh_auth_with_password(ssh_session session, const String& password) {
    Serial.println("[SSH] Authenticating...");
    int rc = ssh_userauth_password(session, nullptr, password.c_str());
    return rc;
}

//---------------------------------------------
// setup
//---------------------------------------------
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("\n===== M5Cardputer SSH Shell Start =====");

    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setTextSize(1);
    M5Cardputer.Display.setTextColor(WHITE, BLACK);
    M5Cardputer.Display.fillScreen(BLACK);
    M5Cardputer.Display.setCursor(0, 0);

    M5Cardputer.Display.println("M5Cardputer SSH Shell");
    M5Cardputer.Display.println("Connecting WiFi...");
    cursorY = M5Cardputer.Display.getCursorY();

    WiFi.mode(WIFI_MODE_STA);

    String wifi_ssid = WIFI_SSID;
    String wifi_pass = WIFI_PASS;

    if (wifi_ssid.isEmpty()) {
        M5Cardputer.Display.print("WiFi SSID: ");
        waitForInput(wifi_ssid, false);
    }

    if (wifi_pass.isEmpty()) {
        M5Cardputer.Display.print("WiFi Password: ");
        waitForInput(wifi_pass, true);
    }

    WiFi.begin(wifi_ssid.c_str(), wifi_pass.c_str());

    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 40) {
        delay(500);
        Serial.print(".");
        retry++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        IPAddress ip = WiFi.localIP();
        Serial.print("[WiFi] Connected. IP = ");
        Serial.println(ip);

        M5Cardputer.Display.println("WiFi Connected.");
        M5Cardputer.Display.print("IP: ");
        M5Cardputer.Display.println(ip.toString());
    } else {
        Serial.println("[WiFi] Failed.");
        M5Cardputer.Display.println("WiFi Failed.");
        return;
    }

    cursorY = M5Cardputer.Display.getCursorY();

    xTaskCreatePinnedToCore(
        sshTask, "sshTask", SSH_TASK_STACK_SIZE,
        nullptr, 1, nullptr, 1
    );
}

void loop() {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
}

//---------------------------------------------
// SSH Task
//---------------------------------------------
void sshTask(void* pv) {
    Serial.println("[SSH] sshTask started.");
    libssh_begin();

    // 输入参数
    M5Cardputer.Display.println();

    if (g_ssh_host.isEmpty()) {
        M5Cardputer.Display.print("SSH Host: ");
        waitForInput(g_ssh_host, false);
    }
    if (g_ssh_user.isEmpty()) {
        M5Cardputer.Display.print("SSH User: ");
        waitForInput(g_ssh_user, false);
    }
    if (g_ssh_password.isEmpty()) {
        M5Cardputer.Display.print("SSH Password: ");
        waitForInput(g_ssh_password, true);
    }

//    if (g_ssh_host.isEmpty() || g_ssh_user.isEmpty()) {
//        M5Cardputer.Display.println("Host/User empty.");
//        vTaskDelete(nullptr);
//        return;
//    }

    // 创建 session
    g_session = ssh_new();
    ssh_options_set(g_session, SSH_OPTIONS_HOST, g_ssh_host.c_str());
    ssh_options_set(g_session, SSH_OPTIONS_USER, g_ssh_user.c_str());

    if (ssh_connect(g_session) != SSH_OK) {
        M5Cardputer.Display.println("ssh_connect() error.");
        vTaskDelete(nullptr);
        return;
    }

    if (ssh_auth_with_password(g_session, g_ssh_password) != SSH_AUTH_SUCCESS) {
        M5Cardputer.Display.println("Auth Failed.");
        vTaskDelete(nullptr);
        return;
    }

    // channel
    g_channel = ssh_channel_new(g_session);
    ssh_channel_open_session(g_channel);
    ssh_channel_request_pty(g_channel);
    ssh_channel_request_shell(g_channel);

    M5Cardputer.Display.println("SSH connected.");
    cursorY = M5Cardputer.Display.getCursorY();

    char buf[256];

    while (!ssh_channel_is_eof(g_channel)) {
        M5Cardputer.update();
        auto st = M5Cardputer.Keyboard.keysState();

        // 调试按键
        debugKeyboard();

        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            // 把 vector<char> 转成 String
            String key = String(st.word.data(), st.word.size());

            // =============================
            //   Fn 模式方向键（发送 ANSI）
            // =============================
            if (st.fn) {

                struct FnKeyMap {
                    const char* key;
                    const char* seq;
                };

                static const FnKeyMap map[] = {
                    {";", "\x1B[A"},  // Up
                    {",", "\x1B[D"},  // Left
                    {".", "\x1B[B"},  // Down
                    {"/", "\x1B[C"},  // Right
                };

                for (auto& m : map) {
                    if (key == m.key) {
                        ssh_channel_write(g_channel, m.seq, strlen(m.seq));
                        goto after_key;
                    }
                }
            }

            // --- 普通字符，逐字发送 ---
            if (!st.word.empty()) {
                for (char ch : st.word) {
                    ssh_channel_write(g_channel, &ch, 1);
                }
            }

            // --- Enter ---
            if (st.enter) {
                char cr = '\r';
                ssh_channel_write(g_channel, &cr, 1);
            }

            // --- Backspace ---
            if (st.del) {
                // 这里用 DEL (0x7F)，更贴近多数终端 erase 设定
                char delChar = 0x7F;
                ssh_channel_write(g_channel, &delChar, 1);
            }

        after_key:;
        }

        // SSH → 屏幕
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
    ssh_finalize();

    vTaskDelete(nullptr);
}

//---------------------------------------------
// ANSI/OSC 解析 & 终端行为
//---------------------------------------------
uint16_t currentFG = WHITE;
uint16_t currentBG = BLACK;

void applyTextColor() {
    M5Cardputer.Display.setTextColor(currentFG, currentBG);
}

void handleSshChar(char c) {

    // -------------------------
    //  Backspace 0x08
    // -------------------------
    if (c == 0x08) {
        int cx = M5Cardputer.Display.getCursorX();
        int cy = M5Cardputer.Display.getCursorY();
        if (cx >= fontW) {
            M5Cardputer.Display.setCursor(cx - fontW, cy);
            cursorY = cy;
        }
        return;
    }

    // -------------------------
    //  OSC: ESC ] ... BEL
    // -------------------------
    if (ansiInOsc) {
        if (c == 0x07) {  // BEL -> 结束
            ansiInOsc = false;
        }
        return;
    }

    // -------------------------
    //  普通字符 OR ESC 开始
    // -------------------------
    if (!ansiInEsc) {
        if (c == 0x1B) {
            ansiInEsc = true;
            ansiInCsi = false;
            ansiBuf = "";
            return;
        }
        terminalPutChar(c);
        return;
    }

    // -------------------------
    //  ESC + ?
    // -------------------------
    if (!ansiInCsi) {

        if (c == '[') {
            ansiInCsi = true;
            ansiBuf = "";
            return;
        }
        else if (c == ']') {
            ansiInOsc = true;
            ansiInEsc = false;
            return;
        }
        else {
            ansiInEsc = false;
            return;
        }
    }

    // -------------------------
    //  CSI 模式 ESC[
    // -------------------------
    ansiBuf += c;

    if (c >= 0x40 && c <= 0x7E) {

        char cmd = c;
        String params = ansiBuf.substring(0, ansiBuf.length() - 1);

        auto parseN = [&](int def = 1) {
            if (!params.length()) return def;
            int v = params.toInt();
            return v <= 0 ? def : v;
        };

        // =========================================================
        //  CSI COMMANDS
        // =========================================================

        // --- 光标定位 ESC[row;colH ---
        if (cmd == 'H' || cmd == 'f') {
            int row = 1, col = 1;

            int sp = params.indexOf(';');
            if (sp >= 0) {
                row = params.substring(0, sp).toInt();
                col = params.substring(sp + 1).toInt();
            } else if (params.length()) {
                row = params.toInt();
            }

            row = max(1, row);
            col = max(1, col);

            int x = (col - 1) * fontW;
            int y = (row - 1) * fontH;

            x = constrain(x, 0, M5Cardputer.Display.width() - fontW);
            y = constrain(y, 0, M5Cardputer.Display.height() - lineHeight);

            M5Cardputer.Display.setCursor(x, y);
            cursorY = y;
        }

        // --- 清屏 ESC[2J ---
        else if (cmd == 'J') {
            if (params == "2" || params == "") {
                M5Cardputer.Display.fillScreen(BLACK);
                M5Cardputer.Display.setCursor(0, 0);
                cursorY = 0;
            }
        }

        // --- 清除行 ESC[K ---
        else if (cmd == 'K') {
            int mode = params.length() ? params.toInt() : 0;
            int cx = M5Cardputer.Display.getCursorX();
            int cy = M5Cardputer.Display.getCursorY();

            if (mode == 0) {
                int w = M5Cardputer.Display.width() - cx;
                M5Cardputer.Display.fillRect(cx, cy, w, lineHeight, BLACK);
            } 
            else if (mode == 1) {
                M5Cardputer.Display.fillRect(0, cy, cx, lineHeight, BLACK);
            } 
            else if (mode == 2) {
                M5Cardputer.Display.fillRect(0, cy, M5Cardputer.Display.width(), lineHeight, BLACK);
            }
            M5Cardputer.Display.setCursor(cx, cy);
            cursorY = cy;
        }

        // --- 光标移动 ---
        else if (cmd == 'A') {   // up
            int n = parseN();
            int cx = M5Cardputer.Display.getCursorX();
            int cy = M5Cardputer.Display.getCursorY();
            cy = max(0, cy - n * lineHeight);
            M5Cardputer.Display.setCursor(cx, cy);
            cursorY = cy;
        }
        else if (cmd == 'B') {   // down
            int n = parseN();
            int cx = M5Cardputer.Display.getCursorX();
            int cy = M5Cardputer.Display.getCursorY();
            cy = cy + n * lineHeight;
            int limit = M5Cardputer.Display.height() - lineHeight;
            if (cy > limit) cy = limit;
            M5Cardputer.Display.setCursor(cx, cy);
            cursorY = cy;
        }
        else if (cmd == 'C') {   // right
            int n = parseN();
            int cx = M5Cardputer.Display.getCursorX();
            cx += n * fontW;
            int limit = M5Cardputer.Display.width() - fontW;
            if (cx > limit) cx = limit;
            M5Cardputer.Display.setCursor(cx, M5Cardputer.Display.getCursorY());
        }
        else if (cmd == 'D') {   // left
            int n = parseN();
            int cx = M5Cardputer.Display.getCursorX();
            cx -= n * fontW;
            if (cx < 0) cx = 0;
            M5Cardputer.Display.setCursor(cx, M5Cardputer.Display.getCursorY());
        }

        // ---------------------------------------------------------
        //  SGR - 颜色控制 ESC[..m
        // ---------------------------------------------------------
        else if (cmd == 'm') {

            // 拆参
            std::vector<int> ps;
            if (params.length()) {
                int start = 0;
                while (true) {
                    int p = params.indexOf(';', start);
                    if (p < 0) {
                        ps.push_back(params.substring(start).toInt());
                        break;
                    }
                    ps.push_back(params.substring(start, p).toInt());
                    start = p + 1;
                }
            } else {
                ps.push_back(0);
            }

            for (int v : ps) {

                // reset
                if (v == 0) {
                    currentFG = WHITE;
                    currentBG = BLACK;
                    applyTextColor();
                }

                // 前景色 30–37
                else if (30 <= v && v <= 37) {
                    switch (v) {
                        case 30: currentFG = BLACK; break;
                        case 31: currentFG = RED; break;
                        case 32: currentFG = GREEN; break;
                        case 33: currentFG = YELLOW; break;
                        case 34: currentFG = BLUE; break;
                        case 35: currentFG = MAGENTA; break;
                        case 36: currentFG = CYAN; break;
                        case 37: currentFG = WHITE; break;
                    }
                    applyTextColor();
                }

                // 背景色 40–47
                else if (40 <= v && v <= 47) {
                    switch (v) {
                        case 40: currentBG = BLACK; break;
                        case 41: currentBG = RED; break;
                        case 42: currentBG = GREEN; break;
                        case 43: currentBG = YELLOW; break;
                        case 44: currentBG = BLUE; break;
                        case 45: currentBG = MAGENTA; break;
                        case 46: currentBG = CYAN; break;
                        case 47: currentBG = WHITE; break;
                    }
                    applyTextColor();
                }
            }
        }

        // ========================================================
        //  退出 CSI 状态
        // ========================================================
        ansiInEsc = false;
        ansiInCsi = false;
        ansiBuf = "";
    }
}



//---------------------------------------------
// 输出字符
//---------------------------------------------
void terminalPutChar(char c) {
    if (c == '\r') return;

    if (c == '\n') {
        terminalNewLine();
        return;
    }

    M5Cardputer.Display.write(c);
    cursorY = M5Cardputer.Display.getCursorY();
    ensureScroll();
}

//---------------------------------------------
void terminalNewLine() {
    M5Cardputer.Display.println();
    cursorY = M5Cardputer.Display.getCursorY();
    ensureScroll();
}

// 滚屏
void ensureScroll() {
    if (cursorY > M5Cardputer.Display.height() - lineHeight) {
        M5Cardputer.Display.scroll(0, -lineHeight);
        cursorY -= lineHeight;
        M5Cardputer.Display.setCursor(
            M5Cardputer.Display.getCursorX(), cursorY
        );
    }
}

