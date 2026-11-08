/*
 * FreeRTOS port for the C-SKY CK803S core of the DH4570 / HR_C7000 (Baofeng DM-32).
 *
 * Context frame, on the task stack, 68 bytes (as the vendor's uC/OS-III port, which this one replaces):
 *   r0 - r13 at 0 - 52, r15 (lr) at 56, PSR at 60, PC at 64.
 * The task's stack pointer is the first member of its TCB (pxTopOfStack).
 *
 * Task level switches are function calls (vPortYield()); interrupts share one dispatcher (VectorIrqISR, portasm.S)
 * that switches to the IRQ stack and switches task on its way out when xPortSwitchRequired is set.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PORTMACRO_H
#define PORTMACRO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Type definitions */
#define portCHAR          char
#define portFLOAT         float
#define portDOUBLE        double
#define portLONG          long
#define portSHORT         short
#define portSTACK_TYPE    uint32_t
#define portBASE_TYPE     long

typedef portSTACK_TYPE   StackType_t;
typedef long             BaseType_t;
typedef unsigned long    UBaseType_t;

#if ( configTICK_TYPE_WIDTH_IN_BITS == TICK_TYPE_WIDTH_32_BITS )
    typedef uint32_t     TickType_t;
    #define portMAX_DELAY              ( TickType_t ) 0xffffffffUL
    #define portTICK_TYPE_IS_ATOMIC    1
#else
    #error configTICK_TYPE_WIDTH_IN_BITS must be TICK_TYPE_WIDTH_32_BITS
#endif

/* Architecture specifics */
#define portARCH_NAME              "CK803S"
#define portSTACK_GROWTH           ( -1 )
#define portTICK_PERIOD_MS         ( ( TickType_t ) 1000 / configTICK_RATE_HZ )
#define portBYTE_ALIGNMENT         8
#define portNOP()                  __asm__ volatile ( "nop" )
#define portMEMORY_BARRIER()       __asm__ volatile ( "" ::: "memory" )
#define portINLINE                 __inline
#ifndef portFORCE_INLINE
    #define portFORCE_INLINE       inline __attribute__( ( always_inline ) )
#endif
#define portDONT_DISCARD           __attribute__( ( used ) )

/* Interrupts: PSR bit 6 (IE) */
static portFORCE_INLINE void vPortDisableInterrupts( void )
{
    __asm__ volatile ( "psrclr ie" ::: "memory" );
}

static portFORCE_INLINE void vPortEnableInterrupts( void )
{
    __asm__ volatile ( "psrset ie" ::: "memory" );
}

static portFORCE_INLINE uint32_t ulPortSetInterruptMask( void )
{
    uint32_t ulPsr;

    __asm__ volatile ( "mfcr %0, psr\n"
                       "psrclr ie" : "=r" ( ulPsr ) :: "memory" );
    return ulPsr;
}

static portFORCE_INLINE void vPortClearInterruptMask( uint32_t ulPsr )
{
    __asm__ volatile ( "mtcr %0, psr" :: "r" ( ulPsr ) : "memory" );
}

extern void vPortEnterCritical( void );
extern void vPortExitCritical( void );
extern void vPortYieldFromTask( void );
extern BaseType_t xPortIsInsideInterrupt( void );

/* Set by the interrupts (tick, API calls from interrupts): the dispatcher switches task on its way out */
extern volatile uint32_t xPortSwitchRequired;

#define portDISABLE_INTERRUPTS()                     vPortDisableInterrupts()
#define portENABLE_INTERRUPTS()                      vPortEnableInterrupts()
#define portENTER_CRITICAL()                         vPortEnterCritical()
#define portEXIT_CRITICAL()                          vPortExitCritical()
#define portSET_INTERRUPT_MASK_FROM_ISR()            ulPortSetInterruptMask()
#define portCLEAR_INTERRUPT_MASK_FROM_ISR( x )       vPortClearInterruptMask( x )

/* From a task: switch now, or at the end of the critical section; from an interrupt: on its way out */
#define portYIELD()                                  vPortYieldFromTask()
#define portEND_SWITCHING_ISR( xSwitchRequired )     do { if( ( xSwitchRequired ) != pdFALSE ) { xPortSwitchRequired = 1; } } while( 0 )
#define portYIELD_FROM_ISR( x )                      portEND_SWITCHING_ISR( x )

/* Task function macros */
#define portTASK_FUNCTION_PROTO( vFunction, pvParameters )    void vFunction( void * pvParameters )
#define portTASK_FUNCTION( vFunction, pvParameters )          void vFunction( void * pvParameters )

#ifdef __cplusplus
}
#endif

#endif /* PORTMACRO_H */
