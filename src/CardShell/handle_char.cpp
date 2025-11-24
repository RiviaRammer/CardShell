#include "handle_char.h"
#include <Arduino.h>

extern ssh_channel g_channel;

// =====================================================
// 内部状态
// =====================================================
namespace {

bool ansiInEsc = false;
bool ansiInCsi = false;
bool ansiInOsc = false;
String ansiBuf = "";

uint16_t currentFG = WHITE;
uint16_t currentBG = BLACK;

int cursorY = 0;
const int fontW      = 6;
const int fontH      = 16;
const int lineHeight = 16;

void applyTextColor() {
    M5Cardputer.Display.setTextColor(currentFG, currentBG);
}

void ensureScroll() {
    if (cursorY > M5Cardputer.Display.height() - lineHeight) {
        M5Cardputer.Display.scroll(0, -lineHeight);
        cursorY -= lineHeight;
        M5Cardputer.Display.setCursor(
            M5Cardputer.Display.getCursorX(), cursorY
        );
    }
}

void terminalNewLine() {
    M5Cardputer.Display.println();
    cursorY = M5Cardputer.Display.getCursorY();
    ensureScroll();
}

void terminalPutChar(char c) {
    if (c == '\r') return;
    if (c == '\n') { terminalNewLine(); return; }

    M5Cardputer.Display.write(c);
    cursorY = M5Cardputer.Display.getCursorY();
    ensureScroll();
}

} // end namespace


// =====================================================
//   SSH 输出处理
// =====================================================
void handleSshChar(char c)
{
    // BACKSPACE (0x08)
    if (c == 0x08) {
        int cx = M5Cardputer.Display.getCursorX();
        int cy = M5Cardputer.Display.getCursorY();
        if (cx >= fontW) {
            M5Cardputer.Display.setCursor(cx - fontW, cy);
            cursorY = cy;
        }
        return;
    }

    // OSC: ESC ] ... BEL
    if (ansiInOsc) {
        if (c == 0x07) ansiInOsc = false;
        return;
    }

    // 普通字符 OR ESC
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

    // ESC 模式
    if (!ansiInCsi) {
        if (c == '[') {
            ansiInCsi = true;
            ansiBuf = "";
            return;
        } else if (c == ']') {
            ansiInOsc = true;
            ansiInEsc = false;
            return;
        } else {
            ansiInEsc = false;
            return;
        }
    }

    // CSI 模式
    ansiBuf += c;

    if (c >= 0x40 && c <= 0x7E) {

        char cmd = c;
        String params = ansiBuf.substring(0, ansiBuf.length() - 1);

        auto parseN = [&](int def = 1) {
            if (!params.length()) return def;
            int v = params.toInt();
            return v <= 0 ? def : v;
        };

        // 光标定位
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

        // 清屏
        else if (cmd == 'J') {
            if (params == "2" || params == "") {
                M5Cardputer.Display.fillScreen(BLACK);
                M5Cardputer.Display.setCursor(0, 0);
                cursorY = 0;
            }
        }

        // 清除行
        else if (cmd == 'K') {
            int mode = params.length() ? params.toInt() : 0;
            int cx = M5Cardputer.Display.getCursorX();
            int cy = M5Cardputer.Display.getCursorY();

            if (mode == 0)
                M5Cardputer.Display.fillRect(cx, cy, M5Cardputer.Display.width() - cx, lineHeight, BLACK);
            else if (mode == 1)
                M5Cardputer.Display.fillRect(0, cy, cx, lineHeight, BLACK);
            else if (mode == 2)
                M5Cardputer.Display.fillRect(0, cy, M5Cardputer.Display.width(), lineHeight, BLACK);

            M5Cardputer.Display.setCursor(cx, cy);
            cursorY = cy;
        }

        // 光标移动
        else if (cmd == 'A') {
            int n = parseN();
            int cx = M5Cardputer.Display.getCursorX();
            int cy = max(0, (int)(M5Cardputer.Display.getCursorY() - n * lineHeight));
            M5Cardputer.Display.setCursor(cx, cy);
            cursorY = cy;
        }
        else if (cmd == 'B') {
            int n = parseN();
            int cx = M5Cardputer.Display.getCursorX();
            int cy = min((int)(M5Cardputer.Display.height() - lineHeight), (int)(M5Cardputer.Display.getCursorY() + n * lineHeight));
            M5Cardputer.Display.setCursor(cx, cy);
            cursorY = cy;
        }
        else if (cmd == 'C') {
            int n = parseN();
            int cx = min((int)(M5Cardputer.Display.width() - fontW), (int)(M5Cardputer.Display.getCursorX() + n * fontW));
            M5Cardputer.Display.setCursor(cx, M5Cardputer.Display.getCursorY());
        }
        else if (cmd == 'D') {
            int n = parseN();
            int cx = max(0, (int)(M5Cardputer.Display.getCursorX() - n * fontW));
            M5Cardputer.Display.setCursor(cx, M5Cardputer.Display.getCursorY());
        }

        // 字体/颜色控制 ESC[m
        else if (cmd == 'm') {
            // 解析多参数
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
                if (v == 0) {
                    currentFG = WHITE;
                    currentBG = BLACK;
                    applyTextColor();
                }
                else if (30 <= v && v <= 37) {
                    static uint16_t fgMap[8] = {BLACK,RED,GREEN,YELLOW,BLUE,MAGENTA,CYAN,WHITE};
                    currentFG = fgMap[v - 30];
                    applyTextColor();
                }
                else if (40 <= v && v <= 47) {
                    static uint16_t bgMap[8] = {BLACK,RED,GREEN,YELLOW,BLUE,MAGENTA,CYAN,WHITE};
                    currentBG = bgMap[v - 40];
                    applyTextColor();
                }
            }
        }

        // reset CSI
        ansiInEsc = ansiInCsi = false;
        ansiBuf = "";
    }
}


