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

#ifndef DMR_COMPAT_H
#define DMR_COMPAT_H

/*
 * OpenDMR, DMR: what the main firmware's HR_C7000 engine (hr_c7000.c, kept as it was) uses from the rest of
 * that firmware, given by this firmware (dmr_compat.c). Names and types are the main firmware's.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "FreeRTOS.h"
#include "task.h"
#include "settings.h"
#include "codeplug.h"
#include "hw.h"
#include "hal.h"

/* Registers (user/DRV/C7000_DRV.h) */

#undef BITCLR
#undef BITSET
#undef SYS_SOFT_RSTN
#undef IO_DIPLEX2
#define BITCLR(n) (~(0x01L << (n)))
#define BITSET(n) (0x01L << (n))

#define MODEM_REG_BASE 0x11000000
#define CODEC_BASE     0x160009C0
#define DAC            0x140F0000

#define SYS_SOFT_RSTN      (volatile uint32_t *)(MODEM_REG_BASE + 0x0000)
#define IO_DIPLEX2         (volatile uint32_t *)(MODEM_REG_BASE + 0x003c)
#define DAC_CONTROL        (volatile uint32_t *)(MODEM_REG_BASE + 0x0070)
#define ADC_CONTROL        (volatile uint32_t *)(MODEM_REG_BASE + 0x0074)
#define AUDIO_CONTROL      (volatile uint32_t *)(MODEM_REG_BASE + 0x0080)
#define AUDIO_BUFFER_CLR   (volatile uint32_t *)(MODEM_REG_BASE + 0x0084)
#define LINEOUT_CTRL       (volatile uint32_t *)(MODEM_REG_BASE + 0x0088)
#define WORK_MODE          (volatile uint32_t *)(MODEM_REG_BASE + 0x0100)
#define RF_MODE            (volatile uint32_t *)(MODEM_REG_BASE + 0x0104)
#define SIG_CENTER         (volatile uint32_t *)(MODEM_REG_BASE + 0x0108)
#define TX_IF_FREQ         (volatile uint32_t *)(MODEM_REG_BASE + 0x010C)
#define RF_CONTROL         (volatile uint32_t *)(MODEM_REG_BASE + 0x0110)
#define RF_MOD_BIAS_CTRL   (volatile uint32_t *)(MODEM_REG_BASE + 0x0114)
#define DEV_LIMITER        (volatile uint32_t *)(MODEM_REG_BASE + 0x0118)
#define RSSI_BIAS_VALUE    (volatile uint32_t *)(MODEM_REG_BASE + 0x011C)
#define THRESHOLD_VALUE    (volatile uint32_t *)(MODEM_REG_BASE + 0x0120)
#define SEND_DATA_SYNC_H   (volatile uint32_t *)(MODEM_REG_BASE + 0x012C)
#define SEND_DATA_SYNC_L   (volatile uint32_t *)(MODEM_REG_BASE + 0x0130)
#define SEND_RC_SYNC_H     (volatile uint32_t *)(MODEM_REG_BASE + 0x0134)
#define SEND_RC_SYNC_L     (volatile uint32_t *)(MODEM_REG_BASE + 0x0138)
#define RECV_MS_SYNC_H     (volatile uint32_t *)(MODEM_REG_BASE + 0x013C)
#define RECV_MS_SYNC_L     (volatile uint32_t *)(MODEM_REG_BASE + 0x0140)
#define RECV_BS_SYNC_H     (volatile uint32_t *)(MODEM_REG_BASE + 0x0144)
#define RECV_BS_SYNC_L     (volatile uint32_t *)(MODEM_REG_BASE + 0x0148)
#define RECV_TDMA1_SYNC_H  (volatile uint32_t *)(MODEM_REG_BASE + 0x014C)
#define RECV_TDMA1_SYNC_L  (volatile uint32_t *)(MODEM_REG_BASE + 0x0150)
#define RECV_TDMA2_SYNC_H  (volatile uint32_t *)(MODEM_REG_BASE + 0x0154)
#define RECV_TDMA2_SYNC_L  (volatile uint32_t *)(MODEM_REG_BASE + 0x0158)
#define RECV_RC_SYNC_H     (volatile uint32_t *)(MODEM_REG_BASE + 0x015C)
#define RECV_RC_SYNC_L     (volatile uint32_t *)(MODEM_REG_BASE + 0x0160)
#define REG_X06            (volatile uint32_t *)(MODEM_REG_BASE + 0x0168)
#define RX_IF_FREQ         (volatile uint32_t *)(MODEM_REG_BASE + 0x01B0)
#define RF_RECV_DELAY      (volatile uint32_t *)(MODEM_REG_BASE + 0x01B4)
#define PI_CRC_MASK        (volatile uint32_t *)(MODEM_REG_BASE + 0x0350)
#define RS_H_MASK          (volatile uint32_t *)(MODEM_REG_BASE + 0x0354)
#define RS_H_INIT          (volatile uint32_t *)(MODEM_REG_BASE + 0x0358)
#define RS_T_MASK          (volatile uint32_t *)(MODEM_REG_BASE + 0x035C)
#define RS_T_INIT          (volatile uint32_t *)(MODEM_REG_BASE + 0x0360)
#define CSBK_CRC_MASK      (volatile uint32_t *)(MODEM_REG_BASE + 0x0364)
#define MBCH_CRC_MASK      (volatile uint32_t *)(MODEM_REG_BASE + 0x0368)
#define DATAH_CRC_MASK     (volatile uint32_t *)(MODEM_REG_BASE + 0x036C)
#define REG_X01            (volatile uint32_t *)(MODEM_REG_BASE + 0x0370)
#define REG_X02            (volatile uint32_t *)(MODEM_REG_BASE + 0x0374)
#define REG_X03            (volatile uint32_t *)(MODEM_REG_BASE + 0x0378)
#define REG_X04            (volatile uint32_t *)(MODEM_REG_BASE + 0x037C)
#define CRC8_INIT          (volatile uint32_t *)(MODEM_REG_BASE + 0x0380)
#define LOCAL_CC           (volatile uint32_t *)(MODEM_REG_BASE + 0x0384) // the colour code, Tx and Rx (OpenDM32 trx.c)
#define VOICE_EMB_CTRL     (volatile uint32_t *)(MODEM_REG_BASE + 0x0388)
#define SCRAMBLE_REG       (volatile uint32_t *)(MODEM_REG_BASE + 0x038C)
#define RX_TYPE_INFO       (volatile uint32_t *)(MODEM_REG_BASE + 0x0390)
#define SYS_INTERP_LIST    (volatile uint32_t *)(MODEM_REG_BASE + 0x0398)
#define SYS_INTERP_MASK    (volatile uint32_t *)(MODEM_REG_BASE + 0x039C)
#define SYS_INTERP_CLEAR   (volatile uint32_t *)(MODEM_REG_BASE + 0x03A0)
#define LAYER3_INTERP_LIST (volatile uint32_t *)(MODEM_REG_BASE + 0x03A4)
#define INTERP_CLEAR       (volatile uint32_t *)(MODEM_REG_BASE + 0x03B0)
#define LAYER2_CONTROL     (volatile uint32_t *)(MODEM_REG_BASE + 0x0400)
#define LAYER2_SLOTON      (volatile uint32_t *)(MODEM_REG_BASE + 0x0404)
#define LAYER2_TXRX_CTRL   (volatile uint32_t *)(MODEM_REG_BASE + 0x0408)
#define LAYER2_SLOT_CNT    (volatile uint32_t *)(MODEM_REG_BASE + 0x040C)
#define LAYER2_SEND_TYPE   (volatile uint32_t *)(MODEM_REG_BASE + 0x0418)
#define LAYER2_SLOT_PRE    (volatile uint32_t *)(MODEM_REG_BASE + 0x0424)
#define FM_BANDWIDTH       (volatile uint32_t *)(MODEM_REG_BASE + 0x0500)
#define FM_DEV_COEF        (volatile uint32_t *)(MODEM_REG_BASE + 0x0504)
#define SUBVOICE_COEF      (volatile uint32_t *)(MODEM_REG_BASE + 0x050C)
#define FM_MOD             (volatile uint32_t *)(MODEM_REG_BASE + 0x0510)
#define CTCSS_ADDR         (volatile uint32_t *)(MODEM_REG_BASE + 0x0518)
#define TONE_SEND_CTRL     (volatile uint32_t *)(MODEM_REG_BASE + 0x0528)
#define FM_TONE1           (volatile uint32_t *)(MODEM_REG_BASE + 0x0550)
#define FM_TONE2           (volatile uint32_t *)(MODEM_REG_BASE + 0x0554)
#define FM_TONE3           (volatile uint32_t *)(MODEM_REG_BASE + 0x0558)
#define FM_TONE4           (volatile uint32_t *)(MODEM_REG_BASE + 0x055C)
#define FM_PTT             (volatile uint32_t *)(MODEM_REG_BASE + 0x0560)
#define FM_DECODE_FLAG2    (volatile uint32_t *)(MODEM_REG_BASE + 0x0568)

