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
 * OpenDMR, DMR: the main firmware's HR_C7000 engine (hr_c7000.c) behind the radio service. From
 * firmware/dh4570_IAP user/main.c (the HR_C7000 interrupt handlers of the OpenDM32 releases), functions/trx.c
 * (trxSetModeAndBandwidth(), trxSetFrequency() in DMR), hardware/radioHardwareInterface.c (radioSetMode()) and
 * applicationMain.c / uiUtilities.c (lastHeardListUpdate() with the LC of a call).
 */

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "hw.h"
#include "hal.h"
#include "factory_cal.h"
#include "last_heard.h"
#include "dmr_ids.h"
#include "ui.h"
#include "sound.h"
#include "dmr_compat.h"
#include "hr_c7000.h"
#include "radio.h"
#include "dmr.h"
#include "p25_phy.h"

#define VECTOR_FIRST       32
#define IRQ_SYSTEM         (61 - VECTOR_FIRST)
#define IRQ_TIMESLOT_TX    (62 - VECTOR_FIRST)
#define IRQ_TIMESLOT_RX    (63 - VECTOR_FIRST)
#define IRQ_TX_BURST_START (64 - VECTOR_FIRST)
#define IRQ_RF_RX_START    (65 - VECTOR_FIRST)

#define FLCO_GROUP   0 // DMR_frame_buffer[0]: the full link control opcode
#define FLCO_PRIVATE 3
#define FLCO_GPS     8 // ETSI GPS Info (TS 102 361-2 7.1.1.3): an embedded LC like a talker alias block
#define FID_STANDARD 0 // DMR_frame_buffer[1]: the feature set ID; others are a maker's own LCs (another layout)

/* The interrupts (user/main.c of the releases) */

// While the physical layer receive runs (p25_phy.c), the DMR engine stands aside: its interrupts are only cleared
static void RAM_CODE system_isr(void)
{
    if (p25_phy_active()) {
        p25_phy_isr();
    } else if (trxGetMode() == RADIO_MODE_DIGITAL) {
        hrc7000SetInIRQHandler(true);
        hrc7000SysInterruptHandler();
        hrc7000SetInIRQHandler(false);
    }
    while (*SYS_INTERP_LIST) {
        *SYS_INTERP_CLEAR = *SYS_INTERP_LIST;
    }
}

// The timeslot: vector 62 is the 30 ms transmit timeslot interrupt, 63 the receive one (HR_C7000 manual, baseband
// interrupt list): two counters on the same timeslot boundary. The receive counter runs once the repeater's timeslots
// are received, the transmit one from the wake-up (alone until the repeater answers) until the end of the reception
// after a transmission; while both run, 62 comes first. The handler (OpenGD77: one interrupt a timeslot) runs once a
// timeslot: from 62 in its states, else from 63. Run by both in the timeslot where DMR_STATE_TX_END_3_RMO goes on to
// receive, the time code was toggled twice: one timeslot off the repeater, and the next transmission went out on the
// other timeslot (TS1, TS2, TS1 ...)
static volatile bool timeslot_handled = false; // by 62, in this timeslot

static void RAM_CODE timeslot_rx_isr(void)
{
    bool handled = timeslot_handled;

    timeslot_handled = false;
    if (p25_phy_active()) {
        p25_phy_rx_slot_isr();
    } else if ((trxGetMode() == RADIO_MODE_DIGITAL) && !handled &&
        ((slotState < DMR_STATE_TX_START_1) || (slotState == DMR_STATE_REPEATER_WAKE_3))) {
        hrc7000SetInIRQHandler(true);
        hrc7000TimeslotInterruptHandler();
        hrc7000SetInIRQHandler(false);
    }
    *INTERP_CLEAR |= 0x01;
    *INTERP_CLEAR |= 0x02;
}

