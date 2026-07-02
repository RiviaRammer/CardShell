#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>

#include <M5Unified.h>
#include <M5UnitUnified.h>
#include <M5UnitUnifiedKEYBOARD.h>
#include <utility/hid_keycode.hpp>

#include <esp_heap_caps.h>

#include "src/arduinoVNC/VNC.h"
#include "config.h"

using namespace m5::unit;
using namespace m5::unit::tab5_keyboard;

namespace {

constexpr const char *WIFI_SSID = WIFI_CONFIG_SSID;
constexpr const char *WIFI_PASS = WIFI_CONFIG_PASSWORD;

constexpr const char *VNC_HOST = VNC_CONFIG_HOST;
constexpr uint16_t VNC_PORT = VNC_CONFIG_PORT;
constexpr const char *VNC_PASSWORD = VNC_CONFIG_PASSWORD;
constexpr uint16_t VNC_CLIENT_WIDTH = VNC_CONFIG_WIDTH;
constexpr uint16_t VNC_CLIENT_HEIGHT = VNC_CONFIG_HEIGHT;
constexpr uint16_t TOUCH_MOVE_DEADZONE = 3;
constexpr uint32_t TOUCH_LONG_PRESS_MS = 650;
constexpr uint16_t DISPLAY_BATCH_LINES = 16;
constexpr uint32_t DEFERRED_FLUSH_MIN_PIXELS = 80000;

constexpr int8_t TAB5_KEYBOARD_SDA = 0;
constexpr int8_t TAB5_KEYBOARD_SCL = 1;
constexpr uint32_t TAB5_KEYBOARD_I2C_CLOCK = 400000UL;
constexpr uint8_t TAB5_KEYBOARD_IRQ = 50;

constexpr gpio_num_t TAB5_WIFI_SDIO_CLK = GPIO_NUM_12;
constexpr gpio_num_t TAB5_WIFI_SDIO_CMD = GPIO_NUM_13;
constexpr gpio_num_t TAB5_WIFI_SDIO_D0 = GPIO_NUM_11;
constexpr gpio_num_t TAB5_WIFI_SDIO_D1 = GPIO_NUM_10;
constexpr gpio_num_t TAB5_WIFI_SDIO_D2 = GPIO_NUM_9;
constexpr gpio_num_t TAB5_WIFI_SDIO_D3 = GPIO_NUM_8;
constexpr gpio_num_t TAB5_WIFI_SDIO_RST = GPIO_NUM_15;

constexpr uint8_t VNC_BUTTON_LEFT = 0x01;
constexpr uint8_t VNC_BUTTON_RIGHT = 0x04;

constexpr uint8_t HID_BACKSPACE = 0x2A;
constexpr uint8_t HID_TAB = 0x2B;
constexpr uint8_t HID_ENTER = 0x28;
constexpr uint8_t HID_ESCAPE = 0x29;
constexpr uint8_t HID_SPACE = 0x2C;
constexpr uint8_t HID_DELETE = 0x4C;
constexpr uint8_t HID_RIGHT = 0x4F;
constexpr uint8_t HID_LEFT = 0x50;
constexpr uint8_t HID_DOWN = 0x51;
constexpr uint8_t HID_UP = 0x52;

UnitUnified Units;
UnitTab5Keyboard tab5Keyboard;

bool touchActive = false;
bool touchMoved = false;
int lastTouchX = 0;
int lastTouchY = 0;
int touchStartX = 0;
int touchStartY = 0;
uint32_t touchStartMs = 0;

class Tab5VNCDisplay final : public VNCdisplay {
public:
    bool beginFramebuffer() {
        if (framebuffer != nullptr) {
            return true;
        }

        const uint32_t bytes = static_cast<uint32_t>(VNC_CLIENT_WIDTH) * VNC_CLIENT_HEIGHT * sizeof(uint16_t);
        framebuffer = static_cast<uint16_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (framebuffer == nullptr) {
            Serial.printf("[Display] framebuffer allocation failed: %lu bytes\n", static_cast<unsigned long>(bytes));
            return false;
        }

        memset(framebuffer, 0, bytes);
        Serial.printf("[Display] framebuffer ready: %lux%lu, %lu bytes\n",
                      static_cast<unsigned long>(VNC_CLIENT_WIDTH),
                      static_cast<unsigned long>(VNC_CLIENT_HEIGHT),
                      static_cast<unsigned long>(bytes));
        return true;
    }

    bool hasCopyRect() override {
        return framebuffer != nullptr;
    }

    uint32_t getHeight() override {
        return M5.Display.height();
    }

    uint32_t getWidth() override {
        return M5.Display.width();
    }

