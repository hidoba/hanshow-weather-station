// Derived from atc1441/ATC_TLSR_Paper (https://github.com/atc1441/ATC_TLSR_Paper), MIT license:
// see firmware/tag/LICENSE-ATC_TLSR_Paper.
// Hanshow Stellar (TLSR8359) serial display firmware.
// Receives black/red framebuffers over UART (PA0 RX / PB1 TX, 115200 8N1) and refreshes the panel.
//
// Frame: A5 5A | cmd | len (u16 LE) | payload | crc16-ccitt (u16 LE, over cmd..payload)
//   'P'                          ping             -> "OK P <panel> v<fw> n=<refreshes since boot>"
//   'F' plane value              fill plane       -> "OK F"
//   'W' plane off(u16) data...   write into plane -> "OK W"
//   'D' [mode [red frames [led16]]] refresh panel  -> "OK D <temp>" when done; mode 0/none = full (~13 s),
//                                                    1 = fast non-flashing update (~1.5 s), red = LUT byte
//                                                    driving red pixels, frames = phase length (default 50),
//                                                    led16 = green LED brightness during the refresh,
//                                                    u16 in 0.1 % units (default 500 = 50 %)
//   'L' level(u16) ms(u16) [col] LED             -> "OK L" after showing the LED (col 0 green, 1 red)
//                                                    at level (0.1 %) for ms
//   'O' size(u32) crc32(u32)     start fw update  -> "OK O <bank>" (erases the idle flash bank)
//   'B' off(u32) data(<=256)     fw block         -> "OK B" (256-byte aligned pages)
//   'E'                          finish update    -> "OK E" then reboots into the new firmware if the CRC matches
// plane: 0 = black (bit 0 = black), 1 = red (bit 1 = red)
#include <stdint.h>
#include "tl_common.h"
#include "drivers.h"
#include "main.h"
#include "epd.h"
#include "ota.h"

#define FW_VERSION "12"
#define MAX_PAYLOAD 600
#define IDLE_SLEEP_MS 300 // suspend after this long without serial traffic
#ifndef BOOT_REFRESH
#define BOOT_REFRESH 0
#endif

static uint8_t panel_ok;
static int refresh_count; // lets the host notice a reboot (planes lost)
static int sleep_count;   // times the tag suspended (diagnostics)
static uint8_t rx_idx, tx_idx;

_attribute_ram_code_ void irq_handler(void)
{
}

static void uart_setup(void)
{
    gpio_set_func(RXD, AS_GPIO);
    gpio_set_output_en(RXD, 0);
    gpio_set_input_en(RXD, 1);
    gpio_setup_up_down_resistor(RXD, PM_PIN_PULLUP_10K);
    uart_gpio_set(UART_TX_PB1, UART_RX_PA0);
    uart_reset();
    uart_init(12, 15, PARITY_NONE, STOP_BIT_ONE); // 24 MHz / (13 * 16) = 115384 baud
    uart_dma_enable(0, 0);
    dma_chn_irq_enable(0, 0);
    uart_irq_enable(0, 0);
    uart_ndma_irq_triglevel(0, 0);
    rx_idx = tx_idx = 0; // the block reset rewinds both hardware FIFO pointers
}

// Own TX index instead of uart_ndma_send_byte(): the SDK's hidden static index
// is not rewound by uart_reset(), which scrambles output after a recovery.
static void uart_putc(char c)
{
    while (uart_tx_is_busy())
        sleep_us(10);
    reg_uart_data_buf(tx_idx) = c;
    tx_idx = (tx_idx + 1) & 3;
    while (uart_tx_is_busy())
        sleep_us(10);
}

static void uart_puts(const char *s)
{
    while (*s)
        uart_putc(*s++);
}

static void uart_putdec(int v)
{
    char b[12];
    int n = 0;
    if (v < 0) { uart_putc('-'); v = -v; }
    do { b[n++] = '0' + v % 10; v /= 10; } while (v);
    while (n) uart_putc(b[--n]);
}

// Line noise (wrong baud, overrun) can leave the RX FIFO out of step with rx_idx:
// resetting the whole UART block is the only reliable way back.
static void uart_recover(void)
{
    uart_setup();
}

static int uart_getc(void)
{
    if (reg_uart_status0 & FLD_UART_RX_ERR_FLAG) {
        uart_recover();
        return -1;
    }
    if ((reg_uart_buf_cnt & FLD_UART_RX_BUF_CNT) == 0)
        return -1;
    uint8_t c = reg_uart_data_buf(rx_idx);
    rx_idx = (rx_idx + 1) & 3;
    return c;
}

