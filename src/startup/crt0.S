/******************************************************************************
 * File:        CRT0.S
 *
 * Description: Startup file.
 *
 *****************************************************************************/

/* OpenDMR bring-up: startup, after OpenDM32 arch/crt0.S */
#define CONFIG_START_STACKSIZE   0x1000
#define CONFIG_IRQ_STACKSIZE     0x1000
#define CONFIG_FASTIRQ_STACKSIZE 0x0

.file     "crt0.S"
.text
.import   _crt0init
.import   VectorIrqISR

/**********************************************************************
 * Setup the initial interrupt vector table
 **********************************************************************/

/*----------------------------------------------------------------------
  Interrupt Vectors (0-31)
 ----------------------------------------------------------------------*/

.section .dahua_entry
.align    4
//dahua magic
.byte 0x74
.byte 0x68
.byte 0x64
.byte 0x0c
// Image entry
.long __start

.section .exp_table
.align    10
vbr_base:
.long   __start
.long   ISR_Misaligned_Access
.long   ISR_Access_Error
.long   ISR_Div_By_Zero
.long   ISR_Illegal
.long   ISR_Privlege_Violation
.long   ISR_Trace_Exection
.long   ISR_Breakpoint_Exception
.long   ISR_Unrecoverable_Error
.long   ISR_Soft_Reset
.long   ISR_INT
.long   ISR_FINT    /* Set LSB for alternate registers */
.long   ISR_Hardware_Accelerator

.long   ISR_Reserved
.long   ISR_Reserved
.long   ISR_Reserved

.long   ISR_Trap0//OSCtxSw//ISR_Trap0
.long   ISR_Trap1
.long   ISR_Trap2
.long   ISR_Trap3
.long   ISR_Reserved
.long   ISR_Reserved
.long   ISR_Reserved
.long   ISR_Reserved

.long   ISR_Reserved
.long   ISR_Reserved
.long   ISR_Reserved
.long   ISR_Reserved
.long   ISR_Reserved
.long   ISR_Reserved
.long   ISR_Reserved
.long   ISR_Reserved
/*----------------------------------------------------------------------
  User Interrupts (32 - 127)
 ----------------------------------------------------------------------*/
.long   VectorIrqISR    // 0 - Not implemented
.long   VectorIrqISR       //OSTickISR    	// 1 - timer0, used for os tick
.long   VectorIrqISR    // 2 - Not implemented
.long   VectorIrqISR    // 3 - Not implemented
.long   VectorIrqISR    // 4 - Not implemented
.long   VectorIrqISR    // 5 - Not implemented
.long   VectorIrqISR    // 6 - Not implemented
.long   VectorIrqISR    // 7 - Not implemented
.long   VectorIrqISR    // 8 - Not implemented
.long   VectorIrqISR    // 9 - Not implemented
.long   VectorIrqISR    // 10 - Not implemented
.long   VectorIrqISR    // 11 - Not implemented
.long   VectorIrqISR    // 12 - Not implemented
.long   VectorIrqISR    // 13 - Not implemented
.long   VectorIrqISR    // 14 - Not implemented
.long   VectorIrqISR    // 15 - Not implemented
.long   VectorIrqISR    // 16 - Not implemented
.long   VectorIrqISR    // 17 - Not implemented
.long   VectorIrqISR    // 18 - Not implemented
.long   VectorIrqISR    // 19 - Not implemented
.long   VectorIrqISR    // 20 - Not implemented
.long   VectorIrqISR    // 21 - Not implemented
.long   VectorIrqISR    // 22 - Not implemented
.long   VectorIrqISR    // 23 - Not implemented
.long   VectorIrqISR    // 24 - Not implemented
.long   VectorIrqISR    // 25 - Not implemented
.long   VectorIrqISR    // 26 - Not implemented
.long   VectorIrqISR    // 27 - Not implemented
.long   VectorIrqISR    // 28 - Not implemented
.long   VectorIrqISR    // 29 - Not implemented
.long   VectorIrqISR    // 30 - Not implemented
.long   VectorIrqISR    // 31 - Not implemented
.long   VectorIrqISR    // 32 - Not implemented
.long   VectorIrqISR    // 33 - Not implemented
.long   VectorIrqISR    // 34 - Not implemented
.long   VectorIrqISR    // 35 - Not implemented
.long   VectorIrqISR    // 36 - Not implemented
.long   VectorIrqISR    // 37 - Not implemented
.long   VectorIrqISR    // 38 - Not implemented
.long   VectorIrqISR    // 39 - Not implemented
.long   VectorIrqISR    // 40 - Not implemented
.long   VectorIrqISR    // 41 - Not implemented
.long   VectorIrqISR    // 42 - Not implemented
.long   VectorIrqISR    // 43 - Not implemented
.long   VectorIrqISR    // 44 - Not implemented
.long   VectorIrqISR    // 45 - Not implemented
.long   VectorIrqISR    // 46 - Not implemented
.long   VectorIrqISR    // 47 - Not implemented
.long   VectorIrqISR    // 48 - Not implemented
.long   VectorIrqISR    // 49 - Not implemented
.long   VectorIrqISR    // 50 - Not implemented
.long   VectorIrqISR    // 51 - Not implemented
.long   VectorIrqISR    // 52 - Not implemented
.long   VectorIrqISR    // 53 - Not implemented
.long   VectorIrqISR    // 54 - Not implemented
.long   VectorIrqISR    // 55 - Not implemented
.long   VectorIrqISR    // 56 - Not implemented
.long   VectorIrqISR    // 57 - Not implemented
.long   VectorIrqISR    // 58 - Not implemented
.long   VectorIrqISR    // 59 - Not implemented
.long   VectorIrqISR    // 60 - Not implemented
.long   VectorIrqISR    // 61 - Not implemented
.long   VectorIrqISR    // 62 - Not implemented
.long   VectorIrqISR    // 63 - Not implemented