    void draw_area(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint8_t *data) override {
        drawRgb565Block(x, y, w, h, reinterpret_cast<const uint16_t *>(data));
    }

    void draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color) override {
        const uint16_t converted = convertColor(color);
        fillFramebuffer(x, y, w, h, converted);
        M5.Display.fillRect(toDisplayX(x), toDisplayY(y), scaleW(w), scaleH(h), converted);
    }

    void copy_rect(uint32_t srcX, uint32_t srcY, uint32_t destX, uint32_t destY, uint32_t w, uint32_t h) override {
        if (framebuffer == nullptr || w == 0 || h == 0) {
            return;
        }

        if (srcX >= VNC_CLIENT_WIDTH || destX >= VNC_CLIENT_WIDTH ||
            srcY >= VNC_CLIENT_HEIGHT || destY >= VNC_CLIENT_HEIGHT) {
            return;
        }

        w = min<uint32_t>(w, VNC_CLIENT_WIDTH - srcX);
        w = min<uint32_t>(w, VNC_CLIENT_WIDTH - destX);
        h = min<uint32_t>(h, VNC_CLIENT_HEIGHT - srcY);
        h = min<uint32_t>(h, VNC_CLIENT_HEIGHT - destY);
        if (w == 0 || h == 0) {
            return;
        }

        if (destY > srcY) {
            for (int32_t row = static_cast<int32_t>(h) - 1; row >= 0; --row) {
                memmove(framebuffer + (destY + row) * VNC_CLIENT_WIDTH + destX,
                        framebuffer + (srcY + row) * VNC_CLIENT_WIDTH + srcX,
                        w * sizeof(uint16_t));
            }
        } else {
            for (uint32_t row = 0; row < h; ++row) {
                memmove(framebuffer + (destY + row) * VNC_CLIENT_WIDTH + destX,
                        framebuffer + (srcY + row) * VNC_CLIENT_WIDTH + srcX,
                        w * sizeof(uint16_t));
            }
        }

        pushFramebufferBlock(destX, destY, w, h);
    }

