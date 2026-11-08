/*
 * Copyright (C) 2026 Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
 * following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following
 *    disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
 *    following disclaimer in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote
 *    products derived from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden without the written
 *    permission of the copyright holder. This includes, without limitation, incorporation in a commercial product or
 *    incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
 * INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * OpenDMR, driver: UART0, the programming cable. After OpenDM32 (user/DRV/C7000_DRV.c DHUart0Init(),
 * DHUart0Write(), DHUart1Init(), user/SOURCE/functions/cps.c cpsUart0IRQHandler()); UART1: the GPS (gps.c).
 */

#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "uart.h"

// The two UARTs are the same 16550-like block
#define UART0_BASE 0x14030000 // the programming cable
#define UART1_BASE 0x14040000 // the GPS module
#define UART_RBR(b) REG((b) + 0x00)
#define UART_THR(b) REG((b) + 0x00)
#define UART_DLL(b) REG((b) + 0x00)
#define UART_DLH(b) REG((b) + 0x04)
#define UART_IER(b) REG((b) + 0x04)
#define UART_IIR(b) REG((b) + 0x08)
#define UART_FCR(b) REG((b) + 0x08)
#define UART_LCR(b) REG((b) + 0x0c)
#define UART_LSR(b) REG((b) + 0x14)
#define UART_USR(b) REG((b) + 0x7c)
#define IRQ_UART0   12 // vector 44
#define IRQ_UART1   10 // vector 42

#define IER_RECEIVE 0x05 // received data available, receiver line status

#define IIR_ID_MASK     0x0F
#define IIR_NONE        0x01
#define IIR_LINE_STATUS 0x06
#define IIR_BUSY        0x07

static uart_rx_handler_t rx_handler[2];

// The interrupt controller takes a UART interrupt on the rising edge of its request: the request must be down when
// the handler returns, or no edge comes again (GPS bytes then pile up for good). Each cause the IIR gives is cleared
// until it gives none.
static void RAM_CODE receive(uint32_t base, uart_rx_handler_t handler)
{
    uint32_t id;

    while ((id = (UART_IIR(base) & IIR_ID_MASK)) != IIR_NONE) {
        if (id == IIR_LINE_STATUS) {
            (void)UART_LSR(base);
        } else if (id == IIR_BUSY) {
            (void)UART_USR(base);
        }
        while (UART_LSR(base) & 0x01) {
            uint8_t c = UART_RBR(base);

            if (handler) {
                handler(c);
            }
        }
    }
}

static void RAM_CODE uart0_isr(void)
{
    receive(UART0_BASE, rx_handler[0]);
}

static void RAM_CODE uart1_isr(void)
{
    receive(UART1_BASE, rx_handler[1]);
}

// 8N1, FIFOs on, interrupts off (DHUart0Init(), DHUart1Init())
static void setup(uint32_t base, uint32_t baud)
{
    uint32_t divisor = ((clock_apb_hz >> 4) / baud);

    UART_LCR(base) |= (1 << 7);
    UART_DLL(base) = divisor & 0xff;
    UART_DLH(base) = (divisor >> 8) & 0xff;
    UART_LCR(base) &= ~(1 << 7);
    UART_IER(base) = 0x00;
    UART_LCR(base) |= 0x03;                            // 8 bits
    UART_FCR(base) = (0x01 << 6) | (0x02 << 4) | 0x07; // FIFOs reset and on, rx trigger 1, tx trigger 2
}

void uart0_init(uint32_t baud, uart_rx_handler_t handler)
{
    rx_handler[0] = handler;
    setup(UART0_BASE, baud);
    irq_register(IRQ_UART0, uart0_isr);
    UART_IER(UART0_BASE) = IER_RECEIVE;
}

void uart1_init(uint32_t baud, uart_rx_handler_t handler)
{
    rx_handler[1] = handler;
    setup(UART1_BASE, baud);
    irq_register(IRQ_UART1, uart1_isr);
}

// gpsDataInputStartStop(): off with the FIFOs reset. On: enabled with the interrupts off, then whatever the UART
// requests at once is handled, so the request starts down (receive())
void uart1_receive(bool on)
{
    taskENTER_CRITICAL();

    UART_IER(UART1_BASE) = 0x00;
    UART_FCR(UART1_BASE) = (0x01 << 6) | (0x02 << 4) | 0x07;
    if (on) {
        UART_IER(UART1_BASE) = IER_RECEIVE;
        receive(UART1_BASE, rx_handler[1]);
    }
    taskEXIT_CRITICAL();
}

void RAM_CODE uart0_write(const uint8_t *data, uint32_t length)
{
    for (uint32_t i = 0; i < length; i++) {
        while ((UART_USR(UART0_BASE) & 0x02) == 0) {
            // transmit FIFO full
        }
        UART_THR(UART0_BASE) = data[i];
    }
}