static void RAM_CODE timeslot_tx_isr(void)
{
    if (p25_phy_tx_active()) {
        p25_phy_tx_isr(); // the physical layer transmit test: the next 30 ms block
    } else if (!p25_phy_active() &&(trxGetMode() == RADIO_MODE_DIGITAL) && (slotState >= DMR_STATE_TX_START_1)) {
        hrc7000SetInIRQHandler(true);
        hrc7000TimeslotInterruptHandler();
        hrc7000SetInIRQHandler(false);
        timeslot_handled = true;
    }
    *INTERP_CLEAR |= 0x01;
    *INTERP_CLEAR |= 0x02;
}

// The start of a Tx burst: the RF on
static void RAM_CODE tx_burst_isr(void)
{
    *INTERP_CLEAR |= 0x04;
    if (!p25_phy_active() && (trxGetMode() == RADIO_MODE_DIGITAL)) {
        hrc7000SetInIRQHandler(true);
        hrc7000TxInterruptHandler();
        hrc7000SetInIRQHandler(false);
    }
}

// The end of a Tx burst (the RF receive start): the RF off
static void RAM_CODE rf_rx_isr(void)
{
    *INTERP_CLEAR |= 0x08;
    if (!p25_phy_active() && (trxGetMode() == RADIO_MODE_DIGITAL)) {
        hrc7000SetInIRQHandler(true);
        hrc7000RfRxInterruptHandler();
        hrc7000SetInIRQHandler(false);
    }
}

void dmr_init(void)
{
    HRC7000InitInterrupts();
    irq_register(IRQ_SYSTEM, system_isr);
    irq_register(IRQ_TIMESLOT_TX, timeslot_tx_isr);
    irq_register(IRQ_TIMESLOT_RX, timeslot_rx_isr);
    irq_register(IRQ_TX_BURST_START, tx_burst_isr);
    irq_register(IRQ_RF_RX_START, rf_rx_isr);
    dmr_sound_init();
}

void dmr_start(void)
{
    HRC7000InitTask();
}

/* The mode and the channel */

// radioSetMode(RADIO_MODE_DIGITAL) of the releases: the modem's DMR calibration and the factory receive level; the
// radio service has written the FD6818's DMR table and tuned it
void dmr_enter(const codeplug_channel_t *channel)
{
    dmr_compat_set_channel(channel);
    HRC7000SetDMRModemCalibration();
    *GCR_DACL = factory_cal_rx_dac_gain();
    dmr_channel(channel);
}

void dmr_retune(void)
{
    HRC7000TerminateDigital();
}

// trxSetFrequency(): direct or repeater mode, then the engine started again. The call shown ends (the channel and VFO
// screens of OpenGD77 set the default screen on a channel change): the restarted engine has no QSO timeout to end it
void dmr_channel(const codeplug_channel_t *channel)
{
    CPU_SR_ALLOC();

    uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
    dmr_compat_set_channel(channel);
    HRC7000ClearColorCodeSynchronisation(); // as uiChannelMode.c after trxSetDMRColourCode()
    HRC7000UpdateRepeaterMode();
    CPU_CRITICAL_ENTER();
    HRC7000InitDigital();
    CPU_CRITICAL_EXIT();
}

// trxSetModeAndBandwidth(RADIO_MODE_ANALOG): the engine stopped, the modem back to direct mode
void dmr_leave(void)
{
    static const codeplug_channel_t fm = {.mode = CODEPLUG_MODE_FM};

    uiDataGlobal.displayQSOState = QSO_DISPLAY_DEFAULT_SCREEN;
    HRC7000TerminateDigital();
    dmr_compat_set_channel(&fm);
    HRC7000UpdateRepeaterMode();
}

void dmr_modem_calibration(void)
{
    HRC7000SetDMRModemCalibration();
}

/* Transmitting (uiTxScreen.c): the engine transmits while trxTransmissionEnabled, from its timeslot interrupts */

void dmr_transmit(bool on)
{
    HRC7000ClearIsWakingState();
    if (on && (settings.location_lat != SETTINGS_UNITIALISED_LOCATION_LAT)) {
        HRC7000SetTalkerAliasLocation(settings.location_lat, settings.location_lon);
    }
    trxTransmissionEnabled = on;
}