// =====================================================
//  键盘输入处理
// =====================================================
void handleKbdChar(const Keyboard_Class::KeysState &st)
{
    if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed())
        return;

    // ───────────────────────────────
    //   普通字符输入
    // ───────────────────────────────
    if (!st.word.empty()) {
        for (char ch : st.word)
            ssh_channel_write(g_channel, &ch, 1);
    }

    // ───────────────────────────────
    //   特殊键
    // ───────────────────────────────
    if (st.enter) {
        char c = '\r';
        ssh_channel_write(g_channel, &c, 1);
    }

    if (st.tab) {
        char c = '\t';
        ssh_channel_write(g_channel, &c, 1);
    }

    if (st.del) {
        char c = 0x7F;
        ssh_channel_write(g_channel, &c, 1);
    }

    // ───────────────────────────────
    //   Fn + 方向键映射
    // ───────────────────────────────
    if (st.fn && !st.word.empty()) {
        char k = st.word[0];
        switch (k) {
        case ';': ssh_channel_write(g_channel, "\x1B[A", 3); return; // Up
        case ',': ssh_channel_write(g_channel, "\x1B[D", 3); return; // Left
        case '.': ssh_channel_write(g_channel, "\x1B[B", 3); return; // Down
        case '/': ssh_channel_write(g_channel, "\x1B[C", 3); return; // Right
        case '`': ssh_channel_write(g_channel, "\x1B", 1);  return;   // ESC
        }
    }

    // ───────────────────────────────
    //   Ctrl + 字母
    // ───────────────────────────────
    if (st.ctrl && st.word.size() == 1) {
        char ch = st.word[0];

        if (ch >= 'a' && ch <= 'z') {
            char c = ch - 'a' + 1;
            ssh_channel_write(g_channel, &c, 1);
            return;
        }

        if (ch >= 'A' && ch <= 'Z') {
            char c = ch - 'A' + 1;
            ssh_channel_write(g_channel, &c, 1);
            return;
        }
    }
}


void termPrint(const String &s) {
    for (int i = 0; i < s.length(); i++) {
        handleSshChar(s[i]);  // 利用终端渲染系统输出字符
    }
}

void termPrintln(const String &s) {
    termPrint(s);
    handleSshChar('\n');
}

void termClear() {
    handleSshChar(0x1B);       // ESC
    handleSshChar('[');
    handleSshChar('2');
    handleSshChar('J');        // 清屏
    handleSshChar(0x1B);
    handleSshChar('[');
    handleSshChar('H');        // 光标回到左上角
}



