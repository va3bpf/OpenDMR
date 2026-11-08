# OpenDMR 固件架构设计

适用于 `src/`：用 FreeRTOS 替换原固件（RA4FHE 公开的 OpenDM32 ucos 版源码（`firmware/dh4570_IAP`，2025-12-28 版））里的 uC/OS-III，硬件是 Baofeng DM-32UV（SoC HR_C7000 / DH4570，CPU C-SKY CK803S，168 MHz）。

## 1. 为什么换

- 2026-11-03 在原固件上观察到 **OS 节拍停摆**：TIMER0 中断照常、我们的 1 ms 计数（TIMER2）也在走，但 uC/OS 的 `OSTickCtr` 冻结了几分钟。所有任务都卡在 `OSTimeDly(1)` 里，DMR 能显示主叫却没有声音；恢复后 DMR 发射热点收不到，要重启才好。
- 原系统节拍链路很长：TIMER0 中断 → `OSTimeTick()` → 投递到只有 10 格的中断延迟队列（`OS_CFG_ISR_POST_DEFERRED_EN = 1`，`OS_CFG_INT_Q_SIZE = 10`）→ 中断队列任务 → 节拍任务 → 唤醒延时中的任务。链上任何一环出问题，节拍都会停。
- 厂商的移植层有可疑之处：任务主动切换（`__task_switch`）保存的是 EPSR（上一次异常留下的值），而不是任务自己的 PSR；中断入口每次都把中断栈重置到栈顶，不支持嵌套。
- OpenGD77 的应用代码原本就是为 FreeRTOS 写的（代码里还留着被注释掉的 `vTaskDelay` / `xTaskCreate`），对应关系直接。

## 2. 分层

```
应用层     UI（菜单、信道/VFO 界面）、trx（收发状态机）、settings/codeplug、APRS、卫星...
服务层     sound（提示音、语音提示）、cps（写频线链路、调试命令）、显示（帧缓冲 → LCD）
驱动层     LCD、按键/旋钮/PTT、SPI Flash、FD6818（FM，软件 SPI）、HR_C7000（DMR、音频）、
           AMBE 编解码器、ADC（电池）、UART（写频线、GPS）
硬件抽象   hw.c：时钟、引脚复用、GPIO、中断登记、定时器
内核       FreeRTOS V11.3.3（tasks/list/queue）+ 移植层 portable/CK803
启动       startup/crt0.S：向量表、缓存、数据段/RAM 代码复制、bss 清零
```

规则：上层只调用下层；驱动层不直接用 FreeRTOS 的阻塞调用，等待交给调用它的任务决定。

目录和分层一一对应（命名见 [coding-style.md](coding-style.md)）：

| 目录 | 层 | 内容 |
|---|---|---|
| `startup/` | 启动 | crt0.S（向量表、缓存、复制、清零） |
| `freertos/`、`freertos/portable/CK803/` | 内核 | FreeRTOS 内核、CK803 移植层 |
| `config/` | 配置 | FreeRTOSConfig.h |
| `hal/` | 硬件抽象 | hw.h（寄存器、引脚）、hal.h（函数）、clock、gpio（含开机时的引脚设置）、delay、irq（中断表、异常）、systick（FreeRTOS 节拍）、newlib（libc 的系统调用）、util.h |
| `drivers/` | 驱动 | panel（屏幕初始化、总线和总线锁）、framebuffer（帧缓冲 → 屏幕，只送变化的部分）、uart、spi0、flash（25Q128，7 MB 窗口）、adc、rtc（I2C2）、keys（键盘、侧键、PTT）、rotary、leds、backlight |
| `graphics/`、`graphics/fonts/` | 图形 | font（西文字体）、font_cjk（中文字库，第 5 步）、draw（图元）、icons、theme（调色板）、字体数据 |
| `ui/` | 界面框架 | ui（事件、UI 任务、界面栈，见 [ui-design.md](ui-design.md) 第 6 节）、list（表描述的列表界面，各菜单记住位置）、codeplug_text（GB2312 名字 → UTF-8） |
| `ui/text/` | 文字 | text.h / text_en.c / text_zh.c 由 `tools/gen_text.py` 生成，text.c 按设置选语言 |
| `ui/widgets/` | 界面 | 控件：status_bar、channel_panel、bottom_bar（含软键条）、s_meter、list_view；其余控件随界面搬入 |
| `services/` | 服务 | input（输入任务：按键、旋钮 → UI 事件）、systime（1 ms 计时、监视器）、power（电源开关关机）、cable_link（写频线链路）、debug（调试命令）、settings（设置）、codeplug（信道数据）、battery、datetime、radio（收发：FM、DMR 引擎的外层、VOX、APRS 发射）、sound（提示音）、vox（VOX 判定）、aprs（APRS 信标）、hotspot（DMR 热点，MMDVM 协议）、gps、display（背光）、idle（自动关机 / 锁键）、last_heard、tones、custom_data、satellite、dmr_ids（CPS 写入的 DMR ID 数据库，主叫名字） |
| `dmr/` | DMR | 原工程的 HR_C7000 引擎（hr_c7000.c 基本原样）、dmr_compat（引擎要的接口）、dmr（中断、模式切换、主叫 → 界面）、dmr_sound、AMBE 编解码器 |
| `app/` | 应用 | main.c（启动）、各界面（channel_screen…）、menus（菜单表） |
| `docs/` | 文档 | 安装手册、使用说明书、架构、界面设计、编码风格 |