/**********************************************************************
 * Code Start Section
 **********************************************************************/
 .text




/**********************************************************************
 * Reset Vector 0 "start" point
 **********************************************************************/

.export   __START
.export   __start
.align 2
__start:
__START:
	lrw     r1, 0x80000100
	mtcr    r1, psr

	lrw     r7, _estartstack
	mov     sp, r7

	lrw     r7, vbr_base
	mtcr    r7, vbr

//#ifdef CONFIG_MPU_REGION0
//	CSKY_MPU_CONFIG 0 CONFIG_REGION0_ADDR CONFIG_REGION0_LEN \
//        CONFIG_REGION0_PERMISSION CONFIG_REGION0_EXECUTABLE CONFIG_REGION0_SECURITY
//#endif
//#ifdef CONFIG_MPU_REGION1
//	CSKY_MPU_CONFIG 1 CONFIG_REGION1_ADDR CONFIG_REGION1_LEN \
//        CONFIG_REGION1_PERMISSION CONFIG_REGION1_EXECUTABLE CONFIG_REGION1_SECURITY
//#endif
//#ifdef CONFIG_MPU_REGION2
//	CSKY_MPU_CONFIG 2 CONFIG_REGION2_ADDR CONFIG_REGION2_LEN \
//        CONFIG_REGION2_PERMISSION CONFIG_REGION2_EXECUTABLE CONFIG_REGION2_SECURITY
//#endif
//#ifdef CONFIG_MPU_REGION3
//	CSKY_MPU_CONFIG 3 CONFIG_REGION3_ADDR CONFIG_REGION3_LEN \
//        CONFIG_REGION3_PERMISSION CONFIG_REGION3_EXECUTABLE CONFIG_REGION3_SECURITY
//#endif
//#ifdef CONFIG_MPU_REGION4
//	CSKY_MPU_CONFIG 4 CONFIG_REGION4_ADDR CONFIG_REGION4_LEN \
//        CONFIG_REGION4_PERMISSION CONFIG_REGION4_EXECUTABLE CONFIG_REGION4_SECURITY
//#endif
//#ifdef CONFIG_MPU_REGION5
//	CSKY_MPU_CONFIG 5 CONFIG_REGION5_ADDR CONFIG_REGION5_LEN \
//        CONFIG_REGION5_PERMISSION CONFIG_REGION5_EXECUTABLE CONFIG_REGION5_SECURITY
//#endif
//#ifdef CONFIG_MPU_REGION6
//	CSKY_MPU_CONFIG 6 CONFIG_REGION6_ADDR CONFIG_REGION6_LEN \
//        CONFIG_REGION6_PERMISSION CONFIG_REGION6_EXECUTABLE CONFIG_REGION6_SECURITY
//#endif
//#ifdef CONFIG_MPU_REGION7
//	CSKY_MPU_CONFIG 7 CONFIG_REGION7_ADDR CONFIG_REGION7_LEN \
//        CONFIG_REGION7_PERMISSION CONFIG_REGION7_EXECUTABLE CONFIG_REGION7_SECURITY
//#endif