// Software UART receiver on PA7 (SWS pad), 8N1 at 115200. System tick runs at 16 MHz.
#define SWS_BIT_TICKS_X2 (2 * 16000000 / 115200)
#define SWS_LOW() (!(read_reg8(0x580) & BIT(7)))

_attribute_ram_code_ static int sws_getc(void)
{
    u32 t0 = clock_time();
    uint8_t c = 0;
    for (int i = 0; i < 8; i++) {
        u32 at = t0 + (SWS_BIT_TICKS_X2 * (2 * i + 3)) / 4;
        while ((u32)(clock_time() - at) > 0x80000000u)
            ;
        c >>= 1;
        if (!SWS_LOW())
            c |= 0x80;
    }
    // wait into the stop bit so we don't retrigger on the last data bit
    u32 at = t0 + (SWS_BIT_TICKS_X2 * 19) / 4;
    while ((u32)(clock_time() - at) > 0x80000000u)
        ;
    return c;
}

static uint16_t crc16(const uint8_t *p, int n, uint16_t crc)
{
    while (n--) {
        crc ^= (uint16_t)(*p++) << 8;
        for (int i = 0; i < 8; i++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}

static void set_px(int x, int y, int color) // landscape 250x122, color: 0 white, 1 black, 2 red
{
    if (x < 0 || x >= 250 || y < 0 || y >= 122)
        return;
    int i = (y >> 3) + (249 - x) * EPD_ROW_BYTES;
    uint8_t m = 0x80 >> (y & 7);
    if (color == 1) epd_black[i] &= ~m; else epd_black[i] |= m;
    if (color == 2) epd_red[i] |= m; else epd_red[i] &= ~m;
}

static void boot_pattern(void)
{
    memset(epd_black, 0xFF, EPD_PLANE_SIZE);
    memset(epd_red, 0x00, EPD_PLANE_SIZE);
    for (int x = 0; x < 250; x++)
        for (int y = 0; y < 122; y++) {
            int c = 0;
            if (x < 3 || x > 246 || y < 3 || y > 118) c = 1;          // black frame
            else if (x >= 20 && x < 110 && y >= 20 && y < 100) c = 2; // red square, left
            else if (x >= 140 && x < 230 && y >= 20 && y < 100)       // black diagonal stripes, right
                c = ((x + y) / 8) & 1;
            set_px(x, y, c);
        }
}

static void reply(const char *s)
{
    uart_puts(s);
    uart_putc('\n');
}

static void handle(uint8_t cmd, uint8_t *p, int len)
{
    switch (cmd) {
    case 'P':
        uart_puts("OK P ");
        uart_puts(panel_ok ? "BWR213" : "UNKNOWN");
        uart_puts(" v" FW_VERSION " bank=");
        uart_putdec(ota_running_bank() ? 1 : 0);
        uart_puts(" n=");
        uart_putdec(refresh_count);
        uart_puts(" s=");
        uart_putdec(sleep_count);
        uart_putc('\n');
        break;
    case 'F':
        if (len != 2 || p[0] > 1) { reply("ER ARG"); break; }
        memset(p[0] ? epd_red : epd_black, p[1], EPD_PLANE_SIZE);
        reply("OK F");
        break;
    case 'W': {
        if (len < 4 || p[0] > 1) { reply("ER ARG"); break; }
        int off = p[1] | (p[2] << 8), n = len - 3;
        if (off + n > EPD_PLANE_SIZE) { reply("ER RANGE"); break; }
        memcpy((p[0] ? epd_red : epd_black) + off, p + 3, n);
        reply("OK W");
        break;
    }
    case 'D': {
        epd_led_level = len > 4 ? p[3] | (p[4] << 8) : 500;
        uint8_t t = epd_refresh(len > 0 && p[0] == 1, len > 1 ? p[1] : 0, len > 2 ? p[2] : 0);
        refresh_count++;
        uart_puts("OK D ");
        uart_putdec((int8_t)t);
        uart_putc('\n');
        break;
    }
    case 'L': // LED: level (u16, 0.1 %), duration ms (u16), [color 0 = green, 1 = red]
        if (len != 4 && len != 5) { reply("ER ARG"); break; }
        led_pwm(p[0] | (p[1] << 8), p[2] | (p[3] << 8), len == 5 && p[4] == 1 ? LED_RED : LED_GREEN);
        reply("OK L");
        break;
    case 'O': {
        if (len != 8) { reply("ER ARG"); break; }
        uint32_t size = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
        uint32_t crc = p[4] | p[5] << 8 | p[6] << 16 | (uint32_t)p[7] << 24;
        if (ota_begin(size, crc)) { reply("ER OTA"); break; }
        uart_puts("OK O ");
        uart_putdec(ota_running_bank() ? 0 : 0x20000);
        uart_putc('\n');
        break;
    }
    case 'B': {
        if (len < 5) { reply("ER ARG"); break; }
        uint32_t off = p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
        reply(ota_write(off, p + 4, len - 4) ? "ER OTA" : "OK B");
        break;
    }
    case 'E': {
        int r = ota_finish();
        if (r) { uart_puts("ER VERIFY "); uart_putdec(r); uart_putc('\n'); break; }
        reply("OK E");
        REG_ADDR8(0x6f) = 0x20; // reboot into the new bank
        while (1)
            ;
    }
    default:
        reply("ER CMD");
    }
}

// Suspend (RAM and panel state kept, ~1000x less current) until RXD or SWS goes low. The host
// sends a wake-up pulse before its first frame; bytes arriving during the wake-up are dropped.
static void tag_sleep(void)
{
    wd_stop(); // the watchdog would reset a sleeping tag
    sleep_count++;
    cpu_set_gpio_wakeup(RXD, Level_Low, 1);
    cpu_set_gpio_wakeup(GPIO_PA7, Level_Low, 1);
    cpu_sleep_wakeup(SUSPEND_MODE, PM_WAKEUP_PAD, 0);
    wd_clear();
    wd_start();
    uart_recover(); // discard the wake-up pulse
}

static uint8_t buf[MAX_PAYLOAD + 8];

_attribute_ram_code_ int main(void)
{
    blc_pm_select_internal_32k_crystal(); // sleep timing source (no 32k crystal on the tag)
    cpu_wakeup_init();
    u32 boot_tick = clock_time();
    gpio_init(1);
    clock_init(SYS_CLK_24M_Crystal);

    gpio_set_func(LED_GREEN, AS_GPIO);
    gpio_set_output_en(LED_GREEN, 1);
    gpio_write(LED_GREEN, 1); // LEDs are active low
    gpio_set_func(LED_RED, AS_GPIO);
    gpio_set_output_en(LED_RED, 1);
    gpio_write(LED_RED, 1);

    uart_setup();
    wd_set_interval_ms(4000, CLOCK_SYS_CLOCK_1MS); // reboot if the main loop ever stalls
    wd_start();
    uart_puts("\nHELLO hanshow-serial v" FW_VERSION "\n");

    panel_ok = epd_detect();
    uart_puts(panel_ok ? "panel BWR213\n" : "panel UNKNOWN\n");

    boot_pattern();
#if BOOT_REFRESH
    uint8_t t = epd_refresh(0, 0, 0);
#else
    uint8_t t = 0;
#endif
    uart_puts("boot refresh done, temp ");
    uart_putdec((int8_t)t);
    uart_putc('\n');

    // RXD is bridged to SWS (PA7) for flashing. Serial traffic would be decoded as SWire
    // commands (e.g. CPU stop), so detach SWS - but only after a window for the flasher.
    while (!clock_time_exceed(boot_tick, 1500 * 1000))
        wd_clear();
    gpio_set_func(GPIO_PA7, AS_GPIO);
    gpio_set_output_en(GPIO_PA7, 0);
    gpio_set_input_en(GPIO_PA7, 1);
    gpio_setup_up_down_resistor(GPIO_PA7, PM_PIN_PULLUP_10K); // don't float if nothing drives SWS
    uart_puts("READY\n");

    int state = 0, pos = 0, need = 0;
    u32 last = clock_time();
    while (1) {
        wd_clear();
        int c = SWS_LOW() ? sws_getc() : uart_getc();
        if (c < 0) {
            if (state && clock_time_exceed(last, 200 * 1000)) {
                state = 0; // inter-byte timeout, drop partial frame
                uart_recover();
            }
            if (!state && clock_time_exceed(last, IDLE_SLEEP_MS * 1000) && !SWS_LOW()) {
                tag_sleep();
                last = clock_time();
            }
            continue;
        }
        last = clock_time();
        switch (state) {
        case 0: if (c == 0xA5) state = 1; break;
        case 1: state = (c == 0x5A) ? 2 : (c == 0xA5 ? 1 : 0); pos = 0; break;
        case 2:
            buf[pos++] = c;
            if (pos == 3) {
                need = buf[1] | (buf[2] << 8);
                if (need > MAX_PAYLOAD) { reply("ER LEN"); state = 0; uart_recover(); break; }
                need += 5; // header(3) + crc(2)
            }
            if (pos >= 3 && pos == need) {
                uint16_t crc = buf[pos - 2] | (buf[pos - 1] << 8);
                if (crc16(buf, pos - 2, 0xFFFF) != crc) {
                    reply("ER CRC");
                    uart_recover();
                }
                else
                    handle(buf[0], buf + 3, pos - 5);
                state = 0;
                last = clock_time(); // stay awake a while after a command (refreshes take long)
            }
            break;
        }
    }
}
