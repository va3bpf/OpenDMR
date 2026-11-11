/*
 * Copyright (C) 2019      Kai Ludwig, DG4KLU
 * Copyright (C) 2019-2024 Roger Clark, VK3KYY / G4KYF
 * Copyright (C) 2026      Junqiang Shi, VA3BPF / BG8AM
 *
 * Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
 *    in the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * 4. Use of this source code or binary releases for commercial purposes is strictly forbidden. This includes, without limitation,
 *    incorporation in a commercial product or incorporation into a product or project which allows commercial use.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 * USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * OpenDMR, DMR: the main firmware's functions and data the HR_C7000 engine (hr_c7000.c) uses, given by this
 * firmware (dmr_compat.h). From firmware/dh4570_IAP functions/trx.c, ticks.c, settings.c, codeplug.c, io/LEDs.c,
 * functions/sound.c (the receive buffers), user/main.c (the interrupt masks).
 */

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "systime.h"
#include "leds.h"
#include "sound.h"
#include "factory_cal.h"
#include "modem.h"
#include "radio.h"
#include "dmr_compat.h"

#define CALLBACKS          4

/* The RTOS */

uint32_t RAM_CODE dmr_critical_enter(void)
{
    return irq_save();
}

void RAM_CODE dmr_critical_exit(uint32_t state)
{
    irq_restore(state);
}

void dhtime_delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void RAM_CODE Delay_us(uint32_t us)
{
    delay_us(us);
}

void Delay_ms(uint32_t ms)
{
    delay_ms(ms);
}

uint32_t RAM_CODE ticksGetMillis(void)
{
    return systime_get_ms();
}

void RAM_CODE ticksTimerStart(ticksTimer_t *timer, uint32_t timeout)
{
    timer->start = systime_get_ms();
    timer->timeout = timeout;
}

bool RAM_CODE ticksTimerHasExpired(ticksTimer_t *timer)
{
    return (timer->timeout == 0) || ((systime_get_ms() - timer->start) >= timer->timeout);
}

void ticksTimerReset(ticksTimer_t *timer)
{
    timer->start = 0;
    timer->timeout = 0;
}

bool ticksTimerIsEnabled(ticksTimer_t *timer)
{
    return (timer->timeout != 0);
}

static struct {
    timerCallback_t callback;
    uint32_t due_ms;
} callbacks[CALLBACKS];

bool RAM_CODE addTimerCallback(timerCallback_t callback, uint32_t delay_ms, int menu, bool update_existing)
{
    CPU_SR_ALLOC();
    int free = -1;

    (void)menu;
    CPU_CRITICAL_ENTER();
    for (int i = 0; i < CALLBACKS; i++) {
        if (callbacks[i].callback == callback) {
            if (update_existing) {
                callbacks[i].due_ms = systime_get_ms() + delay_ms;
            }
            CPU_CRITICAL_EXIT();
            return true;
        }
        if ((callbacks[i].callback == NULL) && (free < 0)) {
            free = i;
        }
    }
    if (free >= 0) {
        callbacks[free].callback = callback;
        callbacks[free].due_ms = systime_get_ms() + delay_ms;
    }
    CPU_CRITICAL_EXIT();
    return (free >= 0);
}

bool cancelTimerCallback(timerCallback_t callback, int menu)
{
    CPU_SR_ALLOC();
    bool found = false;

    (void)menu;
    CPU_CRITICAL_ENTER();
    for (int i = 0; i < CALLBACKS; i++) {
        if (callbacks[i].callback == callback) {
            callbacks[i].callback = NULL;
            found = true;
        }
    }
    CPU_CRITICAL_EXIT();
    return found;
}

void dmr_compat_run_callbacks(void)
{
    for (int i = 0; i < CALLBACKS; i++) {
        timerCallback_t callback = callbacks[i].callback;

        if (callback && ((int32_t)(systime_get_ms() - callbacks[i].due_ms) >= 0)) {
            callbacks[i].callback = NULL;
            callback();
        }
    }
}

/* The radio: the channel the radio service tuned */

static radio_device_t device = {.currentMode = RADIO_MODE_ANALOG};
static codeplug_channel_t channel;
radio_device_t *currentRadioDevice = &device;
codeplug_channel_t *currentChannelData = &channel;
rx_group_t currentRxGroupData;

volatile bool trxTransmissionEnabled = false;
volatile bool trxIsTransmitting = false;
volatile bool trxIsTransmittingDMR = false;
volatile uint32_t trxDMRstartTime = 0;
volatile bool trxDMRSynchronisedRSSIReadPending = false;
uint32_t trxTalkGroupOrPcId = 0;
uint32_t trxDMRID = 0;

static uint8_t colour_code = 0;

// trxSetFrequency(), trxSetDMRColourCode(), the RX group list of loadChannelData(); the contact (trxTalkGroupOrPcId)
// and the timeslot are the UI's (tx_target.c)
void dmr_compat_set_channel(const codeplug_channel_t *tuned)
{
    codeplug_contact_t contact;
    codeplug_rx_group_t group;
    bool dmo = (tuned->rx_freq == tuned->tx_freq) || (tuned->libre_dmr_flag1 & 0x04); // FORCE_DMO

    memcpy(&channel, tuned, sizeof(channel));
    device.currentMode = (tuned->mode == CODEPLUG_MODE_DMR) ? RADIO_MODE_DIGITAL : RADIO_MODE_ANALOG;
    device.trxDMRModeRx = dmo ? DMR_MODE_DMO : DMR_MODE_RMO;
    device.trxDMRModeTx = device.trxDMRModeRx;
    device.currentBandWidthIs25kHz = false;
    trxSetDMRColourCode(tuned->tx_colour & 0x0F);
    trxDMRID = codeplug_get_dmr_id();

    currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup = 0;
    if ((tuned->rx_group_list != 0) && codeplug_get_rx_group(tuned->rx_group_list, &group)) {
        for (int i = 0; (i < group.contact_count) && (i < CODEPLUG_RX_GROUP_CONTACTS); i++) {
            if (codeplug_get_contact(group.contacts[i], &contact)) {
                currentRxGroupData.NOT_IN_CODEPLUG_contactsTG[currentRxGroupData.NOT_IN_CODEPLUG_numTGsInGroup++] =
                    contact.id;
            }
        }
    }
}

