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
 * OpenDMR, service: beeps and melodies. After OpenDM32 functions/sound.c (the melodies,
 * soundSetMelody(), soundTickMelody(), soundBeepTaskFunction()), on the baseband's prompt path (modem.c).
 *
 * A melody is pairs of (Hz, ms), 0 Hz a pause, ended by -1, -1. The sound task plays it as the main firmware: the
 * speaker amplifier on, a delay for it to start, then for each millisecond a block of 16 samples (2 ms at 8 kHz) into
 * the prompt FIFO when it has room (it paces the samples), from the 2048 point sine stepped by Hz / 3.915, the volume a
 * shift by settings.beep_volume_divider.
 */

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "settings.h"
#include "modem.h"
#include "radio.h"
#include "battery.h"
#include "custom_data.h"
#include "sound.h"

#define SOUND_TASK_PRIORITY 3
#define AMP_START_MS        75 // DELAYED_BEEP_DURATION_MS: the amplifier needs this before the first note
#define PATH_SETTLE_MS      5  // the audio path switched, before the amplifier goes on
#define AMP_STOP_MS         20 // the amplifier off, before the audio path switches back
#define LOW_BATTERY_MS      (5 * 60 * 1000) // LOW_BATTERY_INTERVAL: the low battery beep again after this
#define CHECK_MS            1000 // the idle task looks at the battery this often

#define CUSTOM_DATA_TYPE_BEEP 2   // the boot melody of the codeplug: (note, length) byte pairs, ended by a 0 length
#define BOOT_NOTES_MAX        255 // soundCreateSong()
#define BOOT_NOTE_MS          27  // a length unit

#define DIT 60
#define DAH (3 * DIT)

const int16_t SOUND_POWER_ON[] = {880, DAH, 0, DIT, 880, DIT, 0, DIT, 880, DAH, 0, DIT, 880, DIT, 0, DIT, 880, DAH, -1, -1};
const int16_t SOUND_KEY[] = {600, 60, -1, -1};
const int16_t SOUND_KEY_LONG[] = {880, 60, -1, -1};
const int16_t SOUND_KEY_FIRST_ITEM[] = {800, 100, -1, -1};
const int16_t SOUND_ACK[] = {440, 120, 660, 120, 880, 120, -1, -1};
const int16_t SOUND_NACK[] = {494, 120, 466, 120, -1, -1};
const int16_t SOUND_ERROR[] = {440, 30, 0, 30, 440, 30, 0, 30, 440, 30, -1, -1};
const int16_t SOUND_TX_TIMEOUT[] = {440, 60, 494, 60, 440, 60, 494, 60, 440, 60, 494, 60, 440, 60, 494, 60, -1, -1};
const int16_t SOUND_LOW_BATTERY[] = {440, 200, 415, 200, 392, 200, -1, -1};
const int16_t SOUND_APO_TRIGGERED[] = {440, 100, 392, 100, 440, 100, 392, 100, 440, 100, 392, 100, -1, -1};
const int16_t SOUND_RX_BEGIN[] = {1397, 60, -1, -1};
const int16_t SOUND_RX_END[] = {698, 60, -1, -1};
const int16_t SOUND_RX_TALKER_BEGIN[] = {660, 30, 1200, 30, -1, -1};
const int16_t SOUND_RX_TALKER_END[] = {880, 30, 660, 30, -1, -1};
const int16_t SOUND_DMR_TX_START[] = {800, 50, -1, -1};
const int16_t SOUND_DMR_TX_STOP[] = {500, 50, -1, -1};
const int16_t SOUND_1750[] = {1750, 10000, -1, -1};
static int16_t dtmf_melody[] = {0, 10000, -1, -1}; // sound_play_dtmf(): the low tone here, the high one aside

#define SOUND_TX_TONES_DATA_TYPE 0x44540001 // a custom data block of this firmware ("DT", 1)