#define MODEM_TX_RAM (volatile uint8_t *)(0x16000000)
#define MODEM_RX_RAM (volatile uint8_t *)(0x160004B0)
#define SOUND        (volatile uint8_t *)(0x16000980)
#define SOUNDBUFRX   (volatile uint8_t *)(0x180000A0)
#define SOUNDBUFTX   (volatile uint32_t *)(0x18000000)

#define AICR_DAC   (volatile uint8_t *)(CODEC_BASE + 0x08)
#define AICR_ADC   (volatile uint8_t *)(CODEC_BASE + 0x09)
#define CR_MIC1    (volatile uint8_t *)(CODEC_BASE + 0x0B)
#define CR_MIC2    (volatile uint8_t *)(CODEC_BASE + 0x0C)
#define CR_DAC     (volatile uint8_t *)(CODEC_BASE + 0x0D)
#define CR_ADC     (volatile uint8_t *)(CODEC_BASE + 0x0F)
#define CR_VIC     (volatile uint8_t *)(CODEC_BASE + 0x12)
#define CR_CK      (volatile uint8_t *)(CODEC_BASE + 0x13)
#define GCR_DACL   (volatile uint8_t *)(CODEC_BASE + 0x1F)
#define GCR_MIC1   (volatile uint8_t *)(CODEC_BASE + 0x23)
#define GCR_ADCL   (volatile uint8_t *)(CODEC_BASE + 0x25)
#define CR_DAC_AGC (volatile uint8_t *)(CODEC_BASE + 0x2B)
#define CR_ADC_AGC (volatile uint8_t *)(CODEC_BASE + 0x2F)
#define DR_ADC_AGC (volatile uint8_t *)(CODEC_BASE + 0x30)