## 3. 内核与移植层（`freertos/portable/CK803`）

| 项目 | 设计 |
|---|---|
| 内核版本 | FreeRTOS V11.3.3，MIT 许可证 |
| 内存 | 只用静态分配（`configSUPPORT_DYNAMIC_ALLOCATION 0`），任务栈和 TCB 都是全局数组，没有堆 |
| 节拍 | TIMER0（中断号 1，向量 33），1 kHz，和原系统一样 |
| 上下文 | 68 字节：r0~r13、r15、PSR、PC，与厂商 uC/OS 移植一致 |
| 任务主动切换 | `vPortYield()`，以函数调用方式保存上下文，保存的是**调用者自己的 PSR** |
| 临界区中切换 | 推迟到临界区结束（`vPortExitCritical`）再切换 |
| 中断入口 | 所有中断走 `VectorIrqISR`：在被打断的任务栈上保存现场 → 第一层切到中断栈（嵌套时留在中断栈）→ 应答 PIC → 调用 `g_irqvector[n]` → 退出时如果 `xPortSwitchRequired` 被置位，就切换任务 |
| 中断里唤醒任务 | 用 `...FromISR()` 系列 API，`portYIELD_FROM_ISR()` 只置标志，退出中断时统一切换 |
| 断言 | `configASSERT` 失败时关中断，把行号写进 `freertosAssertLine`，然后停住，可以用调试命令读出 |

## 4. 任务和优先级

FreeRTOS 数字越大优先级越高。原 uC/OS 是数字越小越高：主任务和提示音任务为 1，HR_C7000 任务为 3。

| 任务 | 优先级 | 原来的对应 | 唤醒方式（目标） |
|---|---|---|---|
| rf（射频开关） | 7 | 原来在 HR_C7000 中断里 | DMR 发射时每个突发开、关 PA 和 LNA（时序最紧） |
| cable_link（写频线） | 6 | `tick_com_request()` | UART0 中断每个字节通知；调试请求在别的任务不让出时也能回答 |
| hrc7000（DMR） | 5 | `hrc7000TaskFunction`，原来每 1 ms 轮询 | 时隙工作不能等；只在 DMR 模式运行 |
| radio（收发） | 4 | trx.c、主循环里的静噪 | 每 25 ms 静噪和信号、VOX；界面的请求发通知 |
| hotspot（热点） | 4 | hotspot.c，原来在 UI 主循环里 | 热点运行时每 1 ms 跑状态机；写频线来帧、收到射频帧时通知 |
| input（输入） | 3 | 主循环的扫键 | 每 10 ms 扫键；每秒查电量、时间、自动关机；APRS 信标 |
| sound（提示音） | 3 | `soundBeepTaskFunction`，每 1 ms 轮询 | 有提示音要播时通知，播放中按节拍推进 |
| ui（界面） | 2 | `applicationMainTask` | 阻塞在事件队列上，见 [ui-design.md](ui-design.md) 第 6 节 |
| gps | 1 | gps.c | UART1 收到 NMEA 时通知 |
| idle | 0 | uC/OS 空闲任务 | 内核提供，在 RAM 里运行 |

