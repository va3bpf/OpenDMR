# OpenDMR 安装手册

把 **Baofeng DM-32 / DM-32UV** 从原厂固件换成 OpenDMR，以及以后的升级和恢复原厂。English: [installation.en.md](installation.en.md)。
装好以后怎么用见 [使用说明书](manual.md)。

> ⚠️ **刷机有变砖风险，后果自负。**
> **第一步必须是备份**：电台的数据 Flash 里有**本机独有的收发校准数据**，丢了不能从别的机器恢复。
> 备份做好了，就算刷坏了也能完整恢复：芯片里有固化的引导程序，程序 Flash 写坏了照样能写回去。

## 目录

1. [准备](#1-准备)
2. [备份（必须）](#2-备份必须)
3. [写入固件和中文字库](#3-写入固件和中文字库)
4. [第一次开机](#4-第一次开机)
5. [写码表](#5-写码表)
6. [升级](#6-升级)
7. [恢复原厂固件](#7-恢复原厂固件)
8. [常见问题](#8-常见问题)

---

## 1. 准备

### 发布包

从 Release 页面下载 **`OpenDMR-<版本>.zip`**，解压到一个文件夹（下面的命令都在这个文件夹里运行），里面是：

| 文件 | 用途 |
|---|---|
| `opendmr.bin` | 固件 |
| `opendmr_BFUV32.bin` | 同一个固件，带原厂升级模式的文件头（只在 [7](#7-恢复原厂固件) 之类的特殊情况用） |
| `cjk.bin` | 中文字库（2.7 MB，写进数据 Flash） |
| `tools/backup/dm32_dump.py`、`tools/backup/dumper/dumper.bin` | 备份 / 写入工具（走芯片的引导程序） |
| `tools/fonts/ttf/RobotoCondensed-VF.ttf` | 上面这个工具在电台屏幕上显示文字用的字体 |
| `tools/debug/fw_update.py`、`tools/debug/updater/updater.bin` | 平时升级用（电台跑着 OpenDMR 时） |
| `tools/fonts/upload_cjk.py` | 电台跑着 OpenDMR 时单独重写中文字库 |
| `docs/manual.md`、`docs/manual.en.md`、`docs/manual/` | 使用说明书 |
| `docs/installation.md`、`docs/installation.en.md`、`README.md`、`README.en.md` | 本手册、项目说明 |
| `licences/` | 字体（Roboto Condensed、Noto Sans SC）的 OFL 许可 |
| `VERSION.txt` | 版本、git 提交号、打包时间 |

另一个附件 **`SHA256SUMS.txt`** 是校验值，可以用 `certutil -hashfile OpenDMR-<版本>.zip SHA256` 核对。

写码表软件 **OpenGD77 CPS** 不在发布包里，自己从 <https://www.opengd77.com/> 下载（要支持 DM32/UV008 的版本；测试过的是 E2026.07.13.01）。

### 电脑

- Windows 10 / 11。
- **Python 3**（<https://www.python.org/downloads/>，安装时勾选 “Add python.exe to PATH”），然后在 PowerShell 里：

  ```powershell
  py -m pip install pyserial pillow
  ```

  （pillow 可以不装，装了电台屏幕上会显示文字说明。）

### 写频线

- 电台原配的 CH340 写频线就行；FTDI 芯片的线（如 BTECH PC03）更稳。
- 插上后在「设备管理器 → 端口 (COM 和 LPT)」里看它是哪个 COM 口（下面的命令都写 `COM6`，换成你的）。CH340 没有驱动时装 CH340 驱动。
- 线要插到底：DM-32 的写频口很紧，没插到底会通信失败。

### 电池

充满电再开始。整个过程（主要是备份）大约 40 分钟。

---

## 2. 备份（必须）

DM-32 有两颗 Flash：

| 芯片 | 内容 | 备份时间 |
|---|---|---|
| 程序 Flash（25Q08 或 25Q16，1 ~ 2 MB） | 引导程序、原厂固件 | 约 1 分钟 |
| 数据 Flash（25Q128，16 MB） | 原厂码表、字库、语音、**校准数据** | 约 13 分钟 |

**每颗读两次**，两次完全一样才算可靠的备份。

每次运行的步骤都一样：

1. **电台关机**，插好写频线；
2. 运行命令，屏幕出现 `switch the radio on now (within 30 s)`；
3. **30 秒内打开电台，不要按任何键**。电台屏幕是黑的或显示进度，指示灯不亮，这是正常的；
4. 等命令结束（显示多少字节、多少秒），然后**关机**再做下一步。

```powershell
mkdir backup
py -u tools\backup\dm32_dump.py COM6 program backup\program-1.bin
py -u tools\backup\dm32_dump.py COM6 program backup\program-2.bin
py -u tools\backup\dm32_dump.py COM6 data    backup\data-1.bin
py -u tools\backup\dm32_dump.py COM6 data    backup\data-2.bin
```

比较两次读出来的是否一样：

```powershell
fc.exe /b backup\program-1.bin backup\program-2.bin
fc.exe /b backup\data-1.bin backup\data-2.bin
```

两个都要显示 **“找不到差异”**（FC: no differences encountered）。不一样就再读一次，直到有两份一样的。

把 `backup` 文件夹复制到别的地方（网盘、U 盘）保存好，**写上这台电台的序列号**：每台电台的备份只能用在它自己身上。

---

## 3. 写入固件和中文字库

和备份一样：先关机，运行命令，30 秒内开机，不按键。`--write` 只写和文件不同的 4 KB 扇区，每个扇区写完回读校验，最后整体再比一遍。

```powershell
py -u tools\backup\dm32_dump.py COM6 program opendmr.bin --write --start 0xC000
```

（`0xC000` 以下是引导程序，不会被碰。）结束时应显示 `... sectors written, all ... the same as the file`。关机。

```powershell
py -u tools\backup\dm32_dump.py COM6 data cjk.bin --write --start 0x280000
```

中文字库写在数据 Flash 的 0x280000 起（约 5 分钟）。不用中文界面也建议写：中文的信道名、联系人名要用它。结束后关机。

---

## 4. 第一次开机

开机直接进入信道界面。第一次开机没有设置，固件使用默认设置；码表是空的，下一步用 CPS 写。

建议先做：

- **语言**：长按菜单键（屏幕下方左边的键）进主菜单 → 选项 → 语言，按菜单键切换 中文 / English；
- **本机 DMR ID**：选项 → 通用设置 → 本机ID（也可以在 CPS 里设）；
- **时间和日期**：主菜单 → 电台信息 → 时间 / 日期（打开 GPS 定位后会自动校准，见使用说明书「GPS」）。

按键和各个界面见 [使用说明书](manual.md)。

---

## 5. 写码表

1. 从 <https://www.opengd77.com/> 下载 OpenGD77 CPS（支持 DM32/UV008 的版本，测试过 E2026.07.13.01），安装；
2. 打开 OpenGD77 CPS，**Radio type 选 DM32/UV008**；
3. 电台**正常开机**（跑着 OpenDMR），插上写频线；
4. 先 **Read codeplug** 读一次，再编辑信道、区域、联系人、组列表等，然后 **Write codeplug**。

读写时电台屏幕上显示正在读写哪一部分和进度，不要拔线。

- 原厂 CPS 和原厂码表文件**不能**用，码表要在 OpenGD77 CPS 里重新做。
- CPS 也可以写：DMR ID 数据库（显示主叫名字）、业余卫星的轨道根数。
- **CPS 不读写电台的设置**（功率、背光、话筒增益等），设置在电台菜单里改。

---

## 6. 升级

电台跑着 OpenDMR 时，新版本不用关机、不用卡开机时机，插着写频线、电台正常开着，运行：

```powershell
py tools\debug\fw_update.py opendmr.bin --port COM6
```

只写变了的扇区（全部重写约 35 秒），电台屏幕显示进度，写完自动重启。设置和码表都保留。

如果新版本的说明里写着字库有变化，再更新字库（电台照常开着；完成后重启电台）：

```powershell
py tools\fonts\upload_cjk.py COM6 --file cjk.bin
```

`fw_update.py` 失败、电台起不来的时候，用第 3 节的方法（关机、开机握手）重新写 `opendmr.bin` 就行。

---

## 7. 恢复原厂固件

把**这台电台自己的**备份写回去（关机、运行、30 秒内开机）：

```powershell
py -u tools\backup\dm32_dump.py COM6 program backup\program-1.bin --write
py -u tools\backup\dm32_dump.py COM6 data    backup\data-1.bin --write
```

`--write` 只改不同的扇区。两颗都写回去以后，电台和备份时完全一样（原厂固件、码表、语音、校准）。

只想换回原厂固件、保留数据 Flash 的话：按住 **PTT + SK1** 开机进原厂升级模式，用原厂 CPS 的 Firmware loader 写原厂固件。数据 Flash 里 OpenDMR 写的东西（中文字库、码表、设置）还留着，原厂固件用到的数据可能已经被覆盖：要完全恢复，还是要写回数据 Flash 的备份。

---

## 8. 常见问题

**`switch the radio on now` 之后没反应或报错**

- 电台必须是**关机状态**开始，看到提示后再开机，开机时不按任何键。
- 跑着 OpenDMR 的电台旋钮关不掉时，**拔电池**再装上。
- 检查 COM 口号、线是否插到底。
- 换一根不同芯片的线（CH340 卡开机时机比较紧，FTDI 线更稳）。
- `py -u tools\backup\dm32_dump.py COM6 program --speedtest` 可以测这根线能跑多快；不稳定时加 `--baud 115200`（慢一些但更稳）。

**写到一半断了**

没关系。重新开始同一条命令：只写不同的扇区，已经写好的不重写。引导程序在芯片里，程序 Flash 写坏了也能写回去。

**中文显示成方块或空白**

中文字库没写或没写完：按第 3 节写 `cjk.bin`，或电台开着时用 `upload_cjk.py`（第 6 节），完成后重启电台。

**升级后屏幕上的版本号**

主菜单 → 固件信息，第一行是版本（git 提交号）和编译时间。

**备份要保留多久**

一直保留。备份里的校准数据是这台电台出厂时测出来的，没有它电台的发射功率和接收灵敏度就不准。