#define DAC_PD_CTRL    (volatile uint32_t *)(DAC + 0x00)
#define DAC_PD_MODE_EN (volatile uint32_t *)(DAC + 0x04)
#define DAC_DATA_A     (volatile uint32_t *)(DAC + 0x08)
#define DAC_DATA_B     (volatile uint32_t *)(DAC + 0x0C)
#define DAC_DATA_C     (volatile uint32_t *)(DAC + 0x10)

/* The RTOS (uC/OS in the main firmware): critical sections, delays */

#define CPU_SR_ALLOC()       uint32_t cpu_sr = 0
#define CPU_CRITICAL_ENTER() (cpu_sr = dmr_critical_enter())
#define CPU_CRITICAL_EXIT()  dmr_critical_exit(cpu_sr)

uint32_t dmr_critical_enter(void); // interrupts off, from a task or an interrupt
void dmr_critical_exit(uint32_t state);

void dhtime_delay(uint32_t ms); // a task delay

// One shot callbacks (functions/ticks.c addTimerCallback()), run by the HR_C7000 task (dmr_compat_run_callbacks())
typedef void (*timerCallback_t)(void);
#define MENU_ANY 0
bool addTimerCallback(timerCallback_t callback, uint32_t delay_ms, int menu, bool update_existing);
bool cancelTimerCallback(timerCallback_t callback, int menu); // false: it wasn't waiting
void dmr_compat_run_callbacks(void);
void dmr_after_tick(void); // dmr.c, after each hrc7000Tick()
void Delay_us(uint32_t us);     // busy
void Delay_ms(uint32_t ms);

typedef struct {
    uint32_t start;
    uint32_t timeout;
} ticksTimer_t;

uint32_t ticksGetMillis(void);
void ticksTimerStart(ticksTimer_t *timer, uint32_t timeout);
bool ticksTimerHasExpired(ticksTimer_t *timer);
void ticksTimerReset(ticksTimer_t *timer);
bool ticksTimerIsEnabled(ticksTimer_t *timer);

/* The radio (functions/trx.c): the channel the radio service tuned (dmr_compat_set_channel()) */

