/*
 * DM-32 flash reader and writer, uploaded by tools/backup/dm32_dump.py to the RAM (0x10000) through the HR_C7000's
 * internal bootloader (the handshake of Roger VK3KYY's C7000_read_*.py, the radio switched on with the cable in)
 * and started there. It does what the stub of those scripts does (AndyNVKZ's, uploaded to the same place: the watchdog
 * off, GPIO 18 as the 25Q128's chip select, SPI0, the UART polled), but streams 4 KB blocks with a CRC each at a
 * speed the PC raises: those scripts ask 256 bytes at a time at 115200 baud, about 28 min for the 25Q128.
 *
 * The clocks are left as the bootloader has them: the PC knows the divisor of 115200 from the hello and scales it.
 * No library calls (-nostdlib); the stack below 0x50000, as the stub of those scripts has it.
 *
 * Requests from the PC, big endian, each answered:
 *   'H'                          -> "DMP1" divisor(2) (also sent once at the start): the UART divisor in use
 *   'S' divisor(2)               -> 'S' (still at the old speed), then the new divisor: 'H' -> the hello, then
 *                                   'Y' -> 'Y' must follow at the new speed, each within a few seconds, else the
 *                                   old divisor is back (a speed the cable can't carry)
 *   'R' chip(1) addr(4) count(4) -> count blocks of 4 KB from addr, each 'D' data(4096) crc32(4) (zlib's CRC-32);
 *                                   chip 0: the 25Q128 (data, 16 MB), 1: the program flash (GD25Q80 / 25Q16)
 *   'I' chip(1)                  -> 'I' and the 3 bytes of the JEDEC ID (9F)
 *   'C' chip(1) addr(4) count(4) -> count crc32(4): the CRC-32 of each 4 KB sector from addr (to find what differs)
 *   'X' chip(1) addr(4) data(4096) crc32(4)
 *                                -> 'K': the sector at addr erased, written and read back the same; or 'E' error(1).
 *                                   As updater.c: 06 write enable, 20 sector erase, 02 page program, 05 status.
 *                                   The program flash below 0xC000 (the bootloader) only with chip | 0x80
 *   'P' done(2) total(2)         -> 'P' once drawn: the progress bar (the PC sends it between reads). The PC waits
 *                                   for the answer: bytes sent while the bar was drawn overran the receive FIFO
 *   'T' x(2) y(2) w(2) h(2) colour(2) bits(h * ((w + 7) / 8))
 *                                -> 'T' once drawn: a 1 bit picture (rows top first, the left pixel in a byte's high
 *                                   bit) in colour on black, at most 4 KB: the PC renders the text with its fonts
 *   'M' addr(4)                  -> 'M' value(4): a 32 bit read (registers, to look at the bootloader's setup)
 *   'W' addr(4) value(4)         -> 'W': a 32 bit write
 *
 * The screen: the bootloader leaves it off, so it is set up here as our firmware (src/) does (hal/gpio.c pins_init()'s
 * pin mux, under which the UART and SPI0 work as well; drivers/panel.c panel_init(); the backlight as a GPIO, on).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))

// The watchdog (HR_C7000 manual, WDT; src/hal/hw.h): 32 bit writes only
#define WDG_LOCK   REG(0x14010000)
#define WDG_EN     REG(0x14010010)
#define WDG_UNLOCK 0x5ADA7200

// GPIO A: the 25Q128's chip select is pin 18 (src/hal/hw.h PIN_SPI0_CS)
#define GPIOA_DR  REG(0x14020000)
#define GPIOA_DDR REG(0x14020004)
#define CS_BIT    (1u << 18)

// SPI0 (DesignWare SSI), the 25Q128 (src/drivers/spi0.c)
#define SPI0(offset) REG(0x140A0000 + (offset))
// The SSI of the program flash (tools/debug/updater/updater.c)
#define SSI(offset) REG(0x04000000 + (offset))
#define SSI_CTRLR0  0x00
#define SSI_CTRLR1  0x04
#define SSI_SSIENR  0x08
#define SSI_SER     0x10
#define SSI_BAUDR   0x14
#define SSI_TXFTLR  0x18
#define SSI_RXFTLR  0x1C
#define SSI_SR      0x28
#define SSI_IMR     0x2C
#define SSI_DMACR   0x4C
#define SSI_DR      0x60
#define SR_BUSY     0x01
#define SR_TFNF     0x02
#define SR_TFE      0x04

// The pin mux (1: GPIO), the clock gates, GPIO C (src/hal/hw.h)
#define IO_DIPLEX0      REG(0x11000034)
#define IO_DIPLEX1      REG(0x11000038)
#define IO_DIPLEX2      REG(0x1100003C)
#define CLK_MGR_REG0x2c REG(0x1100002C)
#define GPIOC_DR        REG(0x14110000)
#define GPIOC_DDR       REG(0x14110004)
#define BACKLIGHT_BIT   (1u << 15) // GPIO A 15 (PIN_LCD_BKLIGHT), IO_DIPLEX0 bit 17 as GPIO
#define LCD_NRESET_BIT  (1u << 2)  // GPIO C 2 (pin 0x42)
#define LCD_RD_BIT      (1u << 6)  // GPIO C 6 (pin 0x46)

// The LCD (ST7789, 240 x 320, the I8080 8 bit bus)
#define I8080_CMD        REG(0x12000000)
#define I8080_DATA       REG(0x12000004)
#define I8080_TIMING     REG(0x12000010)
#define I8080_CTRL       REG(0x12000028)
#define PANEL_WIDTH      240
#define PANEL_HEIGHT     320
#define PANEL_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define UART0(offset) REG(0x14030000 + (offset))
#define UART_RBR      0x00
#define UART_THR      0x00
#define UART_DLL      0x00
#define UART_DLH      0x04
#define UART_LCR      0x0C
#define UART_LSR      0x14
#define UART_USR      0x7C
#define LSR_TEMT      0x40
#define USR_BUSY      0x01

#define BLOCK      4096
#define BYTE_POLLS 10000000 // UART polls before a request cut short is dropped (a few seconds)

#define CHIP_DATA    0
#define CHIP_PROGRAM 1

static uint8_t block[BLOCK];

// A pause of at least ms milliseconds: the bootloader runs the CPU on the 24 MHz crystal (its UART divisor is 13 for
// 115200, CLK_MGR_REG0x04 has the PLL bypassed), about 10 cycles a turn of this loop
static void pause_ms(uint32_t ms)
{
    for (volatile uint32_t n = ms * 2400; n > 0; n--) {
    }
}

// ---- the screen ----

static void lcd_pause(void) // the LCD needs a short time between the bytes of a command (panel.c)
{
    for (volatile int x = 0; x < 1; x++) {
    }
}

static void lcd_cmd(uint8_t cmd, const uint8_t *data, int length)
{
    I8080_CMD = cmd;
    lcd_pause();
    for (int i = 0; i < length; i++) {
        I8080_DATA = data[i];
        lcd_pause();
    }
}

#define CMD(c, ...)                                                                                                    \
    do {                                                                                                               \
        static const uint8_t d[] = {__VA_ARGS__};                                                                      \
        lcd_cmd((c), d, sizeof(d));                                                                                    \
    } while (0)

static void lcd_fill(int x0, int y0, int x1, int y1, uint16_t colour) // [x0, x1) x [y0, y1)
{
    uint8_t caset[4] = {x0 >> 8, x0 & 0xFF, (x1 - 1) >> 8, (x1 - 1) & 0xFF};
    uint8_t raset[4] = {y0 >> 8, y0 & 0xFF, (y1 - 1) >> 8, (y1 - 1) & 0xFF};

    lcd_cmd(0x2A, caset, 4);
    lcd_cmd(0x2B, raset, 4);
    I8080_CMD = 0x2C;
    for (int n = (x1 - x0) * (y1 - y0); n > 0; n--) {
        I8080_DATA = colour >> 8;
        I8080_DATA = colour & 0xFF;
    }
}

#define BAR_X0 20
#define BAR_X1 220
#define BAR_Y0 190
#define BAR_Y1 214
#define WHITE  0xFFFF
#define BLACK  0x0000
#define BLUE   PANEL_RGB(255, 120, 0) // the panel takes BGR (0x36 = 0xC8): PANEL_RGB's red is shown as blue

// The bar (the percentage above it is text from the PC)
static void show_progress(uint32_t done, uint32_t total)
{
    uint32_t part = (done > total) ? total : done;
    int filled = BAR_X0 + 2 + (int)(((BAR_X1 - BAR_X0 - 4) * part) / ((total == 0) ? 1 : total));
    lcd_fill(BAR_X0 + 2, BAR_Y0 + 2, filled, BAR_Y1 - 2, BLUE);
    lcd_fill(filled, BAR_Y0 + 2, BAR_X1 - 2, BAR_Y1 - 2, BLACK);
}

static int uart_get(bool wait);
static bool uart_get_word(uint32_t *v);
static void uart_put(uint8_t c);

// 'T': the whole picture first into the block buffer (at most 240 x 136 pixels), then drawn, then 'T': drawing while
// it came in, the UART's receive FIFO overran and the requests after it were read as picture bytes
static void handle_picture(void)
{
    uint32_t xy, wh;
    int c1, c2;

    if (!uart_get_word(&xy) || !uart_get_word(&wh) || ((c1 = uart_get(false)) < 0) || ((c2 = uart_get(false)) < 0)) {
        return;
    }
    uint16_t colour = (uint16_t)((c1 << 8) | c2);
    int x = xy >> 16, y = xy & 0xFFFF, w = wh >> 16, h = wh & 0xFFFF;
    uint32_t length = (uint32_t)h * (uint32_t)((w + 7) / 8);
    if ((w <= 0) || (h <= 0) || ((x + w) > PANEL_WIDTH) || ((y + h) > PANEL_HEIGHT) || (length > BLOCK)) {
        return;
    }
    for (uint32_t i = 0; i < length; i++) {
        int c = uart_get(false);
        if (c < 0) {
            return;
        }
        block[i] = (uint8_t)c;
    }

    uint8_t caset[4] = {x >> 8, x & 0xFF, (x + w - 1) >> 8, (x + w - 1) & 0xFF};
    uint8_t raset[4] = {y >> 8, y & 0xFF, (y + h - 1) >> 8, (y + h - 1) & 0xFF};
    const uint8_t *bits = block;

    lcd_cmd(0x2A, caset, 4);
    lcd_cmd(0x2B, raset, 4);
    I8080_CMD = 0x2C;
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col += 8, bits++) {
            for (int k = 0; (k < 8) && ((col + k) < w); k++) {
                uint16_t pixel = (*bits & (0x80 >> k)) ? colour : BLACK;
                I8080_DATA = pixel >> 8;
                I8080_DATA = pixel & 0xFF;
            }
        }
    }
    uart_put('T');
}

// The pins, the LCD controller and the panel as our firmware (src/) sets them up, then the empty bar
static void screen_init(void)
{
    CLK_MGR_REG0x2c = 0xFFFFFFFF; // the clocks on, as clock_init()
    IO_DIPLEX0 = 0xFFFDFD80 | (1u << 17);
    IO_DIPLEX1 = 0xFFFFFE07 | 0x38000000;
    IO_DIPLEX2 = 0x0FC80100 & 0xFFF80107; // the LCD pins to the I8080 interface, RD a GPIO

    GPIOC_DR |= LCD_NRESET_BIT | LCD_RD_BIT;
    GPIOC_DDR |= LCD_NRESET_BIT | LCD_RD_BIT;
    I8080_TIMING = 0x303;
    I8080_CTRL |= 1;
    for (uint32_t n = 0; ((I8080_CTRL & 0x02) == 0) && (n < 1000000); n++) {
    }

    // The ST7789's waits as its manual has them (panel.c's 10 ms are enough after the firmware's start, not here: the
    // panel stayed dark; a software reset and 150 ms waits, tried over the cable with 'W', showed the picture)
    pause_ms(1);
    GPIOC_DR &= ~LCD_NRESET_BIT;
    pause_ms(1);
    GPIOC_DR |= LCD_NRESET_BIT;
    pause_ms(150);
    I8080_CMD = 0x01; // software reset
    pause_ms(150);
    I8080_CMD = 0x11; // sleep out
    pause_ms(150);
    CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33);
    CMD(0x35, 0x00);
    CMD(0x36, 0xC8);
    CMD(0x3A, 0x05);
    CMD(0xB7, 0x56);
    CMD(0xBB, 0x17);
    CMD(0xC0, 0x2C);
    CMD(0xC2, 0x01);
    CMD(0xC3, 0x0B);
    CMD(0xC6, 0xEF);
    CMD(0xD0, 0xA7);
    CMD(0xD0, 0xA4, 0xA1);
    CMD(0xD6, 0xA1);
    CMD(0xE0, 0x70, 0x03, 0x08, 0x07, 0x08, 0x15, 0x30, 0x54, 0x47, 0x3A, 0x15, 0x14, 0x2B, 0x2E);
    CMD(0xE1, 0x70, 0x06, 0x0A, 0x09, 0x06, 0x03, 0x2F, 0x44, 0x47, 0x38, 0x12, 0x15, 0x29, 0x2F);
    I8080_CMD = 0x21; // display inversion on (the panel needs it)

    lcd_fill(0, 0, PANEL_WIDTH, PANEL_HEIGHT, BLACK);
    lcd_fill(BAR_X0, BAR_Y0, BAR_X1, BAR_Y1, WHITE);
    show_progress(0, 1);
    I8080_CMD = 0x29; // display on
    GPIOA_DR |= BACKLIGHT_BIT;
    GPIOA_DDR |= BACKLIGHT_BIT;
}

// ---- UART0, polled ----

// LSR TEMT: the transmitter empty, each byte sent before the next. With USR TFNF (updater.c) 7 bytes of a 4 KB block
// went missing at 375000 baud; LSR THRE hung: the bootloader leaves the programmable THRE mode on, where it means full
static void uart_put(uint8_t c)
{
    while ((UART0(UART_LSR) & LSR_TEMT) == 0) {
    }
    UART0(UART_THR) = c;
}

static void uart_put_word(uint32_t v)
{
    uart_put(v >> 24);
    uart_put(v >> 16);
    uart_put(v >> 8);
    uart_put(v);
}

// A byte, or -1 after BYTE_POLLS empty polls (wait: no limit)
static int uart_get(bool wait)
{
    for (uint32_t n = 0; wait || (n < BYTE_POLLS); n++) {
        if (UART0(UART_LSR) & 0x01) {
            return UART0(UART_RBR) & 0xFF;
        }
    }
    return -1;
}

static bool uart_get_word(uint32_t *v)
{
    uint32_t w = 0;

    for (int i = 0; i < 4; i++) {
        int c = uart_get(false);
        if (c < 0) {
            return false;
        }
        w = (w << 8) | (uint32_t)c;
    }
    *v = w;
    return true;
}

static uint32_t uart_divisor(void)
{
    UART0(UART_LCR) |= 0x80; // DLAB
    uint32_t divisor = (UART0(UART_DLL) & 0xFF) | ((UART0(UART_DLH) & 0xFF) << 8);
    UART0(UART_LCR) &= ~0x80;
    return divisor;
}

static void uart_set_divisor(uint32_t divisor)
{
    while ((UART0(UART_LSR) & LSR_TEMT) == 0) {
    }
    while (UART0(UART_USR) & USR_BUSY) { // LCR can't change while busy
        (void)UART0(UART_RBR);
    }
    UART0(UART_LCR) |= 0x80;
    UART0(UART_DLL) = divisor & 0xFF;
    UART0(UART_DLH) = (divisor >> 8) & 0xFF;
    UART0(UART_LCR) &= ~0x80;
}

// ---- the two flashes ----

static void spi0_init(void)
{
    SPI0(SSI_SSIENR) = 0;
    SPI0(SSI_CTRLR0) = 0x07; // 8 bit frames, mode 0
    SPI0(SSI_CTRLR1) = 0;
    SPI0(SSI_BAUDR) = 2;
    SPI0(SSI_TXFTLR) = 0;
    SPI0(SSI_RXFTLR) = 0;
    SPI0(SSI_IMR) = 0;
    SPI0(SSI_DMACR) = 0;
    SPI0(SSI_SER) = 0;
    SPI0(SSI_SSIENR) = 1;
    GPIOA_DR |= CS_BIT;
    GPIOA_DDR |= CS_BIT;
}

// The program flash's SSI as plain SPI, as updater.c (8 bit frames in DFS_32 and DFS)
static void ssi_init(void)
{
    SSI(SSI_SSIENR) = 0;
    SSI(SSI_CTRLR0) = 0x00070007;
    SSI(SSI_CTRLR1) = 7;
    SSI(SSI_BAUDR) = 4;
    SSI(SSI_TXFTLR) = 7;
    SSI(SSI_RXFTLR) = 7;
    SSI(SSI_IMR) = 0;
    SSI(SSI_DMACR) = 0;
    SSI(SSI_SER) = 0;
    SSI(SSI_SSIENR) = 1;
}

static uint8_t transfer(uint32_t base, uint8_t out)
{
    while ((REG(base + SSI_SR) & SR_TFNF) == 0) {
    }
    REG(base + SSI_DR) = out;
    while ((REG(base + SSI_SR) & SR_TFE) == 0) {
    }
    while (REG(base + SSI_SR) & SR_BUSY) {
    }
    return (uint8_t)REG(base + SSI_DR);
}

// A command and its reply on one chip: the 25Q128's select is the GPIO (and SER), the program flash's SER only
static void command(int chip, const uint8_t *cmd, uint32_t cmd_length, uint8_t *reply, uint32_t reply_length)
{
    uint32_t base = (chip == CHIP_DATA) ? 0x140A0000 : 0x04000000;

    if (chip == CHIP_DATA) {
        GPIOA_DR &= ~CS_BIT;
    }
    REG(base + SSI_SER) = 1;
    for (uint32_t i = 0; i < cmd_length; i++) {
        (void)transfer(base, cmd[i]);
    }
    for (uint32_t i = 0; i < reply_length; i++) {
        reply[i] = transfer(base, 0xFF);
    }
    REG(base + SSI_SER) = 0;
    if (chip == CHIP_DATA) {
        GPIOA_DR |= CS_BIT;
    }
}

static uint32_t crc32_add(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int k = 0; k < 8; k++) {
        crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return crc;
}

// ---- writing, as updater.c ----

#define SECTOR_ERASE 0x20
#define PAGE_PROGRAM 0x02
#define PAGE         256
#define BUSY_POLLS   2000000 // status reads before giving up (a sector erase takes up to 0.4 s)
#define BOOT_AREA    0xC000  // the program flash below it: the bootloader, written only when asked
#define ERR_CRC      1       // the sector came in damaged
#define ERR_ADDRESS  2
#define ERR_VERIFY   3       // read back different
#define ERR_FLASH    4       // the flash stayed busy
#define ERR_TIMEOUT  5       // the request was cut short

// A command, its address (none: -1) and data out, as one selection of the chip
static void command_out(int chip, uint8_t code, int32_t address, const uint8_t *data, uint32_t length)
{
    uint8_t cmd[4] = {code, (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address};
    uint32_t base = (chip == CHIP_DATA) ? 0x140A0000 : 0x04000000;

    if (chip == CHIP_DATA) {
        GPIOA_DR &= ~CS_BIT;
    }
    REG(base + SSI_SER) = 1;
    for (uint32_t i = 0; i < ((address < 0) ? 1u : 4u); i++) {
        (void)transfer(base, cmd[i]);
    }
    for (uint32_t i = 0; i < length; i++) {
        (void)transfer(base, data[i]);
    }
    REG(base + SSI_SER) = 0;
    if (chip == CHIP_DATA) {
        GPIOA_DR |= CS_BIT;
    }
}

static bool flash_wait(int chip)
{
    for (uint32_t n = 0; n < BUSY_POLLS; n++) {
        uint8_t cmd = 0x05, status; // read status register 1
        command(chip, &cmd, 1, &status, 1);
        if ((status & 0x01) == 0) {
            return true;
        }
    }
    return false;
}

// The CRC-32 of a sector read from the flash into the block buffer
static uint32_t sector_crc(int chip, uint32_t address)
{
    uint8_t cmd[4] = {0x03, (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address};
    uint32_t crc = 0xFFFFFFFF;

    command(chip, cmd, sizeof(cmd), block, BLOCK);
    for (uint32_t i = 0; i < BLOCK; i++) {
        crc = crc32_add(crc, block[i]);
    }
    return ~crc;
}

// data: the sector to write; it is erased, written page by page and read back (its CRC against the data's)
static int write_sector(int chip, uint32_t address, const uint8_t *data, uint32_t crc)
{
    if (!flash_wait(chip)) {
        return ERR_FLASH;
    }
    command_out(chip, 0x06, -1, NULL, 0); // write enable
    command_out(chip, SECTOR_ERASE, (int32_t)address, NULL, 0);
    for (uint32_t page = 0; page < BLOCK; page += PAGE) {
        if (!flash_wait(chip)) {
            return ERR_FLASH;
        }
        command_out(chip, 0x06, -1, NULL, 0);
        command_out(chip, PAGE_PROGRAM, (int32_t)(address + page), data + page, PAGE);
    }
    if (!flash_wait(chip)) {
        return ERR_FLASH;
    }
    return (sector_crc(chip, address) == crc) ? 0 : ERR_VERIFY;
}

// ---- the requests ----

static void hello(void)
{
    uint32_t divisor = uart_divisor();

    uart_put('D');
    uart_put('M');
    uart_put('P');
    uart_put('1');
    uart_put(divisor >> 8);
    uart_put(divisor);
}

static void handle_crc(void)
{
    int chip = uart_get(false);
    uint32_t address, count;

    if ((chip < 0) || !uart_get_word(&address) || !uart_get_word(&count)) {
        return;
    }
    for (uint32_t n = 0; n < count; n++, address += BLOCK) {
        uart_put_word(sector_crc(chip & 1, address));
    }
}

static void handle_write(void)
{
    int chip = uart_get(false);
    uint32_t address = 0, crc_sent = 0, crc = 0xFFFFFFFF;
    int error = 0;

    if ((chip < 0) || !uart_get_word(&address)) {
        error = ERR_TIMEOUT;
    }
    for (uint32_t i = 0; (error == 0) && (i < BLOCK); i++) {
        int c = uart_get(false);
        if (c < 0) {
            error = ERR_TIMEOUT;
        } else {
            block[i] = (uint8_t)c;
            crc = crc32_add(crc, (uint8_t)c);
        }
    }
    if ((error == 0) && !uart_get_word(&crc_sent)) {
        error = ERR_TIMEOUT;
    }
    if ((error == 0) && (~crc != crc_sent)) {
        error = ERR_CRC;
    }
    if ((error == 0) && (((address % BLOCK) != 0) ||
                         (((chip & 1) == CHIP_PROGRAM) && (address < BOOT_AREA) && !(chip & 0x80)))) {
        error = ERR_ADDRESS;
    }
    if (error == 0) {
        error = write_sector(chip & 1, address, block, crc_sent);
    }
    if (error == 0) {
        uart_put('K');
    } else {
        uart_put('E');
        uart_put((uint8_t)error);
    }
}

static void handle_read(void)
{
    int chip = uart_get(false);
    uint32_t address, count;

    if ((chip < 0) || !uart_get_word(&address) || !uart_get_word(&count)) {
        return;
    }
    for (uint32_t n = 0; n < count; n++, address += BLOCK) {
        uint8_t cmd[4] = {0x03, (uint8_t)(address >> 16), (uint8_t)(address >> 8), (uint8_t)address};
        uint32_t crc = 0xFFFFFFFF;

        command(chip, cmd, sizeof(cmd), block, BLOCK);
        uart_put('D');
        for (uint32_t i = 0; i < BLOCK; i++) {
            uart_put(block[i]);
            crc = crc32_add(crc, block[i]);
        }
        uart_put_word(~crc);
    }
}

static void handle_speed(void)
{
    uint32_t old = uart_divisor();
    int hi = uart_get(false), lo = uart_get(false);

    if ((hi < 0) || (lo < 0) || (((hi << 8) | lo) == 0)) {
        return;
    }
    uart_put('S');
    uart_set_divisor((uint32_t)((hi << 8) | lo));
    bool both_ways = false; // the PC sends 'Y' only after a sound hello
    if (uart_get(false) == 'H') {
        hello();
        both_ways = (uart_get(false) == 'Y');
    }
    if (both_ways) {
        uart_put('Y');
    } else {
        uart_set_divisor(old);
    }
}

void __attribute__((noreturn)) dumper_main(void)
{
    WDG_LOCK = WDG_UNLOCK;
    WDG_EN = 0;
    WDG_LOCK = 0;
    screen_init();
    spi0_init();
    ssi_init();
    hello();

    for (;;) {
        int request = uart_get(true);

        // a stray byte is ignored
        if (request == 'H') {
            hello();
        } else if (request == 'S') {
            handle_speed();
        } else if (request == 'R') {
            handle_read();
        } else if (request == 'C') {
            handle_crc();
        } else if (request == 'X') {
            handle_write();
        } else if (request == 'T') {
            handle_picture();
        } else if (request == 'M') {
            uint32_t address;
            if (uart_get_word(&address)) {
                uart_put('M');
                uart_put_word(REG(address));
            }
        } else if (request == 'W') {
            uint32_t address, value;
            if (uart_get_word(&address) && uart_get_word(&value)) {
                REG(address) = value;
                uart_put('W');
            }
        } else if (request == 'P') {
            uint32_t word;
            if (uart_get_word(&word)) {
                show_progress(word >> 16, word & 0xFFFF);
                uart_put('P');
            }
        } else if (request == 'I') {
            int chip = uart_get(false);
            uint8_t cmd = 0x9F, id[3];
            if (chip >= 0) {
                command(chip, &cmd, 1, id, sizeof(id));
                uart_put('I');
                uart_put(id[0]);
                uart_put(id[1]);
                uart_put(id[2]);
            }
        }
    }
}

// The entry, first in the image (dumper.ld): the interrupts off, the stack, then C
void __attribute__((naked, section(".text.entry"))) dumper_entry(void)
{
    __asm__ volatile("psrclr ie\n"
                     "lrw r14, 0x50000\n"
                     "br dumper_main\n");
}