// The talk group or private call (tx_target.c): the engine sends it and filters the private calls with it
void dmr_set_tx_id(uint32_t id)
{
    trxTalkGroupOrPcId = id;
}

void dmr_set_own_id(uint32_t id)
{
    trxDMRID = id;
}

// The HR_C7000 found the timeslots of a signal (scan: HRC7000HasGotSync())
bool dmr_has_sync(void)
{
    return HRC7000HasGotSync();
}

bool dmr_is_transmitting(void)
{
    return trxTransmissionEnabled || trxIsTransmitting;
}

bool dmr_repeater_failed(void)
{
    return (HRC7000GetIsWakingState() == WAKING_MODE_FAILED);
}

/* The call: the LC of the engine to the last heard list and the UI (lastHeardListUpdate()) */

#define TA_BLOCKS     4  // LC opcodes 0x04 - 0x07: the talker alias header and 3 blocks
#define TA_BLOCK_SIZE 7
#define TA_SIZE       32

static dmr_call_t call;
static volatile bool call_shown = false;
static int qso_state = QSO_DISPLAY_IDLE;

// The talker alias being put together (lastHeardClearWorkingTAData())
static struct {
    uint8_t buffer[TA_SIZE];
    uint8_t blocks; // bit n: block n received
    bool override;  // the header changed: the new alias replaces the shown one
    bool caller;    // the caller is known: its blocks may come
    char shown[TA_SIZE];
} ta;

static void post(void)
{
    ui_event_t ev = {.type = UI_EVENT_DATA, .what = UI_DATA_SIGNAL};

    ui_post(&ev);
}

static void ta_clear(void)
{
    memset(&ta, 0, sizeof(ta));
}

// decodeTA(): 7 bit, ISO 8 bit / UTF-8, UTF-16 (the low bytes); its length
static int ta_decode(char *out, int size, const uint8_t *alias)
{
    int format = (alias[0] >> 6) & 0x03;
    int length = (alias[0] >> 1) & 0x1F;
    int count = 0;

    memset(out, 0, (size_t)size);
    if (length >= size) {
        length = size - 1;
    }
    if (format == 0) {
        uint8_t bits = 0;
        uint8_t c = 0;

        for (int i = 0; (i < TA_SIZE) && (count < length); i++) {
            for (int j = 7; j >= 0; j--) {
                c = (uint8_t)((c << 1) | ((alias[i] >> j) & 1));
                if (++bits == 7) {
                    if (i > 0) {
                        out[count++] = (char)(c & 0x7F);
                    }
                    bits = 0;
                    c = 0;
                }
            }
        }
    } else if (format == 3) {
        for (int i = 0; (i < 15) && (count < length); i++) {
            out[count++] = (alias[(2 * i) + 1] == 0) ? (char)alias[(2 * i) + 2] : '?';
        }
    } else {
        memcpy(out, alias + 1, (size_t)(size - 1));
        count = (int)strlen(out);
    }
    out[length] = '\0';
    return (int)strlen(out);
}

// The ETSI GPS Info LC (FLCO 8): four reserved bits, the position error (3 bits), the longitude (25 bits, two's
// complement, 360 / 2^25 degree) and the latitude (24 bits, two's complement, 180 / 2^24 degree). false: the LC
// carries no position (0 / 0; RadioFoundry's dmr_gps_decode() does the same).
static bool gps_decode(const volatile uint8_t *lc, int32_t *latitude_e5, int32_t *longitude_e5)
{
    uint32_t raw_lon = ((uint32_t)(lc[2] & 0x01) << 24) | ((uint32_t)lc[3] << 16) | ((uint32_t)lc[4] << 8) | lc[5];
    uint32_t raw_lat = ((uint32_t)lc[6] << 16) | ((uint32_t)lc[7] << 8) | lc[8];
    int32_t lat, lon;
    double lat_deg, lon_deg;

    if ((raw_lon == 0) && (raw_lat == 0)) {
        return false;
    }
    lat = ((raw_lat & 0x00800000UL) != 0) ? ((int32_t)raw_lat - 0x01000000) : (int32_t)raw_lat;
    lon = ((raw_lon & 0x01000000UL) != 0) ? ((int32_t)raw_lon - 0x02000000) : (int32_t)raw_lon;
    lat_deg = (double)lat * (180.0 / 16777216.0);
    lon_deg = (double)lon * (360.0 / 33554432.0);
    *latitude_e5 = (int32_t)((lat_deg * 100000.0) + ((lat >= 0) ? 0.5 : -0.5));
    *longitude_e5 = (int32_t)((lon_deg * 100000.0) + ((lon >= 0) ? 0.5 : -0.5));
    return (*latitude_e5 >= -9000000) && (*latitude_e5 <= 9000000) && (*longitude_e5 >= -18000000) &&
           (*longitude_e5 <= 18000000);
}

