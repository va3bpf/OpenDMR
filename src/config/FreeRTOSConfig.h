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
 * OpenDMR bring-up: kernel configuration. Static allocation only (no heap), 1 kHz tick on TIMER0 as uC/OS had.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#define configCPU_CLOCK_HZ                      168000000
#define configTICK_RATE_HZ                      1000
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configMAX_PRIORITIES                    8
#define configMINIMAL_STACK_SIZE                256 // words
#define configMAX_TASK_NAME_LEN                 12
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_QUEUE_SETS                    0
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configUSE_MINI_LIST_ITEM                1
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        uint32_t

#define configSUPPORT_STATIC_ALLOCATION     1
#define configSUPPORT_DYNAMIC_ALLOCATION    0
#define configKERNEL_PROVIDED_STATIC_MEMORY 1 // the idle task's TCB and stack

#define configUSE_IDLE_HOOK                  0
#define configUSE_TICK_HOOK                  1 // vApplicationTickHook(): the 1 ms timer watch (systime.c)
#define configCHECK_FOR_STACK_OVERFLOW       0
#define configUSE_MALLOC_FAILED_HOOK         0
#define configUSE_DAEMON_TASK_STARTUP_HOOK   0
#define configGENERATE_RUN_TIME_STATS        0
#define configUSE_TRACE_FACILITY             0
#define configUSE_STATS_FORMATTING_FUNCTIONS 0
#define configUSE_CO_ROUTINES                0
#define configUSE_TIMERS                     0
#define configUSE_EVENT_GROUPS               0
#define configUSE_STREAM_BUFFERS             0

#define INCLUDE_vTaskPrioritySet            1
#define INCLUDE_uxTaskPriorityGet           1
#define INCLUDE_vTaskDelete                 0
#define INCLUDE_vTaskSuspend                1
#define INCLUDE_vTaskDelayUntil             1
#define INCLUDE_vTaskDelay                  1
#define INCLUDE_xTaskGetSchedulerState      1
#define INCLUDE_xTaskGetCurrentTaskHandle   1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle      0
#define INCLUDE_eTaskGetState               0
#define INCLUDE_xTaskResumeFromISR          1

// A failed check stops here, readable with a debugger (freertos_assert_line)
extern volatile unsigned long freertos_assert_line;
#define configASSERT(x)                                                                                                \
    do {                                                                                                               \
        if ((x) == 0) {                                                                                                \
            __asm__ volatile("psrclr ie");                                                                             \
            freertos_assert_line = __LINE__;                                                                           \
            for (;;) {                                                                                                 \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

#endif /* FREERTOS_CONFIG_H */
