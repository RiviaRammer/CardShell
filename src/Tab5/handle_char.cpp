#include "handle_char.h"
#include <Arduino.h>
#include <vector>

namespace {

struct Cell {
    uint32_t cp = ' ';
    uint16_t fg = WHITE;
    uint16_t bg = BLACK;
    uint8_t width = 1;
    bool continuation = false;
    bool reverse = false;
};

struct ScreenBuffer {
    std::vector<Cell> cells;
    int cursorCol = 0;
    int cursorRow = 0;
    int savedCol = 0;
    int savedRow = 0;
};

bool ansiInEsc = false;
bool ansiInCsi = false;
bool ansiInOsc = false;
bool ansiOscEsc = false;
String ansiBuf = "";

uint16_t currentFG = WHITE;
uint16_t currentBG = BLACK;
bool attrReverse = false;

constexpr int fontW = 12;
constexpr int lineHeight = 24;

int termCols = 0;
int termRows = 0;
int scrollTop = 0;
int scrollBottom = 0;
bool alternateScreen = false;
bool cursorVisible = true;
bool originMode = false;
bool autoWrap = true;
bool cursorDrawn = false;
int cursorDrawCol = -1;
int cursorDrawRow = -1;

ScreenBuffer mainScreen;
ScreenBuffer altScreen;
std::vector<bool> tabStops;

uint32_t utf8Codepoint = 0;
uint8_t utf8Remaining = 0;

auto &lcd = M5.Display;

int maxInt(int a, int b) {
    return a > b ? a : b;
}

int minInt(int a, int b) {
    return a < b ? a : b;
}

ScreenBuffer &activeScreen() {
    return alternateScreen ? altScreen : mainScreen;
}

int indexOf(int row, int col) {
    return row * termCols + col;
}

uint16_t effectiveFG(const Cell &cell) {
    return cell.reverse ? cell.bg : cell.fg;
}

uint16_t effectiveBG(const Cell &cell) {
    return cell.reverse ? cell.fg : cell.bg;
}

uint8_t cellWidthForCodepoint(uint32_t cp) {
    if (cp == 0) return 0;
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0xFE00 && cp <= 0xFE0F)) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) ||
        (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) ||
        (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE10 && cp <= 0xFE19) ||
        (cp >= 0xFE30 && cp <= 0xFE6F) ||
        (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6)) {
        return 2;
    }
    return 1;
}