// A talker alias block (block 0: the header with the format and the length)
static void ta_block(const volatile uint8_t *lc)
{
    int block = lc[0] - 4;
    char alias[TA_SIZE];
    char *p;

    if ((block == 0) && (ta.blocks & 1) && (ta.buffer[0] != lc[2])) {
        // another alias: the blocks received are from the old one
        ta.blocks = 0;
        memset(ta.buffer + TA_BLOCK_SIZE, 0, TA_SIZE - TA_BLOCK_SIZE);
        ta.override = true;
    }
    if (ta.blocks & (1 << block)) {
        return;
    }
    ta.blocks |= (uint8_t)(1 << block);
    for (int i = 0; i < TA_BLOCK_SIZE; i++) {
        ta.buffer[(block * TA_BLOCK_SIZE) + i] = lc[2 + i];
    }
    if ((ta.buffer[0] == 0) || (ta_decode(alias, sizeof(alias), ta.buffer) == 0)) {
        return;
    }
    if (!ta.override && (strlen(alias) <= strlen(ta.shown))) {
        return;
    }
    // Brandmeister sends "DMR ID:xxxxxxx" after the callsign
    if ((ta.blocks & 2) && (((p = strstr(alias, "DMR ID:")) != NULL) || ((p = strstr(alias, "DMR I")) != NULL))) {
        *p = '\0';
    }
    ta.override = false;
    snprintf(ta.shown, sizeof(ta.shown), "%s", alias);
    last_heard_set_talker_alias(call.source, ta.shown);
    post();
}

// The caller's text (updateLHItem()): its contact or its DMR ID database entry, looked up once a caller
static const char *caller_text(uint32_t source)
{
    static uint32_t looked_up = 0;
    static char text[LAST_HEARD_TEXT_SIZE];

    if (source != looked_up) {
        looked_up = source;
        dmr_ids_caller_text(source, text, sizeof(text));
    }
    return text[0] ? text : NULL;
}

/* The receive beeps the engine asks for (applicationMain.c rxBeepsHandler()) */

#define BEEP_RX_CARRIER      0x04 // settings.beep_options
#define BEEP_RX_TALKER       0x08
#define BEEP_RX_TALKER_BEGIN 0x10
#define CARRIER_END_DELAY_MS 150  // after a talker end beep that may come at the same time

static bool must_beep(uint8_t option)
{
    return !radio_is_scanning() && (settingsUsbMode != USB_MODE_HOTSPOT) && (settings.beep_options & option) &&
           (settings.audio_prompt_mode != SOUND_PROMPT_SILENT);
}

static void carrier_end_beep(void)
{
    sound_play(SOUND_RX_END);
}

