# OpenDMR installation guide

Replacing the stock firmware of the **Baofeng DM-32 / DM-32UV** with OpenDMR, updating it later, and going back to
the stock firmware. 中文：[installation.md](installation.md). Using the radio afterwards: the
[user manual](manual.en.md).

> ⚠️ **Flashing can brick the radio. You do it at your own risk.**
> **The first step must be the backup**: the data flash holds **this radio's own transmit and receive calibration**,
> which can't be recovered from another radio.
> With a backup everything can be put back, even after a failed write: the chip has a bootloader in ROM, so a damaged
> program flash can always be written again.

## Contents

1. [What you need](#1-what-you-need)
2. [Backup (required)](#2-backup-required)
3. [Writing the firmware and the Chinese font](#3-writing-the-firmware-and-the-chinese-font)
4. [First start](#4-first-start)
5. [Writing the codeplug](#5-writing-the-codeplug)
6. [Updating](#6-updating)
7. [Going back to the stock firmware](#7-going-back-to-the-stock-firmware)
8. [Troubleshooting](#8-troubleshooting)

---

## 1. What you need

### The release

Download **`OpenDMR-<version>.zip`** from the Releases page and unzip it into a folder (run all the commands below in
that folder). It holds:

| File | What for |
|---|---|
| `opendmr.bin` | The firmware |
| `opendmr_BFUV32.bin` | The same firmware with the header of the stock update mode (special cases only, as in [7](#7-going-back-to-the-stock-firmware)) |
| `cjk.bin` | The Chinese font (2.7 MB, goes into the data flash) |
| `tools/backup/dm32_dump.py`, `tools/backup/dumper/dumper.bin` | Backup / write tool (through the chip's bootloader) |
| `tools/fonts/ttf/RobotoCondensed-VF.ttf` | The font that tool uses for the text on the radio's screen |
| `tools/debug/fw_update.py`, `tools/debug/updater/updater.bin` | Updates (while the radio runs OpenDMR) |
| `tools/fonts/upload_cjk.py` | Writes the Chinese font again while the radio runs OpenDMR |
| `docs/manual.md`, `docs/manual.en.md`, `docs/manual/` | User manual |
| `docs/installation.md`, `docs/installation.en.md`, `README.md`, `README.en.md` | This guide, the project's README |
| `licences/` | The OFL licences of the fonts (Roboto Condensed, Noto Sans SC) |
| `VERSION.txt` | Version, git commit, packing time |

One more download, **`SHA256SUMS.txt`**, holds the checksums (check with
`certutil -hashfile OpenDMR-<version>.zip SHA256`).

The codeplug software, the **OpenGD77 CPS**, isn't in the release: download it from <https://www.opengd77.com/>
(a version with DM32/UV008; tested with E2026.07.13.01).

### Computer

- Windows 10 / 11.
- **Python 3** (<https://www.python.org/downloads/>; tick "Add python.exe to PATH" in the installer), then in
  PowerShell:

  ```powershell
  py -m pip install pyserial pillow
  ```

  (pillow is optional: with it the radio's screen shows text as well as the progress.)

### Programming cable

- The CH340 cable that came with the radio works; an FTDI cable (e.g. BTECH PC03) is more reliable.
- Plugged in, Device Manager → Ports (COM & LPT) shows its COM port (the commands below say `COM6`: use yours).
  Install the CH340 driver if Windows has none.
- Push the plug all the way in: the DM-32's socket is tight, and a plug that isn't fully in fails to communicate.

### Battery

Charge it fully first. The whole job (mostly the backup) takes about 40 minutes.

---

## 2. Backup (required)

The DM-32 has two flash chips:

| Chip | Contents | Backup time |
|---|---|---|
| Program flash (25Q08 or 25Q16, 1 – 2 MB) | Bootloader, stock firmware | About 1 minute |
| Data flash (25Q128, 16 MB) | Stock codeplug, fonts, voice, **calibration** | About 13 minutes |

**Read each chip twice**: only two identical reads make a reliable backup.

Every run goes the same way:

1. **Switch the radio off** and plug in the cable;
2. Run the command; it says `switch the radio on now (within 30 s)`;
3. **Switch the radio on within 30 s, without pressing any key.** A black screen or a progress screen, and the LED
   off, are normal;
4. Wait until the command ends (it shows the bytes and the seconds), then **switch the radio off** before the next one.

```powershell
mkdir backup
py -u tools\backup\dm32_dump.py COM6 program backup\program-1.bin
py -u tools\backup\dm32_dump.py COM6 program backup\program-2.bin
py -u tools\backup\dm32_dump.py COM6 data    backup\data-1.bin
py -u tools\backup\dm32_dump.py COM6 data    backup\data-2.bin
```

Compare the two reads:

```powershell
fc.exe /b backup\program-1.bin backup\program-2.bin
fc.exe /b backup\data-1.bin backup\data-2.bin
```

Both must say **"FC: no differences encountered"**. If not, read again until two reads are the same.

Copy the `backup` folder somewhere safe (cloud, USB stick) and **label it with the radio's serial number**: a backup
belongs to its own radio only.

---

## 3. Writing the firmware and the Chinese font

As for the backup: radio off, run the command, switch on within 30 s, no key. `--write` writes only the 4 KB sectors
that differ from the file, reads each one back, and compares everything again at the end.

```powershell
py -u tools\backup\dm32_dump.py COM6 program opendmr.bin --write --start 0xC000
```

(The bootloader, below `0xC000`, is never touched.) It should end with `... sectors written, all ... the same as the
file`. Switch the radio off.

```powershell
py -u tools\backup\dm32_dump.py COM6 data cjk.bin --write --start 0x280000
```

The Chinese font goes into the data flash from 0x280000 (about 5 minutes). Write it even for the English interface:
Chinese channel and contact names need it. Switch the radio off at the end.

---

## 4. First start

The radio starts on the channel screen. With no settings yet it uses the defaults; the codeplug is empty: the CPS
writes it in the next step.

Worth doing first:

- **Language**: hold the menu key (the left key under the screen) for the main menu → Options → Language, the menu
  key switches English / 中文;
- **Your DMR ID**: Options → General options → User DMR ID (or in the CPS);
- **Time and date**: main menu → Radio info → Time / Date (with a GPS fix the clock is set by itself: see "GPS" in
  the manual).

Keys and screens: the [user manual](manual.en.md).

---

## 5. Writing the codeplug

1. Download the OpenGD77 CPS from <https://www.opengd77.com/> (a version with DM32/UV008; tested with
   E2026.07.13.01) and install it;
2. Start the OpenGD77 CPS, **Radio type DM32/UV008**;
3. Switch the radio **on normally** (running OpenDMR) and plug in the cable;
4. **Read codeplug** once first, edit the channels, zones, contacts, TG lists, ..., then **Write codeplug**.

While it reads or writes, the radio shows which part and how far: don't unplug the cable.

- The stock CPS and stock codeplug files **can't** be used: make the codeplug again in the OpenGD77 CPS.
- The CPS also writes the DMR ID database (callers' names) and the satellite keps.
- **The CPS doesn't read or write the radio's settings** (power, backlight, mic gain, ...): they are in the radio's
  menus.

---

## 6. Updating

With OpenDMR running, a new version needs no power cycle and no timing: radio on, cable in, then

```powershell
py tools\debug\fw_update.py opendmr.bin --port COM6
```

Only the changed sectors are written (about 35 s for all of them), the screen shows the progress, the radio restarts
at the end. Settings and codeplug stay.

If the release notes say the font changed, write the font again too (the radio on as usual; restart it afterwards):

```powershell
py tools\fonts\upload_cjk.py COM6 --file cjk.bin
```

If `fw_update.py` fails or the radio doesn't start, write `opendmr.bin` again as in section 3 (power off, power on
handshake).

---

## 7. Going back to the stock firmware

Write **this radio's own** backup back (radio off, run, switch on within 30 s):

```powershell
py -u tools\backup\dm32_dump.py COM6 program backup\program-1.bin --write
py -u tools\backup\dm32_dump.py COM6 data    backup\data-1.bin --write
```

`--write` changes only the sectors that differ. With both chips written back, the radio is exactly as it was at the
backup (stock firmware, codeplug, voice, calibration).

To put back only the stock firmware and leave the data flash: hold **PTT + SK1** while switching on (the stock update
mode), then write the stock firmware with the stock CPS's Firmware loader.

What OpenDMR wrote into the data flash (Chinese font, codeplug, settings) stays there, and data the stock firmware
uses may have been overwritten: for a full restore, write the data flash backup back.

---

## 8. Troubleshooting

**Nothing happens after `switch the radio on now`, or an error**

- Start with the radio **off**, switch it on after the message, no key while it starts.
- When a radio running OpenDMR doesn't switch off with the knob, **take the battery out** and put it back.
- Check the COM port number and that the plug is fully in.
- Try a cable with another chip (the CH340 is fussy about the timing; FTDI cables do better).
- `py -u tools\backup\dm32_dump.py COM6 program --speedtest` shows what speeds the cable manages; if it is unreliable,
  add `--baud 115200` (slower, steadier).

**A write was interrupted**

No harm: run the same command again. Only the sectors that differ are written, the ones already done are left. The
bootloader is in the chip, so a damaged program flash can always be written again.

**Chinese text shows as boxes or nothing**

The Chinese font is missing or incomplete: write `cjk.bin` as in section 3, or with `upload_cjk.py` while the radio
runs (section 6), then restart the radio.

**Which version is on the radio**

Main menu → Firmware info: the first line is the version (git commit), then the build time.

**How long to keep the backup**

For good. Its calibration was measured on this radio in the factory: without it the transmit power and the receiver
are no longer accurate.
