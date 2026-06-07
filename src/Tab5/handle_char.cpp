#include "handle_char.h"
#include <Arduino.h>
#include <vector>

namespace {

struct Cell {
    char ch = ' ';
    uint16_t fg = WHITE;
    uint16_t bg = BLACK;
};

bool ansiInEsc = false;
bool ansiInCsi = false;
bool ansiInOsc = false;
String ansiBuf = "";

uint16_t currentFG = WHITE;
uint16_t currentBG = BLACK;

constexpr int fontW = 12;
constexpr int fontH = 16;
constexpr int lineHeight = 16;

int termCols = 0;
int termRows = 0;
int cursorCol = 0;
int cursorRow = 0;

std::vector<Cell> screen;

auto &lcd = M5.Display;

int maxInt(int a, int b) {
    return a > b ? a : b;
}

int minInt(int a, int b) {
    return a < b ? a : b;
}

int indexOf(int row, int col) {
    return row * termCols + col;
}

void ensureBuffer() {
    int cols = maxInt(1, (int)lcd.width() / fontW);
    int rows = maxInt(1, (int)lcd.height() / lineHeight);

    if (cols == termCols && rows == termRows && screen.size() == (size_t)(cols * rows)) {
        return;
    }

    termCols = cols;
    termRows = rows;
    cursorCol = 0;
    cursorRow = 0;
    screen.assign(termCols * termRows, Cell{});
}

void drawCell(int row, int col) {
    if (row < 0 || row >= termRows || col < 0 || col >= termCols) return;

    const Cell &cell = screen[indexOf(row, col)];
    const int x = col * fontW;
    const int y = row * lineHeight;

    lcd.fillRect(x, y, fontW, lineHeight, cell.bg);
    if (cell.ch != ' ') {
        lcd.setTextColor(cell.fg, cell.bg);
        lcd.setCursor(x, y);
        lcd.write(cell.ch);
    }
}

void redrawLine(int row) {
    if (row < 0 || row >= termRows) return;
    for (int col = 0; col < termCols; ++col) {
        drawCell(row, col);
    }
}

void redrawScreen() {
    for (int row = 0; row < termRows; ++row) {
        redrawLine(row);
    }
}

void blankCell(int row, int col) {
    if (row < 0 || row >= termRows || col < 0 || col >= termCols) return;
    screen[indexOf(row, col)] = Cell{' ', currentFG, currentBG};
}

void setCell(int row, int col, char ch) {
    if (row < 0 || row >= termRows || col < 0 || col >= termCols) return;
    screen[indexOf(row, col)] = Cell{ch, currentFG, currentBG};
    drawCell(row, col);
}

void setCursorCell(int col, int row) {
    cursorCol = constrain(col, 0, maxInt(0, termCols - 1));
    cursorRow = constrain(row, 0, maxInt(0, termRows - 1));
    lcd.setCursor(cursorCol * fontW, cursorRow * lineHeight);
}

void scrollUpOneLine() {
    if (termRows <= 1) return;

    for (int row = 1; row < termRows; ++row) {
        for (int col = 0; col < termCols; ++col) {
            screen[indexOf(row - 1, col)] = screen[indexOf(row, col)];
        }
    }

    for (int col = 0; col < termCols; ++col) {
        blankCell(termRows - 1, col);
    }

    redrawScreen();
}

void newLine() {
    cursorCol = 0;
    ++cursorRow;
    if (cursorRow >= termRows) {
        cursorRow = termRows - 1;
        scrollUpOneLine();
    }
    setCursorCell(cursorCol, cursorRow);
}

void putChar(char c) {
    ensureBuffer();

    if (c == '\r') {
        setCursorCell(0, cursorRow);
        return;
    }

    if (c == '\n') {
        newLine();
        return;
    }

    if (c == '\t') {
        int spaces = 4 - (cursorCol % 4);
        while (spaces-- > 0) putChar(' ');
        return;
    }

    if (c < 0x20) return;

    setCell(cursorRow, cursorCol, c);
    ++cursorCol;
    if (cursorCol >= termCols) {
        newLine();
    } else {
        setCursorCell(cursorCol, cursorRow);
    }
}

void clearScreen() {
    ensureBuffer();
    for (int row = 0; row < termRows; ++row) {
        for (int col = 0; col < termCols; ++col) {
            blankCell(row, col);
        }
    }
    lcd.fillScreen(currentBG);
    setCursorCell(0, 0);
}

void clearScreenRange(int fromRow, int fromCol, int toRow, int toCol) {
    ensureBuffer();
    if (termRows <= 0 || termCols <= 0) return;

    fromRow = constrain(fromRow, 0, termRows - 1);
    toRow = constrain(toRow, 0, termRows - 1);
    fromCol = constrain(fromCol, 0, termCols - 1);
    toCol = constrain(toCol, 0, termCols - 1);
    if (fromRow > toRow) return;

    for (int row = fromRow; row <= toRow; ++row) {
        int startCol = (row == fromRow) ? fromCol : 0;
        int endCol = (row == toRow) ? toCol : termCols - 1;
        for (int col = startCol; col <= endCol; ++col) {
            blankCell(row, col);
            drawCell(row, col);
        }
    }
}

void clearLineRange(int row, int fromCol, int toCol) {
    ensureBuffer();
    if (row < 0 || row >= termRows) return;

    fromCol = constrain(fromCol, 0, maxInt(0, termCols - 1));
    toCol = constrain(toCol, 0, maxInt(0, termCols - 1));
    if (fromCol > toCol) return;

    for (int col = fromCol; col <= toCol; ++col) {
        blankCell(row, col);
        drawCell(row, col);
    }
}

void deleteChars(int count) {
    ensureBuffer();
    count = maxInt(1, count);
    if (cursorRow < 0 || cursorRow >= termRows || cursorCol >= termCols) return;

    for (int col = cursorCol; col < termCols; ++col) {
        int src = col + count;
        if (src < termCols) {
            screen[indexOf(cursorRow, col)] = screen[indexOf(cursorRow, src)];
        } else {
            screen[indexOf(cursorRow, col)] = Cell{' ', currentFG, currentBG};
        }
    }
    redrawLine(cursorRow);
}

void insertBlankChars(int count) {
    ensureBuffer();
    count = maxInt(1, count);
    if (cursorRow < 0 || cursorRow >= termRows || cursorCol >= termCols) return;

    for (int col = termCols - 1; col >= cursorCol; --col) {
        int src = col - count;
        if (src >= cursorCol) {
            screen[indexOf(cursorRow, col)] = screen[indexOf(cursorRow, src)];
        } else {
            screen[indexOf(cursorRow, col)] = Cell{' ', currentFG, currentBG};
        }
    }
    redrawLine(cursorRow);
}

std::vector<int> parseParams(const String &params) {
    std::vector<int> result;
    if (!params.length()) {
        result.push_back(0);
        return result;
    }

    int start = 0;
    while (start <= params.length()) {
        int p = params.indexOf(';', start);
        String token = (p < 0) ? params.substring(start) : params.substring(start, p);
        result.push_back(token.length() ? token.toInt() : 0);
        if (p < 0) break;
        start = p + 1;
    }
    return result;
}

int paramOrDefault(const std::vector<int> &params, size_t index, int def) {
    if (index >= params.size() || params[index] <= 0) return def;
    return params[index];
}

void applySgr(const std::vector<int> &params) {
    static uint16_t colorMap[8] = {BLACK, RED, GREEN, YELLOW, BLUE, MAGENTA, CYAN, WHITE};

    for (int v : params) {
        if (v == 0) {
            currentFG = WHITE;
            currentBG = BLACK;
        } else if (30 <= v && v <= 37) {
            currentFG = colorMap[v - 30];
        } else if (40 <= v && v <= 47) {
            currentBG = colorMap[v - 40];
        } else if (90 <= v && v <= 97) {
            currentFG = colorMap[v - 90];
        } else if (100 <= v && v <= 107) {
            currentBG = colorMap[v - 100];
        }
    }
}

void handleCsi(char cmd, const String &rawParams) {
    ensureBuffer();
    std::vector<int> params = parseParams(rawParams);

    if (cmd == 'H' || cmd == 'f') {
        int row = paramOrDefault(params, 0, 1);
        int col = paramOrDefault(params, 1, 1);
        setCursorCell(col - 1, row - 1);
    } else if (cmd == 'J') {
        int mode = paramOrDefault(params, 0, 0);
        if (mode == 0) {
            clearScreenRange(cursorRow, cursorCol, termRows - 1, termCols - 1);
        } else if (mode == 1) {
            clearScreenRange(0, 0, cursorRow, cursorCol);
        } else if (mode == 2) {
            clearScreen();
        }
    } else if (cmd == 'K') {
        int mode = paramOrDefault(params, 0, 0);
        if (mode == 0) {
            clearLineRange(cursorRow, cursorCol, termCols - 1);
        } else if (mode == 1) {
            clearLineRange(cursorRow, 0, cursorCol);
        } else if (mode == 2) {
            clearLineRange(cursorRow, 0, termCols - 1);
        }
    } else if (cmd == 'A') {
        setCursorCell(cursorCol, cursorRow - paramOrDefault(params, 0, 1));
    } else if (cmd == 'B') {
        setCursorCell(cursorCol, cursorRow + paramOrDefault(params, 0, 1));
    } else if (cmd == 'C') {
        setCursorCell(cursorCol + paramOrDefault(params, 0, 1), cursorRow);
    } else if (cmd == 'D') {
        setCursorCell(cursorCol - paramOrDefault(params, 0, 1), cursorRow);
    } else if (cmd == 'G') {
        setCursorCell(paramOrDefault(params, 0, 1) - 1, cursorRow);
    } else if (cmd == 'P') {
        deleteChars(paramOrDefault(params, 0, 1));
    } else if (cmd == '@') {
        insertBlankChars(paramOrDefault(params, 0, 1));
    } else if (cmd == 'X') {
        int count = paramOrDefault(params, 0, 1);
        clearLineRange(cursorRow, cursorCol, minInt(termCols - 1, cursorCol + count - 1));
    } else if (cmd == 'm') {
        applySgr(params);
    }
}

void resetAnsiState() {
    ansiInEsc = false;
    ansiInCsi = false;
    ansiBuf = "";
}

} // namespace

void handleSshChar(char c)
{
    ensureBuffer();

    if (c == 0x08 || c == 0x7F) {
        setCursorCell(cursorCol - 1, cursorRow);
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
        putChar(c);
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
        resetAnsiState();
        return;
    }

    ansiBuf += c;
    if (c < 0x40 || c > 0x7E) return;

    handleCsi(c, ansiBuf.substring(0, ansiBuf.length() - 1));
    resetAnsiState();
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
    clearScreen();
    termFlush();
}

void termFlush() {
    lcd.display();
}
