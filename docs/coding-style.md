# 固件（src/）编码风格

本工程有自己的命名和代码风格，**不照搬原工程（`firmware/dh4570_IAP`）的文件名、函数名和格式**。原工程是参考：从那里搬代码时，要按本文改名、改格式，只保留行为。

格式由工具保证：`clang-format -i <文件>`（配置在 `.clang-format`，clang-format 19）。命名和其余规则由人来遵守，代码评审时检查。

## 1. 文件和目录

- 文件名全小写，多个单词用下划线：`cable_link.c`、`status_bar.c`、`roboto17_bold.c`。
- 名字说明用途，不用芯片代号或历史名字：屏幕驱动叫 `panel.c`，不叫 `HX8353E.c`（实际芯片是 ST7789 命令集）。
- 每个 `.c` 配一个同名 `.h`，头文件只放对外的接口。
- 目录按分层放（见 [architecture.md](architecture.md) 第 2 节），文件放在它所属的层。
- `#include` 只写文件名：`#include "panel.h"`，目录由 Makefile 的 `INCDIRS` 给出，所以文件名在整个工程里必须唯一。

## 2. 命名

全部小写加下划线（snake_case），对外的名字都带**模块前缀**，前缀就是文件名（或它的简写，见下表）。

| 种类 | 规则 | 例子 |
|---|---|---|
| 对外函数 | `前缀_动作[_对象]` | `panel_init()`、`fb_fill()`、`keys_read_keypad()` |
| 模块内函数 | `static`，可以不带前缀 | `static void settle(void)` |
| 中断处理函数 | `前缀_isr` / `前缀_动作_isr` | `uart0_isr()`、`systime_isr()` |
| 任务函数 | `前缀_task` | `cable_link_task()`、`app_ui_task()` |
| 全局变量 | 尽量不用；要用就带前缀 | `systime_ms` |
| 模块内变量 | `static`，不带前缀 | `static uint32_t request_length;` |
| 类型 | `前缀_名字_t` | `font_glyph_t`、`fb_colour_t` |
| 枚举值、宏、常量 | 全大写，带前缀 | `FB_WIDTH`、`KEYS_GREEN`、`BUTTONS_PTT` |
| 寄存器和引脚（`hw.h`） | 全大写，按外设分组 | `UART0_LSR`、`PIN_PTT` |
| 头文件保护 | `DM32_文件名_H` | `DM32_PANEL_H` |

模块前缀：

| 层 | 模块 → 前缀 |
|---|---|
| hal | clock → `clock_`，gpio → `gpio_`，delay → `delay_`，irq → `irq_`，systick → `systick_`，引脚初始化 → `pins_` |
| drivers | panel → `panel_`，framebuffer → `fb_`，keys → `keys_`，rotary → `rotary_`，leds → `leds_`，backlight → `backlight_`，uart → `uart0_` |
| graphics | font → `font_`，font_cjk → `font_cjk_`，icons → `icons_`，draw → `draw_`，theme → `theme_`，字体数据 → `font_roboto17` 等 |
| ui | 控件 → 控件名，如 `status_bar_draw()`、`list_view_draw_row()` |
| services | systime → `systime_`，power → `power_`，cable_link → `cable_link_`，debug → `debug_` |
| app | `app_` |

单位写进名字里：`_ms`、`_us`、`_hz`、`_percent`：`delay_ms()`、`backlight_set_percent()`。

布尔量用问句或形容词：`is_busy`、`down`、`cable_link_is_busy()`。

## 3. 格式（`.clang-format`）

- 缩进 4 个空格，不用 Tab。
- 大括号：函数的 `{` 另起一行，`if` / `for` / `while` / `switch` / `struct` 的 `{` 跟在同一行（K&R / Linux 风格）。
- `if`、`for`、`while` 的语句体**总是加大括号**，哪怕只有一行。
- 行宽 120。
- 指针的 `*` 靠变量名：`uint8_t *data`。
- `switch` 的 `case` 缩进一级。

```c
void keys_init(void)
{
    for (int column = 0; column < KEYS_COLUMNS; column++) {
        gpio_input(PIN_KEY_1 + column);
    }
}
```

## 4. 注释

- 注释用**英文**。文档（`docs/`）用中文。
- 每个文件开头一段注释：这个文件是什么、参照了原工程哪里、有什么要注意的硬件事实。
  - **本固件重写的文件**写 `After OpenDM32 <文件>（<函数>）`：代码是新写的，行为、寄存器顺序参照了 OpenDM32 的那个文件
    （OpenDM32 = RA4FHE 公开的 OpenDM32 ucos 版源码（`firmware/dh4570_IAP`，2025-12-28 版））。
  - **从原工程搬来的代码**（和原工程逐行相同的比例高，如 `dmr/hr_c7000.c`、`services/hotspot.c`）保留原来的
    来源说明和版权声明（OpenGD77 的 BSD 式许可，第 4 条禁止商用）。
- 注释写**为什么**和硬件事实（引脚共用、时序、实测结论），不复述代码在做什么。
- 实测换来的约束要写在用到它的地方，并说明出处：例如“LCD_DB6/DB7 sit next to the codec microphone inputs (HR_C7000 A12/B12)”。

## 5. 代码规则

- 整数用 `<stdint.h>` 的定宽类型，布尔用 `<stdbool.h>`。
- 不对外的函数和变量一律 `static`。
- 寄存器只通过 `hw.h` 里的宏访问（`REG()`），不在代码里写裸地址；新外设的寄存器先加进 `hw.h`。
- 不用魔数：引脚、寄存器位、时间常数都定义成有名字的宏，并注明单位。
- 放进 RAM 的代码用 `RAM_CODE`，放在 Flash 的大常量（字体）用 `FLASH_CONST`（`hw.h`）。哪些代码必须在 RAM 里，见 architecture.md 第 7 节。
- 中断处理函数：不阻塞、不等节拍、不打开中断；唤醒任务只用 FreeRTOS 的 `...FromISR()` 接口，再 `portYIELD_FROM_ISR()`。
- 任务之间优先用**任务通知**传递事件，少用轮询。
- 不用动态内存：FreeRTOS 对象都用 `...Static()` 创建。
- 给调试工具读的状态变量（`dm32dbg.py mr`）可以是全局 `volatile`，要带模块前缀，并在定义处注释说明它的含义。

## 6. 例外

- **FreeRTOS 内核**（`freertos/*.c`、`freertos/include/`）是第三方代码，原样保留，不改名、不改格式（`freertos/.clang-format` 禁止格式化）。
- **移植层**（`freertos/portable/CK803/`）的函数名由内核规定（`pxPortInitialiseStack`、`vPortYield`、`xPortSysTickHandler`…），沿用 FreeRTOS 的命名和格式。
- **启动代码**（`startup/crt0.S`）是汇编，汇编里的标号保持现状。
- 链接器和启动代码要求的符号（`main`、`__start`、`g_irqvector`、`g_top_irqstack`…）保持原名。
