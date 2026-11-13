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
 * OpenDMR bring-up, Baofeng bootloader layout: the app at 0x0300C000 (flashed with PTT+SK1 and
 * the stock firmware loader), after OpenDM32 ckcpu.ld.
 * The startup code, the kernel and its port (the startup and freertos dirs) run from RAM, as uC/OS did (ucos_in_ram).
 */
ENTRY(__start)

MEMORY
{
	MEM0 : ORIGIN = 0x0300C000 , LENGTH = 0xD2000   /* flash, app area */
	MEM1 : ORIGIN = 0x00010000 , LENGTH = 0x48000   /* RAM */
}

PROVIDE (_estartstack =  0x00057FFC);

SECTIONS
{
    .text : {
		. = ALIGN(0x4) ;
		*crt0.o(.text)
		__stext = . ;
		*(EXCLUDE_FILE(*/startup/* */freertos/* */vocoder/ambe_enc* */p25/imbe/imbe_dec* */p25/imbe/imbe_parms* */p25/imbe/imbe_enc* */p25/imbe/imbe_ana.*) .text)
		*(EXCLUDE_FILE(*/startup/* */freertos/* */vocoder/ambe_enc* */p25/imbe/imbe_dec* */p25/imbe/imbe_parms* */p25/imbe/imbe_enc* */p25/imbe/imbe_ana.*) .text*)
		. = ALIGN(0x10) ;
		__etext = . ;
    } > MEM0

    .data : {
		. = ALIGN(0x4) ;
		__sdata = . ;
		*(.data)
		*(.data*)
		*(.sdata)
		*(.sdata.*)
		. = ALIGN(0x4) ;
		__edata = . ;
	} > MEM1 AT> MEM0

	.rodata_in_ram : {
		. = ALIGN(0x4) ;
		__srodata_in_ram = . ;
		*/freertos/*(.rodata*)
		. = ALIGN(0x4) ;
		__erodata_in_ram = . ;
    } > MEM1 AT> MEM0

    .rodata : {
		. = ALIGN(0x4) ;
		__srodata = . ;
		*(.rodata*)
		*(.flashconst*)
		. = ALIGN(0x4) ;
		__erodata = . ;
    } > MEM0

    .text_in_ram : {
		. = ALIGN(0x4) ;
		__text_in_ram_start = . ;
		*crt0.o(.exp_table)
		*/startup/*(.text*)
		*/freertos/*(.text*)
		*(.staticcode*)
		. = ALIGN(0x4) ;
		__text_in_ram_end = . ;
    } > MEM1 AT> MEM0

	.bss : {
		. = ALIGN(0x4) ;
		__bss_start = . ;
		*(EXCLUDE_FILE(*/vocoder/ambe_* */p25/imbe/imbe_dec* */p25/imbe/imbe_parms* */p25/imbe/imbe_enc* */p25/imbe/imbe_ana.*) .sbss)
		*(EXCLUDE_FILE(*/vocoder/ambe_* */p25/imbe/imbe_dec* */p25/imbe/imbe_parms* */p25/imbe/imbe_enc* */p25/imbe/imbe_ana.*) .sbss.*)
		*(.scommon)
		*(EXCLUDE_FILE(*/vocoder/ambe_* */p25/imbe/imbe_dec* */p25/imbe/imbe_parms* */p25/imbe/imbe_enc* */p25/imbe/imbe_ana.*) .bss)
		*(EXCLUDE_FILE(*/vocoder/ambe_* */p25/imbe/imbe_dec* */p25/imbe/imbe_parms* */p25/imbe/imbe_enc* */p25/imbe/imbe_ana.*) .bss.*)
		*(EXCLUDE_FILE(*/vocoder/ambe_* */p25/imbe/imbe_dec* */p25/imbe/imbe_parms* */p25/imbe/imbe_enc* */p25/imbe/imbe_ana.*) COMMON)
		. = ALIGN(0x10) ;
		__end = . ;
	} > MEM1

	/* Not cleared at the start: kept through a watchdog reset (services/systime.c, the reset record) */
	.noinit (NOLOAD) : {
		. = ALIGN(0x4) ;
		*(.noinit*)
	} > MEM1

	.heapstack : {
		. = ALIGN(0x4);
		__sirqstack = .;
		KEEP(*(.irqstack.sec))
		__eirqstack = .;
		g_top_irqstack = .;
	} > MEM1

	/* Our DMR vocoder (vocoder/c/ambe) runs from RAM: its encoder code, the tables of the hot loops and its
	   variables (zeros in the flash, so a load starts it afresh; the other tables stay in the flash), copied by
	   dmr/vocoder.c vocoder_load(). The stock codec is not supported.
	   The update area: the RAM services/serial_update.c gives the update program (tools/debug/updater, linked at
	   0x44400; fw_update.py checks the address and the size). Kept where and as large as it always was, so any
	   version updates any other; the vocoder is in it, unused while the update runs.
	   The P25 receiver's IMBE decoder (p25/c/imbe: its code, the tables of the hot loops, its state and the received
	   audio) shares that RAM: one overlay or the other is loaded (dmr/vocoder.c overlay_use()), DMR or P25. */
	__update_area_start = 0x00044400 ;
	__update_area_end = 0x00056F24 ;
	OVERLAY 0x00044400 : AT(LOADADDR(.text_in_ram) + SIZEOF(.text_in_ram)) {
		.vocoder_overlay {
			__vocoder_ram_start = . ;
			*/vocoder/ambe_enc*(.text*)
			*(.vocoder_rodata*) /* the tables of the hot loops (vocoder/c/ambe/ambe_tables.h AMBE_RAM_DATA) */
			*/vocoder/ambe_*(.sbss* .bss* COMMON)
			*(.vocoder_bss*)
			. = ALIGN(0x4) ;
			__vocoder_ram_end = . ;
		}
		.p25_overlay {
			__p25_ram_start = . ;
			*/p25/imbe/imbe_dec*(.text*)
			*/p25/imbe/imbe_parms*(.text*)
			*/p25/imbe/imbe_enc*(.text*)   /* the encoder: its quantizer and analysis */
			*/p25/imbe/imbe_ana.*(.text*)
			*(.p25_rodata*) /* the IMBE tables of the hot loops (p25/c/imbe/imbe_tables.h IMBE_RAM_DATA) */
			*/p25/imbe/imbe_dec*(.sbss* .bss* COMMON)
			*/p25/imbe/imbe_parms*(.sbss* .bss* COMMON)
			*/p25/imbe/imbe_enc*(.sbss* .bss* COMMON)
			*/p25/imbe/imbe_ana.*(.sbss* .bss* COMMON)
			*(.p25_bss*)
			. = ALIGN(0x4) ;
			__p25_ram_end = . ;
		}
	}
	__vocoder_flash_start = LOADADDR(.vocoder_overlay);
	__p25_flash_start = LOADADDR(.p25_overlay);
	__text_in_ram_load = LOADADDR(.text_in_ram);

	/* the start stack is at _estartstack, not a section */
	/DISCARD/ : { *(.startstack.sec) *(.fstack.sec) *(.dahua_entry) }

    ASSERT(g_top_irqstack <= __update_area_start, "RAM overflow: bss and the IRQ stack run into the vocoder / update area")
    ASSERT(__vocoder_ram_end <= __update_area_end, "The vocoder does not fit in its RAM")
    ASSERT(__p25_ram_end <= __update_area_end, "The P25 decoder does not fit in its RAM")
    ASSERT(__update_area_end <= _estartstack - 0x1000, "The update area leaves less than 4 KB for the start stack")
    ASSERT(__vocoder_ram_end <= _estartstack - 0x1000, "Our vocoder leaves less than 4 KB for the start stack")
}
