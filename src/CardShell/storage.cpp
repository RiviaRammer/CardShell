#include "storage.h"

#include <Arduino.h>
#include "USB.h"
#include "USBMSC.h"
#include <LittleFS.h>
#include <string.h>

#include <M5Cardputer.h>

// ===========================================
// RAM Disk 定义（U 盘内容存在这里）
// 64 KB 示例
// ===========================================
static constexpr uint16_t MSC_BLOCK_SIZE  = 512;
static constexpr uint32_t MSC_BLOCK_COUNT = 128;
static uint8_t g_ramDisk[MSC_BLOCK_SIZE * MSC_BLOCK_COUNT];

// USB MSC 实例
static USBMSC g_msc;

// 盘是否已被 PC 弹出
static volatile bool g_diskEjected = false;
// 是否已经尝试导入（防止重复导入）
static bool g_importTried = false;


// ===========================================
// 工具：在内存中搜索子串
// ===========================================
static int32_t memsearch(const uint8_t *haystack, size_t haystack_len,
                         const char *needle, size_t needle_len)
{
    if (needle_len == 0 || haystack_len < needle_len)
        return -1;

    for (size_t i = 0; i <= haystack_len - needle_len; ++i) {
        if (memcmp(haystack + i, needle, needle_len) == 0) {
            return (int32_t)i;
        }
    }
    return -1;
}

// ===========================================
// 屏幕输出工具
// ===========================================
static void screenPrint(const String &s)
{
    M5Cardputer.Display.println(s);
    delay(5);
}


// ===========================================
// 导入 SSH Key（修复版）
// ===========================================
static void import_ssh_key_from_ramdisk()
{
    if (g_importTried) return;
    g_importTried = true;

    //--------------------------------
    // Mount FS
    //--------------------------------
    if (!LittleFS.begin(true)) {
        screenPrint("LittleFS mount failed!");
        Serial.println("[SSH-KEY] LittleFS mount failed");
        return;
    }

    const uint8_t *data = g_ramDisk;
    const size_t data_len = sizeof(g_ramDisk);

    const char *beginToken = "-----BEGIN RSA PRIVATE KEY-----";
    const char *endToken   = "-----END RSA PRIVATE KEY-----";

    const size_t beginLen = strlen(beginToken);
    const size_t endLen   = strlen(endToken);

    //--------------------------------
    // find BEGIN / END markers
    //--------------------------------
    int32_t begin_pos = memsearch(data, data_len, beginToken, beginLen);
    int32_t end_pos   = memsearch(data, data_len, endToken, endLen);

    if (begin_pos < 0 || end_pos < 0) {
        screenPrint("Key markers NOT found");
        Serial.println("[SSH-KEY] markers missing");
        return;
    }

    end_pos += endLen; // include END marker
    size_t key_len = end_pos - begin_pos;

    Serial.printf("[SSH-KEY] Key detected, length=%u bytes\n", key_len);

    //--------------------------------
    // Delete existing key
    //--------------------------------
    LittleFS.remove("/ssh_key");

    //--------------------------------
    // Write new key (single write)
    //--------------------------------
    File fw = LittleFS.open("/ssh_key", "w");
    if (!fw) {
        screenPrint("Open /ssh_key FAILED");
        Serial.println("[SSH-KEY] open fail");
        return;
    }

    size_t written = fw.write(data + begin_pos, key_len);

    fw.flush();
    delay(20); // safer for LittleFS
    fw.close();

    Serial.printf("[SSH-KEY] Written=%u bytes\n", written);

    if (written != key_len) {
        screenPrint("Write size mismatch!");
    }

    Serial.println("[SSH-KEY] Import DONE");
}





// ===========================================
// USB MSC 回调
// ===========================================
static int32_t cb_msc_read(uint32_t lba, uint32_t offset,
                           void *buffer, uint32_t bufsize)
{
    uint32_t addr = lba * MSC_BLOCK_SIZE + offset;

    if (addr >= sizeof(g_ramDisk)) return 0;
    if (addr + bufsize > sizeof(g_ramDisk))
        bufsize = sizeof(g_ramDisk) - addr;

    memcpy(buffer, g_ramDisk + addr, bufsize);
    return (int32_t)bufsize;
}


static int32_t cb_msc_write(uint32_t lba, uint32_t offset,
                            uint8_t *buffer, uint32_t bufsize)
{
    uint32_t addr = lba * MSC_BLOCK_SIZE + offset;

    if (addr >= sizeof(g_ramDisk)) return 0;
    if (addr + bufsize > sizeof(g_ramDisk))
        bufsize = sizeof(g_ramDisk) - addr;

    memcpy(g_ramDisk + addr, buffer, bufsize);
    return (int32_t)bufsize;
}


static bool cb_msc_startstop(uint8_t power_condition,
                             bool start, bool load_eject)
{
    if (!start && load_eject) {
        g_diskEjected = true;
        Serial.println("[MSC] Disk ejected by host.");
    }

    return true;
}


// ===========================================
// Public API
// ===========================================
void storage_mountMCU()
{
    memset(g_ramDisk, 0x00, sizeof(g_ramDisk));
    g_diskEjected = false;
    g_importTried = false;
}

void storage_mountPC()
{
    g_msc.vendorID("M5CARD");
    g_msc.productID("CardShell");
    g_msc.productRevision("1.0");

    g_msc.mediaPresent(true);
    g_msc.isWritable(true);

    g_msc.onStartStop(cb_msc_startstop);
    g_msc.onRead(cb_msc_read);
    g_msc.onWrite(cb_msc_write);

    if (!g_msc.begin(MSC_BLOCK_COUNT, MSC_BLOCK_SIZE)) {
        Serial.println("[USB MSC] begin() failed!");
    }

    USB.begin();
}

void storage_task()
{
    if (g_diskEjected && !g_importTried) {
        import_ssh_key_from_ramdisk();
    }
}