// The patterns of the FM transmit tones
static const int16_t TX_TONE_BEEP[] = {1000, 120, -1, -1};
static const int16_t TX_TONE_DOUBLE[] = {1000, 60, 0, 60, 1000, 60, -1, -1};
static const int16_t TX_TONE_RISING[] = {800, 80, 1200, 80, -1, -1};
static const int16_t TX_TONE_FALLING[] = {1200, 80, 800, 80, -1, -1};
static const int16_t TX_TONE_MORSE_K[] = {1000, DAH, 0, DIT, 1000, DIT, 0, DIT, 1000, DAH, -1, -1};
static const int16_t *const tx_tones[SOUND_TX_TONE_PATTERNS] = {
    NULL, TX_TONE_BEEP, TX_TONE_DOUBLE, TX_TONE_RISING, TX_TONE_FALLING, TX_TONE_MORSE_K};

// Off but for the air level and the local volume, until set
sound_tx_tones_t sound_tx_tones = {.air_level = 5, .local_volume = 2};
static bool tx_tones_changed = false;

// The boot melody notes: semitones from 104 Hz (freqs[] of the main firmware), 0 a pause
static const int16_t notes[64] = {
    0,    104,  110,  117,  123,  131,  139,  147,  156,  165,  175,  185,  196,  208,  220,  233,
    247,  262,  277,  294,  311,  330,  349,  370,  392,  415,  440,  466,  494,  523,  554,  587,
    622,  659,  698,  740,  784,  831,  880,  932,  988,  1047, 1109, 1175, 1245, 1319, 1397, 1480,
    1568, 1661, 1760, 1865, 1976, 2093, 2217, 2349, 2489, 2637, 2794, 2960, 3136, 3322, 3520, 3729};
static int16_t boot_melody[(2 * BOOT_NOTES_MAX) + 2];