    void area_update_start(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override {
        areaX = x;
        areaY = y;
        areaW = w;
        areaH = h;
        areaPixel = 0;
        deferAreaFlush = framebuffer != nullptr && (w * h) >= DEFERRED_FLUSH_MIN_PIXELS;
    }

    void area_update_data(char *data, uint32_t pixels) override {
        if (areaW == 0 || areaH == 0 || pixels == 0) {
            return;
        }

        const uint16_t *src = reinterpret_cast<const uint16_t *>(data);
        while (pixels > 0 && areaPixel < areaW * areaH) {
            const uint32_t rowOffset = areaPixel % areaW;
            const uint32_t rowRemain = areaW - rowOffset;
            const uint32_t x = areaX + rowOffset;
            const uint32_t y = areaY + (areaPixel / areaW);

            if (rowOffset == 0 && pixels >= areaW) {
                const uint32_t rows = min<uint32_t>(pixels / areaW, (areaW * areaH - areaPixel) / areaW);
                const uint32_t batchRows = min<uint32_t>(rows, DISPLAY_BATCH_LINES);
                const uint32_t chunk = batchRows * areaW;
                drawRgb565Block(x, y, areaW, batchRows, src, !deferAreaFlush);
                src += chunk;
                areaPixel += chunk;
                pixels -= chunk;
                continue;
            }

            const uint32_t chunk = min<uint32_t>(pixels, rowRemain);
            drawRgb565Block(x, y, chunk, 1, src, !deferAreaFlush);
            src += chunk;
            areaPixel += chunk;
            pixels -= chunk;
        }
    }

    void area_update_end() override {
        if (deferAreaFlush) {
            pushFramebufferBlock(areaX, areaY, areaW, areaH);
            deferAreaFlush = false;
        }
    }

    void vnc_options_override(dfb_vnc_options *opt) override {
        opt->client.width = min<uint32_t>(VNC_CLIENT_WIDTH, M5.Display.width());
        opt->client.height = min<uint32_t>(VNC_CLIENT_HEIGHT, M5.Display.height());
        opt->client.bpp = 16;
        opt->client.depth = 16;
        opt->client.bigendian = 0;
        opt->client.truecolour = 1;
        opt->client.redmax = 31;
        opt->client.greenmax = 63;
        opt->client.bluemax = 31;
        opt->client.redshift = 11;
        opt->client.greenshift = 5;
        opt->client.blueshift = 0;
    }

private:
    uint32_t scaleX() const {
        return max<uint32_t>(1, M5.Display.width() / VNC_CLIENT_WIDTH);
    }

    uint32_t scaleY() const {
        return max<uint32_t>(1, M5.Display.height() / VNC_CLIENT_HEIGHT);
    }

    uint32_t toDisplayX(uint32_t x) const {
        return x * scaleX();
    }

    uint32_t toDisplayY(uint32_t y) const {
        return y * scaleY();
    }

    uint32_t scaleW(uint32_t w) const {
        return max<uint32_t>(1, w * scaleX());
    }

    uint32_t scaleH(uint32_t h) const {
        return max<uint32_t>(1, h * scaleY());
    }

    void drawRgb565Block(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *src, bool flushToDisplay = true) {
        const uint32_t sx = scaleX();
        const uint32_t sy = scaleY();
        static uint16_t batchBuffer[1280 * DISPLAY_BATCH_LINES];
        static uint16_t scaledLine[1280];

        if (sx == 1 && sy == 1) {
            uint32_t row = 0;
            if (flushToDisplay) {
                M5.Display.startWrite();
            }
            while (row < h) {
                const uint32_t rows = min<uint32_t>(h - row, DISPLAY_BATCH_LINES);
                convertBlock(src + row * w, batchBuffer, w * rows);
                writeFramebuffer(x, y + row, w, rows, batchBuffer, w);
                if (flushToDisplay) {
                    M5.Display.pushImage(x, y + row, w, rows, batchBuffer);
                }
                row += rows;
            }
            if (flushToDisplay) {
                M5.Display.endWrite();
            }
            return;
        }

        const uint32_t outW = min<uint32_t>(w * sx, 1280);

        if (flushToDisplay) {
            M5.Display.startWrite();
        }
        for (uint32_t row = 0; row < h; ++row) {
            uint32_t out = 0;
            const uint16_t *srcLine = src + row * w;
            writeFramebufferLine(x, y + row, w, srcLine);
            if (!flushToDisplay) {
                continue;
            }
            for (uint32_t col = 0; col < w && out < outW; ++col) {
                const uint16_t color = convertColor(srcLine[col]);
                for (uint32_t repeat = 0; repeat < sx && out < outW; ++repeat) {
                    scaledLine[out++] = color;
                }
            }

            const uint32_t dy = toDisplayY(y + row);
            for (uint32_t repeatY = 0; repeatY < sy; ++repeatY) {
                M5.Display.pushImage(toDisplayX(x), dy + repeatY, outW, 1, scaledLine);
            }
        }
        if (flushToDisplay) {
            M5.Display.endWrite();
        }
    }

    void pushFramebufferBlock(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        if (framebuffer == nullptr) {
            return;
        }

        const uint32_t sx = scaleX();
        const uint32_t sy = scaleY();
        static uint16_t batchBuffer[1280 * DISPLAY_BATCH_LINES];
        static uint16_t scaledLine[1280];

        if (sx == 1 && sy == 1) {
            uint32_t row = 0;
            M5.Display.startWrite();
            while (row < h) {
                const uint32_t rows = min<uint32_t>(h - row, DISPLAY_BATCH_LINES);
                copyFramebufferRows(x, y + row, w, rows, batchBuffer);
                M5.Display.pushImage(x, y + row, w, rows, batchBuffer);
                row += rows;
            }
            M5.Display.endWrite();
            return;
        }

        M5.Display.startWrite();
        for (uint32_t row = 0; row < h; ++row) {
            const uint16_t *srcLine = framebuffer + (y + row) * VNC_CLIENT_WIDTH + x;
            const uint32_t outW = expandConvertedLine(srcLine, w, sx, scaledLine, 1280);
            const uint32_t dy = toDisplayY(y + row);
            for (uint32_t repeatY = 0; repeatY < sy; ++repeatY) {
                M5.Display.pushImage(toDisplayX(x), dy + repeatY, outW, 1, scaledLine);
            }
        }
        M5.Display.endWrite();
    }

    static uint16_t convertColor(uint16_t color) {
        return (color << 8) | (color >> 8);
    }

    static void convertLine(const uint16_t *src, uint16_t *dst, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            dst[i] = convertColor(src[i]);
        }
    }

    static void convertBlock(const uint16_t *src, uint16_t *dst, uint32_t count) {
        convertLine(src, dst, count);
    }

    static uint32_t expandConvertedLine(const uint16_t *src, uint32_t count, uint32_t scale, uint16_t *dst, uint32_t maxCount) {
        uint32_t out = 0;
        for (uint32_t col = 0; col < count && out < maxCount; ++col) {
            for (uint32_t repeat = 0; repeat < scale && out < maxCount; ++repeat) {
                dst[out++] = src[col];
            }
        }
        return out;
    }