int RAM_CODE trxGetMode(void)
{
    return device.currentMode;
}

uint32_t trxGetFrequency(void)
{
    return channel.rx_freq;
}

bool trxGetBandwidthIs25kHz(void)
{
    return false; // DMR: 12.5 kHz
}

uint8_t trxGetDMRColourCode(void)
{
    return colour_code;
}

// The modem's colour code too (OpenDM32 trx.c *LOCAL_CC): without it the HR_C7000 kept its reset value, 1, and sent and
// received colour code 1 whatever the channel's (GitHub issue #3)
void trxSetDMRColourCode(uint8_t code)
{
    colour_code = code & 0x0F;
    *LOCAL_CC = colour_code;
}

int trxGetDMRTimeSlot(void)
{
    return (channel.flag2 & 0x40) ? 1 : 0;
}

int trxGetSNRMargindBm(RadioDevice_t radio)
{
    (void)radio;
    return (int)radio_signal_raw() - (int)radio_noise(); // the dBm offsets are the same
}

// Monitor mode only (none here)
void trxSetModeAndBandwidth(int mode, bool wide)
{
    (void)mode;
    (void)wide;
}

void trxSetRxCSS(RadioDevice_t radio, uint16_t tone)
{
    (void)radio;
    (void)tone;
}

// Transmitting DMR: from the engine's timeslot interrupts (trx.c, radioHardwareInterface.c)
void RAM_CODE trxActivateRx(bool critical)
{
    (void)critical;
    trxIsTransmittingDMR = false;
    radio_dmr_rx_on();
    trxIsTransmitting = false;
}

void RAM_CODE trxActivateDMRTx(void)
{
    radio_dmr_tx_on();
    trxIsTransmitting = true;
    trxIsTransmittingDMR = true;
}

void RAM_CODE trxFastDMRTx(bool on)
{
    radio_dmr_burst(on);
}

void dmr_compat_frequency_calibration(void)
{
    const factory_cal_t *cal = factory_cal_get();

    modem_set_frequency_calibration(cal->valid, cal->osc_tune, cal->rx_adjust, factory_cal_mod_bias(channel.tx_freq));
}

/* Settings and the codeplug */

bool settingsIsOptionBitSet(uint32_t bit)
{
    return (settings.bitfield_options & bit) != 0;
}

uint8_t codeplugChannelGetFlag(codeplug_channel_t *data, ChannelFlag_t flag)
{
    (void)flag; // CHANNEL_FLAG_FORCE_DMO
    return (data->libre_dmr_flag1 & 0x04) ? 1 : 0;
}

bool codeplugContactsContainsPC(uint32_t pc)
{
    return codeplug_contact_find(pc, CODEPLUG_CALL_PRIVATE, 0) != 0;
}

int codeplugGetRepeaterWakeAttempts(void)
{
    return 4; // hard coded in the main firmware too
}

// The sign, 8 bits of integer, 23 bits of decimals in 1 / 100000 (uiUtilities.c)
#define LOCATION_DECIMALS 100000

double latLongFixed32ToDouble(uint32_t fixed)
{
    double value = (double)((fixed & 0x7FFFFFFF) >> 23) + ((double)(fixed & 0x7FFFFF) / LOCATION_DECIMALS);

    return (fixed & 0x80000000) ? -value : value;
}

/* The UI state */

ui_data_global_t uiDataGlobal;
monitor_mode_data_t monitorModeData;
bool headerRowIsDirty = false;
volatile int settingsUsbMode = USB_MODE_CPS;

void lastHeardClearLastID(void)
{
}

/* LEDs, audio */

static uint8_t led_state[2];

void RAM_CODE LedWrite(LEDs_t led, uint8_t on)
{
    led_state[led] = on;
    if (led == LED_GREEN_R) {
        leds_green(on != 0);
    } else {
        leds_red(on != 0);
    }
}

uint8_t RAM_CODE LedRead(LEDs_t led)
{
    return led_state[led];
}

static volatile uint8_t amp_modes = 0;

void RAM_CODE enableAudioAmp(uint8_t mode)
{
    amp_modes |= mode;
    radio_dmr_audio(true);
}

void RAM_CODE disableAudioAmp(uint8_t mode)
{
    amp_modes &= (uint8_t)~mode;
    if (amp_modes == 0) {
        radio_dmr_audio(false);
    }
}

uint8_t getAudioAmpStatus(void)
{
    return amp_modes;
}

int8_t getVolumeControl(void)
{
    return 0; // HRC7000SetDmrRxGain() does nothing on the DM-32
}

bool voicePromptsIsPlaying(void)
{
    return false;
}

bool soundMelodyIsPlaying(void)
{
    return sound_is_playing();
}

/* The interrupts of the HR_C7000 (user/main.c). The main firmware's mask functions set and cleared PIC_MASK_1 bits
   29 - 31, which are vectors 93 - 95: its HR_C7000 interrupts stayed on from boot (each handler checks for DMR), and the
   engine was only ever run that way. Kept so: dmr.c registers them once, these do nothing. */

void interruptsInitC7000Interface(void)
{
}

void interruptsEnableC7000Interrupts(void)
{
}

void interruptsDisableC7000Interrupts(void)
{
}