说明：
- DMR 的时隙处理和射频开关有严格的时间要求，所以优先级最高。原系统里它的优先级比主任务低，靠每 1 ms 轮询来赶时间。
- 按照“尽量用事件通知”的原则，各任务的轮询逐步改成由中断发任务通知来唤醒。第一版先保持原来的轮询节奏（1 ms / 10 ms），功能齐了再逐个改。

## 5. 中断

| 中断 | 向量 | 处理 | 规则 |
|---|---|---|---|
| TIMER0 | 33 | FreeRTOS 节拍 | 只推进节拍 |
| TIMER2 | 34 | 1 ms 计数 `uwTick`、秒计数、旋钮、卡死检测 | 不做阻塞操作；卡死检测在这里代答写频线请求 |
| TIMER3 | 35 | 电池 ADC | |
| UART0 | 44 | 写频线（CPS / 调试命令；热点的 MMDVM 帧） | 每收到一个字节就通知 cable_link 任务，由任务判断请求是否完整；以 0xE0 开头的 MMDVM 帧在中断里拼好交给 hotspot 任务（热点类型打开时） |
| UART1 | 42 | GPS | |
| GPIOB | 46 | 旋钮 | |
| PCM 读 / 写 | 59 / 60 | 音频采样（话筒采集、播放） | 必须快，放在 RAM |
| HR_C7000 系统 / 时隙收 / 时隙发 / 发射突发 | 61 / 63 / 62 / 64 | DMR | **不能在里面等 `ticksGetMillis()`**（节拍中断打断不了它们，会卡死）；向量 64 超出前 32 个中断，需要用 PIC 的第二组寄存器（`PIC_MASK_1`） |

所有中断处理函数都**不打开中断**，不嵌套。移植层虽然能容忍嵌套，但设计上不依赖它。

## 6. 内存布局

| 区域 | 地址 | 内容 |
|---|---|---|
| Flash 应用区 | 0x0300C000 起，0xD2000 字节 | Baofeng 引导程序在 0x1000~0xBFFF，应用镜像带 256 字节的 BFUV32-V2 头 |
| Flash 缓存 | 0x03000000 起 8 MB | 4 路回写缓存，由 crt0 打开 |
| RAM | 0x00010000~0x00058000 | 数据段、RAM 代码（向量表、内核、移植层、`.staticcode`）、bss（任务栈）、中断栈 4 KB |
| 启动栈 | 0x00057FFC 向下 4 KB | 只在 `main()` 启动调度器之前使用 |
| 声码器 / 升级区 | 0x00044400 ~ 0x00056F24（74.8 KB，`ckcpu.ld` 的 `__update_area_start/end`） | 我们的声码器（`.vocoder_overlay`），第一次 DMR 编解码时 `vocoder_sync()` 从 Flash 拷入；不支持原厂编解码器。声码器占 53.4 KB：编码器代码（放 RAM 是为了避开 Flash 总线干扰话筒）、常用的表（`AMBE_RAM_DATA`，`.vocoder_rodata`）、编解码状态；解码器代码留在有缓存的 Flash 里，那样更快 |
| 升级程序 | 0x00044400 | 数据线升级时由固件里的装载器放到这里运行（见第 8 节），这时收发已停 |

RAM 已经很紧（原固件 bss 约 145 KB）。任务栈大小要按实测的高水位来定（`uxTaskGetStackHighWaterMark`）。g_top_irqstack 到 0x44400 之间约 40 KB 是空的，写频线调试工具（跳板、大栈）用它。