String utf8FromCodepoint(uint32_t cp) {
    String out;
    if (cp <= 0x7F) {
        out += (char)cp;
    } else if (cp <= 0x7FF) {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    } else {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    return out;
}

void resizeBuffer(ScreenBuffer &buf) {
    buf.cells.assign(termCols * termRows, Cell{});
    buf.cursorCol = 0;
    buf.cursorRow = 0;
    buf.savedCol = 0;
    buf.savedRow = 0;
}

void resetTabStops() {
    tabStops.assign(termCols, false);
    for (int col = 8; col < termCols; col += 8) {
        tabStops[col] = true;
    }
}

void ensureBuffer() {
    int cols = maxInt(1, (int)lcd.width() / fontW);
    int rows = maxInt(1, (int)lcd.height() / lineHeight);

    if (cols == termCols && rows == termRows &&
        mainScreen.cells.size() == (size_t)(cols * rows) &&
        altScreen.cells.size() == (size_t)(cols * rows)) {
        return;
    }

    termCols = cols;
    termRows = rows;
    scrollTop = 0;
    scrollBottom = termRows - 1;
    resizeBuffer(mainScreen);
    resizeBuffer(altScreen);
    resetTabStops();
}

Cell blankCell() {
    return Cell{' ', currentFG, currentBG, 1, false, attrReverse};
}

void drawCell(int row, int col) {
    ensureBuffer();
    if (row < 0 || row >= termRows || col < 0 || col >= termCols) return;

    ScreenBuffer &buf = activeScreen();
    const Cell &cell = buf.cells[indexOf(row, col)];
    if (cell.continuation) return;

    const int x = col * fontW;
    const int y = row * lineHeight;
    const int drawWidth = (cell.width == 2 && col + 1 < termCols) ? fontW * 2 : fontW;
    const uint16_t fg = effectiveFG(cell);
    const uint16_t bg = effectiveBG(cell);

    lcd.fillRect(x, y, drawWidth, lineHeight, bg);
    if (cell.cp != ' ') {
        lcd.setTextColor(fg, bg);
        lcd.setCursor(x, y);
        auto style = lcd.getTextStyle();
        const auto *font = lcd.getFont();
        lcd.setFont(&fonts::efontCN_24);
        lcd.setTextSize(1);
        lcd.drawString(utf8FromCodepoint(cell.cp), x, y);
        lcd.setFont(font);
        lcd.setTextStyle(style);
    }
}

void hideDrawnCursor() {
    if (!cursorDrawn) return;
    cursorDrawn = false;
    drawCell(cursorDrawRow, cursorDrawCol);
}

void drawCursor() {
    if (!cursorVisible) return;
    ScreenBuffer &buf = activeScreen();
    cursorDrawCol = buf.cursorCol;
    cursorDrawRow = buf.cursorRow;
    cursorDrawn = true;

    int x = cursorDrawCol * fontW;
    int y = cursorDrawRow * lineHeight;
    lcd.drawRect(x, y, fontW, lineHeight, currentFG);
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

void setCursorCell(int col, int row) {
    ensureBuffer();
    ScreenBuffer &buf = activeScreen();
    buf.cursorCol = constrain(col, 0, maxInt(0, termCols - 1));
    buf.cursorRow = constrain(row, 0, maxInt(0, termRows - 1));
    lcd.setCursor(buf.cursorCol * fontW, buf.cursorRow * lineHeight);
}

void clearCellAt(int row, int col, bool redraw = false) {
    if (row < 0 || row >= termRows || col < 0 || col >= termCols) return;

    ScreenBuffer &buf = activeScreen();
    Cell &cell = buf.cells[indexOf(row, col)];
    if (cell.continuation && col > 0) {
        clearCellAt(row, col - 1, redraw);
        return;
    }

    if (cell.width == 2 && col + 1 < termCols) {
        buf.cells[indexOf(row, col + 1)] = blankCell();
        if (redraw) drawCell(row, col + 1);
    }
    cell = blankCell();
    if (redraw) drawCell(row, col);
}

void normalizeLine(int row) {
    if (row < 0 || row >= termRows) return;

    ScreenBuffer &buf = activeScreen();
    for (int col = 0; col < termCols; ++col) {
        Cell &cell = buf.cells[indexOf(row, col)];
        if (cell.continuation) {
            if (col == 0 || buf.cells[indexOf(row, col - 1)].width != 2) {
                cell = blankCell();
            }
            continue;
        }
        if (cell.width == 2) {
            if (col + 1 >= termCols) {
                cell = blankCell();
            } else {
                Cell &next = buf.cells[indexOf(row, col + 1)];
                next = Cell{' ', cell.fg, cell.bg, 0, true, cell.reverse};
                ++col;
            }
        } else if (cell.width == 0) {
            cell = blankCell();
        }
    }
}

void putCodepointAt(int row, int col, uint32_t cp) {
    ensureBuffer();
    if (row < 0 || row >= termRows || col < 0 || col >= termCols) return;

    uint8_t width = cellWidthForCodepoint(cp);
    if (width == 0) return;
    if (width == 2 && col >= termCols - 1) return;

    ScreenBuffer &buf = activeScreen();
    clearCellAt(row, col, true);
    if (width == 2) clearCellAt(row, col + 1, true);
    if (col > 0 && buf.cells[indexOf(row, col - 1)].width == 2) clearCellAt(row, col - 1, true);

    buf.cells[indexOf(row, col)] = Cell{cp, currentFG, currentBG, width, false, attrReverse};
    if (width == 2) {
        buf.cells[indexOf(row, col + 1)] = Cell{' ', currentFG, currentBG, 0, true, attrReverse};
    }
    drawCell(row, col);
}

void clearLineRange(int row, int fromCol, int toCol) {
    ensureBuffer();
    if (row < 0 || row >= termRows) return;

    fromCol = constrain(fromCol, 0, maxInt(0, termCols - 1));
    toCol = constrain(toCol, 0, maxInt(0, termCols - 1));
    if (fromCol > toCol) return;

    for (int col = fromCol; col <= toCol; ++col) {
        clearCellAt(row, col);
    }
    redrawLine(row);
}

void clearScreen() {
    ensureBuffer();
    ScreenBuffer &buf = activeScreen();
    for (int i = 0; i < termCols * termRows; ++i) {
        buf.cells[i] = blankCell();
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
            clearCellAt(row, col);
        }
        redrawLine(row);
    }
}

void scrollRegionUp(int top, int bottom, int count) {
    ensureBuffer();
    count = maxInt(1, count);
    top = constrain(top, 0, termRows - 1);
    bottom = constrain(bottom, 0, termRows - 1);
    if (top >= bottom) return;

    ScreenBuffer &buf = activeScreen();
    count = minInt(count, bottom - top + 1);
    for (int row = top; row <= bottom - count; ++row) {
        for (int col = 0; col < termCols; ++col) {
            buf.cells[indexOf(row, col)] = buf.cells[indexOf(row + count, col)];
        }
    }
    for (int row = bottom - count + 1; row <= bottom; ++row) {
        for (int col = 0; col < termCols; ++col) {
            buf.cells[indexOf(row, col)] = blankCell();
        }
    }
    for (int row = top; row <= bottom; ++row) normalizeLine(row);
    redrawScreen();
}

void scrollRegionDown(int top, int bottom, int count) {
    ensureBuffer();
    count = maxInt(1, count);
    top = constrain(top, 0, termRows - 1);
    bottom = constrain(bottom, 0, termRows - 1);
    if (top >= bottom) return;

    ScreenBuffer &buf = activeScreen();
    count = minInt(count, bottom - top + 1);
    for (int row = bottom; row >= top + count; --row) {
        for (int col = 0; col < termCols; ++col) {
            buf.cells[indexOf(row, col)] = buf.cells[indexOf(row - count, col)];
        }
    }
    for (int row = top; row < top + count; ++row) {
        for (int col = 0; col < termCols; ++col) {
            buf.cells[indexOf(row, col)] = blankCell();
        }
    }
    for (int row = top; row <= bottom; ++row) normalizeLine(row);
    redrawScreen();
}

void newLine() {
    ScreenBuffer &buf = activeScreen();
    buf.cursorCol = 0;
    ++buf.cursorRow;
    if (buf.cursorRow > scrollBottom) {
        buf.cursorRow = scrollBottom;
        scrollRegionUp(scrollTop, scrollBottom, 1);
    }
    setCursorCell(buf.cursorCol, buf.cursorRow);
}

void putCodepoint(uint32_t cp) {
    ensureBuffer();
    ScreenBuffer &buf = activeScreen();

    if (cp == '\r') {
        setCursorCell(0, buf.cursorRow);
        return;
    }
    if (cp == '\n') {
        newLine();
        return;
    }
    if (cp == '\t') {
        int next = termCols - 1;
        for (int col = buf.cursorCol + 1; col < termCols; ++col) {
            if (col < (int)tabStops.size() && tabStops[col]) {
                next = col;
                break;
            }
        }
        setCursorCell(next, buf.cursorRow);
        return;
    }
    if (cp < 0x20) return;

    uint8_t width = cellWidthForCodepoint(cp);
    if (width == 0) return;
    if (buf.cursorCol + width > termCols) {
        newLine();
    }

    putCodepointAt(buf.cursorRow, buf.cursorCol, cp);
    if (buf.cursorCol + width >= termCols) {
        if (autoWrap) {
            newLine();
        } else {
            setCursorCell(termCols - 1, buf.cursorRow);
        }
    } else {
        setCursorCell(buf.cursorCol + width, buf.cursorRow);
    }
}

void resetTerminalState() {
    ensureBuffer();
    currentFG = WHITE;
    currentBG = BLACK;
    attrReverse = false;
    originMode = false;
    autoWrap = true;
    cursorVisible = true;
    alternateScreen = false;
    scrollTop = 0;
    scrollBottom = termRows - 1;
    resetTabStops();
    clearScreen();
}

void setCursorByCup(int col, int row) {
    if (originMode) {
        row = constrain(scrollTop + row, scrollTop, scrollBottom);
    }
    setCursorCell(col, row);
}

void cursorUp(int count) {
    ScreenBuffer &buf = activeScreen();
    int minRow = originMode ? scrollTop : 0;
    setCursorCell(buf.cursorCol, maxInt(minRow, buf.cursorRow - maxInt(1, count)));
}

void cursorDown(int count) {
    ScreenBuffer &buf = activeScreen();
    int maxRow = originMode ? scrollBottom : termRows - 1;
    setCursorCell(buf.cursorCol, minInt(maxRow, buf.cursorRow + maxInt(1, count)));
}

void setTabStopAtCursor() {
    ScreenBuffer &buf = activeScreen();
    if (buf.cursorCol >= 0 && buf.cursorCol < (int)tabStops.size()) {
        tabStops[buf.cursorCol] = true;
    }
}

void clearTabStops(int mode) {
    ScreenBuffer &buf = activeScreen();
    if (mode == 0) {
        if (buf.cursorCol >= 0 && buf.cursorCol < (int)tabStops.size()) {
            tabStops[buf.cursorCol] = false;
        }
    } else if (mode == 3) {
        for (size_t i = 0; i < tabStops.size(); ++i) tabStops[i] = false;
    }
}

void eraseChars(int count) {
    ScreenBuffer &buf = activeScreen();
    int endCol = minInt(termCols - 1, buf.cursorCol + maxInt(1, count) - 1);
    for (int col = buf.cursorCol; col <= endCol; ++col) {
        clearCellAt(buf.cursorRow, col);
    }
    redrawLine(buf.cursorRow);
}

void cursorForwardTab(int count) {
    ScreenBuffer &buf = activeScreen();
    int col = buf.cursorCol;
    count = maxInt(1, count);
    while (count-- > 0) {
        int next = termCols - 1;
        for (int i = col + 1; i < termCols; ++i) {
            if (i < (int)tabStops.size() && tabStops[i]) {
                next = i;
                break;
            }
        }
        col = next;
    }
    setCursorCell(col, buf.cursorRow);
}

void cursorBackTab(int count) {
    ScreenBuffer &buf = activeScreen();
    int col = buf.cursorCol;
    count = maxInt(1, count);
    while (count-- > 0) {
        int prev = 0;
        for (int i = col - 1; i >= 0; --i) {
            if (i < (int)tabStops.size() && tabStops[i]) {
                prev = i;
                break;
            }
        }
        col = prev;
    }
    setCursorCell(col, buf.cursorRow);
}

void indexDown() {
    ScreenBuffer &buf = activeScreen();
    if (buf.cursorRow == scrollBottom) {
        scrollRegionUp(scrollTop, scrollBottom, 1);
    } else {
        setCursorCell(buf.cursorCol, buf.cursorRow + 1);
    }
}

void reverseIndex() {
    ScreenBuffer &buf = activeScreen();
    if (buf.cursorRow == scrollTop) {
        scrollRegionDown(scrollTop, scrollBottom, 1);
    } else {
        setCursorCell(buf.cursorCol, buf.cursorRow - 1);
    }
}

void nextLine() {
    ScreenBuffer &buf = activeScreen();
    setCursorCell(0, buf.cursorRow);
    indexDown();
}

void deleteLines(int count) {
    ScreenBuffer &buf = activeScreen();
    int top = maxInt(buf.cursorRow, scrollTop);
    if (top <= scrollBottom) scrollRegionUp(top, scrollBottom, count);
}

void insertLines(int count) {
    ScreenBuffer &buf = activeScreen();
    int top = maxInt(buf.cursorRow, scrollTop);
    if (top <= scrollBottom) scrollRegionDown(top, scrollBottom, count);
}

void deleteChars(int count) {
    ensureBuffer();
    ScreenBuffer &buf = activeScreen();
    count = maxInt(1, count);
    if (buf.cursorRow < 0 || buf.cursorRow >= termRows || buf.cursorCol >= termCols) return;

    for (int col = buf.cursorCol; col < termCols; ++col) {
        int src = col + count;
        if (src < termCols) {
            buf.cells[indexOf(buf.cursorRow, col)] = buf.cells[indexOf(buf.cursorRow, src)];
        } else {
            buf.cells[indexOf(buf.cursorRow, col)] = blankCell();
        }
    }
    normalizeLine(buf.cursorRow);
    redrawLine(buf.cursorRow);
}

void insertBlankChars(int count) {
    ensureBuffer();
    ScreenBuffer &buf = activeScreen();
    count = maxInt(1, count);
    if (buf.cursorRow < 0 || buf.cursorRow >= termRows || buf.cursorCol >= termCols) return;

    for (int col = termCols - 1; col >= buf.cursorCol; --col) {
        int src = col - count;
        if (src >= buf.cursorCol) {
            buf.cells[indexOf(buf.cursorRow, col)] = buf.cells[indexOf(buf.cursorRow, src)];
        } else {
            buf.cells[indexOf(buf.cursorRow, col)] = blankCell();
        }
    }
    normalizeLine(buf.cursorRow);
    redrawLine(buf.cursorRow);
}

void saveCursor() {
    ScreenBuffer &buf = activeScreen();
    buf.savedCol = buf.cursorCol;
    buf.savedRow = buf.cursorRow;
}

void restoreCursor() {
    ScreenBuffer &buf = activeScreen();
    setCursorCell(buf.savedCol, buf.savedRow);
}

std::vector<int> parseParams(String params, bool &privateMode) {
    std::vector<int> result;
    privateMode = false;
    if (params.length() && (params[0] == '?' || params[0] == '>')) {
        privateMode = params[0] == '?';
        params.remove(0, 1);
    }
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

    for (size_t i = 0; i < params.size(); ++i) {
        int v = params[i];
        if (v == 0) {
            currentFG = WHITE;
            currentBG = BLACK;
            attrReverse = false;
        } else if (v == 1) {
            // Bold is approximated by bright foreground only when a later color selects it.
        } else if (v == 7) {
            attrReverse = true;
        } else if (v == 22) {
            // Normal intensity: ignored for now.
        } else if (v == 27) {
            attrReverse = false;
        } else if (30 <= v && v <= 37) {
            currentFG = colorMap[v - 30];
        } else if (40 <= v && v <= 47) {
            currentBG = colorMap[v - 40];
        } else if (90 <= v && v <= 97) {
            currentFG = colorMap[v - 90];
        } else if (100 <= v && v <= 107) {
            currentBG = colorMap[v - 100];
        } else if ((v == 38 || v == 48) && i + 2 < params.size() && params[i + 1] == 5) {
            uint8_t idx = params[i + 2] & 7;
            if (v == 38) currentFG = colorMap[idx];
            else currentBG = colorMap[idx];
            i += 2;
        } else if ((v == 38 || v == 48) && i + 4 < params.size() && params[i + 1] == 2) {
            uint16_t color = lcd.color888(params[i + 2], params[i + 3], params[i + 4]);
            if (v == 38) currentFG = color;
            else currentBG = color;
            i += 4;
        }
    }
}

void setPrivateMode(int mode, bool enabled) {
    if (mode == 25) {
        cursorVisible = enabled;
    } else if (mode == 6) {
        originMode = enabled;
        setCursorCell(0, enabled ? scrollTop : 0);
    } else if (mode == 7) {
        autoWrap = enabled;
    } else if (mode == 47 || mode == 1047 || mode == 1049) {
        if (enabled && mode == 1049) saveCursor();
        alternateScreen = enabled;
        if (enabled) clearScreen();
        else {
            redrawScreen();
            if (mode == 1049) restoreCursor();
        }
    }
}

void handleCsi(char cmd, const String &rawParams) {
    ensureBuffer();
    bool privateMode = false;
    std::vector<int> params = parseParams(rawParams, privateMode);
    ScreenBuffer &buf = activeScreen();

    if (cmd == 'H' || cmd == 'f') {
        int row = paramOrDefault(params, 0, 1);
        int col = paramOrDefault(params, 1, 1);
        setCursorByCup(col - 1, row - 1);
    } else if (cmd == 'J') {
        int mode = paramOrDefault(params, 0, 0);
        if (mode == 0) clearScreenRange(buf.cursorRow, buf.cursorCol, termRows - 1, termCols - 1);
        else if (mode == 1) clearScreenRange(0, 0, buf.cursorRow, buf.cursorCol);
        else if (mode == 2 || mode == 3) clearScreen();
    } else if (cmd == 'K') {
        int mode = paramOrDefault(params, 0, 0);
        if (mode == 0) clearLineRange(buf.cursorRow, buf.cursorCol, termCols - 1);
        else if (mode == 1) clearLineRange(buf.cursorRow, 0, buf.cursorCol);
        else if (mode == 2) clearLineRange(buf.cursorRow, 0, termCols - 1);
    } else if (cmd == 'A') {
        cursorUp(paramOrDefault(params, 0, 1));
    } else if (cmd == 'B') {
        cursorDown(paramOrDefault(params, 0, 1));
    } else if (cmd == 'C') {
        setCursorCell(buf.cursorCol + paramOrDefault(params, 0, 1), buf.cursorRow);
    } else if (cmd == 'D') {
        setCursorCell(buf.cursorCol - paramOrDefault(params, 0, 1), buf.cursorRow);
    } else if (cmd == 'E') {
        cursorDown(paramOrDefault(params, 0, 1));
        setCursorCell(0, activeScreen().cursorRow);
    } else if (cmd == 'F') {
        cursorUp(paramOrDefault(params, 0, 1));
        setCursorCell(0, activeScreen().cursorRow);
    } else if (cmd == 'G') {
        setCursorCell(paramOrDefault(params, 0, 1) - 1, buf.cursorRow);
    } else if (cmd == 'P') {
        deleteChars(paramOrDefault(params, 0, 1));
    } else if (cmd == '@') {
        insertBlankChars(paramOrDefault(params, 0, 1));
    } else if (cmd == 'X') {
        eraseChars(paramOrDefault(params, 0, 1));
    } else if (cmd == 'L') {
        insertLines(paramOrDefault(params, 0, 1));
    } else if (cmd == 'M') {
        deleteLines(paramOrDefault(params, 0, 1));
    } else if (cmd == 'S') {
        scrollRegionUp(scrollTop, scrollBottom, paramOrDefault(params, 0, 1));
    } else if (cmd == 'T') {
        scrollRegionDown(scrollTop, scrollBottom, paramOrDefault(params, 0, 1));
    } else if (cmd == 'r') {
        int top = paramOrDefault(params, 0, 1) - 1;
        int bottom = paramOrDefault(params, 1, termRows) - 1;
        if (top >= 0 && bottom > top && bottom < termRows) {
            scrollTop = top;
            scrollBottom = bottom;
            setCursorByCup(0, 0);
        }
    } else if (cmd == 's') {
        saveCursor();
    } else if (cmd == 'u') {
        restoreCursor();
    } else if (cmd == 'm') {
        applySgr(params);
    } else if (cmd == 'g') {
        clearTabStops(paramOrDefault(params, 0, 0));
    } else if (cmd == 'I') {
        cursorForwardTab(paramOrDefault(params, 0, 1));
    } else if (cmd == 'Z') {
        cursorBackTab(paramOrDefault(params, 0, 1));
    } else if ((cmd == 'h' || cmd == 'l') && privateMode) {
        for (int mode : params) setPrivateMode(mode, cmd == 'h');
    }
}

void resetAnsiState() {
    ansiInEsc = false;
    ansiInCsi = false;
    ansiOscEsc = false;
    ansiBuf = "";
}

void resetUtf8() {
    utf8Codepoint = 0;
    utf8Remaining = 0;
}

bool feedUtf8(uint8_t b, uint32_t &cp) {
    if (utf8Remaining == 0) {
        if (b < 0x80) {
            cp = b;
            return true;
        }
        if ((b & 0xE0) == 0xC0) {
            utf8Codepoint = b & 0x1F;
            utf8Remaining = 1;
            return false;
        }
        if ((b & 0xF0) == 0xE0) {
            utf8Codepoint = b & 0x0F;
            utf8Remaining = 2;
            return false;
        }
        if ((b & 0xF8) == 0xF0) {
            utf8Codepoint = b & 0x07;
            utf8Remaining = 3;
            return false;
        }
        cp = '?';
        return true;
    }

    if ((b & 0xC0) != 0x80) {
        resetUtf8();
        cp = '?';
        return true;
    }

    utf8Codepoint = (utf8Codepoint << 6) | (b & 0x3F);
    --utf8Remaining;
    if (utf8Remaining == 0) {
        cp = utf8Codepoint;
        utf8Codepoint = 0;
        return true;
    }
    return false;
}

} // namespace