static void rx_beeps(void)
{
    volatile uint8_t *state = &uiDataGlobal.rxBeepState;

    if (*state & RX_BEEP_CARRIER_HAS_STARTED_EXEC) {
        *state &= (uint8_t)~RX_BEEP_CARRIER_HAS_STARTED_EXEC;
        // a carrier end beep waiting: the same over, no beeps
        if (must_beep(BEEP_RX_CARRIER) && !cancelTimerCallback(carrier_end_beep, MENU_ANY)) {
            sound_play(SOUND_RX_BEGIN);
        }
    } else if (*state & RX_BEEP_TALKER_HAS_STARTED_EXEC) {
        *state &= (uint8_t)~(RX_BEEP_TALKER_HAS_STARTED_EXEC | RX_BEEP_TALKER_HAS_ENDED | RX_BEEP_TALKER_HAS_ENDED_EXEC);
        if ((settings.beep_options & BEEP_RX_TALKER_BEGIN) && must_beep(BEEP_RX_TALKER)) {
            sound_play(SOUND_RX_TALKER_BEGIN);
        }
    } else if (*state & RX_BEEP_TALKER_HAS_ENDED_EXEC) {
        *state &= (uint8_t)~(RX_BEEP_TALKER_HAS_ENDED_EXEC | RX_BEEP_TALKER_HAS_STARTED | RX_BEEP_TALKER_HAS_STARTED_EXEC |
                             RX_BEEP_TALKER_IDENTIFIED);
        if (must_beep(BEEP_RX_TALKER)) {
            sound_play(SOUND_RX_TALKER_END);
        }
    } else if (*state & RX_BEEP_CARRIER_HAS_ENDED) {
        *state = RX_BEEP_UNSET;
        if (must_beep(BEEP_RX_CARRIER)) {
            (void)addTimerCallback(carrier_end_beep, CARRIER_END_DELAY_MS, MENU_ANY, false);
        }
    }
}

// After each tick of the engine (its task)
void dmr_after_tick(void)
{
    rx_beeps();
    if (!trxTransmissionEnabled && updateLastHeard) {
        const volatile uint8_t *lc = DMR_frame_buffer;

        updateLastHeard = false;
        if (HRC7000GetReceivedTgOrPcId() == 0) {
            // nothing
        } else if ((lc[0] == FLCO_GROUP) || (lc[0] == FLCO_PRIVATE)) {
            uint32_t source = ((uint32_t)lc[6] << 16) | ((uint32_t)lc[7] << 8) | lc[8];
            uint32_t destination = ((uint32_t)lc[3] << 16) | ((uint32_t)lc[4] << 8) | lc[5];

            if ((source != 0) && (destination != 0)) {
                if (source != call.source) {
                    ta_clear();
                    call.has_location = false;
                }
                ta.caller = true;
                call.source = source;
                call.tg_or_pc = ((uint32_t)lc[0] << 24) | destination;
                call.timeslot = (uint8_t)((dmrMonitorCapturedTS != -1) ? dmrMonitorCapturedTS : trxGetDMRTimeSlot());
                call_shown = true;
                // shown: the engine's QSO timeout sets it back to the default screen at the end of the call (the
                // main firmware's UI sets it when it draws the caller)
                uiDataGlobal.displayQSOState = QSO_DISPLAY_CALLER_DATA;
                qso_state = QSO_DISPLAY_CALLER_DATA;
                last_heard_add(call.source, call.tg_or_pc, call.timeslot, caller_text(call.source));
                post();
            }
        } else if (ta.caller && (lc[0] >= 4) && (lc[0] < (4 + TA_BLOCKS))) {
            ta_block(lc);
        } else if (call_shown && (lc[0] == FLCO_GPS) && (lc[1] == FID_STANDARD)) {
            int32_t latitude_e5, longitude_e5;

            if (gps_decode(lc, &latitude_e5, &longitude_e5)) {
                call.has_location = true;
                call.latitude_e5 = latitude_e5;
                call.longitude_e5 = longitude_e5;
                last_heard_set_location(call.source, latitude_e5, longitude_e5);
                post();
            }
        }
    }
    if (uiDataGlobal.displayQSOState != qso_state) {
        qso_state = uiDataGlobal.displayQSOState;
        if (qso_state != QSO_DISPLAY_CALLER_DATA) {
            call_shown = false;
        }
        post();
    }
}

bool dmr_call(dmr_call_t *out)
{
    if (!call_shown) {
        return false;
    }
    *out = call;
    return true;
}