//	CSKY_MPU_ENABLE

//movi      	r0, 17
//bseti      	r0, 4
//mtcr      	r0, cr<17, 0>
//
//mfcr      	r0, cr<18, 0>
//bseti      	r0, 2
//mtcr      	r0, cr<18, 0>
//
//movi      	r0, 0
//mtcr      	r0, cr<21, 0>
//movi      	r0, 63
//mtcr      	r0, cr<20, 0>
//movi      	r0, 1
//mtcr      	r0, cr<21, 0>
//lrw      	r0, 0x8000035
//mtcr      	r0, cr<20, 0>
//movi      	r0, 3842
//mtcr      	r0, cr<19, 0>
//mfcr      	r0, cr<18, 0>
//bseti      	r0, 0
//mtcr      	r0, cr<18, 0>

	/* config cache */
  	lrw      	r8, 0xe000f000
  	// invaild all cache
   	movi      	r7, 1
  	st.w      	r7, (r8, 0x4)
  	//set write back mode, 4 way, instruct & data in cache
   	movi      	r7, 0x04
  	st.w      	r7, (r8, 0x0)
	//set cache region, from 0x03000000 to 0x03000000+ 8MByte
   	lrw      	r7, 0x300002d
  	st.w      	r7, (r8, 0x8)
  	 //enable cache
  	ld.w      	r7, (r8, 0x0)
   	bseti      	r7, 0
  	st.w      	r7, (r8, 0x0)


#define CONFIG_RUN_IN_FLASH
/*copy data & rodata_in_ram section from LMA(flash to ram)*/
#ifdef CONFIG_RUN_IN_FLASH
	lrw      r7, __sdata
	lrw      r6, __erodata_in_ram
	lrw      r5, __etext
	cmphs    r7, r6
	bt       .Lcopy_data_end

.Lcopy_data_loop:
	ldw      r4, (r5, 0)
	stw      r4, (r7, 0)
	addi     r5, 4
	addi     r7, 4
	cmphs    r7, r6
	bf       .Lcopy_data_loop
.Lcopy_data_end:
#endif


    /*
     * Copy text_in_ram  section from it's LMA (__etext) .
     * NOTE: _end_rodata and _start_data must align 4
     */

    lrw     r7, __text_in_ram_start
    lrw     r6, __text_in_ram_end
    lrw     r5, __text_in_ram_load

    cmphs   r7, r6
    bt      2f
1:
    ld.w    r4, (r5, 0)
    st.w    r4, (r7, 0)
    addi    r5, 4
    addi    r7, 4
    cmphs   r7, r6
    bf      1b
2:


	/*
	 * Zero out the bss region.
	 * NOTE: __sbss must align 4
	 */
	lrw     r7, __bss_start  /* Get start of bss from linking script file */
	lrw     r6, __end        /* Get end of bss from linking script file */
	subu    r6, r7           /* Calculate size of bss */
	lsri    r6, 2            /* Size of whole words */
	cmpnei  r6, 0
	bf      __goto_c
	movi    r5, 0            /* Set zero value to write */
2:
	stw     r5, (r7)         /* Zero next word */
	addi    r7, 4            /* Increase bss pointer */
	decne   r6               /* Decrease counter */
	bt      2b               /* Repeat for all bss */