void handleSshChar(char c)
{
    ensureBuffer();
    hideDrawnCursor();

    if (c == 0x08 || c == 0x7F) {
        ScreenBuffer &buf = activeScreen();
        setCursorCell(buf.cursorCol - 1, buf.cursorRow);
        return;
    }

    if (ansiInOsc) {
        if (ansiOscEsc) {
            ansiInOsc = false;
            ansiOscEsc = false;
            if (c != '\\') {
                handleSshChar(c);
            }
            return;
        }
        if (c == 0x07) {
            ansiInOsc = false;
        } else if (c == 0x1B) {
            ansiOscEsc = true;
        }
        return;
    }

    if (!ansiInEsc) {
        if (c == 0x1B) {
            ansiInEsc = true;
            ansiInCsi = false;
            ansiBuf = "";
            resetUtf8();
            return;
        }
        uint32_t cp = 0;
        if (feedUtf8((uint8_t)c, cp)) {
            putCodepoint(cp);
        }
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
            ansiOscEsc = false;
            return;
        }
        if (c == '7') {
            saveCursor();
        } else if (c == '8') {
            restoreCursor();
        } else if (c == 'D') {
            indexDown();
        } else if (c == 'E') {
            nextLine();
        } else if (c == 'H') {
            setTabStopAtCursor();
        } else if (c == 'M') {
            reverseIndex();
        } else if (c == 'c') {
            resetTerminalState();
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
    hideDrawnCursor();
    clearScreen();
    termFlush();
}

void termFlush() {
    hideDrawnCursor();
    drawCursor();
    lcd.display();
}