    void writeFramebuffer(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *src, uint32_t srcStride) {
        if (framebuffer == nullptr || x >= VNC_CLIENT_WIDTH || y >= VNC_CLIENT_HEIGHT) {
            return;
        }

        w = min<uint32_t>(w, VNC_CLIENT_WIDTH - x);
        h = min<uint32_t>(h, VNC_CLIENT_HEIGHT - y);
        for (uint32_t row = 0; row < h; ++row) {
            memcpy(framebuffer + (y + row) * VNC_CLIENT_WIDTH + x,
                   src + row * srcStride,
                   w * sizeof(uint16_t));
        }
    }

    void writeFramebufferLine(uint32_t x, uint32_t y, uint32_t w, const uint16_t *src) {
        if (framebuffer == nullptr || x >= VNC_CLIENT_WIDTH || y >= VNC_CLIENT_HEIGHT) {
            return;
        }

        w = min<uint32_t>(w, VNC_CLIENT_WIDTH - x);
        uint16_t *dst = framebuffer + y * VNC_CLIENT_WIDTH + x;
        for (uint32_t i = 0; i < w; ++i) {
            dst[i] = convertColor(src[i]);
        }
    }

    void fillFramebuffer(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t color) {
        if (framebuffer == nullptr || x >= VNC_CLIENT_WIDTH || y >= VNC_CLIENT_HEIGHT) {
            return;
        }

        w = min<uint32_t>(w, VNC_CLIENT_WIDTH - x);
        h = min<uint32_t>(h, VNC_CLIENT_HEIGHT - y);
        for (uint32_t row = 0; row < h; ++row) {
            uint16_t *dst = framebuffer + (y + row) * VNC_CLIENT_WIDTH + x;
            for (uint32_t col = 0; col < w; ++col) {
                dst[col] = color;
            }
        }
    }

    void copyFramebufferRows(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t *dst) {
        for (uint32_t row = 0; row < h; ++row) {
            memcpy(dst + row * w,
                   framebuffer + (y + row) * VNC_CLIENT_WIDTH + x,
                   w * sizeof(uint16_t));
        }
    }

    uint32_t areaX = 0;
    uint32_t areaY = 0;
    uint32_t areaW = 0;
    uint32_t areaH = 0;
    uint32_t areaPixel = 0;
    bool deferAreaFlush = false;
    uint16_t *framebuffer = nullptr;
};

Tab5VNCDisplay display;
arduinoVNC vnc(&display);

void setupTab5WiFiPins() {
#if defined(BOARD_SDIO_ESP_HOSTED_CLK) && defined(BOARD_SDIO_ESP_HOSTED_CMD) && \
    defined(BOARD_SDIO_ESP_HOSTED_D0) && defined(BOARD_SDIO_ESP_HOSTED_D1) && \
    defined(BOARD_SDIO_ESP_HOSTED_D2) && defined(BOARD_SDIO_ESP_HOSTED_D3) && \
    defined(BOARD_SDIO_ESP_HOSTED_RESET)
    WiFi.setPins(BOARD_SDIO_ESP_HOSTED_CLK, BOARD_SDIO_ESP_HOSTED_CMD,
                 BOARD_SDIO_ESP_HOSTED_D0, BOARD_SDIO_ESP_HOSTED_D1,
                 BOARD_SDIO_ESP_HOSTED_D2, BOARD_SDIO_ESP_HOSTED_D3,
                 BOARD_SDIO_ESP_HOSTED_RESET);
#else
    WiFi.setPins(TAB5_WIFI_SDIO_CLK, TAB5_WIFI_SDIO_CMD,
                 TAB5_WIFI_SDIO_D0, TAB5_WIFI_SDIO_D1,
                 TAB5_WIFI_SDIO_D2, TAB5_WIFI_SDIO_D3,
                 TAB5_WIFI_SDIO_RST);
#endif
}

bool setupTab5Keyboard() {
    auto cfg = tab5Keyboard.config();
    cfg.mode = Mode::Normal;
    cfg.start_periodic = true;
    cfg.irq_pin = TAB5_KEYBOARD_IRQ;
    tab5Keyboard.config(cfg);

    Wire.end();
    Wire.begin(TAB5_KEYBOARD_SDA, TAB5_KEYBOARD_SCL, TAB5_KEYBOARD_I2C_CLOCK);

    if (!Units.add(tab5Keyboard, Wire) || !Units.begin()) {
        return false;
    }

    if (!tab5Keyboard.writeMode(Mode::HID)) {
        return false;
    }

    tab5Keyboard.flush();
    Serial.printf("[Keyboard] firmware=%02X\n", tab5Keyboard.firmwareVersion());
    return true;
}

