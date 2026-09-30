// Derived from atc1441/ATC_TLSR_Paper (https://github.com/atc1441/ATC_TLSR_Paper), MIT license:
// see firmware/tag/LICENSE-ATC_TLSR_Paper.
#include <stdint.h>
#include "tl_common.h"
#include "drivers.h"
#include "main.h"
#include "epd.h"
#include "epd_spi.h"

uint8_t epd_black[EPD_PLANE_SIZE];
uint8_t epd_red[EPD_PLANE_SIZE];

// Fast LUT (from ATC's partial LUT): black/white pixels get a short push. The red groups
// LUT2/LUT3 get a host-supplied phase-A drive (0x00 = undriven, 0xC0 = VSH2 "hold red")
// because undriven thin red strokes fade over repeated fast refreshes.
static uint8_t lut_fast[153] = {
    0x40, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // LUT0: black  (B=0 R=0) -> VSH
    0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // LUT1: white  (B=1 R=0) -> VSL
    0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // LUT2: red    -> untouched
    0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // LUT3: red    -> untouched
    0x00, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,  // LUT4: VCOM
    50, 0, 0, 0, 0, 0, 0,                   // group 0 timing: 50 frames phase A
    [144] = 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,  // frame rate
};

static void epd_power_up(void)
{
    EPD_init();
    EPD_POWER_ON();
    WaitMs(10);
    gpio_write(EPD_RESET, 0);
    WaitMs(10);
    gpio_write(EPD_RESET, 1);
    WaitMs(10);
}

_attribute_ram_code_ uint8_t epd_detect(void)
{
    epd_power_up();
    EPD_WriteCmd(0x12); // SW reset
    WaitMs(10);
    // The BWR 2.13" controller has a 153 byte LUT register that reads back
    EPD_WriteCmd(0x32);
    for (int i = 0; i < 153; i++)
        EPD_WriteData(0xA5);
    EPD_WriteCmd(0x33);
    uint8_t ok = 1;
    for (int i = 0; i < 153; i++)
        if (EPD_SPI_read() != 0xA5)
            ok = 0;
    EPD_POWER_OFF();
    return ok;
}

static void epd_set_ram_pos(void)
{
    EPD_WriteCmd(0x4E);
    EPD_WriteData(0x00);
    EPD_WriteCmd(0x4F);
    EPD_WriteData(0x28);
    EPD_WriteData(0x01);
}

uint16_t epd_led_level; // green "refreshing" LED duty in 0.1 % units (0..1000)

// Show an LED (LED_GREEN / LED_RED) at level (0.1 % units, 0..1000) for ms milliseconds
// (1 kHz software PWM, active low: 1 unit = 1 us on-time).
void led_pwm(uint16_t level, int ms, uint32_t pin)
{
    int on_us = level > 1000 ? 1000 : level;
    for (int t = 0; t < ms; t++) {
        if (on_us) { gpio_write(pin, 0); sleep_us(on_us); }
        gpio_write(pin, 1);
        if (on_us < 1000) sleep_us(1000 - on_us);
        wd_clear();
    }
}

// Busy pin is high while the controller works. Meanwhile PWM the green LED (active low)
// at 1 kHz with epd_led_level duty.
static void epd_wait_idle(int max_ms)
{
    int on_us = epd_led_level > 1000 ? 1000 : epd_led_level;
    WaitMs(1);
    for (int t = 0; t < max_ms && gpio_read(EPD_BUSY); t++) {
        if (on_us) { gpio_write(LED_GREEN, 0); sleep_us(on_us); }
        gpio_write(LED_GREEN, 1);
        if (on_us < 1000) sleep_us(1000 - on_us);
        wd_clear();
    }
    gpio_write(LED_GREEN, 1);
}

_attribute_ram_code_ uint8_t epd_refresh(uint8_t fast, uint8_t red_drive, uint8_t frames)
{
    epd_power_up();

    EPD_WriteCmd(0x12); // SW reset
    epd_wait_idle(100);

    EPD_WriteCmd(0x74); EPD_WriteData(0x54);                   // analog block control
    EPD_WriteCmd(0x7E); EPD_WriteData(0x3B);                   // digital block control
    EPD_WriteCmd(0x0C);                                        // booster soft start
    EPD_WriteData(0x8B); EPD_WriteData(0x9C); EPD_WriteData(0x96); EPD_WriteData(0x0F);
    EPD_WriteCmd(0x01);                                        // driver output: 296 gates
    EPD_WriteData(0x28); EPD_WriteData(0x01); EPD_WriteData(0x01);
    EPD_WriteCmd(0x11); EPD_WriteData(0x01);                   // data entry: X+, Y-
    EPD_WriteCmd(0x44); EPD_WriteData(0x00); EPD_WriteData(0x0F);
    EPD_WriteCmd(0x45);
    EPD_WriteData(0x28); EPD_WriteData(0x01); EPD_WriteData(0x2E); EPD_WriteData(0x00);
    EPD_WriteCmd(0x3C); EPD_WriteData(0x05);                   // border waveform
    EPD_WriteCmd(0x21); EPD_WriteData(0x00); EPD_WriteData(0x80); // red RAM normal
    EPD_WriteCmd(0x18); EPD_WriteData(0x80);                   // internal temp sensor

    EPD_WriteCmd(0x22); EPD_WriteData(0xB1);                   // load temperature + LUT
    EPD_WriteCmd(0x20);
    epd_wait_idle(100);

    EPD_WriteCmd(0x1B);
    uint8_t temp = EPD_SPI_read();
    EPD_SPI_read();
    WaitMs(5);

    epd_set_ram_pos();
    EPD_LoadImage(epd_black, EPD_PLANE_SIZE, 0x24);
    epd_set_ram_pos();
    EPD_LoadImage(epd_red, EPD_PLANE_SIZE, 0x26);

    if (fast) {
        lut_fast[24] = lut_fast[36] = red_drive; // LUT2/LUT3 phase A
        lut_fast[60] = frames ? frames : 50;    // group 0 phase A length
        EPD_WriteCmd(0x32);
        for (int i = 0; i < sizeof(lut_fast); i++)
            EPD_WriteData(lut_fast[i]);
    }
    EPD_WriteCmd(0x22); EPD_WriteData(0xC7);                   // update with the current LUT
    EPD_WriteCmd(0x20);
    epd_wait_idle(30000);

    EPD_WriteCmd(0x10); EPD_WriteData(0x01);                   // deep sleep
    EPD_POWER_OFF();
    return temp;
}