// One period, 2048 points (sine_beep16[] of the main firmware)
static const int16_t sine[2048] = {
    0, 101, 201, 302, 402, 503, 603, 704, 804, 905, 1005, 1106, 1206, 1307, 1407, 1507,
    1608, 1708, 1809, 1909, 2009, 2110, 2210, 2310, 2410, 2511, 2611, 2711, 2811, 2911, 3012, 3112,
    3212, 3312, 3412, 3512, 3612, 3712, 3811, 3911, 4011, 4111, 4210, 4310, 4410, 4509, 4609, 4708,
    4808, 4907, 5007, 5106, 5205, 5305, 5404, 5503, 5602, 5701, 5800, 5899, 5998, 6096, 6195, 6294,
    6393, 6491, 6590, 6688, 6786, 6885, 6983, 7081, 7179, 7277, 7375, 7473, 7571, 7669, 7767, 7864,
    7962, 8059, 8157, 8254, 8351, 8448, 8545, 8642, 8739, 8836, 8933, 9030, 9126, 9223, 9319, 9416,
    9512, 9608, 9704, 9800, 9896, 9992, 10087, 10183, 10278, 10374, 10469, 10564, 10659, 10754, 10849, 10944,
    11039, 11133, 11228, 11322, 11417, 11511, 11605, 11699, 11793, 11886, 11980, 12074, 12167, 12260, 12353, 12446,
    12539, 12632, 12725, 12817, 12910, 13002, 13094, 13187, 13279, 13370, 13462, 13554, 13645, 13736, 13828, 13919,
    14010, 14101, 14191, 14282, 14372, 14462, 14553, 14643, 14732, 14822, 14912, 15001, 15090, 15180, 15269, 15358,
    15446, 15535, 15623, 15712, 15800, 15888, 15976, 16063, 16151, 16238, 16325, 16413, 16499, 16586, 16673, 16759,
    16846, 16932, 17018, 17104, 17189, 17275, 17360, 17445, 17530, 17615, 17700, 17784, 17869, 17953, 18037, 18121,
    18204, 18288, 18371, 18454, 18537, 18620, 18703, 18785, 18868, 18950, 19032, 19113, 19195, 19276, 19357, 19438,
    19519, 19600, 19680, 19761, 19841, 19921, 20000, 20080, 20159, 20238, 20317, 20396, 20475, 20553, 20631, 20709,
    20787, 20865, 20942, 21019, 21096, 21173, 21250, 21326, 21403, 21479, 21554, 21630, 21705, 21781, 21856, 21930,
    22005, 22079, 22154, 22227, 22301, 22375, 22448, 22521, 22594, 22667, 22739, 22812, 22884, 22956, 23027, 23099,
    23170, 23241, 23311, 23382, 23452, 23522, 23592, 23662, 23731, 23801, 23870, 23938, 24007, 24075, 24143, 24211,
    24279, 24346, 24413, 24480, 24547, 24613, 24680, 24746, 24811, 24877, 24942, 25007, 25072, 25137, 25201, 25265,
    25329, 25393, 25456, 25519, 25582, 25645, 25708, 25770, 25832, 25893, 25955, 26016, 26077, 26138, 26198, 26259,
    26319, 26378, 26438, 26497, 26556, 26615, 26674, 26732, 26790, 26848, 26905, 26962, 27019, 27076, 27133, 27189,
    27245, 27300, 27356, 27411, 27466, 27521, 27575, 27629, 27683, 27737, 27790, 27843, 27896, 27949, 28001, 28053,
    28105, 28157, 28208, 28259, 28310, 28360, 28411, 28460, 28510, 28560, 28609, 28658, 28706, 28755, 28803, 28850,
    28898, 28945, 28992, 29039, 29085, 29131, 29177, 29223, 29268, 29313, 29358, 29403, 29447, 29491, 29534, 29578,
    29621, 29664, 29706, 29749, 29791, 29832, 29874, 29915, 29956, 29997, 30037, 30077, 30117, 30156, 30195, 30234,
    30273, 30311, 30349, 30387, 30424, 30462, 30498, 30535, 30571, 30607, 30643, 30679, 30714, 30749, 30783, 30818,
    30852, 30885, 30919, 30952, 30985, 31017, 31050, 31082, 31113, 31145, 31176, 31206, 31237, 31267, 31297, 31327,
    31356, 31385, 31414, 31442, 31470, 31498, 31526, 31553, 31580, 31607, 31633, 31659, 31685, 31710, 31736, 31760,
    31785, 31809, 31833, 31857, 31880, 31903, 31926, 31949, 31971, 31993, 32014, 32036, 32057, 32077, 32098, 32118,
    32137, 32157, 32176, 32195, 32213, 32232, 32250, 32267, 32285, 32302, 32318, 32335, 32351, 32367, 32382, 32397,
    32412, 32427, 32441, 32455, 32469, 32482, 32495, 32508, 32521, 32533, 32545, 32556, 32567, 32578, 32589, 32599,
    32609, 32619, 32628, 32637, 32646, 32655, 32663, 32671, 32678, 32685, 32692, 32699, 32705, 32711, 32717, 32722,
    32728, 32732, 32737, 32741, 32745, 32748, 32752, 32755, 32757, 32759, 32761, 32763, 32765, 32766, 32766, 32767,
    32767, 32767, 32766, 32766, 32765, 32763, 32761, 32759, 32757, 32755, 32752, 32748, 32745, 32741, 32737, 32732,
    32728, 32722, 32717, 32711, 32705, 32699, 32692, 32685, 32678, 32671, 32663, 32655, 32646, 32637, 32628, 32619,
    32609, 32599, 32589, 32578, 32567, 32556, 32545, 32533, 32521, 32508, 32495, 32482, 32469, 32455, 32441, 32427,
    32412, 32397, 32382, 32367, 32351, 32335, 32318, 32302, 32285, 32267, 32250, 32232, 32213, 32195, 32176, 32157,
    32137, 32118, 32098, 32077, 32057, 32036, 32014, 31993, 31971, 31949, 31926, 31903, 31880, 31857, 31833, 31809,
    31785, 31760, 31736, 31710, 31685, 31659, 31633, 31607, 31580, 31553, 31526, 31498, 31470, 31442, 31414, 31385,
    31356, 31327, 31297, 31267, 31237, 31206, 31176, 31145, 31113, 31082, 31050, 31017, 30985, 30952, 30919, 30885,
    30852, 30818, 30783, 30749, 30714, 30679, 30643, 30607, 30571, 30535, 30498, 30462, 30424, 30387, 30349, 30311,
    30273, 30234, 30195, 30156, 30117, 30077, 30037, 29997, 29956, 29915, 29874, 29832, 29791, 29749, 29706, 29664,
    29621, 29578, 29534, 29491, 29447, 29403, 29358, 29313, 29268, 29223, 29177, 29131, 29085, 29039, 28992, 28945,
    28898, 28850, 28803, 28755, 28706, 28658, 28609, 28560, 28510, 28460, 28411, 28360, 28310, 28259, 28208, 28157,
    28105, 28053, 28001, 27949, 27896, 27843, 27790, 27737, 27683, 27629, 27575, 27521, 27466, 27411, 27356, 27300,
    27245, 27189, 27133, 27076, 27019, 26962, 26905, 26848, 26790, 26732, 26674, 26615, 26556, 26497, 26438, 26378,
    26319, 26259, 26198, 26138, 26077, 26016, 25955, 25893, 25832, 25770, 25708, 25645, 25582, 25519, 25456, 25393,
    25329, 25265, 25201, 25137, 25072, 25007, 24942, 24877, 24811, 24746, 24680, 24613, 24547, 24480, 24413, 24346,
    24279, 24211, 24143, 24075, 24007, 23938, 23870, 23801, 23731, 23662, 23592, 23522, 23452, 23382, 23311, 23241,
    23170, 23099, 23027, 22956, 22884, 22812, 22739, 22667, 22594, 22521, 22448, 22375, 22301, 22227, 22154, 22079,
    22005, 21930, 21856, 21781, 21705, 21630, 21554, 21479, 21403, 21326, 21250, 21173, 21096, 21019, 20942, 20865,
    20787, 20709, 20631, 20553, 20475, 20396, 20317, 20238, 20159, 20080, 20000, 19921, 19841, 19761, 19680, 19600,
    19519, 19438, 19357, 19276, 19195, 19113, 19032, 18950, 18868, 18785, 18703, 18620, 18537, 18454, 18371, 18288,
    18204, 18121, 18037, 17953, 17869, 17784, 17700, 17615, 17530, 17445, 17360, 17275, 17189, 17104, 17018, 16932,
    16846, 16759, 16673, 16586, 16499, 16413, 16325, 16238, 16151, 16063, 15976, 15888, 15800, 15712, 15623, 15535,
    15446, 15358, 15269, 15180, 15090, 15001, 14912, 14822, 14732, 14643, 14553, 14462, 14372, 14282, 14191, 14101,
    14010, 13919, 13828, 13736, 13645, 13554, 13462, 13370, 13279, 13187, 13094, 13002, 12910, 12817, 12725, 12632,
    12539, 12446, 12353, 12260, 12167, 12074, 11980, 11886, 11793, 11699, 11605, 11511, 11417, 11322, 11228, 11133,
    11039, 10944, 10849, 10754, 10659, 10564, 10469, 10374, 10278, 10183, 10087, 9992, 9896, 9800, 9704, 9608,
    9512, 9416, 9319, 9223, 9126, 9030, 8933, 8836, 8739, 8642, 8545, 8448, 8351, 8254, 8157, 8059,
    7962, 7864, 7767, 7669, 7571, 7473, 7375, 7277, 7179, 7081, 6983, 6885, 6786, 6688, 6590, 6491,
    6393, 6294, 6195, 6096, 5998, 5899, 5800, 5701, 5602, 5503, 5404, 5305, 5205, 5106, 5007, 4907,
    4808, 4708, 4609, 4509, 4410, 4310, 4210, 4111, 4011, 3911, 3811, 3712, 3612, 3512, 3412, 3312,
    3212, 3112, 3012, 2911, 2811, 2711, 2611, 2511, 2410, 2310, 2210, 2110, 2009, 1909, 1809, 1708,
    1608, 1507, 1407, 1307, 1206, 1106, 1005, 905, 804, 704, 603, 503, 402, 302, 201, 101,
    0, -101, -201, -302, -402, -503, -603, -704, -804, -905, -1005, -1106, -1206, -1307, -1407, -1507,
    -1608, -1708, -1809, -1909, -2009, -2110, -2210, -2310, -2410, -2511, -2611, -2711, -2811, -2911, -3012, -3112,
    -3212, -3312, -3412, -3512, -3612, -3712, -3811, -3911, -4011, -4111, -4210, -4310, -4410, -4509, -4609, -4708,
    -4808, -4907, -5007, -5106, -5205, -5305, -5404, -5503, -5602, -5701, -5800, -5899, -5998, -6096, -6195, -6294,
    -6393, -6491, -6590, -6688, -6786, -6885, -6983, -7081, -7179, -7277, -7375, -7473, -7571, -7669, -7767, -7864,
    -7962, -8059, -8157, -8254, -8351, -8448, -8545, -8642, -8739, -8836, -8933, -9030, -9126, -9223, -9319, -9416,
    -9512, -9608, -9704, -9800, -9896, -9992, -10087, -10183, -10278, -10374, -10469, -10564, -10659, -10754, -10849, -10944,
    -11039, -11133, -11228, -11322, -11417, -11511, -11605, -11699, -11793, -11886, -11980, -12074, -12167, -12260, -12353, -12446,
    -12539, -12632, -12725, -12817, -12910, -13002, -13094, -13187, -13279, -13370, -13462, -13554, -13645, -13736, -13828, -13919,
    -14010, -14101, -14191, -14282, -14372, -14462, -14553, -14643, -14732, -14822, -14912, -15001, -15090, -15180, -15269, -15358,
    -15446, -15535, -15623, -15712, -15800, -15888, -15976, -16063, -16151, -16238, -16325, -16413, -16499, -16586, -16673, -16759,
    -16846, -16932, -17018, -17104, -17189, -17275, -17360, -17445, -17530, -17615, -17700, -17784, -17869, -17953, -18037, -18121,
    -18204, -18288, -18371, -18454, -18537, -18620, -18703, -18785, -18868, -18950, -19032, -19113, -19195, -19276, -19357, -19438,
    -19519, -19600, -19680, -19761, -19841, -19921, -20000, -20080, -20159, -20238, -20317, -20396, -20475, -20553, -20631, -20709,
    -20787, -20865, -20942, -21019, -21096, -21173, -21250, -21326, -21403, -21479, -21554, -21630, -21705, -21781, -21856, -21930,
    -22005, -22079, -22154, -22227, -22301, -22375, -22448, -22521, -22594, -22667, -22739, -22812, -22884, -22956, -23027, -23099,
    -23170, -23241, -23311, -23382, -23452, -23522, -23592, -23662, -23731, -23801, -23870, -23938, -24007, -24075, -24143, -24211,
    -24279, -24346, -24413, -24480, -24547, -24613, -24680, -24746, -24811, -24877, -24942, -25007, -25072, -25137, -25201, -25265,
    -25329, -25393, -25456, -25519, -25582, -25645, -25708, -25770, -25832, -25893, -25955, -26016, -26077, -26138, -26198, -26259,
    -26319, -26378, -26438, -26497, -26556, -26615, -26674, -26732, -26790, -26848, -26905, -26962, -27019, -27076, -27133, -27189,
    -27245, -27300, -27356, -27411, -27466, -27521, -27575, -27629, -27683, -27737, -27790, -27843, -27896, -27949, -28001, -28053,
    -28105, -28157, -28208, -28259, -28310, -28360, -28411, -28460, -28510, -28560, -28609, -28658, -28706, -28755, -28803, -28850,
    -28898, -28945, -28992, -29039, -29085, -29131, -29177, -29223, -29268, -29313, -29358, -29403, -29447, -29491, -29534, -29578,
    -29621, -29664, -29706, -29749, -29791, -29832, -29874, -29915, -29956, -29997, -30037, -30077, -30117, -30156, -30195, -30234,
    -30273, -30311, -30349, -30387, -30424, -30462, -30498, -30535, -30571, -30607, -30643, -30679, -30714, -30749, -30783, -30818,
    -30852, -30885, -30919, -30952, -30985, -31017, -31050, -31082, -31113, -31145, -31176, -31206, -31237, -31267, -31297, -31327,
    -31356, -31385, -31414, -31442, -31470, -31498, -31526, -31553, -31580, -31607, -31633, -31659, -31685, -31710, -31736, -31760,
    -31785, -31809, -31833, -31857, -31880, -31903, -31926, -31949, -31971, -31993, -32014, -32036, -32057, -32077, -32098, -32118,
    -32137, -32157, -32176, -32195, -32213, -32232, -32250, -32267, -32285, -32302, -32318, -32335, -32351, -32367, -32382, -32397,
    -32412, -32427, -32441, -32455, -32469, -32482, -32495, -32508, -32521, -32533, -32545, -32556, -32567, -32578, -32589, -32599,
    -32609, -32619, -32628, -32637, -32646, -32655, -32663, -32671, -32678, -32685, -32692, -32699, -32705, -32711, -32717, -32722,
    -32728, -32732, -32737, -32741, -32745, -32748, -32752, -32755, -32757, -32759, -32761, -32763, -32765, -32766, -32766, -32767,
    -32767, -32767, -32766, -32766, -32765, -32763, -32761, -32759, -32757, -32755, -32752, -32748, -32745, -32741, -32737, -32732,
    -32728, -32722, -32717, -32711, -32705, -32699, -32692, -32685, -32678, -32671, -32663, -32655, -32646, -32637, -32628, -32619,
    -32609, -32599, -32589, -32578, -32567, -32556, -32545, -32533, -32521, -32508, -32495, -32482, -32469, -32455, -32441, -32427,
    -32412, -32397, -32382, -32367, -32351, -32335, -32318, -32302, -32285, -32267, -32250, -32232, -32213, -32195, -32176, -32157,
    -32137, -32118, -32098, -32077, -32057, -32036, -32014, -31993, -31971, -31949, -31926, -31903, -31880, -31857, -31833, -31809,
    -31785, -31760, -31736, -31710, -31685, -31659, -31633, -31607, -31580, -31553, -31526, -31498, -31470, -31442, -31414, -31385,
    -31356, -31327, -31297, -31267, -31237, -31206, -31176, -31145, -31113, -31082, -31050, -31017, -30985, -30952, -30919, -30885,
    -30852, -30818, -30783, -30749, -30714, -30679, -30643, -30607, -30571, -30535, -30498, -30462, -30424, -30387, -30349, -30311,
    -30273, -30234, -30195, -30156, -30117, -30077, -30037, -29997, -29956, -29915, -29874, -29832, -29791, -29749, -29706, -29664,
    -29621, -29578, -29534, -29491, -29447, -29403, -29358, -29313, -29268, -29223, -29177, -29131, -29085, -29039, -28992, -28945,
    -28898, -28850, -28803, -28755, -28706, -28658, -28609, -28560, -28510, -28460, -28411, -28360, -28310, -28259, -28208, -28157,
    -28105, -28053, -28001, -27949, -27896, -27843, -27790, -27737, -27683, -27629, -27575, -27521, -27466, -27411, -27356, -27300,
    -27245, -27189, -27133, -27076, -27019, -26962, -26905, -26848, -26790, -26732, -26674, -26615, -26556, -26497, -26438, -26378,
    -26319, -26259, -26198, -26138, -26077, -26016, -25955, -25893, -25832, -25770, -25708, -25645, -25582, -25519, -25456, -25393,
    -25329, -25265, -25201, -25137, -25072, -25007, -24942, -24877, -24811, -24746, -24680, -24613, -24547, -24480, -24413, -24346,
    -24279, -24211, -24143, -24075, -24007, -23938, -23870, -23801, -23731, -23662, -23592, -23522, -23452, -23382, -23311, -23241,
    -23170, -23099, -23027, -22956, -22884, -22812, -22739, -22667, -22594, -22521, -22448, -22375, -22301, -22227, -22154, -22079,
    -22005, -21930, -21856, -21781, -21705, -21630, -21554, -21479, -21403, -21326, -21250, -21173, -21096, -21019, -20942, -20865,
    -20787, -20709, -20631, -20553, -20475, -20396, -20317, -20238, -20159, -20080, -20000, -19921, -19841, -19761, -19680, -19600,
    -19519, -19438, -19357, -19276, -19195, -19113, -19032, -18950, -18868, -18785, -18703, -18620, -18537, -18454, -18371, -18288,
    -18204, -18121, -18037, -17953, -17869, -17784, -17700, -17615, -17530, -17445, -17360, -17275, -17189, -17104, -17018, -16932,
    -16846, -16759, -16673, -16586, -16499, -16413, -16325, -16238, -16151, -16063, -15976, -15888, -15800, -15712, -15623, -15535,
    -15446, -15358, -15269, -15180, -15090, -15001, -14912, -14822, -14732, -14643, -14553, -14462, -14372, -14282, -14191, -14101,
    -14010, -13919, -13828, -13736, -13645, -13554, -13462, -13370, -13279, -13187, -13094, -13002, -12910, -12817, -12725, -12632,
    -12539, -12446, -12353, -12260, -12167, -12074, -11980, -11886, -11793, -11699, -11605, -11511, -11417, -11322, -11228, -11133,
    -11039, -10944, -10849, -10754, -10659, -10564, -10469, -10374, -10278, -10183, -10087, -9992, -9896, -9800, -9704, -9608,
    -9512, -9416, -9319, -9223, -9126, -9030, -8933, -8836, -8739, -8642, -8545, -8448, -8351, -8254, -8157, -8059,
    -7962, -7864, -7767, -7669, -7571, -7473, -7375, -7277, -7179, -7081, -6983, -6885, -6786, -6688, -6590, -6491,
    -6393, -6294, -6195, -6096, -5998, -5899, -5800, -5701, -5602, -5503, -5404, -5305, -5205, -5106, -5007, -4907,
    -4808, -4708, -4609, -4509, -4410, -4310, -4210, -4111, -4011, -3911, -3811, -3712, -3612, -3512, -3412, -3312,
    -3212, -3112, -3012, -2911, -2811, -2711, -2611, -2511, -2410, -2310, -2210, -2110, -2009, -1909, -1809, -1708,
    -1608, -1507, -1407, -1307, -1206, -1106, -1005, -905, -804, -704, -603, -503, -402, -302, -201, -101,
};