bool connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    for (int retry = 0; retry < 40 && WiFi.status() != WL_CONNECTED; ++retry) {
        delay(250);
        Serial.printf("[WiFi] status=%d\n", static_cast<int>(WiFi.status()));
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] failed");
        return false;
    }

    Serial.printf("[WiFi] IP=%s\n", WiFi.localIP().toString().c_str());
    return true;
}

int hidToVncKey(uint8_t keycode, uint8_t modifier) {
    const char printable = hidUsageToChar(keycode, modifier);
    if (printable >= 0x20 && printable <= 0x7E) {
        return printable;
    }

    switch (keycode) {
    case HID_ENTER: return 0xFF0D;
    case HID_BACKSPACE: return 0xFF08;
    case HID_TAB: return 0xFF09;
    case HID_ESCAPE: return 0xFF1B;
    case HID_SPACE: return ' ';
    case HID_DELETE: return 0xFFFF;
    case HID_LEFT: return 0xFF51;
    case HID_UP: return 0xFF52;
    case HID_RIGHT: return 0xFF53;
    case HID_DOWN: return 0xFF54;
    default: return 0;
    }
}

void handleKeyboard() {
    while (!tab5Keyboard.empty()) {
        const auto evt = tab5Keyboard.oldest();
        tab5Keyboard.discard();

        const int key = hidToVncKey(evt.hid.keycode, evt.modifier);
        if (key == 0) {
            continue;
        }

        vnc.keyEvent(key, 1);
        vnc.keyEvent(key, 0);
    }
}

void handleTouch() {
    const auto touch = M5.Touch.getDetail();
    const uint32_t sx = max<uint32_t>(1, M5.Display.width() / VNC_CLIENT_WIDTH);
    const uint32_t sy = max<uint32_t>(1, M5.Display.height() / VNC_CLIENT_HEIGHT);

    if (touch.isPressed()) {
        if (!touchActive) {
            touchActive = true;
            touchMoved = false;
            lastTouchX = touch.x;
            lastTouchY = touch.y;
            touchStartX = touch.x;
            touchStartY = touch.y;
            touchStartMs = millis();
            return;
        }

        const int dx = touch.x - lastTouchX;
        const int dy = touch.y - lastTouchY;
        if (abs(dx) >= TOUCH_MOVE_DEADZONE || abs(dy) >= TOUCH_MOVE_DEADZONE) {
            lastTouchX = touch.x;
            lastTouchY = touch.y;
            touchMoved = true;
        }
    } else if (touch.wasReleased()) {
        if (!touchActive) {
            return;
        }

        const bool longPress = millis() - touchStartMs >= TOUCH_LONG_PRESS_MS;
        if (!touchMoved) {
            const uint16_t vncX = min<uint32_t>(VNC_CLIENT_WIDTH - 1, touchStartX / sx);
            const uint16_t vncY = min<uint32_t>(VNC_CLIENT_HEIGHT - 1, touchStartY / sy);
            const uint8_t button = longPress ? VNC_BUTTON_RIGHT : VNC_BUTTON_LEFT;
            vnc.mouseEvent(vncX, vncY, button);
            delay(40);
            vnc.mouseEvent(vncX, vncY, 0);
        }

        touchActive = false;
    }
}

void setupDisplay() {
    M5.Display.wakeup();
    M5.Display.setBrightness(180);
    M5.Display.setColorDepth(16);
    if (M5.Display.height() > M5.Display.width()) {
        M5.Display.setRotation(3);
    }
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(0, 0);
    M5.Display.println("Tab5 VNC test");
}

}  // namespace

void setup() {
    Serial.begin(115200);
    delay(100);

    auto cfg = M5.config();
    cfg.serial_baudrate = 115200;
    cfg.internal_mic = false;
    cfg.internal_spk = false;
    M5.begin(cfg);

    setupDisplay();
    display.beginFramebuffer();
    setupTab5WiFiPins();

    if (!setupTab5Keyboard()) {
        Serial.println("[Keyboard] init failed");
        M5.Display.println("Keyboard init failed");
    }

    if (!connectWiFi()) {
        M5.Display.println("WiFi failed");
        return;
    }

    vnc.setPassword(VNC_PASSWORD);
    vnc.setMaxFPS(20);
    vnc.begin(VNC_HOST, VNC_PORT, false);
}

void loop() {
    M5.update();
    Units.update();

    handleTouch();
    handleKeyboard();
    vnc.loop();

    delay(1);
}
