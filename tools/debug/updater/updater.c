/*
 * DM-32 firmware update program, loaded over the cable into the RAM of the running firmware (src/
 * services/serial_update.c: the loader, after the radio is stopped and the interrupts are off) and started there by
 * tools/debug/fw_update.py. Linked once for the fixed address of that RAM (updater.ld; src/ckcpu.ld keeps
 * the update area, which the loader gives it, at 0x44400). Everything runs from that RAM, the UART is polled, no
 * library calls (-nostdlib: a call would not link).
 *
 * The program flash (GD25Q80, 1 MB; the code runs from it through the SSI at 0x04000000 in quad read mode) is written
 * as the stock programmer stub does it (Roger VK3KYY's C7000_write_progmem.py, the code it uploads to 0x10000):
 * the SSI switched to plain SPI (8 bit frames, SER held for a command), 06 write enable, 20 sector erase, 02 page
 * program, 05 status. CTRLR0 needs 8 bits in DFS_32 (bits 20:16) as well: the quad read mode leaves 32 there, and
 * with only DFS (bits 3:0) set every byte read as 0xFF (the stub runs from reset, DFS_32 already 7). At the end the
 * SSI gets its registers back, the cache is invalidated and the bootloader starts the new firmware, as a power on.
 *
 * Requests from the PC, big endian, each answered:
 *   'H'                                   -> "UPD1" (also sent once at the start)
 *   'C' addr(4)                           -> 'C' crc32(4) of the 4 KB sector at addr
 *   'W' addr(4) data(4096) crc32(4)       -> 'K' written and read back the same, or 'E' error(1)
 *   'P' done(2) total(2)                  -> 'P', the progress bar on the screen
 *   'T' x(2) y(2) w(2) h(2) colour(2) bits -> 'T' once drawn: a 1 bit picture, the PC's text (fw_update.py)
 *   'S' divisor(2)                        -> 'S' (still at the old speed), then the UART at 84 MHz / 16 / divisor:
 *                                            'H' -> "UPD1" then 'Y' -> 'Y' must follow at the new speed, each
 *                                            within about 2 s, else the old divisor is back (a speed the cable
 *                                            can't carry: over the mic jack about 375000 is the most)
 *   'B'                                   -> 'B', then the restart
 * addr: a program flash address, 4 KB aligned, in the firmware area [0xC000, 0xDE000) only (the bootloader below
 * stays as it is: the upgrade mode still works whatever happens here).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The LCD (src/drivers/panel.c: ST7789, 240 x 320, the I8080 8 bit bus)
#define I8080_CMD        (*(volatile uint32_t *)0x12000000)
#define I8080_DATA       (*(volatile uint32_t *)0x12000004)
#define PANEL_WIDTH      240
#define PANEL_HEIGHT     320
#define PANEL_CMD_CASET  0x2A
#define PANEL_CMD_RASET  0x2B
#define PANEL_CMD_RAMWR  0x2C
#define PANEL_RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

#define SSI(offset) (*(volatile uint32_t *)(0x04000000 + (offset)))
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

#define CACHE_CIR (*(volatile uint32_t *)0xE000F004) // 1: invalidate all (crt0.S)

#define UART0(offset) (*(volatile uint32_t *)(0x14030000 + (offset)))
#define UART_RBR      0x00
#define UART_THR      0x00
#define UART_DLL      0x00
#define UART_DLH      0x04
#define UART_LCR      0x0C
#define UART_LSR      0x14
#define UART_USR      0x7C
#define LSR_TEMT      0x40 // the transmitter empty
#define USR_BUSY      0x01

#define FLASH_AREA_START 0xC000
#define FLASH_AREA_END   0xDE000
#define SECTOR           4096
#define PAGE             256
#define BUSY_POLLS       2000000  // flash status reads before giving up (a sector erase takes up to 0.4 s)
#define BYTE_POLLS       30000000 // UART polls before a request cut short is dropped (about 2 s)

#define ERR_CRC     1 // the sector came in damaged
#define ERR_ADDRESS 2
#define ERR_VERIFY  3 // read back different
#define ERR_FLASH   4 // the flash stayed busy
#define ERR_TIMEOUT 5 // the request was cut short

// The SSI registers of the quad read mode, put back at the end
static uint32_t saved_values[14];
static uint32_t saved_enable;
static const uint8_t regs[14] = {
    SSI_CTRLR0, SSI_CTRLR1, SSI_SER, SSI_BAUDR, SSI_TXFTLR, SSI_RXFTLR, SSI_IMR, SSI_DMACR, 0x50, 0x54, 0xF0, 0xF4,
    0xF8, 0xFC};

// ---- UART0, polled ----

static void uart_put(uint8_t c)
{
    while ((UART0(UART_USR) & 0x02) == 0) {
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

static void uart_drain(void)
{
    while ((UART0(UART_LSR) & LSR_TEMT) == 0) {
    }
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
    uart_drain();
    while (UART0(UART_USR) & USR_BUSY) { // LCR can't change while busy
        (void)UART0(UART_RBR);
    }
    UART0(UART_LCR) |= 0x80;
    UART0(UART_DLL) = divisor & 0xFF;
    UART0(UART_DLH) = (divisor >> 8) & 0xFF;
    UART0(UART_LCR) &= ~0x80;
}

// ---- the program flash ----

static void ssi_plain_spi(void)
{
    saved_enable = SSI(SSI_SSIENR);
    for (uint32_t i = 0; i < sizeof(regs); i++) {
        saved_values[i] = SSI(regs[i]);
    }
    SSI(SSI_SSIENR) = 0;
    SSI(SSI_CTRLR0) = 0x00070007; // 8 bit frames (DFS_32 and DFS), transmit and receive, standard SPI
    SSI(SSI_CTRLR1) = 7;
    SSI(SSI_BAUDR) = 4;
    SSI(SSI_TXFTLR) = 7;
    SSI(SSI_RXFTLR) = 7;
    SSI(SSI_IMR) = 0;
    SSI(SSI_DMACR) = 0;
    SSI(SSI_SER) = 0;
    SSI(SSI_SSIENR) = 1;
}

static void ssi_quad_read(void)
{
    SSI(SSI_SSIENR) = 0;
    for (uint32_t i = 0; i < sizeof(regs); i++) {
        SSI(regs[i]) = saved_values[i];
    }
    SSI(SSI_SSIENR) = saved_enable;
    CACHE_CIR = 1;
}

static uint8_t spi_byte(uint8_t out)
{
    while ((SSI(SSI_SR) & SR_TFNF) == 0) {
    }
    SSI(SSI_DR) = out;
    while ((SSI(SSI_SR) & SR_TFE) == 0) {
    }
    while (SSI(SSI_SR) & SR_BUSY) {
    }
    return SSI(SSI_DR);
}

static void spi_command(uint8_t cmd, int32_t address) // address < 0: none
{
    SSI(SSI_SER) = 1;
    spi_byte(cmd);
    if (address >= 0) {
        spi_byte(address >> 16);
        spi_byte(address >> 8);
        spi_byte(address);
    }
}

static void spi_end(void)
{
    SSI(SSI_SER) = 0;
}

static bool flash_wait(void)
{
    for (uint32_t n = 0; n < BUSY_POLLS; n++) {
        spi_command(0x05, -1); // read status register
        uint8_t status = spi_byte(0xFF);
        spi_end();
        if ((status & 0x01) == 0) {
            return true;
        }
    }
    return false;
}

static void flash_write_enable(void)
{
    spi_command(0x06, -1);
    spi_end();
}

static uint32_t crc32_add(uint32_t crc, uint8_t byte)
{
    crc ^= byte;
    for (int k = 0; k < 8; k++) {
        crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return crc;
}

// The CRC-32 (zlib's) of a sector read from the flash; against: compared with it too (NULL: no)
static uint32_t flash_sector_crc(uint32_t address, const uint8_t *against, bool *same)
{
    uint32_t crc = 0xFFFFFFFF;

    *same = true;
    spi_command(0x03, address); // read data
    for (uint32_t i = 0; i < SECTOR; i++) {
        uint8_t b = spi_byte(0xFF);
        crc = crc32_add(crc, b);
        if (against && (b != against[i])) {
            *same = false;
        }
    }
    spi_end();
    return ~crc;
}

static int flash_write_sector(uint32_t address, const uint8_t *data)
{
    bool same;

    if (!flash_wait()) {
        return ERR_FLASH;
    }
    flash_write_enable();
    spi_command(0x20, address); // sector erase
    spi_end();
    for (uint32_t page = 0; page < SECTOR; page += PAGE) {
        if (!flash_wait()) {
            return ERR_FLASH;
        }
        flash_write_enable();
        spi_command(0x02, address + page); // page program
        for (uint32_t i = 0; i < PAGE; i++) {
            spi_byte(data[page + i]);
        }
        spi_end();
    }
    if (!flash_wait()) {
        return ERR_FLASH;
    }
    (void)flash_sector_crc(address, data, &same);
    return same ? 0 : ERR_VERIFY;
}

// ---- the screen ----

static void lcd_pause(void) // the LCD needs a short time between the bytes of a command (panel.c)
{
    for (volatile int x = 0; x < 1; x++) {
    }
}

static void lcd_cmd_data4(uint8_t cmd, uint16_t a, uint16_t b)
{
    uint8_t bytes[4] = {a >> 8, a & 0xFF, b >> 8, b & 0xFF};

    I8080_CMD = cmd;
    lcd_pause();
    for (int i = 0; i < 4; i++) {
        I8080_DATA = bytes[i];
        lcd_pause();
    }
}

static void lcd_fill(int x0, int y0, int x1, int y1, uint16_t colour) // [x0, x1) x [y0, y1)
{
    lcd_cmd_data4(PANEL_CMD_CASET, x0, x1 - 1);
    lcd_cmd_data4(PANEL_CMD_RASET, y0, y1 - 1);
    I8080_CMD = PANEL_CMD_RAMWR;
    for (int n = (x1 - x0) * (y1 - y0); n > 0; n--) {
        I8080_DATA = colour >> 8;
        I8080_DATA = colour & 0xFF;
    }
}

#define BAR_X0   20
#define BAR_X1   220
#define BAR_Y0   190
#define BAR_Y1   214
#define WHITE    0xFFFF
#define BLACK    0x0000
#define GREEN    PANEL_RGB(0, 200, 0)
#define RED      PANEL_RGB(230, 0, 0)

// The bar (the percentage above it is text from the PC: 'T')
static void show_progress(uint32_t done, uint32_t total, uint16_t colour)
{
    int filled = BAR_X0 + 2 + (int)(((BAR_X1 - BAR_X0 - 4) * ((total == 0) ? 0 : ((done > total) ? total : done))) /
                                    ((total == 0) ? 1 : total));
    lcd_fill(BAR_X0 + 2, BAR_Y0 + 2, filled, BAR_Y1 - 2, colour);
    lcd_fill(filled, BAR_Y0 + 2, BAR_X1 - 2, BAR_Y1 - 2, BLACK);
}

static uint8_t buffer[SECTOR]; // a sector to write, or a picture to draw

// 'T' x(2) y(2) w(2) h(2) colour(2) bits: taken in whole first, then drawn (dumper.c: drawing while the bytes came in
// overran the UART's receive FIFO), then 'T'
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
    if ((w <= 0) || (h <= 0) || ((x + w) > PANEL_WIDTH) || ((y + h) > PANEL_HEIGHT) || (length > SECTOR)) {
        return;
    }
    for (uint32_t i = 0; i < length; i++) {
        int c = uart_get(false);
        if (c < 0) {
            return;
        }
        buffer[i] = (uint8_t)c;
    }
    const uint8_t *bits = buffer;
    lcd_cmd_data4(PANEL_CMD_CASET, x, x + w - 1);
    lcd_cmd_data4(PANEL_CMD_RASET, y, y + h - 1);
    I8080_CMD = PANEL_CMD_RAMWR;
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

static void show_start(void)
{
    lcd_fill(0, 0, PANEL_WIDTH, PANEL_HEIGHT, BLACK);
    lcd_fill(BAR_X0, BAR_Y0, BAR_X1, BAR_Y1, WHITE);
    show_progress(0, 1, GREEN);
}

// ---- the requests ----

// A whole chip reset by the watchdog after about 3 s, as at power on: a jump to the bootloader would leave the modem
// as it was. The watchdog stays on through its reset: a firmware since the watchdog restart turns it off in
// clock_init(), and fw_update.py turns it off over the cable as soon as the firmware answers (an older firmware
// would be reset again and again); 3 s leave it the time (the firmware answers 0.3 s after the reset).
static void restart(void)
{
    ssi_quad_read();
    *(volatile uint32_t *)0x14010000 = 0x5ada7200; // WDG_LOCK: unlocked
    *(volatile uint32_t *)0x14010004 = 0x7;        // WDG_OT_LOAD_H:L counts up to the overflow:
    *(volatile uint32_t *)0x14010008 = 0xFCCCCCCC; //   0x3333333 counts, 3 s (0x40000000 are 60 s)
    *(volatile uint32_t *)0x14010010 = 1;          // WDG_EN
    *(volatile uint32_t *)0x14010014 = 0x55aadd22; // WDG_KNOCK: counting from the value above
    for (;;) {
    }
}

static void hello(void)
{
    uart_put('U');
    uart_put('P');
    uart_put('D');
    uart_put('1');
}

static void handle_write(void)
{
    uint32_t address, crc_sent, crc = 0xFFFFFFFF;
    int error = 0;

    if (!uart_get_word(&address)) {
        error = ERR_TIMEOUT;
    }
    for (uint32_t i = 0; (error == 0) && (i < SECTOR); i++) {
        int c = uart_get(false);
        if (c < 0) {
            error = ERR_TIMEOUT;
        } else {
            buffer[i] = c;
            crc = crc32_add(crc, c);
        }
    }
    if ((error == 0) && !uart_get_word(&crc_sent)) {
        error = ERR_TIMEOUT;
    }
    if ((error == 0) && (~crc != crc_sent)) {
        error = ERR_CRC;
    }
    if ((error == 0) && (((address % SECTOR) != 0) || (address < FLASH_AREA_START) || (address >= FLASH_AREA_END))) {
        error = ERR_ADDRESS;
    }
    if (error == 0) {
        error = flash_write_sector(address, buffer);
    }
    if (error == 0) {
        uart_put('K');
    } else {
        uart_put('E');
        uart_put(error);
    }
}

// The entry (updater.ld puts it first, at UPDATER_AREA), jumped to by the loader with the interrupts off
void __attribute__((section(".text.entry"), noreturn)) updater_main(void)
{
    ssi_plain_spi();
    show_start();
    hello();

    for (;;) {
        int request = uart_get(true);
        uint32_t address, word;
        bool same;

        // a stray byte is ignored
        if (request == 'H') {
            hello();
        } else if (request == 'C') {
            if (uart_get_word(&address)) {
                word = flash_sector_crc(address, NULL, &same);
                uart_put('C');
                uart_put_word(word);
            }
        } else if (request == 'W') {
            handle_write();
        } else if (request == 'T') {
            handle_picture();
        } else if (request == 'P') {
            if (uart_get_word(&word)) {
                show_progress(word >> 16, word & 0xFFFF, GREEN);
                uart_put('P');
            }
        } else if (request == 'S') {
            uint32_t old = uart_divisor();
            int hi = uart_get(false), lo = uart_get(false);
            if ((hi >= 0) && (lo >= 0) && (((hi << 8) | lo) != 0)) {
                uart_put('S');
                uart_set_divisor((hi << 8) | lo);
                bool both_ways = false; // the PC sends 'Y' only after a sound "UPD1"
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
        } else if (request == 'B') {
            uart_put('B');
            while ((UART0(UART_LSR) & 0x40) == 0) { // the transmitter empty
            }
            restart();
        }
    }
}