**代码放 RAM 还是 Flash**（2026-11-06 实测）：RAM 只有一个端口，取指令要等数据访问，所以小循环在有缓存的 Flash 里反而更快（解码器 2.13 ms 对 2.48 ms）；但随机查的大表放 Flash 很慢（缓存装不下），要放 RAM。例外是 DMR 编码器的代码：放 Flash 时发射的话音变差，Flash 总线活动串进了话筒。

## 7. 必须保留的设计约束

这些是在原固件上靠实测换来的经验，新架构必须照做：

1. **FM 音频不走 HR_C7000 基带，和原厂一样**：接收时音频开关 0x2F（引脚表 "Switch audio PWM/FD6818"）拉高，喇叭直接接 FD6818 的音频输出；发射时只用 FD6818 自己的话筒通路，基带不开 FM 发射（FM_PTT = 0）。音频一旦经过基带（codec ADC、DAC），CPU、Flash、LCD 的总线活动和 GPS 模块的干扰都会被听到——原固件上"总线活动会被听到"的所有问题都是这个原因（2026-11-04，靠给原厂固件打补丁读出它收发时的寄存器才找到）。按原厂走以后，不需要为降噪把代码放进 RAM、限制刷屏或在发射时固定 LCD 数据线，这些都已撤掉并逐项试听过。
2. **DMR 发射时注意 LCD 的 DB6/DB7**：这两根线的焊球紧挨着 Codec 的话筒输入（A12/B12）。DMR 的话筒经过 Codec，原固件在 FM 发射时听到过它们翻转的咔声（当时 FM 也错走了 Codec）。做 DMR 发射时实测，需要的话从 git 历史取回固定这两根线的做法（6a855ea 之前的 `fb_hold_data_lines_begin/end`）。
3. **屏幕送什么只由帧缓冲决定**：界面每次完整重画，由帧缓冲比较出哪些地方变了再送；界面代码不记录“哪里要重画”。
4. **HR_C7000 中断里不能等节拍**。
5. **写频线上的请求会屏蔽 PTT 1 秒**（`cpsLinkBusy`）：线上的字节会被误读成耳机插孔的 PTT。调试时不要持续轮询。
6. **FD6818 在 DMR 下寄存器 0x43 = 0x4048**，接收音量用工厂校准值等，见 `firmware/dh4570_IAP` 各处注释和提交记录。
7. **拿不准时和原厂对比寄存器**：原厂固件可以从备份刷回；可以给它打补丁，在收发时把寄存器存进数据 Flash，再用本固件读出来比较。

3. **DMR 解码不能关着中断做**：一个突发 3 帧要 7 ~ 15 ms，播放中断每 10 ms 要一块；关中断解码会让播放断流（60 ms 的空档和咔声）。`hr_c7000.c` 在临界区里只拷出 27 字节，出临界区再解码。
4. **话筒按模式接，不在每次发射时切**：MIC_MUT 高接 FD6818（FM），低接调制解调器的 codec（DMR），在 `radio.c apply_channel()` 里按信道模式设，和原厂一样。每次发射时切换会在采集里留下一个到 −32000 的冲击，发出去就是开头的爆音。
5. **DMR 发射开头的采集要丢掉，编码器也要重来**：打开采集（AUDIO_CONTROL 第 4 位）后 ADC 从约 12000 用 150 ms 落到话筒的直流（约 2470）。这段在发帧头时采到，`DMR_STATE_TX_START_4` 丢掉积压数据时同时调用 `codecInit()` 重置编码器，否则编码器看到过这段，会发出“噗”声、把后面的话判成小声。启动时 `dmr_sound_init()` 关掉 AUDIO_CONTROL 第 4、5 位（复位后不会自己清）。

### 历史：被取代的第 1、2 条（2026-11-03 定，2026-11-04 改）

原来的两条规则如下，保留作记录。当时的实测现象都是真的（RAM 里空转听不到，读 Flash 有轻微滴答，读外设寄存器滴答很响，写 LCD 有咔声，DB6/DB7 单独翻转听得到），但结论错了：这些声音能被听到，是因为 FM 音频经过了 HR_C7000 基带。音频按原厂路由之后，这些措施逐项撤掉试听，都不需要。