static StaticTask_t task_tcb;
static StackType_t task_stack[256];
static TaskHandle_t task = NULL;

static const int16_t *volatile next = NULL; // asked, not started yet
static volatile int8_t next_volume = -1;     // its volume divider; -1: settings.beep_volume_divider
static volatile uint16_t next_hz2 = 0;        // its second tone (DTMF) with each note, 0: none
static volatile bool stop = false;
static volatile bool playing = false;

// Plays one note (0 Hz: a pause) for ms milliseconds, as soundBeepTaskFunction(); with hz2 the two tones summed at
// half each (DTMF); false: stopped
static bool note(int32_t hz, int32_t hz2, uint32_t ms, int32_t *index, int32_t *index2, int shift)
{
    uint8_t block[MODEM_BEEP_BLOCK];

    for (uint32_t t = 0; t < ms; t++) {
        if (stop || next) {
            return false;
        }
        if (!modem_beep_fifo_full()) {
            for (int i = 0; i < (MODEM_BEEP_BLOCK / 2); i++) {
                int16_t sample = (hz != 0) ? (int16_t)(sine[*index] >> shift) : 0; // a pause: silence, not a level

                if (hz2 != 0) {
                    sample = (int16_t)((sine[*index] + sine[*index2]) >> (shift + 1));
                    *index2 += (int32_t)(hz2 / 3.915f);
                    while (*index2 >= 2048) {
                        *index2 -= 2048;
                    }
                }

                block[2 * i] = (uint8_t)sample; // low byte first (the main firmware's comments say otherwise)
                block[(2 * i) + 1] = (uint8_t)((uint16_t)sample >> 8);
                if (hz != 0) {
                    *index += (int32_t)(hz / 3.915f);
                    while (*index >= 2048) {
                        *index -= 2048;
                    }
                }
            }
            modem_beep_write(block);
        }
        vTaskDelay(1);
    }
    return true;
}

