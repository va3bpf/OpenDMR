/*
 * FreeRTOS port for the C-SKY CK803S core of the DH4570 / HR_C7000 (Baofeng DM-32): see portmacro.h.
 *
 * SPDX-License-Identifier: MIT
 */

#include "FreeRTOS.h"
#include "task.h"

#define portINITIAL_PSR    ( 0x80000140UL ) /* supervisor mode, exceptions (EE) and interrupts (IE) on */

/* Nesting of taskENTER_CRITICAL(); interrupts stay off from the first task created until the scheduler starts */
static volatile UBaseType_t uxCriticalNesting = 0xaaaaaaaaUL;

/* A task asked to switch inside a critical section: switch when it ends */
static volatile uint32_t ulYieldPending = 0;

/* Interrupt nesting (VectorIrqISR) and the switch requested by the interrupts, done on their way out */
volatile uint32_t ulPortInterruptNesting = 0;
volatile uint32_t xPortSwitchRequired = 0;

extern void vPortStartFirstTask( void );
extern void vPortYield( void );

static void prvTaskExitError( void )
{
    /* A task function returned: not allowed */
    portDISABLE_INTERRUPTS();
    for( ;; )
    {
    }
}

StackType_t * pxPortInitialiseStack( StackType_t * pxTopOfStack, TaskFunction_t pxCode, void * pvParameters )
{
    *( --pxTopOfStack ) = ( StackType_t ) pxCode;           /* PC */
    *( --pxTopOfStack ) = ( StackType_t ) portINITIAL_PSR;  /* PSR */
    *( --pxTopOfStack ) = ( StackType_t ) prvTaskExitError; /* r15 (lr) */

    for( int i = 13; i >= 1; i-- )                          /* r13 - r1 */
    {
        *( --pxTopOfStack ) = 0;
    }

    *( --pxTopOfStack ) = ( StackType_t ) pvParameters;     /* r0 */

    return pxTopOfStack;
}

BaseType_t xPortStartScheduler( void )
{
    /* The tick (TIMER0, OSTickISR() in bsp_coretimer.c) is set up by BSP_Init() */
    uxCriticalNesting = 0;
    vPortStartFirstTask();

    return pdFALSE;
}

void vPortEndScheduler( void )
{
}

void vPortEnterCritical( void )
{
    portDISABLE_INTERRUPTS();
    uxCriticalNesting++;
}

void vPortExitCritical( void )
{
    uxCriticalNesting--;

    if( uxCriticalNesting == 0 )
    {
        if( ulYieldPending != 0 )
        {
            ulYieldPending = 0;
            vPortYield(); /* interrupts back on with the task switched to */
        }

        portENABLE_INTERRUPTS();
    }
}

void vPortYieldFromTask( void )
{
    if( ulPortInterruptNesting != 0 )
    {
        xPortSwitchRequired = 1;
    }
    else if( uxCriticalNesting != 0 )
    {
        ulYieldPending = 1;
    }
    else
    {
        vPortYield();
    }
}

BaseType_t xPortIsInsideInterrupt( void )
{
    return ( ulPortInterruptNesting != 0 ) ? pdTRUE : pdFALSE;
}

/* Called by the TIMER0 interrupt */
void xPortSysTickHandler( void )
{
    if( xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED )
    {
        if( xTaskIncrementTick() != pdFALSE )
        {
            xPortSwitchRequired = 1;
        }
    }
}