> 1. **FM 接收时不访问 Flash 和外设**：总线活动会被听到。凡是 FM 接收静噪打开时会运行的代码，包括空闲任务、等待循环、软件 SPI 读 RSSI，都放在 RAM 里。内核已经放在 RAM 里。
> 2. **FM 发射时 DB6/DB7 不能翻转**：LCD 数据线 DB6/DB7 的焊球紧挨着 Codec 的话筒输入（A12/B12）。发射期间要把这两根线固定成 GPIO 低电平，只刷新预设窗口里的计时和话筒电平条（原固件 `lcdHoldDataLinesBegin/End`）。

## 8. 调试手段

- 异常：crt0 把异常号、EPC、EPSR、LR 写进 `dm32Exception[]` 后停住。
- 断言：`freertosAssertLine`。
- 写频线调试命令（`dm32dbg.py`）：读写内存、调用函数，搬过来后是排查问题的主要手段。
- 卡死检测：主循环超过 2 秒没跑，就由 TIMER2 中断记录被打断的 PC，并代答写频线请求。
- **看门狗**（`services/systime.c`）：启动完成后打开（超时 3 秒），由 1 ms 中断在 tick 和主任务都在跑时喂狗。卡死 5 秒后停止喂狗，约 8 秒复位；中断全停或 CPU 异常（中断关闭）时 3 秒后复位。写频线在用（10 秒内有请求，`dm32dbg.py` 读卡死现场）或 `systime_watchdog_hold` 非 0 时照常喂狗。关机（`power_off()`，自动关机时旋钮仍开着、电源不断）和数据线升级前关掉它。复位前把原因写进不清零的 RAM（`.noinit` 段的复位记录）；重启后在 `systime_last_reset`（[0] 魔数，[1] 原因：bit 0 tick 停 / bit 1 主任务停，或 0x100+异常号，[2] PC，其余同 `systime_monitor_last` 或异常的 PSR、LR）和 `systime_watchdog_resets`（上电以来的看门狗复位次数），用 `dm32dbg.py` 读。看门狗寄存器在它的复位后保留：卡死复位后如果要用 PTT+SK1 进升级模式，先断电再开机更稳妥（引导程序是否关看门狗未确认）。
- **数据线升级**（不进升级模式）：`python tools/debug/fw_update.py`。固件里的装载器（`services/serial_update.c`，调试命令 'D' 'U' "UPD!"）先停掉收发、锁住所有操作、关中断，再把电脑传来的升级程序（`tools/debug/updater/updater.bin`，在 git 里，改了 `updater.c` 后用 `build.sh` 重新编译）放到 0x44400 运行。它按 4 KB 扇区比较 CRC，只写变了的扇区，写完读回，最后用看门狗复位。速率 250000（375000 时电台发往电脑的字节偶尔会丢，绿色那台升级到一半断过），全部重写约 33 秒。只写带装载器标识的固件；引导程序（0xC000 以下）不碰，失败时 PTT+SK1 仍然能用。
- **复位**：只有看门狗能做整片复位（和上电一样，调制解调器、codec 都回到复位状态；RAM 保留）。看门狗计数器从初值往上数到溢出（`WDG_OT_LOAD` 7:C0000000 约 60 秒），寄存器只能 32 位写，而且它自己的寄存器在它触发的复位后**保留**：固件 `clock_init()` 第一件事就是关掉它，否则会一直复位。`watchdog_reset()`（`hal/watchdog.c`）约 1 秒后复位，写频后的重启用它。`SYS_SOFT_RSTN = 0` 会卡死（时钟和 Flash 接口的配置不复位）；跳到引导程序入口（0x03001000）能启动，但外设保持原状（曾导致复位后 DMR 发射没声音）。
- 写频线下读写 RAM 测声码器：`vocoder/py/vocoder_bench.py time`（速度和各步骤耗时）、直接调用 `vocoderEncodeFrame` / `vocoderDecodeFrame` 和电脑上的 C 版逐位比较（见 `vocoder/README.md`）。往 0x44400 写测试程序前确认电台空闲（`slotState` 为 0），否则会覆盖正在运行的解码器。