__goto_c:

	jbsr    main

	/* NOTE: Breakpoint if we return from main() */
exit:
	bkpt

/**********************************************************************
 * Routine for initialization of C++ constructors and destructors
 **********************************************************************/
.align 2
.global __main
__main:
	rts

/*----------------------------------------------------------------------
  Initial interrupt service routines (ISRs)
 ----------------------------------------------------------------------*/
/* DM32 (debug and release builds): report the exception number, EPC and EPSR on UART0 instead of hanging silently */
#if 1
ISR_Misaligned_Access:
	/* the registers of the faulting code, for irq_exception() to report */
	subi    sp, 64
	stm     r0-r13, (sp)
	stw     r15, (sp, 56)
	lrw     r1, irq_exception_frame
	stw     sp, (r1)
	movi    r0, 1
	br      dm32ExceptionCommon
ISR_Access_Error:
	movi    r0, 2
	br      dm32ExceptionCommon
ISR_Div_By_Zero:
	movi    r0, 3
	br      dm32ExceptionCommon
ISR_Reserved:
	movi    r0, 15
	br      dm32ExceptionCommon
ISR_Illegal:
	movi    r0, 4
	br      dm32ExceptionCommon
ISR_Privlege_Violation:
	movi    r0, 5
	br      dm32ExceptionCommon
ISR_Trace_Exection:
	movi    r0, 6
	br      dm32ExceptionCommon
ISR_Breakpoint_Exception:
	rte
ISR_Unrecoverable_Error:
	movi    r0, 8
	br      dm32ExceptionCommon
ISR_Soft_Reset:
	movi    r0, 9
	br      dm32ExceptionCommon
ISR_INT:               /* Not set at initialization, reserved for RTOS */
	movi    r0, 10
	br      dm32ExceptionCommon
ISR_FINT:              /* All fast interrupts use alternate regs (see above) */
	movi    r0, 11
	br      dm32ExceptionCommon
ISR_Hardware_Accelerator:
	movi    r0, 12
	br      dm32ExceptionCommon
dm32ExceptionCommon:
	mfcr    r1, cr<4, 0>    /* EPC */
	mfcr    r2, cr<2, 0>    /* EPSR */
	mov     r3, r15         /* LR at the time of the exception */
	jbsr    irq_exception
1:
	br      1b
#else
ISR_Misaligned_Access:
	br      ISR_Misaligned_Access
ISR_Access_Error:
	br      ISR_Access_Error
ISR_Div_By_Zero:
	br      ISR_Div_By_Zero
ISR_Reserved:
	br      ISR_Reserved
ISR_Illegal:
	br      ISR_Illegal
ISR_Privlege_Violation:
	br      ISR_Privlege_Violation
ISR_Trace_Exection:
	br      ISR_Trace_Exection
ISR_Breakpoint_Exception:
	rte
ISR_Unrecoverable_Error:
	br      ISR_Unrecoverable_Error
ISR_Soft_Reset:
	br      ISR_Soft_Reset
ISR_INT:               /* Not set at initialization, reserved for RTOS */
	br      ISR_INT        /* timer tick using the PIT */
ISR_FINT:              /* All fast interrupts use alternate regs (see above) */
	br      ISR_FINT
ISR_Hardware_Accelerator:
	br      ISR_Hardware_Accelerator
#endif
ISR_Trap0:
	br      ISR_Trap0
ISR_Trap1:
	br      ISR_Trap1
ISR_Trap2:
	br      ISR_Trap2
ISR_Trap3:
	br      ISR_Trap3
ISR_Vectored:
	lrw     r1, 0x80000100
	mtcr    r1, epsr
	rte
	br      ISR_Vectored


	.section .startstack.sec
	.align 2
.rept (CONFIG_START_STACKSIZE >> 2)
	.long 0
.endr

	.section .irqstack.sec
    .align 2
.rept (CONFIG_IRQ_STACKSIZE >> 2)
    .long 0
.endr

	.section .fstack.sec
	.align 2
.rept (CONFIG_FASTIRQ_STACKSIZE >> 2)
	.long 0
.endr





