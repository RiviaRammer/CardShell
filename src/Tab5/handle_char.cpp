#include "handle_char.h"
#include <Arduino.h>
#include <vector>

namespace {

bool ansiInEsc = false;
bool ansiInCsi = false;
bool ansiInOsc = false;
String ansiBuf = "";

uint16_t currentFG = WHITE;
uint16_t currentBG = BLACK;

int cursorY = 0;
const int fontW      = 12;
const int fontH      = 16;
const int lineHeight = 16;

auto &lcd = M5.Display;

void applyTextColor() {
    lcd.setTextColor(currentFG, currentBG);
}

void ensureScroll() {
    if (cursorY > lcd.height() - lineHeight) {
        lcd.scroll(0, -lineHeight);
        cursorY -= lineHeight;
        lcd.setCursor(lcd.getCursorX(), cursorY);
    }
}

void terminalNewLine() {
    lcd.println();
    cursorY = lcd.getCursorY();
    ensureScroll();
}

void terminalPutChar(char c) {
    if (c == '\r') {
        lcd.setCursor(0, lcd.getCursorY());
        return;
    }
    if (c == '\n') {
        terminalNewLine();
        return;
    }

    lcd.write(c);
    cursorY = lcd.getCursorY();
    ensureScroll();
}

} // namespace

void handleSshChar(char c)
{
    if (c == 0x08) {
        int cx = lcd.getCursorX();
        int cy = lcd.getCursorY();
        if (cx >= fontW) {
            lcd.setCursor(cx - fontW, cy);
            cursorY = cy;
        }
        return;
    }

    if (ansiInOsc) {
        if (c == 0x07) ansiInOsc = false;
        return;
    }

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

    if (!ansiInCsi) {
        if (c == '[') {
            ansiInCsi = true;
            ansiBuf = "";
            return;
        }
        if (c == ']') {
            ansiInOsc = true;
            ansiInEsc = false;
            return;
        }
        ansiInEsc = false;
        return;
    }

    ansiBuf += c;

    if (c < 0x40 || c > 0x7E) return;

    char cmd = c;
    String params = ansiBuf.substring(0, ansiBuf.length() - 1);

    auto parseN = [&](int def = 1) {
        if (!params.length()) return def;
        int v = params.toInt();
        return v <= 0 ? def : v;
    };

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

        int x = constrain((col - 1) * fontW, 0, lcd.width() - fontW);
        int y = constrain((row - 1) * fontH, 0, lcd.height() - lineHeight);

        lcd.setCursor(x, y);
        cursorY = y;
    }
    else if (cmd == 'J') {
        if (params == "2" || params == "") {
            lcd.fillScreen(BLACK);
            lcd.setCursor(0, 0);
            cursorY = 0;
        }
    }
    else if (cmd == 'K') {
        int mode = params.length() ? params.toInt() : 0;
        int cx = lcd.getCursorX();
        int cy = lcd.getCursorY();

        if (mode == 0)
            lcd.fillRect(cx, cy, lcd.width() - cx, lineHeight, currentBG);
        else if (mode == 1)
            lcd.fillRect(0, cy, cx, lineHeight, currentBG);
        else if (mode == 2)
            lcd.fillRect(0, cy, lcd.width(), lineHeight, currentBG);

        lcd.setCursor(cx, cy);
        cursorY = cy;
    }
    else if (cmd == 'A') {
        int cx = lcd.getCursorX();
        int cy = max(0, (int)(lcd.getCursorY() - parseN() * lineHeight));
        lcd.setCursor(cx, cy);
        cursorY = cy;
    }
    else if (cmd == 'B') {
        int cx = lcd.getCursorX();
        int cy = min((int)(lcd.height() - lineHeight), (int)(lcd.getCursorY() + parseN() * lineHeight));
        lcd.setCursor(cx, cy);
        cursorY = cy;
    }
    else if (cmd == 'C') {
        int cx = min((int)(lcd.width() - fontW), (int)(lcd.getCursorX() + parseN() * fontW));
        lcd.setCursor(cx, lcd.getCursorY());
    }
    else if (cmd == 'D') {
        int cx = max(0, (int)(lcd.getCursorX() - parseN() * fontW));
        lcd.setCursor(cx, lcd.getCursorY());
    }
    else if (cmd == 'm') {
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
                static uint16_t fgMap[8] = {BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE};
                currentFG = fgMap[v - 30];
                applyTextColor();
            }
            else if (40 <= v && v <= 47) {
                static uint16_t bgMap[8] = {BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE};
                currentBG = bgMap[v - 40];
                applyTextColor();
            }
        }
    }

    ansiInEsc = false;
    ansiInCsi = false;
    ansiBuf = "";
}

void termPrint(const String &s) {
    for (int i = 0; i < s.length(); i++) {
        handleSshChar(s[i]);
    }
    termFlush();
}

void termPrintln(const String &s) {
    termPrint(s);
    handleSshChar('\n');
    termFlush();
}

void termClear() {
    handleSshChar(0x1B);
    handleSshChar('[');
    handleSshChar('2');
    handleSshChar('J');
    handleSshChar(0x1B);
    handleSshChar('[');
    handleSshChar('H');
    termFlush();
}

void termFlush() {
    lcd.display();
}