// batteryAndPowerManagement.c: the low battery beep when nothing plays, not while transmitting, then each 5 minutes
static void low_battery_check(void)
{
    static bool beeped = false;
    static TickType_t last;

    if (!battery_is_low()) {
        beeped = false;
    } else if (!radio_is_transmitting() && (next == NULL) &&
               (!beeped || ((xTaskGetTickCount() - last) >= pdMS_TO_TICKS(LOW_BATTERY_MS)))) {
        beeped = true;
        last = xTaskGetTickCount();
        next_volume = -1;
        next = SOUND_LOW_BATTERY;
    }
}

static void sound_task(void *arg)
{
    bool amp_on = false;

    (void)arg;

    for (;;) {
        while (next == NULL) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_MS));
            low_battery_check();
        }

        taskENTER_CRITICAL();
        const int16_t *melody = next;
        int shift = (next_volume >= 0) ? next_volume : settings.beep_volume_divider;
        int32_t index = 0;
        int32_t index2 = 0;
        int32_t hz2 = next_hz2;

        next = NULL;
        taskEXIT_CRITICAL();
        stop = false;
        playing = true;
        if (!amp_on) {
            radio_beep_path(true);
            modem_beep(true);
            vTaskDelay(pdMS_TO_TICKS(PATH_SETTLE_MS));
            radio_beep_amp(true);
            vTaskDelay(pdMS_TO_TICKS(AMP_START_MS));
            amp_on = true;
        }
        for (int i = 0; (melody[i] >= 0) && note(melody[i], ((melody[i] != 0) ? hz2 : 0), (uint32_t)melody[i + 1], &index, &index2, shift); i += 2) {
        }
        if (next == NULL) { // else another follows at once (DTMF digits): the amplifier stays on
            radio_beep_amp(false);
            vTaskDelay(pdMS_TO_TICKS(AMP_STOP_MS));
            modem_beep(false);
            radio_beep_path(false);
            amp_on = false;
        }
        playing = false;
    }
}

