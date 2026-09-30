#include <stdint.h>
#include "tl_common.h"
#include "drivers.h"
#include "ota.h"

#define BANK_SIZE 0x20000
#define BOOT_FLAG_OFF 8
#define BOOT_FLAG 0x4B

static uint32_t target, fw_size, fw_crc;
static uint8_t active;

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, int n)
{
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

static uint8_t flash_byte(uint32_t addr)
{
    uint8_t b;
    flash_read_page(addr, 1, &b);
    return b;
}

uint32_t ota_running_bank(void)
{
    return flash_byte(BOOT_FLAG_OFF) == BOOT_FLAG ? 0 : BANK_SIZE;
}

int ota_begin(uint32_t size, uint32_t crc)
{
    if (size < 16 || size > BANK_SIZE - 0x1000)
        return -1;
    target = ota_running_bank() ? 0 : BANK_SIZE;
    fw_size = size;
    fw_crc = crc;
    for (uint32_t a = 0; a < size; a += 0x1000) {
        flash_erase_sector(target + a);
        wd_clear();
    }
    active = 1;
    return 0;
}

int ota_write(uint32_t off, const uint8_t *p, int n)
{
    uint8_t buf[256];
    if (!active || n <= 0 || n > 256 || off + n > fw_size || (off & 0xFF) + n > 256)
        return -1;
    memcpy(buf, p, n);
    if (off <= BOOT_FLAG_OFF && BOOT_FLAG_OFF < off + n)
        buf[BOOT_FLAG_OFF - off] = 0xFF; // boot flag is written last, after verification
    flash_write_page(target + off, n, buf);
    return 0;
}

int ota_finish(void)
{
    uint8_t buf[256];
    if (!active)
        return -1;
    active = 0;
    uint32_t crc = 0;
    for (uint32_t a = 0; a < fw_size; a += sizeof buf) {
        int n = fw_size - a < sizeof buf ? fw_size - a : sizeof buf;
        flash_read_page(target + a, n, buf);
        if (a == 0) {
            if (buf[BOOT_FLAG_OFF] != 0xFF)
                return -2;
            buf[BOOT_FLAG_OFF] = BOOT_FLAG; // CRC is over the image as built
        }
        crc = crc32_update(crc, buf, n);
        wd_clear();
    }
    if (crc != fw_crc)
        return -3;
    uint8_t flag = BOOT_FLAG, zero = 0;
    flash_write_page(target + BOOT_FLAG_OFF, 1, &flag);                        // new bank bootable
    flash_write_page((target ? 0 : BANK_SIZE) + BOOT_FLAG_OFF, 1, &zero);      // old bank not
    return 0;
}