enum { RADIO_MODE_NONE = 0, RADIO_MODE_ANALOG, RADIO_MODE_DIGITAL };
enum { DMR_MODE_AUTO = 0, DMR_MODE_DMO, DMR_MODE_RMO, DMR_MODE_SFR };
typedef enum { RADIO_DEVICE_PRIMARY = 0 } RadioDevice_t;

typedef struct {
    int currentMode;
    int trxDMRModeRx;
    int trxDMRModeTx;
    bool currentBandWidthIs25kHz;
} radio_device_t;

extern radio_device_t *currentRadioDevice;
extern codeplug_channel_t *currentChannelData;

extern volatile bool trxTransmissionEnabled;
extern volatile bool trxIsTransmitting;
extern volatile bool trxIsTransmittingDMR;
extern volatile uint32_t trxDMRstartTime;
extern volatile bool trxDMRSynchronisedRSSIReadPending;
extern uint32_t trxTalkGroupOrPcId;
extern uint32_t trxDMRID;

int trxGetMode(void);
uint32_t trxGetFrequency(void);
bool trxGetBandwidthIs25kHz(void);
uint8_t trxGetDMRColourCode(void);
void trxSetDMRColourCode(uint8_t colour_code);
int trxGetDMRTimeSlot(void);
int trxGetSNRMargindBm(RadioDevice_t device);
void trxSetModeAndBandwidth(int mode, bool wide);
void trxSetRxCSS(RadioDevice_t device, uint16_t tone);
void trxActivateRx(bool critical);
void trxActivateDMRTx(void);
void trxFastDMRTx(bool on);
void dmr_compat_set_channel(const codeplug_channel_t *channel); // trxSetFrequency() and loadChannelData() of it
void dmr_compat_frequency_calibration(void); // the channel's frequency calibration again (the radio service's)

/* Settings and the codeplug */

#define nonVolatileSettings settings
#define dmrCcTsFilter        dmr_cc_ts_filter
#define dmrCaptureTimeout    dmr_capture_timeout
#define micGainDMR           mic_gain_dmr
#define locationLat          location_lat
#define dmrDestinationFilter dmr_destination_filter
#define privateCalls         private_calls

#define DMR_CC_FILTER_PATTERN 0x01
#define DMR_TS_FILTER_PATTERN 0x02
enum { DMR_DESTINATION_FILTER_NONE = 0, DMR_DESTINATION_FILTER_TG, DMR_DESTINATION_FILTER_DC, DMR_DESTINATION_FILTER_RXG };
enum { ALLOW_PRIVATE_CALLS_OFF = 0, ALLOW_PRIVATE_CALLS_ON, ALLOW_PRIVATE_CALLS_PTT, ALLOW_PRIVATE_CALLS_AUTO };
#define SETTINGS_UNITIALISED_LOCATION_LAT 0x7F000000
#define BIT_DMR_CRC_IGNORED               (1UL << 12)
bool settingsIsOptionBitSet(uint32_t bit);

typedef enum { CHANNEL_FLAG_FORCE_DMO = 0 } ChannelFlag_t;
uint8_t codeplugChannelGetFlag(codeplug_channel_t *channel, ChannelFlag_t flag);
#define CODEPLUG_CSS_TONE_NONE 0xFFFF
#define CODEPLUG_MIN_VARIABLE_SQUELCH 1
#define ALL_CALL_VALUE 16777215
#define TA_TX_OFF  0
#define TA_TX_APRS 1
#define TA_TX_TEXT 2
#define TA_TX_BOTH 3

typedef struct {
    int NOT_IN_CODEPLUG_numTGsInGroup;
    uint32_t NOT_IN_CODEPLUG_contactsTG[32];
} rx_group_t;

extern rx_group_t currentRxGroupData;

bool codeplugContactsContainsPC(uint32_t pc);
int codeplugGetRepeaterWakeAttempts(void);
double latLongFixed32ToDouble(uint32_t fixed);

/* The UI state the engine sets (uiDataGlobal, monitorModeData of the main firmware) */

#define RX_BEEP_UNSET                   0x00
#define RX_BEEP_CARRIER_HAS_STARTED     0x01
#define RX_BEEP_CARRIER_HAS_STARTED_EXEC 0x02
#define RX_BEEP_TALKER_IDENTIFIED       0x04
#define RX_BEEP_TALKER_HAS_STARTED      0x08
#define RX_BEEP_TALKER_HAS_STARTED_EXEC 0x10
#define RX_BEEP_TALKER_HAS_ENDED        0x20
#define RX_BEEP_TALKER_HAS_ENDED_EXEC   0x40
#define RX_BEEP_CARRIER_HAS_ENDED       0x80