void sound_start(void)
{
    sound_tx_tones_t stored;

    if (custom_data_read(SOUND_TX_TONES_DATA_TYPE, (uint8_t *)&stored, sizeof(stored))) {
        sound_tx_tones = stored;
    }
    task = xTaskCreateStatic(sound_task, "sound", sizeof(task_stack) / sizeof(task_stack[0]), NULL,
                             SOUND_TASK_PRIORITY, task_stack, &task_tcb);
}

void sound_play_dtmf(uint16_t low, uint16_t high)
{
    taskENTER_CRITICAL();
    dtmf_melody[0] = (int16_t)low;
    next_hz2 = high;
    next_volume = -1;
    next = dtmf_melody;
    taskEXIT_CRITICAL();
    if (task) {
        xTaskNotifyGive(task);
    }
}

void sound_play(const int16_t *melody)
{
    if (((melody == SOUND_KEY) || (melody == SOUND_KEY_LONG)) &&
        (settings.audio_prompt_mode == SOUND_PROMPT_NO_KEY_BEEP)) {
        return;
    }
    sound_play_volume(melody, 0xFF);
}

void sound_play_volume(const int16_t *melody, uint8_t volume_divider)
{
    if (melody == NULL) {
        stop = true;
        return;
    }
    taskENTER_CRITICAL();
    next_volume = (volume_divider == 0xFF) ? -1 : (int8_t)volume_divider;
    next_hz2 = 0;
    next = melody;
    taskEXIT_CRITICAL(); // a melody playing stops at its next millisecond, this one follows
    if (task) {
        xTaskNotifyGive(task);
    }
}

// uiSplashScreen.c: the codeplug's boot melody (the CPS writes it), the power on melody without one, nothing when it
// is empty. Only with the boot screen option, as the DM-32 main firmware (it starts fast without).
void sound_play_boot(void)
{
    uint8_t data[2 * BOOT_NOTES_MAX];
    uint32_t length = 0;
    int count = 0;

    if (!(settings.bitfield_options & SETTINGS_BIT_BOOT_SCREEN)) {
        return;
    }
    if ((custom_data_find(CUSTOM_DATA_TYPE_BEEP, &length) == 0) || (length > sizeof(data)) ||
        !custom_data_read(CUSTOM_DATA_TYPE_BEEP, data, length)) {
        sound_play(SOUND_POWER_ON);
        return;
    }
    for (uint32_t i = 0; ((i + 1) < length) && (data[i + 1] != 0); i += 2) { // soundCreateSong()
        boot_melody[count++] = notes[(data[i] > 63) ? 63 : data[i]];
        boot_melody[count++] = (int16_t)(data[i + 1] * BOOT_NOTE_MS);
    }
    boot_melody[count] = -1;
    boot_melody[count + 1] = -1;
    if (count > 0) {
        sound_play(boot_melody);
    }
}

const int16_t *sound_tx_tone(uint8_t pattern)
{
    return (pattern < SOUND_TX_TONE_PATTERNS) ? tx_tones[pattern] : NULL;
}

void sound_tx_tones_changed(void)
{
    tx_tones_changed = true;
}

void sound_tx_tones_save(void)
{
    if (tx_tones_changed &&
        custom_data_write(SOUND_TX_TONES_DATA_TYPE, (const uint8_t *)&sound_tx_tones, sizeof(sound_tx_tones))) {
        tx_tones_changed = false;
    }
}

bool sound_is_playing(void)
{
    return playing || (next != NULL);
}