enum { QSO_DISPLAY_IDLE = 0, QSO_DISPLAY_DEFAULT_SCREEN, QSO_DISPLAY_CALLER_DATA, QSO_DISPLAY_CALLER_DATA_UPDATE };
enum { SCAN_STATE_SCANNING = 0, SCAN_STATE_SHORT_PAUSED, SCAN_STATE_PAUSED };
enum { PRIVATE_CALL_NOT_IN_CALL = 0, PRIVATE_CALL_ACCEPT, PRIVATE_CALL, PRIVATE_CALL_DECLINED };

typedef struct {
    volatile uint8_t rxBeepState;
    volatile int displayQSOState;
    struct {
        bool active;
        int state;
    } Scan;
    struct {
        int state;
    } PrivateCall;
} ui_data_global_t;

typedef struct {
    bool isEnabled;
    bool triggered;
    volatile bool dmrIsValid;
    volatile bool qsoInfoUpdated;
    volatile int dmrFrameSkip;
    volatile int dmrTimeout;
} monitor_mode_data_t;

extern ui_data_global_t uiDataGlobal;
extern monitor_mode_data_t monitorModeData;
extern bool headerRowIsDirty;

enum { USB_MODE_CPS = 0, USB_MODE_HOTSPOT };
extern volatile int settingsUsbMode;
void lastHeardClearLastID(void);

/* LEDs, audio */

typedef enum { LED_GREEN_R = 0, LED_RED_R } LEDs_t;
void LedWrite(LEDs_t led, uint8_t on);
uint8_t LedRead(LEDs_t led);

#define AUDIO_AMP_MODE_RF (1 << 1)
void enableAudioAmp(uint8_t mode);
void disableAudioAmp(uint8_t mode);
uint8_t getAudioAmpStatus(void);
int8_t getVolumeControl(void);

bool voicePromptsIsPlaying(void);
bool soundMelodyIsPlaying(void);

/* The audio of the calls (dmr_sound.c; functions/sound.c, interfaces/i2s.c of the main firmware) */

#define HOTSPOT_BUFFER_COUNT 48U
#define HOTSPOT_BUFFER_SIZE  50U
#define WAV_BUFFER_SIZE  160
#define WAV_BUFFER_COUNT 30
extern volatile int16_t wavbuffer_read_idx;
extern volatile int16_t wavbuffer_write_idx;
extern volatile int16_t wavbuffer_count;
extern volatile bool isReceiving;
void soundInit(void);
void soundTerminateSound(void);
bool soundReceiveData(void);
void soundStopMicCapture(void);
void soundTickRXBuffer(void);
void soundSetupBuffer(void);    // currentWaveBuffer: the next one to fill
void soundStoreBuffer(void);    // it is filled
void soundRetrieveBuffer(void); // currentWaveBuffer: the next one to read
extern uint8_t *currentWaveBuffer;
void dmr_sound_init(void);      // the PCM interrupts (dmr_sound.c)

// The wav buffers of the main firmware (sound.h): the decoded audio, the mic; the hotspot's frames (hotspot.c)
typedef union {
    volatile uint8_t wavbuffer[WAV_BUFFER_COUNT][WAV_BUFFER_SIZE];
    volatile uint8_t hotspotBuffer[HOTSPOT_BUFFER_COUNT][HOTSPOT_BUFFER_SIZE];
} audio_and_hotspot_buffer_t;
extern audio_and_hotspot_buffer_t audioAndHotspotDataBuffer;

void codecInit(bool from_voice_prompts);
void codecDecode(uint8_t *data, int blocks);
void codecEncodeBlock(uint8_t *data);

/* The hotspot (services/hotspot.c) */

#define HOTSPOT_RX_IDLE         0
#define HOTSPOT_RX_START        1
#define HOTSPOT_RX_START_LATE   2
#define HOTSPOT_RX_AUDIO_FRAME  3
#define HOTSPOT_RX_STOP         4
void hotspotRxFrameHandler(uint8_t *frame); // a received frame, from the engine's task

/* The interrupts of the HR_C7000 (vectors 61 - 64) */

void interruptsInitC7000Interface(void);
void interruptsEnableC7000Interrupts(void);
void interruptsDisableC7000Interrupts(void);

#endif
