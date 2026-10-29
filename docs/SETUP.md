# 环境搭建、编译与烧录（STC32G12K128 + Air724UG）

> 目标：从零把 `OTA_STM32` 编译出来、烧进板子、确认它跑起来。
> 每个阶段都有明确的**通过判据**，不要跳阶段 —— OTA 的坑九成来自"地基没验就往上盖"。
>
> 本文是原 5498 行工作文档里**只保留操作步骤**的部分。同源材料还有两份：
> [`appendix/rework_log.md`](appendix/rework_log.md)（按时间顺序的返工全记录）、
> [`appendix/c251_notes.md`](appendix/c251_notes.md)（C251 语言限制与编译错误对照表）。

---

## 0. 软件清单（一次性准备）

| 软件 | 版本要求 | 说明 |
|---|---|---|
| Keil µVision | **必须装 C251 工具链** | 只有 C51 的 Keil 编译不了 STC32G。STC 官网提供"C251 安装包" |
| STC-ISP | V6.90D 或更新（STC32G 专用版） | 老版本不认 STC32G12K128 |
| 串口驱动 | CH340 / CP2102 / STC-USB Link1D | 按你手上的下载器选 |
| GPU 无关 | — | — |

**安装顺序**：Keil µVision → C251 工具链 → STC-ISP → 串口驱动 → 插上下载器确认能列出 COM 口。

---

## 1. 把 STC32G 器件和头文件装进 Keil

Keil 官方器件库里没有 STC。STC-ISP 自带一键导入：

1. 打开 **STC-ISP**，切到顶部 **「Keil仿真设置」** 标签页。
2. 点 **「添加型号和头文件到 Keil 中」**，选择 Keil 安装根目录
   （形如 `C:\Keil_v5\`，不是 `C:\Keil_v5\UV4\`）。
3. 成功后 `C:\Keil_v5\C251\INC\STC\` 下会出现 `STC32G.H`（即工程里 `#include "stc32g.h"` 的来源）。
4. 回 Keil 新建工程时，器件树里应能看到 **STC32G12K128 Series**。

> ✅ 判据：Keil 器件列表能搜到 `STC32G12K128`，且 `STC32G.H` 文件存在。
> ❌ 若搜不到：多半是第 2 步路径选到了 `UV4` 子目录，重来一次。

---

## 2. 建两个工程（BL 与 APP 必须分开）

```
Project/
├─ Bootloader/  Bootloader.uvproj    → CODE 段 0xFF0000（FF:0000h）
└─ APP/         STC32G_OTA.uvproj     → CODE 段 0xFF4000（FF:4000h）
```

Keil 操作：

1. `Project → New µVision Project`，路径指到 `Project/Bootloader/`，文件名 `Bootloader`。
2. 器件选 **STC32G12K128 Series**。
3. 弹出"Copy 'START251.A51' to Project Folder?" → **选「否」**
   （用库里的默认启动文件即可；只有要改 SP/内存清零策略时才复制）。
4. 重复一次建 `Project/App/STC32G_OTA.uvproj`。

---

## 3. 工程选项（Options for Target，两个工程**必须一致**的部分我标了 ★）

`Project → Options for Target 'Target 1'`（快捷键 Alt+F7）。

对话框共 11 个标签，本工程只需要动其中 **4 个**，其余保持默认：

| 标签 | 本工程要不要改 | 要点 |
|---|---|---|
| Device | ✔（建工程时选好） | `STC32G12K128 Series` |
| **Target** | ✔ | ★ Code Rom Size = **Huge**；Xtal = 11.0592 |
| **Output** | ✔ | ★ Create HEX File + **HEX Format = HEX-386** |
| Listing | ✘ | 可选：勾 Assembly Code 便于查汇编 |
| User | ✘ | 预/后构建命令，本工程不用 |
| **C251** | ✔ | ★ Misc controls = `LARGE`；Define = `OTA_PTR_GENERIC=1`，见 §3.4.1 |
| A251 | ✘ | 汇编器，不用动 |
| L251 Locate | ✔ | ★ **留空**，并取消勾选「Use Memory Layout from Target Dialog」 |
| **L251 Misc** | ✔ | ★ 真正决定地址的地方：`CLASSES(...)`，见 §3.4.2 |
| Debug | 本阶段不动 | 见 3.5，配错比不配更糟 |
| Utilities | ✘ | 烧录走 STC-ISP，不在这里配 Flash 算法 |

### 3.0 ★ Include Paths（漏了这个，满屏 `can't open file`）

工程有 4 个源码目录，头文件互相直接 `#include "xxx.h"`（不带路径），
所以**必须**把 4 个目录都加进搜索路径，否则每个 .c 都会报
`error C318: can't open file 'ota_types.h'` 之类。

`Options for Target → C251` 标签页 → `Include Paths` 右边的 `...` 按钮，逐行加：

```
..\..\BSP
..\..\COMMON
..\..\DRV
..\..\OTA
```

（路径相对于 `Project/Bootloader/`，所以是 `..\..\`。若你把 .uvproj 放在别处，按实际相对路径改。）

同一个 `C251` 页还要确认：

| 项 | 取值 | 说明 |
|---|---|---|
| Define | 留空即可 | 若你的 `stc32g.h` **自带** `u8/u16/u32`，这里加 `OTA_HAS_STD_TYPES` 跳过本地 typedef |
| Optimization | Level 0（调通后再升） | 先保证行为可复现 |
| Include Paths | 上面四行 | ★ |

> **关于 `u8/u16/u32`**：不同版本的 `stc32g.h` 有的自带、有的不带。本工程统一在
> `COMMON/ota_mcu.h` 里补齐 typedef，并且**只有这一个文件 include "stc32g.h"**。
> 如果将来出现 `error C231: 'u8': redefinition`，就是你的头文件自带了，
> 在 Define 里加 `OTA_HAS_STD_TYPES` 即可。

### 3.1 Target 页

| 项 | Bootloader | APP | 备注 |
|---|---|---|---|
| ★ Code Rom Size | **Huge** | **Huge** | 代码超 64KB 必需。不一致会导致函数指针宽度不同、跳转必飞 |
| **Memory Model** | **不要动（保持 Tiny）** | **不要动** | ★ 内存模型改用 Misc controls 里的 `LARGE` 指定，见 §3.4。**为什么不用下拉框**：下拉框在 .uvproj 里存的是数字，不同版本枚举顺序不一定一致；而写进 Misc controls 是纯文本、编出来的命令行肉眼可查。更关键的是：万一两边冲突，C251 会直接报 `RESPECIFIED OR CONFLICTING CONTROL`（实测），不会静默用错模型 |
| Xtal (MHz) | 11.0592 | 11.0592 | 必须与 STC-ISP 里 IRC 频率一致 |
| Use Multiple DPTR | 可勾 | 可勾 | STC32G 支持双 DPTR，勾了提速 |

> ★★ **为什么内存模型必须是 LARGE（这条比地址还关键）**
>
> C251 里裸指针 `u8 *` 的宽度**随内存模型变**（本机 C251 V5.60 实测）：
>
> | 模型 | `sizeof(u8 *)` | 能指向的空间 |
> |---|---|---|
> | XSMALL / TINY | **2** | 只有 EDATA `0x0000~0x0FFF`（4KB） |
> | SMALL / LARGE | **4** | 通用指针：data / edata / xdata / code 全都能指 |
>
> 本工程所有缓冲区参数都写成裸 `u8 *`（`flash_iap_read` 的 `buf`、`ring_init` 的
> `buf`、`log_printf` 的 `tag`……），而缓冲区实际全在 **xdata**（物理 `0x010000` 起）。
> 用 Tiny 模型时编译器只给一句
>
> ```
> warning C150: 'xdata' pointer converted to 'edata' pointer
> ```
>
> 的**警告**，然后把高 8 位直接丢掉——函数会跑到 EDATA 里一个完全无关的地址去读写。
> **数据静默损坏、程序看着还在跑**，数据量小于 256 字节时甚至看不出问题。
> 你上一轮贴的构建日志里那 30 多条 C150 就是它。
>
> 换成 `LARGE` 后同样的代码**零警告**，`data` 段也从 131 掉到 **8**（只剩寄存器组）。
>
> 为了防止这件事再被悄悄改回去，`COMMON/ota_mcu.h` 里加了两道编译期门禁：
> 没定义 `OTA_PTR_GENERIC` 会 `#error` 给人话提示；`sizeof(u8 *) < 4` 直接编不过。

### 3.2 Output 页

| 项 | 取值 | 为什么 |
|---|---|---|
| Create HEX File | **✔** | 烧录文件 |
| ★ HEX Format | **HEX-386** | **最关键的一项**。APP 起始在 `0xFF4000` 远超 64KB，HEX-80 只有 16 位地址，会被截断成 `0x4000`，烧进去后 BL 找不到入口 |
| Debug Information | ✔（调试期） | 便于看 map |

### 3.3 L251 Locate 页（**这里留空**）

| 工程 | Code Range |
|---|---|
| Bootloader | **留空**（用设备默认 `0xFF0000`，正是 BL 要的） |
| APP | **留空** |

> ★★ **`CODE(0xFF4000)` 写在这个页里是无效的**——这是 v4 阶段真实踩到的事故。
>
> 原因：`L251 Misc` 里的 `CLASSES(...)` 会**整体替换**链接器的内存类表。
> `CODE` 类只要不在那张表里，就退回设备默认基址 `0xFF0000`，
> 于是 APP 的启动代码被链接到 `0xFF0000~0xFF00E2`，**正好压在 Bootloader 的
> 复位向量和向量表上**。用 `merge_hex.py --info` 看到的现象就是
> `[1] APP.hex 0xFF0000-…`，而不是 `0xFF4000`。
>
> 唯一有效的写法是把 `CODE` 也塞进 CLASSES，见 §3.4.2。

### 3.4 C251 页 / L251 Misc 页

#### 3.4.1 C251 页（编译器）

| 项 | Bootloader | APP | 备注 |
|---|---|---|---|
| Optimization | Level 7 | Level 7 | 两个工程一致，避免"优化等级不同 → 时序/尺寸不同"的干扰 |
| **Misc controls** | `LARGE` | `LARGE` | ★ 内存模型就在这里指定 |
| **Define** | `OTA_PTR_GENERIC=1` | `OTA_BUILD_APP=1,OTA_PTR_GENERIC=1` | 多个宏用**逗号**分隔（空格会报 `')' AFTER PARAMETER EXPECTED`，实测） |
| Include Paths | `..\..\BSP;..\..\COMMON;..\..\DRV;..\..\OTA` | 同上 + `;..\..\APP` | 见 §3.0 |

> ⚠ **APP 工程里绝对不要写 `void xxx(void) interrupt n`**。
> 一写，链接器就会在代码块起始生成中断向量表，直接盖掉 Bootloader 的向量区。
> APP 的中断一律写成普通函数 `void uart_isr(void)`，启动时用
> `ota_isr_bind(irq_no, uart_isr)` 挂到 BL 的向量跳板上。

#### 3.4.2 L251 Misc 页（`Misc controls` 文本框）

**Bootloader**：

```
CLASSES(EDATA(0x0-0xFFF), HDATA(0x0-0xFFF), NCONST(0xE000-0xEFFF), HCONST(0xFF0100-0xFFFFFF), ECODE(0xFF0100-0xFFFFFF))
```

**APP**（注意 `CODE(...)` 在 `CLASSES` **里面**）：

```
CLASSES(CODE(0xFF4000-0xFFFFFF), EDATA(0x0-0xFFF), HDATA(0x0-0xFFF), NCONST(0xF000-0xFFFF), HCONST(0xFF4200-0xFFFFFF), ECODE(0xFF4200-0xFFFFFF))
```

| 片段 | 作用 |
|---|---|
| `CODE(0xFF4000-…)` | APP 的启动代码/复位跳转落 `FF:4000h`。**只有写进 CLASSES 才生效** |
| `ECODE`/`HCONST(0xFF4200-…)` | 函数体和 far 常量跟着搬进 FF 块。留 `0xFF4000~0xFF41FF` 共 512B 给 CODE 类 |
| `NCONST(0xE000-0xFFFF)` | 16 位 near 常量的兜底位置（LARGE 模型下用量为 0，见 §5.5） |
| `EDATA`/`HDATA(0x0-0xFFF)` | 4KB 的 edata/hdata |

> **两个工程的 L251 Misc `Misc controls` 只允许出现 1 条 `CLASSES`**。
> 若 `L251 Locate` 页的「Use Memory Layout from Target Dialog」还勾着，
> Keil 会自动再追加一条覆盖手写配置。判断方法：Rebuild 后打开 `.lnp` 数一数。


### 3.5 Debug 页（**本工程可以不配，配错了会浪费半天**）

Options 对话框的标签顺序是：
`Device | Target | Output | Listing | User | C251 | A251 | L251 Locate | L251 Misc | Debug | Utilities`

**先看一个典型错配**（实际截到的样子）：

```
Use:  STC Monitor-51 Driver        ← 选的是 51 的监控驱动
Driver DLL:  S251.DLL              ← 但 DLL 还是模拟器那套
Dialog DLL:  DCORE51.DLL  p251     ← 同上
```

这三行本身就是矛盾的：`S251.DLL` + `DCORE51.DLL(p251)` 是 **Keil 自带模拟器**的组合，
真正挂上 STC 监控驱动时，Dialog DLL 会变成 STC 注册的那个（形如 `DP51.DLL` / `DP251.DLL`）。
换句话说：**下拉里选了 STC 驱动，但 Keil 实际没加载它**，一旦点 Debug 就会连不上或直接跑飞。

两条路，按阶段选：

#### 路线 A（推荐，P0~P3 全程都用这条）：不接调试器

- `Use:` 保持 **`Use Simulator`**，或者干脆不点 Debug 按钮。
- 原因：本工程每个阶段的判据都是**串口日志**（`[IAP] eeprom=128KB ok...`、状态迁移日志），
  单步调试解决不了"EEPROM 是不是 128K""IAP 能不能写 0x14000"这类问题。
- 更重要：STC 的监控调试**要占用一个串口**，而 STC32G 的 ISP 下载固定走 UART1（P3.0/P3.1），
  Y100E 若也在 UART1，三者会互相踩。少接一个调试器就少一处冲突源。
- 只需把 Debug 页两个复选框理解为"调试时才生效"，平时不用管：
  - `Load Application at Startup`：点 Debug 时先下载程序；
  - `Run to main()`：下完直接跑到 main，省去从复位单步。

#### 路线 B（确需在线单步 / 看变量）：用 STC 的 **251** 监控驱动

1. 下拉里找 **`STC Monitor-251 Driver`**（251 核专用）。**不能用 Monitor-51** ——
   STC32G 是 251 核，指令集与寄存器组都不同，51 驱动连上去表现为"能连、但单步就跑飞"。
2. 选中后点右侧 **`Settings`**，在 STC 的对话框里选**串口号**与波特率
   （建议 115200；线长或干扰大时降到 57600）。
3. 该串口会被调试独占：**不能与 AT 串口、也不能与下载串口是同一路**。
4. 若下拉里根本没有 251 驱动，按顺序排查：
   - 是否只装了 C51、没装 **C251 工具链**（没有 C251 就不会有 251 的驱动项）；
   - 重跑 STC-ISP「**Keil仿真设置**」→「添加型号和头文件到 Keil 中」，它会一并注册驱动；
   - 装完后重启 Keil（器件库与驱动是启动时加载的，热改不生效）。

> 结论：**现在不要在这页花时间**。先把 `Target`（Code Rom Size=Huge）、
> `Output`（Create HEX File + **HEX-386**）、`C251 Misc`（`LARGE`）、`L251 Misc`（`CLASSES(...)`）
> 三页配对，编译出 hex 才是当前卡点。Debug 页等 P2 阶段"BL 跳 APP 后中断丢字节"这类
> 必须单步的问题出现时再回来配。

---

## 4. 把源文件加进工程

右键 `Target 1 → Add Group`，按目录建组，再 `Add Existing Files`。

### 4.1 当前文件实际状态（别再加不存在的文件）

**已实现，可以直接加进工程**（15 个 `.c`）：

| 层 | 文件 | 说明 |
|---|---|---|
| COMMON | `ringbuf.c` | 环形缓冲（UART 收包地基） |
| COMMON | `crc32.c` | 4 位查表 CRC32，含 `crc32_selftest()` |
| COMMON | `log.c` | 精简格式化日志（不依赖 printf 库） |
| COMMON | `param.c` | 参数区双备份 + 断点读写 |
| BSP | `bsp_clock.c` | 主频确认（IRC 由 ISP 设定） |
| BSP | `bsp_gpio.c` | 端口模式、状态灯、PWRKEY |
| BSP | `bsp_uart.c` | UART1 中断收 + 轮询发（Timer2 波特率） |
| BSP | `bsp_timer.c` | Timer0 1ms 时基、阻塞延时 |
| BSP | `bsp_wdt.c` | 看门狗 |
| BSP | `flash_iap.c` | **IAP 三寄存器 + 容量闸门 + BL 写保护 + 探针/自检** |
| OTA | `ota_partition.c` | 分区查询表（`ota_part_of/writable/contains`） |
| OTA | `ota_jump.c` | 向量跳板表、`ota_jump_to_app`、软复位 |
| — | `Project/Bootloader/boot_main.c` | **BL 入口，P0 探针 + P1 启动流程** |
| — | `APP/app_main.c` | **APP 入口，P0 上电自检（不联网、不开中断）** |
| — | `APP/app_config.h` | APP 版本号与阶段开关（纯头文件） |

**尚未实现，暂时不要 Add**（否则链接报 undefined）：

| 文件 | 归属 | 里程碑 |
|---|---|---|
| `OTA/ota_fsm.c` `OTA/ota_verify.c` | BL + APP | P1 之后（镜像校验/搬运/回滚） |
| `DRV/uart_at.c` `http_ota.c` `mqtt_ctrl.c` `net_dev.c` | APP | P2 |
| `APP/app_ota_task.c` `app_report.c` | APP | P2 |

### 4.2 现在就能编译的**最小 BL 工程**（P0 用）

只需 Add 这 **11** 个文件：

```
Project/Bootloader/boot_main.c
BSP/bsp_clock.c   BSP/bsp_gpio.c   BSP/bsp_uart.c
BSP/bsp_timer.c   BSP/bsp_wdt.c    BSP/flash_iap.c
COMMON/crc32.c    COMMON/log.c     COMMON/ringbuf.c
OTA/ota_partition.c
```

**不要加** `param.c` 和 `ota_jump.c`。原因写死在 `boot_main.c` 顶部：
`BL_PROBE_ONLY = 1` 时这两个宏自动为 0，包含与调用都被条件编译掉了：

```c
#define BL_PROBE_ONLY   1        /* 1 = 只跑探针死循环；0 = 走 P1 启动流程 */
#if (BL_PROBE_ONLY != 0)
  #define OTA_BL_USE_JUMP    0   /* ota_jump.c：P0 不需要 */
  #define OTA_BL_USE_PARAM   0   /* param.c   ：P0 不需要 */
#endif
```

> **不想从工程里删文件？** 右键文件名 → `Options for File 'xxx.c'` →
> 取消勾选 **Include in Target Build**。效果等价，且随时能勾回来。
>
> ★ 这不是"为了省事"。`ota_jump.c` 里的 `ota_jump_to_app()` 用函数指针做间接调用，
> 链接器一旦遇到间接调用就**放弃 overlay 分析**，于是所有函数的 data 帧不再复用、
> 全部累加，128 字节的 `data` 段立刻爆掉（实测 135 字节）。
> P0 阶段去掉它，overlay 恢复正常，`data` 会掉到 60 字节上下。
>
> ⚠ **只把调用摘掉（条件编译）是不够的**：只要 `ota_jump.c` 还参与链接，
> 它里面的间接调用就还在目标文件里，链接器照样不敢做 overlay；
> 而且 `L16 UNCALLED SEGMENT` 说明——**没被调用的段依然会占 `data` 字节**，
> 只是"不参与 overlay 复用"。所以必须让它**不参与链接**（删掉或取消 Include
> in Target Build），`data` 才会真正降下来。

### 4.3 APP 工程（P0 现在就能编）

`Project/APP/STC32G_OTA.uvproj` 已经存在，把下面 **12 个文件**加进去即可
（`app_main.c` 已在工程里，其余按需 Add）：

```
APP/app_main.c
BSP/bsp_clock.c   BSP/bsp_gpio.c   BSP/bsp_uart.c
BSP/bsp_timer.c   BSP/bsp_wdt.c    BSP/flash_iap.c
COMMON/crc32.c    COMMON/log.c     COMMON/param.c    COMMON/ringbuf.c
OTA/ota_partition.c
```

**不要加 `ota_jump.c`**：它里面的间接调用会让链接器放弃 overlay，`data` 段必爆
（和 BL 那边同一个原因）。APP 不跳转，也不需要向量跳板表。

#### APP 与 BL 的四处差异（其余选项必须一模一样）

| 位置 | BL | APP |
|---|---|---|
| `C251` → Misc controls | `LARGE` | `LARGE`（相同，但都要填） |
| `C251` → Define | `OTA_PTR_GENERIC=1` | **`OTA_BUILD_APP=1,OTA_PTR_GENERIC=1`** |
| `L251 Misc` → `CODE(...)` | 不写（用设备默认 `0xFF0000`） | **`CODE(0xFF4000-0xFFFFFF)`，且必须在 CLASSES 里** |
| `L251 Misc` → `NCONST(...)` | `0xE000-0xEFFF` | **`0xF000-0xFFFF`** |

APP 的完整 Misc controls（一整行，`CODE` 在最前面、在 `CLASSES` 的括号**内部**）：

```
CLASSES(CODE(0xFF4000-0xFFFFFF), EDATA(0x0-0xFFF), HDATA(0x0-0xFFF), NCONST(0xF000-0xFFFF), HCONST(0xFF4200-0xFFFFFF), ECODE(0xFF4200-0xFFFFFF))
```

> `ECODE/HCONST` 从 `0xFF4200` 起（而不是紧贴 `0xFF4000`）是故意的：
> `0xFF4000-0xFF41FF` 那 512 B 留给 `CODE` 类（复位跳转 + 启动代码 + 代码区常量）。
> 实测 CODE 类用了 224 B，余量够。

#### `OTA_BUILD_APP=1` 到底改了什么

它只影响两个文件里的**中断函数形态**：

```c
#if defined(OTA_BUILD_APP)
void bsp_timer_isr(void)              /* APP：普通函数，等 BL 的向量回调 */
#else
void bsp_timer_isr(void) interrupt 1  /* BL ：自己拥有中断向量 */
#endif
```

原因：中断向量固定在 `FF:0003h/000Bh/0023h`，只有**一份**，它属于 BL。
APP 若还写 `interrupt`，就会在 `0xFF4000` 区生成一份**永远不会被执行**的副本，
而 APP 自己的 `s_ms`、`s_rx` 永远是空的——现象是"串口收不到模组应答"、
"`bsp_timer_delay_ms()` 死等"，极难排查。

P0 阶段干脆全程 `EA=0`（`app_main.c` 里两次显式写死），连这个坑都不碰：
发送走 `bsp_uart_putc()` 里 `EA==0` 的轮询分支，够用。等 P1 做向量跳板时再打开。

---

## 5. 编译

1. `Project → Rebuild all target files`（F7 旁边的重建，快捷键因版本而异）。
2. 看 **Build Output** 窗口最后两行：

```
Program Size: data=xxx xdata=xxx code=xxxxx
".\Objects\Bootloader" - 0 Error(s), 0 Warning(s).
```

3. **核对起始地址**（最容易出错的一步）：打开 `Objects/Bootloader.map`（或 `*.M51`），
   搜 `CODE` 段，确认类似：

```
CODE    0FF0000H   00001234H   ...
```

   必须是 `0FF0000H`（BL）/ `0FF4000H`（APP）。如果是 `00000000H` 或 `0FF0000H`
   （APP 却是 FF:0000），说明 `CLASSES` 里的 `CODE(...)` 没生效，见 §3.3。

4. 确认 `.hex` 已生成在 `Objects/` 下，且用文本编辑器打开能看到 `:0200000400FF...` 这类
   **04 型扩展地址记录**（HEX-386 的标志）。只有 `:10xxxx00` 而没有 `04` 记录 → HEX 格式没改对。

### 5.4 不想开 Keil 时的回归构建（可选）

本机装了 Keil 的话，可以用脚本按 uVision 一模一样的命令行跑一遍全量构建，
不用等 IDE、也不用点对话框：

```bash
bash tools/build_local.sh
```

它会做三件事：编译 BL 与 APP 的全部源文件 → 链接 → 生成 HEX-386 并回读体检。
输出里的 `Program Size` 与 Keil Build Output 里的完全一致（同一套 exe、同样的参数）。

> 前提：Keil 装在 `C:/KEIL_V5`。路径不同就改脚本头部的 `CC`/`LD`/`OH` 三个变量。
> 脚本里的编译参数是从 `Listings/*.lst` 头部的 `COMPILER INVOKED BY:` 抄下来的，
> 与工程选项一一对应，**不要凭记忆改**。

### 5.5 用工具体检 hex（比肉眼查 `04` 记录快）

#### 先看一个真实的失败样本

```
0x008000-0x0081BF     448 B    ← 全是 ASCII：字符串常量
0xFE0000-0xFE126B    4716 B    ← C251 机器码（函数体）
0xFF0000-0xFF00F0     213 B    ← 稀疏小段（中断向量 + startup）
```

**镜像被拆成三个块**，原因在于 `L251 Locate` 只把 **startup / 向量**搬到了
`0xFF0000`，而函数体属于 **`ECODE` 类**，仍留在设备默认的
`CLASSES(ECODE(0xFE0000-0xFFFFFF))` 基址 `0xFE0000`。

后果有两个，都很致命：

1. **代码落在 `0xFE0000`**，也就是 IAP 地址 `0x0000`——正是本工程规划的
   暂存区起点。BL 把自己的代码写在了 OTA 要擦写的区域里；
2. **向量在 `0xFF0000`、代码在 `0xFE0000`**，两者不在同一块，
   中断向量跳过去以后行为不可预期。

#### 陷阱：`.lnp` 里出现两条 CLASSES

`Objects\Bootloader.lnp` 是链接器真正读到的命令文件。如果里面是这样：

```
CLASSES(EDATA(...), ..., NCONST(0xE000-0xFFFF), HCONST(0xFF0100-...), ECODE(0xFF0100-...))   ← 我们填的
CLASSES (EDATA (0x0-0xFFF), HDATA (0x0-0xFFF),                                              ← 自动生成的
HCONST (0xFE0000-0xFFFFFF), ECODE (0xFE0000-0xFFFFFF))
```

**后一条会把前一条覆盖掉**，而且**不报任何错**。症状很隐蔽：
`NCONST` 的改动生效了（因为它只在第一条里出现），而 `HCONST/ECODE` 纹丝不动
（被第二条重置回 FE0000）——看起来像"部分生效"。

原因和修正在**两个不同的页**上，这是最容易走错的地方：

| 页签 | 控件 | 要做的事 |
|---|---|---|
| `L251 Locate` | 勾选框「Use Memory Layout from Target Dialog」 | **取消勾选** |
| `L251 Misc` | 文本框 `Misc controls` | 填入手写的那**一条** `CLASSES(...)` |

前一次修正里我把勾选框错记在 `L251 Misc` 页，导致在 Misc 页上找不到它、
勾选框一直没被取消，自动生成的第二条 `CLASSES` 就始终追加在末尾。

> 判断方法：Rebuild 后先看 `.lnp` 里有几条 `CLASSES`。**只能有一条。**
> 有两条 → 说明 `L251 Locate` 页那个勾还在。

#### 处理

勾掉之后，把 `L251 Misc` 页的 Misc controls 填成（**一整行**，不要换行）：

```
CLASSES(EDATA(0x0-0xFFF), HDATA(0x0-0xFFF), NCONST(0xE000-0xEFFF), HCONST(0xFF0100-0xFFFFFF), ECODE(0xFF0100-0xFFFFFF))
```

三个字段的意图要清楚，否则后面改分区表会看不懂：

| 字段 | 值 | 意图 |
|---|---|---|
| `ECODE` | `0xFF0100` 起 | 函数体从**向量之后**开始，挤进 FF 块，不再占 `0xFE0000` |
| `HCONST` | `0xFF0100` 起 | far 常量（长字符串）跟着函数体走 |
| `NCONST` | `0xE000-0xEFFF` | near 常量是 **16 位寻址**，物理上搬不出低 64K，只能安排在 `0xE000` 以上。LARGE 下用量为 0（见下） |

#### 改用 LARGE 模型之后：NCONST 那段自己消失了

v4 曾为 near 常量单独切了 8 KB（`0xE000` / `0xF000` 两块），因为 TINY 模型下
字符串字面量走 NCONST（16 位地址）。**换成 LARGE 后实测 NCONST 用量 = 0 字节**：

```
BASE        START       END         USED      MEMORY CLASS
000000H     00F000H     00FFFFH               NCONST      ← USED 是空的
000000H     FF4200H     FFFFFFH     000353H   HCONST      ← 字面量全跑这儿了
```

所以 BL 的 `--info` 现在是**干净的一段**（不再是"两段 + 向量碎块"）：

```
0xFF0000-0xFF0002       3 B  IAP 0x10000  BOOTLOADER  代码   ← 复位跳转
0xFF000B-0xFF000E       4 B  IAP 0x1000B  BOOTLOADER  代码   ← 向量（稀疏）
0xFF0023-0xFF1FC7    8101 B  IAP 0x10023  BOOTLOADER  代码   ← 函数体+常量
[身份] Bootloader（0xFF0000~0xFF1FC7，IAP 0x10000~0x11FC7）
```

> 中间那几段碎块是中断向量表——没被用到的向量不产生字节，所以是稀疏的，正常。
> **"代码段被打成 3 段"不等于失败**，看 `[连续性]` 那行有没有报错才算数。

#### APP 的 hex 起点为什么是 0xFF0000

链接器会在 **CODE 类的 BASE 地址**（设备默认 `0xFF0000`）强制生成 3 字节复位跳转
`?CO?start251?4`，无论 `CODE()` 还是 `CLASSES()` 都搬不走它（实测：设了
`CLASSES(CODE(0xFF4000-…))` 后 CODE 段从 `0xFF4000` 起，但这段仍在 `0xFF0000`）。
于是 **APP.hex 里必然带这 3 个字节**，而这段属于 Bootloader。

`merge_hex.py` 默认会按分区裁剪：判定为 APP 镜像后，把它落在 `BOOTLOADER` 分区的
字节丢掉，并打印一行说明：

```
[裁剪] 判定为 APP 镜像，丢弃不属于它的字节：BOOTLOADER 3 B
       （0xFF0000 上的 ?CO?start251 是链接器强制生成的复位跳转，归 Bootloader）
```

想看到未裁剪的原始镜像就加 `--no-trim`。

#### 分区表 v5（已落到 `ota_partition.h`）

```
STAGING      0x00000-0x0BFFF  48 KB
PARAM A/B    0x0C000 / 0x0C800  2K + 2K
RESERVED     0x0D000-0x0DFFF   4 KB
NCONST       0x0E000-0x0FFFF   8 KB  ★ 只读（LARGE 下用量 0，留作兜底）
BOOTLOADER   0x10000-0x13FFF  16 KB  ★ 只读（FF:0000）
APP          0x14000-0x1FFFF  48 KB  （FF:4000）
```

相对 v4 的三处变化，每一处都有实测理由：

| 变化 | 为什么 |
|---|---|
| 两块 NCONST 槽 → 合并成一块 8 KB 只读兜底 | LARGE 模型下 NCONST 用量为 0，8 KB 白占 |
| **BL 8 KB → 16 KB** | LARGE 下 BL 镜像到 `0xFF1FC7`，8 KB 槽只剩 **57 字节**；P1 还要加 `ota_verify.c`/`ota_fsm.c`，必爆 |
| **APP 入口 0xFF2000 → 0xFF4000** | 跟着 16 KB 的 BL 走 |

暂存区必须 ≥ APP 才装得下整包，所以两者都是 48 KB。

> ⚠ **两个工程的 `NCONST` 窗口仍然必须不同**（BL `0xE000-0xEFFF`，APP `0xF000-0xFFFF`）。
> 现在用量是 0，但哪天某个文件写出 16 位寻址的常量，填成一样就会互相覆盖。

#### 修完后的基准输出

Rebuild 成功后跑：

```bash
python tools/merge_hex.py --in Project/Bootloader/Objects/Bootloader.hex --info
python tools/merge_hex.py --in Project/APP/Objects/STC32G_OTA.hex --info
```

应当看到（本机 CLI 复刻构建的实际数字，你的会差几十字节）：

| 工程 | Program Size | hex 落点 |
|---|---|---|
| BL | `data=8.0 edata+hdata=256 xdata=3590 const=538 code=7570` | `0xFF0000~0xFF1FC7` |
| APP | `data=8.0 edata+hdata=256 xdata=3374 const=851 code=7875` | `0xFF4000~0xFF40DF` + `0xFF4200~0xFF6332` |

三个异常一眼可辨：

| 输出 | 含义 | 处理 |
|---|---|---|
| APP 出现 `0xFF0000~…` 且**只有 3 字节** | 正常，链接器强制的复位跳转，合并时会被裁掉 | 无需处理 |
| APP 出现 `0xFF0000~0xFF00E2`（224 B） | `CODE` 没写进 `CLASSES`，启动代码压在 BL 上 | 见 §3.4.2 |
| `起始地址 : 0x008000` 或 `0xFE0000` | `ECODE` 没搬走 / 两条 CLASSES 覆盖 | 见本节上面两条 |

> `data=8` 是 LARGE 模型的正常值（只剩 8 字节寄存器组）。如果你看到 `data=131`
> 这种接近 128 的数字，说明模型又变回 TINY 了。

---

## 6. STC-ISP 一次性配置

打开 STC-ISP，按下面逐项核对（**这一屏是所有分区设置的物理依据，必须与 `ota_partition.h` 一致**）：

| 位置 | 项 | 取值 | 说明 |
|---|---|---|---|
| 左上 | 芯片型号 | `STC32G12K128-Beta` | 选错芯片 SFR 地址全错 |
| 左上 | 串口 | 你下载器的 COM 口 | 点「扫描串口」刷新 |
| 左上 | 最低/最高波特率 | 2400 / 115200 | 下载时自动协商 |
| 左上 | 起始地址 | `0x0000` | 保持默认 |
| 硬件选项 | 输入用户程序运行的 IRC 频率 | **11.0592 MHz** | 必须与 Keil 的 Xtal 一致 |
| 硬件选项 | 上电复位使用较长延时 | **✔** | 模组上电时序需要 |
| 硬件选项 | 允许低电压复位检测 | ✔ | 掉电保护 |
| 硬件选项 | 复位脚用作 I/O 口 | 按硬件 | 若 P5.4 当复位脚就取消勾选 |
| **硬件选项** | **设置用户EEPROM大小** | **128 K** | ★ 必须 128K，否则 IAP 到不了 `0x10000`，整个分区表作废 |
| 硬件选项 | 用户 ISP 区 | **不使用 / 0 K** | 见下方说明，本工程不用这个机制 |

> **关于"用户 ISP 区"**：本工程把 Bootloader 当**普通代码**烧到代码块起始 `FF:0000h`
> （复位就是从那里开始执行），所以**不需要** STC 的"用户ISP程序"机制。
> 若你的 STC-ISP 版本有该选项，保持 0 / 不勾选；一旦启用，它会从代码块头部再切一块，
> 导致 `0x10000` 的归属变化，分区表就要重算。
>
> 于是 `OTA_USER_ISP_SIZE = 0x2000` 这个名字稍微有点误导，它实际含义是
> **"BL 占用代码块的前 8 KB"**，不是 STC 的用户ISP区。以 `ota_partition.h` 注释为准。

### 6.0 STC-ISP 的可寻址范围（「HEX地址异常」的真正原因）

打开程序文件后如果提示：

```
HEX地址异常,超出可寻址范围 !
打开文件 "....\Objects\Bootloader.hex" 成功 !
```

**这不是 hex 损坏，是地址超出了 STC-ISP 认的范围。** 规则（国芯论坛官方回复）：

> 可寻址范围 = `FE:0000` ~ `FE:0000 + 设置的用户EEPROM大小`
> 「isp 烧录位置，就是"程序文件"选项卡内显示的位置，一般 0xFF0000 开始」

所以有两条硬性要求，**缺一不可**：

| 条件 | 正确值 | 不满足时 |
|---|---|---|
| EEPROM 大小 | **128 K**（→ 范围 `0xFE0000~0xFFFFFF`） | EEPROM 若是 4K，范围只有 `FE:0000~FE:0FFF`，`0xFF0000` 直接超范围 |
| hex 里所有地址 | **必须 ≥ `0xFE0000`** | near 常量段 `0x00Exxx` 会被判为非法 |

第二条容易被忽略：**Keil 的 near-code 别名窗口 `0x0000~0xFFFF` 硬件上完全合法**
（`NCONST` near 常量就落在那儿，物理上等同 `FE:0000+addr`），
但 **STC-ISP 不认这个写法**。

> ★ 本项目真实踩过：LARGE 模型改造**之前**生成的 `Bootloader.hex` 里带着
> `0x00E000-0x00E1BF` 那 448 B 的 near 常量，STC-ISP 直接拒收。
> 改造后 `NCONST` 用量归零，hex 里只剩 `0xFF0000` 起的段，问题自然消失。
>
> **判断方法**（不用猜）：
> ```bash
> python tools/merge_hex.py --in Objects/Bootloader.hex --info
> ```
> 出现下面这段就是它（退出码 5）：
> ```
> [ISP兼容] !! 448 字节落在 0xFE0000 以下 —— STC-ISP 会报
>            「HEX地址异常,超出可寻址范围 !」
>            问题段：0x00E000-0x00E1BF
> ```
> 处理：**Rebuild 重新生成**，别用旧 hex。

**顺序也很重要**：先在 STC-ISP 里把 EEPROM 改成 128K、点「下载/编程」让它写进选项字节，
**再打开程序文件**。反过来先打开文件、后改 EEPROM，校验用的还是旧值。

### 6.05 三个勾选框（每一项都能让这一步白做）

打开 hex 成功 ≠ 能烧对。STM32 出身的习惯在这里会踩坑：STC 的「硬件选项」是**独立的一组
选项字节**，界面上那个 EEPROM 大小只是**工具侧的声明**，勾选框不勾就一个字都不会写进芯片。

| 勾选框 | 首次烧 BL | 以后换 BL/APP | 原因 |
|---|---|---|---|
| **本次下载需要修改硬件选项** | **必须勾** | **必须取消** | 「设置用户EEPROM大小 = 128K」在这一组里，不勾就不写进选项字节。而修改硬件选项在多数 STC 型号上伴随**整片擦除**，升级时勾它会把 STAGING / PARAM 一起清掉 |
| 上电复位时由硬件自动启动看门狗 | 建议取消 | 取消 | 上电到 `bsp_wdt_init()` 之间没人喂狗；`flash_iap_selftest()` 里整扇区备份→擦→写→逐字节还原要几百 ms。等 P2 稳定后再考虑打开 |
| 清除EEPROM缓冲区 | 保持勾选 | 保持勾选 | 缓冲清空后工具不会去写 `FE:` 段，暂存区/参数区不会被工具覆盖 |

> ★ **`flash_iap_init(128)` 里的 128 是我们"声明"的值，不是从芯片读回来的实测值。**
> `s_eeprom_size` 只保存形参，`flash_iap_selftest()` 里那句
> `capacity mismatch: cfg=%uKB want=128KB` **永远不会因为芯片设置错了而触发**。
> 所以芯片真实值只能靠两条路确认：
>
> 1. STC-ISP 点 **「检测MCU选项」**，读回芯片里的真实选项（含用户EEPROM大小）；
> 2. `flash_iap_selftest()` 的 `probe@0x14000=W/NA` 探针结果（写进去能不能读回来）。
>
> 如果只做 2 而不做 1，`probe=NA` 会被误判成"第三地址寄存器没生效，得改走方案 (b)"，
> 实际原因可能只是 **EEPROM 没真的设成 128K**。这两个结论的修法完全相反，别混。

---

## 6.1 两个环境级问题（代码改不动，必须先解决）

### ⓪ 先看 C500：授权没生效

```
warning C500: INCORRECT SERIAL NUMBER IN 'TOOLS.INI'      ← 出现在每个 .c 的第一行
```

这条说明 `TOOLS.INI` 里登记的序列号与本机安装的 PID **对不上**，所以工具仍按
评估版运行。常见三种原因：

1. 贴的是 **C51/MDK** 的号，不是 **C251** 的号（两者授权独立）；
2. `File → License Management` 里显示的 **PID** 与授权文件里的 PID 不一致；
3. 序列号分行贴错/漏字符（一条授权是 5 组，每组 5 位）。

C500 消失、且不再出现 `RESTRICTED VERSION` 那一行，才算授权真正生效。
**这一步没法用代码绕过**，也不应该绕过——评估版就是硬性 2KB。

### ① Keil C251 未授权 → `ERROR L250: CODE SIZE LIMIT`

```
******************************************************************************
* RESTRICTED VERSION WITH 0800H BYTE CODE SIZE LIMIT; USED: 1861H BYTE (304%) *
******************************************************************************
*** ERROR L250: CODE SIZE LIMIT IN RESTRICTED VERSION EXCEEDED
```

这是 **Keil C251 评估版限制 2 KB 代码**（0x800），不是代码问题。
本工程 Bootloader 约 5.9 KB，**任何精简都过不了 2 KB 这条线**（连 P0 探针都要 3 KB 左右）。

处理：给 µVision 装上 C251 的正式授权（`File → License Management` 里确认
"C251" 一栏不是 Evaluation）。装好后再 Rebuild，`L250` 自动消失。

> 想先验证工具链/烧录链路是否通，可以单独建一个只有 `bsp_uart.c + log.c` 的
> hello 工程（<1 KB）烧一次，确认串口能出字。但完整 BL 必须授权。

### ② `L107: ADDRESS SPACE OVERFLOW, SPACE: DATA`

```
Program Size: data=162.0 edata+hdata=256 xdata=3457 const=506 code=5928
                                        ^^^^^^^^^^^ data 只有 128 字节
```

`data` 段（直接寻址，128 B）装不下。**根治办法就是换成 LARGE 模型**（见 §3.1），
换完之后 `data` 从 131 直接掉到 8——因为所有未限定变量都默认落 xdata 了。

| 办法 | 操作 | 说明 |
|---|---|---|
| **A（根治）** | C251 的 Misc controls 填 `LARGE` | `data=8`，从此不用再数那 128 字节。**同时修掉指针截断**（这才是真正的原因，见 §3.1） |
| B | 大对象/大帧显式加 `xdata` | 代码里已做 20 余处；LARGE 之后这层仍然保留，属于"显式优于隐式" |
| C | 把 P0 用不到的 .c 取消 Include in Target Build | 只是缓解，且会掩盖真实尺寸，**不再作为首选** |

#### 我两次关于 Memory Model 的判断都错在哪

- **第一次**（早先）：说"设成 Large，变量默认落 xdata，`data` 就不爆了"。
  方向对，但当时的理由是"省 data"，漏掉了真正要紧的东西——**指针宽度**，
  所以后来又撤回了，属于对了一半。
- **第二次**（撤回）：说"Large 会让 `const char *` 变成 2 字节 xdata 指针，
  指到 code 区的字面量会报 different memory space"。
  **这句是错的**。实测 C251 V5.60：LARGE 下 `sizeof(u8 *)` = **4 字节通用指针**
  （带存储类字节），能同时指 data/edata/xdata/code，字符串字面量传参**零警告**：

  ```
  TINY : warning C150 'code' pointer converted to 'edata' pointer      ← 会截断
  LARGE: （无警告）
  ```

  只有 XSMALL/TINY（2 字节 edata 指针）才会截断。
- **正确的结论**：内存模型必须 LARGE，理由不是省 data，而是**裸指针必须是通用指针**。
  `data=8` 只是附带好处。

现在这条已经做成编译期门禁（`ota_mcu.h`），改错了直接编不过，不会再靠人记。

### ③ 顺带核对：链接命令行里的 CODE 类

链接命令行（`Objects\*.lnp`）里应该看到：

```
TO ".\Objects\Bootloader"
CLASSES(EDATA(0x0-0xFFF), HDATA(0x0-0xFFF), NCONST(0xE000-0xEFFF),
        HCONST(0xFF0100-0xFFFFFF), ECODE(0xFF0100-0xFFFFFF))
```

APP 的则是：

```
CLASSES(CODE(0xFF4000-0xFFFFFF), EDATA(0x0-0xFFF), HDATA(0x0-0xFFF),
        NCONST(0xF000-0xFFFF), HCONST(0xFF4200-0xFFFFFF), ECODE(0xFF4200-0xFFFFFF))
```

两条都要满足：

1. **`CLASSES` 只有 1 条**（有 2 条 → `L251 Locate` 页的勾没取消）；
2. **CODE 地址在 FF 块里**：BL 用设备默认 `0xFF0000`，APP 必须写进 CLASSES
   才是 `0xFF4000`（外面写 `CODE(0xFF4000)` 无效，见 §3.3）。
   若看到 `0xFE0000` 之类，说明 ECODE 没搬走。

---

## 7. 烧写

### 7.0 第一次上电该烧哪个文件（先看这张表）

工程里有**两个编译开关**，它们决定"烧进去之后能不能看到 APP 在跑"：

| 开关 | 位置 | 现在的值 | 含义 |
|---|---|---|---|
| `BL_PROBE_ONLY` | `Project/Bootloader/boot_main.c` | **0**（第一步时是 1） | 0 = 读参数 → 校验 → 跳 APP；1 = 只跑 IAP 自检后死循环 |
| `APP_STAGE_P0` | `APP/app_config.h` | **1** | APP 只做上电自检，不联网、不开总中断 |

`BL_PROBE_ONLY = 1` 时，**APP 编得再正确也不会被执行**。所以分两步：

| 步骤 | 烧什么 | 目的 | 状态 |
|---|---|---|---|
| **第一步** | BL-only（`step1f_BL_realprobe_v0.1.hex`） | 验证 IAP 能否写、EEPROM 是否真的 128K | ✅ 已通过 |
| **第二步** | **合并镜像** `build/step8_BL_APP_v0.8.hex`（BL+APP，≈27 KB，`0xFF0000–0xFF7393`） | 验证 BL → APP 跳转 + 日志可信 | ✅ 已通过（v0.7 全部数字正确；v0.8 加行校验和用于查偶发空格） |
| **第三步（A1）** | **合并镜像** `build/step15_iap_probe_BL_APP.hex`（BL+APP，29692 B，`0xFF0000–0xFF777D`） | 验证 **BL 能读懂并校验 STAGING 里的镜像**（只读，不搬不写） | ← **当前这一步**，A1 的实现见 **§8.13**，v0.9 翻车与 `NOOVERLAY` 根治见 **§8.14** |

> **第二步不需要再改任何开关**：`BL_PROBE_ONLY` 已经切到 0、`BL_DIAG_WINDOW` 已切到 0。
> 直接按 §8.5 烧 `build/step8_BL_APP_v0.8.hex` 即可；
> 若想自己在 Keil 里重编再合并，按 §8.3 的两条命令走。
>
> **第三步（A1）的操作序列与 P0 不同**，它不是"烧完就完"，而是三段：
> 烧 → 第一次上电造镜像并停机 → **断电再上电**看 BL 校验。
> 详见 **§8.13.6**，别漏掉中间那次断电。

**第一步的 hex 演化史**（每一版都是修掉一个具体问题，别拿旧版去烧）：

| 文件 | 加了什么 | 为什么被淘汰 |
|---|---|---|
| `step1_BL_only_v0.1.hex` | 最初的 BL-only | 无 BOOT-POLL 自检、无 TXD 推挽、无心跳 |
| `step1b_BL_poll_v0.1.hex` | 裸通路 `[BOOT-POLL]` + 三个"死在第一句日志前"的修复 | 缺 TXD 推挽 |
| `step1c_BL_txprobe_v0.1.hex` | TXD 推挽 + 灯码自诊断 | 无周期输出，串口打开晚了就一个字都看不到 |
| `step1d_BL_heartbeat_v0.1.hex` | 主循环每 2 s 心跳 `[BL] alive #n p3=xx` | 开机 9 秒内仍可能错过轮询段 |
| `step1e_BL_window_v0.1.hex` | 开机 **9 秒纯轮询宽限期** + 心跳 + 每 8 s 重播摘要 | ★ **探针有假阳性**：APP 区非空时跳过探针却硬设 `writable=1`，打出假的 `selftest OK` |
| **`step1f_BL_realprobe_v0.1.hex`** | 探针改为**必然探 STAGING** + APP 非空标记 `untested` + 打印 BL/APP 头字节 + 中文日志改英文 | ← **当前推荐**，结论只来自实测 |

> ⚠ **别用 `Project/Bootloader/Objects/Bootloader.hex`，除非你刚刚 Rebuild 过。**
> 那个目录里的文件是上次编译留下的，可能还是 LARGE 改造前的旧镜像
> （地址带 `0x00E000` 段），STC-ISP 会直接报「HEX地址异常」。
> 烧之前先跑一次体检确认：
> ```bash
> python tools/merge_hex.py --in build/step1f_BL_realprobe_v0.1.hex --info
> ```
> 必须看到 `起始地址 : 0xFF0000`，且**没有** `[ISP兼容] !!` 警告。

> ★ **为什么第一步不能直接烧合并镜像**
>
> `flash_iap_selftest()` 里有一句探针，动作是"备份扇区 → 擦除 → 写特征值 → 还原"。
> 第一次烧合并镜像时，APP 区已经被烧录器写满了固件；此时若盲探，
> 就等于拿固件赌命 —— 还原到一半掉电（或被看门狗复位），
> 而 APP 的复位入口正好在扇区首地址，**直接变砖**。
>
> 现在代码里已经加了保护：**只有 APP 区首字节为 `0xFF`（空片）时才探**，
> 非空则跳过并打印 `app@0x14000 not blank, skip probe (firmware kept)`。
> 但这也意味着"跳过探针 = 没验证到 IAP 写能力"，所以第一步仍应烧 BL-only。

> ★ 另一个必须知道的点：**第一步烧 BL 时，EEPROM 设置必须是 128K**。
> 烧完再改 EEPROM 大小，STC-ISP 会重新划分 IAP 域，之前烧的内容地址会整体错位。
> 所以 §6 那一屏要**在第一次下载之前**就配好。

### 7.05 串口接线（日志 / 下载 / 模组都在 UART1）

工程用的就是 **UART1**（`SCON`/`SBUF`，中断 4），引脚是 STC 的默认映射：

| 功能 | 引脚 | 方向 |
|---|---|---|
| TXD1 | **P3.1** | MCU 输出 → 电脑收（日志全从这根出） |
| RXD1 | **P3.0** | 电脑输出 → MCU 收（ISP 握手，以后接 AT 回包） |

代码里**从未写过 `P_SW1`**，所以 `S1_S[1:0] = 00`，映射保持默认。接线（CH340 这类 USB 转 TTL）：

| MCU 板 | CH340 下载器 | 说明 |
|---|---|---|
| P3.1 (TXD1) | **RXD** | ★ 交叉接，不是同名对接 |
| P3.0 (RXD1) | **TXD** | ★ 交叉接 |
| GND | GND | **必须共地**，不共地必乱码 |
| VCC (5V/3V3) | 5V / 3V3 | 用下载器供电，方便「点下载→断电→上电」的冷启动 |

> 板子自己独立供电时只接 **TX / RX / GND 三根**，冷启动靠手动拔插电源。
> 你现在的选项里「复位脚用作 I/O 口」是勾着的，P5.4 已经当普通 IO 用了，
> **没有复位键可以进 ISP**，只能靠断电冷启动。

串口助手：**115200、8 数据位、无校验、1 停止位、无流控**，建议勾上"显示时间戳"。

几个必须知道的点：

1. **P3 口没被 `bsp_gpio_init()` 碰过**，保持上电默认「准双向」——这正是 STC 推荐的
   UART 引脚模式，**不要**改成推挽输出。
2. 波特率由 **Timer2** 产生（`AUXR.S1BRT=1`），**Timer0** 单独做 1 ms 时基，两者不冲突。
3. `11.0592MHz / 115200 → RL = 0xFFE8`，误差 **0%**。
4. ★ **主频不是程序设的**，是 STC-ISP 写进选项字节的。所以「输入用户程序运行的 IRC 频率」
   必须是 `11.0592 MHz` 并且**真的写进去过**（即 §6.05 里那个勾选框）。芯片若还停在出厂
   频率，「115200 的波特率」实际会变成别的值，看到的就是一片乱码。

**串口异常速查：**

| 现象 | 先查这条 |
|---|---|
| 全乱码 | IRC 频率选项不是 11.0592MHz / 没写进芯片；或串口助手不是 115200 |
| 收到一两个字符就没了 | 老式 `while(!TI)` 死等（本工程已改成 `s_tx_idle` 握手，留意别改回去） |
| 一个字节都没有 | TX/RX 没交叉、没共地；没冷启动；板子没供电 |
| 下载能成功、但就是没日志 | 说明 P3.0/P3.1 是通的（ISP 走的就是它），问题在波特率/主频 |

> **P1 接模组时的坑（现在还不到，先记着）**：`bsp_uart.c` 目前把日志和 Y100E 放在
> **同一路 UART1** 上。P0 阶段只接 CH340，**千万别把模组的 TX 也接到 P3.0** ——
> 两路推挽 TX 对着打，日志和下载会一起坏。
>
> 换脚位的开关是 `P_SW1`(0xA2) 的 `S1_S[1:0]`（头文件里已核实 `S1_S0/S1_S1` 存在），
> 但 **STC-ISP 下载固定走 P3.0/P3.1**：一旦把 UART1 切到别的脚，就必须两根都引出来，
> 否则"下载"和"看日志"只能二选一。
> **所以结论是：日志永远留 P3.0/P3.1，模组改走 UART2/UART3。**

#### 下载口 = 日志口 = UART1(P3.0/P3.1)，同一个 COM 口

**是的，就是同一对脚。** STC 没有独立的调试/下载接口（不像 ARM 的 SWD/JTAG）：
ISP 是**芯片内部固化的一段监控程序**，上电复位后先跑几十毫秒，在 P3.0/P3.1 上
用自动波特率等主机发来的握手字节；等不到就跳到用户程序的复位地址。

由此派生四条硬约束：

1. **`P_SW1` 改不了下载口。** 它只改"程序运行时"UART1 映射到哪对脚；
   固化 ISP 用的引脚是死的，永远是 P3.0/P3.1。所以**不存在"把下载口换到 P4.3"这回事**。
2. **必须冷启动。** 握手窗口只在**上电瞬间**开一次，程序已经在跑的时候再点下载是没用的
   （这也是为什么你板子上「复位脚用作 I/O 口」勾上以后，只能靠拔插电源）。
3. **下载和看日志抢同一个 COM 口。** 同一个物理串口只能被一个程序打开：
   下载前先**关掉串口助手**（否则 STC-ISP 报"串口已被占用/无法打开串口"）；
   想看日志可以直接用 STC-ISP 自带的「串口助手」页，省得来回开关。
4. ★ **P3.0/P3.1 永远留给 UART1，绝不挪作普通 IO。** 一旦 APP 把 P3.1 配成推挽输出
   并常态拉低，下次上电 ISP 监控程序就发不出握手 → **下载不进去，只能拆线救砖**。
   同理，任何外部器件（模组、电平转换、上拉电阻）挂在这两根线上，都可能在握手窗口
   把电平拽偏。这是最贵的一个坑，写代码时就要守住。

### 7.1 冷启动下载顺序

STC 的 ISP 下载是**冷启动触发**的，顺序错了会一直"正在检测目标单片机..."：

1. 在 STC-ISP 点 **「打开程序文件」**，选 `build/step1_BL_only_v0.1.hex`（第一步）。
2. 点 **「下载/编程」**（此时软件进入等待状态）。
3. **给板子断电，再上电**（或按下载器的"冷启动"按钮）。STC32G 检测到上电握手才开始烧。
4. 观察进度条 → `操作成功`。
5. 烧完自动运行。串口应看到 BL 的启动日志。

> 若一直卡在"正在检测目标单片机"：
> · 下载器 TX/RX 是否与 MCU 的 P3.0/P3.1 交叉接对；
> · MCU 是否已被上一次程序占用 UART1（见第 9 节的坑）；
> · 波特率降到 2400 再试（线长/干扰大时）。

**"检测MCU选项"** 按钮可以先点一次：它会读出芯片型号、固件版本、当前 EEPROM 配置，
确认无误再下载。

### 7.2 第一步的通过判据

串口（**115200 8N1**）应输出：

```
UUUUUUUU                                       ← bl_tx_probe 探针字节（0x55），正常
[BOOT-POLL] poll-mode ok, uart1 p3.1, ...      ← 约 9 秒宽限期，反复刷这一行
[BOOT-POLL] poll-mode ok, uart1 p3.1, ...
...（略，共约 40 行）
[BL] boot, bl_code=0xff0000 app_code=0xff4000
[BL] ==== BL v0.1 (P0) ====
[BL] fosc=11059200Hz
[BL] flash_iap_init(128) -> 0
[IAP] eeprom=128KB
[IAP] bl head: 2 0 2 0                         ← ★ IAP 读通路是否可信，就靠这一行
[IAP] app@0x14000 not blank, probe skipped (fw kept)   ← APP 非空时才有
[IAP]   app head: xx xx xx xx
[IAP] 3addr-reg ok, stg@0x0=W, app@0x14000=untested, BL intact   ← ★ 最关键的一行
[BL] selftest OK
[BL] --- P0 done, see the summary line below ---
[BL] loop alive, heartbeat 2s                  ← 进主循环
[BL] alive #1 p3=3                             ← 每 2 s 一行，永续
[BL] alive #2 p3=3
[BL] == summary: init=0 selftest=0 stg=W app=untested ==        ← 每 8 s 重播
[BL] -> STAGING writable: IAP write path OK
[BL] -> APP untested (region not blank), NOT yet verified
```

**几个字段的含义：**

| 字段 | 含义 |
|---|---|
| `p3=3` | P3 口读回 `0x03`，bit1=1 即 **P3.1 是高电平**（TXD 空闲态正确）。若显示 `p3=fd` 或带 `(txd low!)`，说明该脚被拽低 |
| `bl head: 2 0 2 0` | BL 区（IAP `0x10000`）头 4 字节。有真实机器码 → **IAP 读通路可信**；若是全 `0` 或乱值 → 读通路有问题，下面的探针结论也不可信 |
| `stg@0x0=W` | STAGING 区写-回读**通过**（`W`=writable）。这是 IAP 写能力的**实测**证据 |
| `app@0x14000=untested` | APP 区**没测**（该区非空，探针主动跳过）。**不是失败，但也不是通过** |

| 日志 | 结论 | 处理 |
|---|---|---|
| `stg@0x0=W` + `app@0x14000=W` | ★ **完全通过**，IAP 可写、APP 落点可写 | `BL_PROBE_ONLY` 改 0，进第二步 |
| `stg@0x0=W` + `app=untested` | **有条件通过**：写通路成立，APP 可写性未验证 | 见 §7.6 |
| `stg@0x0=NA` | IAP 写不进去 | **先**用「检测MCU选项」确认 EEPROM 真是 128K；确认无误才判方案 (b) |
| `bl head` 全 `0` | IAP 读通路不可信 | 先查 `IAP_TPS` / 三地址寄存器，别信其它结论 |
| `BL CHANGED after probe!` | 硬件行为异常 | **立即停手**，查 `flash_iap_check_range` 白名单 |
| 有 `alive #n` 但一直没看到 `[BOOT-POLL]` | 串口是开的，只是**开机那 9 秒错过了** | 拔插 USB 重看；不影响结论 |
| 一个字节都没有 | 串口接线/COM 口/上位机 | 先按 §7.05 查接线，再按 §7.3 从第 0 步开始 |
| 灯不闪、也没有日志 | 程序没跑起来 | 查 `main` 是否被链接到 `0xFF0000`（`--info` 看一眼） |

> ⚠ **`eeprom=128KB` 这一行不能当证据**：`flash_iap_init(128)` 里的 128 是代码写死的形参，
> `s_eeprom_size` 只是把它存下来，**读不到芯片选项字节**。所以
> `capacity mismatch: cfg=%uKB want=128KB` 那句**永远因芯片设错而触发不了**，
> 别等它。芯片真实值只看两个地方：「检测MCU选项」的读回值、以及 `probe` 的写-回读结果。

#### 开机第一段是纯轮询的 `[BOOT-POLL]`，长约 9 秒

`build/step1e_BL_window_v0.1.hex` 在 `main()` 里加了一段**裸通路自检 + 宽限期**：
`EA=0`、Timer0 没开、日志模块没初始化，直接走 `bsp_uart_putc` 的轮询分支，
反复播报约 9 秒（`BL_DIAG_WINDOW` 控制遍数）：

```
[BOOT-POLL] poll-mode ok, uart1 p3.1, fosc=11.0592MHz
```

这一段能看到字符，就同时证明了四件事：**程序跑到了 main、主频对、P3.1 线通、
UART1 寄存器配置对**。看不到，问题必定在这四者之内，且与日志/中断/定时器无关。
后面那行 `[BL] boot, bl_code=...` 才是走中断 + 日志模块的正常路径。

> **为什么要有这 9 秒**：板载 CH340 + USB 供电的板子没法"先开串口再上电"
> （见 §7.3 第 0 步）。窗口太短的话，用户关掉 STC-ISP、再打开串口助手就已经错过了。
> `BL_DIAG_WINDOW` 在 `Project/Bootloader/boot_main.c`，**P1 恢复正常启动时改 0**，
> 否则每次上电都白等 9 秒。

### 7.3 串口一个字节都没有，怎么查

**先记住一个判据：波特率错 → 一定会有乱码；什么都没有 → 字节根本没上总线。**
这两件事的排查方向完全不同，别混在一起查。

#### ★ 第 0 步：先排除"打开太晚"（最高优先级，最容易误诊）

原来的固件**所有日志都只在开机瞬间发一次**：`[BOOT-POLL]` ×3 加 `bl_probe` 的 6 行，
合计约 40 个字节，115200 下 **4 ms 就发完了**。之后主循环里只有闪灯，一个字符都不打。

所以下面这种操作会得到**和"根本没发出去"完全一样的结果**：

```
STC-ISP 点下载 → 芯片复位运行 → 4 ms 内把日志全发完
                                    ↓
                    用户这时才去打开串口助手 → 缓冲区里什么都没有
```

**这是"程序在跑（灯闪）+ 下载正常（灯闪）+ 串口一个字都没有"最常见的真因。**

##### ⚠ 不要指望"先开串口再冷启动"

**板载 CH340 + USB 直接供电的最小系统板做不到这件事**，别再往这个方向试：

| 障碍 | 说明 |
|---|---|
| USB 插上就是上电 | 没有"先开串口、后上电"的操作空间；拔插 USB 本身就是上电动作 |
| 下载时 COM 口被独占 | 串口助手必须关掉，否则 STC-ISP 打不开串口 |
| 下载完立刻运行 | STC-ISP 没有"下载完不运行"的选项，芯片马上跳到 `0xFF0000` |
| 没有复位键 | 「复位脚用作 I/O 口」已勾选，P5.4 是普通 IO，只能冷启动 |

所以真实可用的操作序列只有一条，而这**恰恰会让开机日志全部错过**：

```
点下载 → 等"操作成功" → 关掉 STC-ISP → 打开串口助手
                                    ↑ 这几步要好几秒，开机日志 4 ms 前就发完了
```

**结论：不能靠"抢时间"，必须让固件自己持续说话。**

##### 正确做法：烧 `build/step1e_BL_window_v0.1.hex`

这个版本加了两层互补的输出窗口，覆盖两种不同的故障可能：

| 层 | 时机 | 路径 | 窗口长度 |
|---|---|---|---|
| **① 诊断宽限期** | 上电后立刻 | `EA=0` **纯轮询**，不依赖中断/定时器/日志模块 | **约 9 秒**，反复播报 `[BOOT-POLL]` |
| **② 周期心跳** | 之后永续 | `EA=1` **中断发送**，走日志模块 | **永续**，每 2 s 一行 `alive #n`，每 8 s 重播诊断摘要 |

两层是**故意分开的**：如果 ② 因为中断问题出不来，① 还在；
如果 ① 的 9 秒窗口过了用户才开串口，② 还在。任何时刻打开串口，都一定有东西可看。

**烧完之后的完整操作（不需要抢任何时间）：**

```
1. STC-ISP 点下载，等"操作成功"
2. 关掉 STC-ISP（释放 COM 口）
   —— 或者：不关，直接切到 STC-ISP 自带的「串口助手」页，选同一个 COM
3. 打开串口助手：115200 / 8N1 / 无流控
4. 什么都不用做，等着
      若赶上上电 9 秒内  -> 刷出 [BOOT-POLL] 多行
      若已经过了 9 秒     -> 每 2 秒一行 [BL] alive #n
5. 想重新看一遍开机段 -> 拔插一次 USB（这是唯一需要冷启动的场景）
```

> 串口助手里会先看到 8 个 `U` 字符，那是 `bl_tx_probe()` 的探针字节（`0x55`），
> 不是乱码，看到它反而说明发送链路是通的。

> 心跳日志还有个附带作用：它走的是 `EA=1` 之后的中断发送路径。
> 心跳能一行一行连续打出来，就说明 `TI` 中断和 `s_tx_idle` 握手都是活的；
> 如果只打了第一行就永久安静、连灯也停了，那才是真的卡在 `bsp_uart_putc` 里。

#### 排查表（从第 1 步开始）

| 步骤 | 做什么 | 结果说明 |
|---|---|---|
| 1 | 下载时盯住 CH340 的 TX/RX 灯 | 下载**成功**就必然有双向通信。如果下载时灯也不闪，说明这块小板压根没焊指示灯，**后面别再用灯判断任何事** |
| 2 | 看状态灯 P2.0（`bsp_gpio.h` 的 `LED_PORT_SEL/LED_PIN`） | 上电后闪 1 下 → 程序已跑过串口初始化；之后每 500 ms 翻转一次 → 主循环活着。**灯完全不亮 = 程序没跑到 / 复位循环**，此时串口问题都是次要的 |
| 3 | 烧 `build/step1b_BL_poll_v0.1.hex`，看有没有 `[BOOT-POLL]` | 有 → 硬件链路 OK，问题在中断/日志（看 §7.2 后面几行）；没有 → 进第 4 步 |
| 4 | STC-ISP 点「检测MCU选项」，看**IRC 频率** | 本机已实测：设定 `11.059MHz` / 调节后 `11.071MHz` → **正常，误差 0.11%，不用管**，见 §7.35。若读回的是别的档位（如 24MHz/35MHz），才是真的跑偏 |
| 5 | 把串口助手波特率挨个试 9600/57600 | 全都一个字节没有 → 回到第 2 步确认程序在跑 |

**已排除的软件坑**（这几个都改过了，别再来一遍）：

| 问题 | 症状 | 现状 |
|---|---|---|
| `s_tx_idle` 没加 `volatile` | 优化等级 7 把轮询读出循环 → **只发 1 个字符就永久安静** | 已加 `volatile`，且两个等待都带自旋上限，不会再锁死 |
| 开机第一次闪灯用 `bsp_timer_delay_ms()` | 它依赖 Timer0 中断；中断没跑起来就死等，**死在第一句日志之前** | 改成 `bl_busy_blink()` 纯空转，不依赖任何外设 |
| 主动 `bsp_wdt_init(WDT_PS_32)` | 选项字节里「上电自动启动看门狗」已勾选，芯片上电就在跑狗；再把 PS 改小是在给自己加复位 | `BL_INIT_WDT=0`：不配置、不使能，但**照常喂狗**（`bsp_wdt_feed()` 改为保留 EN/PS 重写，不把溢出标志写回 1） |

> ★ 最后一条容易搞反：**「关看门狗」和「关喂狗」是两件事**。
> 选项字节让狗上电就跑，停掉喂狗只会更早复位。P0 只关"初始化"。

### 7.35 IRC 显示「设定 11.059MHz / 调节后 11.071MHz」是正常的

STC-ISP 的「检测MCU选项」里如果读到：

```
用户设定频率: 11.059MHz
  调节后的频率: 11.071MHz
```

**这不是故障，不用改任何东西。** 两个值的含义不同：

- **设定频率** = 你在「输入用户程序运行时的IRC频率」里填的值（本工程填 `11.0592`）
- **调节后频率** = 芯片内部 IRC 校准后**实际跑**的频率。STC 的 IRC 是按档位微调的，
  不可能精确到小数点后四位，落在 `11.071MHz` 属于正常偏差

**误差核算（本项目代码用 `BSP_FOSC_HZ = 11059200` 算重装值）：**

```
RL2 = 65536 - 11059200/4/115200 = 65536 - 24 = 65512 = 0xFFE8
芯片实跑 11.071MHz 时实际波特率 = 11071000/4/24 = 115323 bps
        相对 115200 误差  = +0.107%
        10 位帧累计漂移    = +1.01%      （UART 容差 ±2%~3%，安全）
Timer0 1ms 实际周期       = 0.9989 ms   （误差 -0.108%，心跳 500ms 无感）
```

**结论：`+0.107%` 完全在容差内，串口助手不用换波特率，`BSP_FOSC_HZ` 也不用改。**

> **什么时候才需要改 `BSP_FOSC_HZ`？**
> 只有当"设定值"与"调节后值"相差 **超过 1%** 时才需要（这时累计漂移会吃到容差的一半以上）。
> 改法是把 `BSP/bsp_clock.h` 的宏改成调节后的值，两个工程一起 Rebuild。
> 现在只差 0.11%，改了也只能把误差从 1.01% 降到 0.1%，属于优化而非修复，**不值得动**。

### 7.4 能下载、程序在跑，但一个字节都发不出来

这是本地实测遇到过的组合，也是 STC 上最容易被漏掉的一环：

| 现象 | 说明 |
|---|---|
| STC-ISP 下载成功，烧写时 CH340 收发灯闪 | **CH340 ↔ P3.0/P3.1 电气上是通的**，下载方向没问题 |
| 状态灯 P2.0 会闪（500 ms） | **程序跑到了主循环**，说明所有 `LOG_I` 都执行完了、`bsp_uart_putc` 也没卡住 |
| 串口助手一个字节都没有 | 字节确实被写进 `SBUF` 并移出去了（否则就卡在 `putc` 里，主循环根本到不了），**传输链路成立，问题在"引脚到 CH340"这一段** |

**根因：准双向口的驱动能力不够。**

- STC 的准双向口（`PxM1:PxM0 = 00`）输出高电平靠的是**内部弱上拉**，只有几百 µA 级。
- 板上 P3.1 只要挂了点负载（指示灯、下拉电阻、保护电阻、排阻），高电平就被拽下来，
  接收端看到的是一条恒低的线。
- **为什么 ISP 下载却正常？** 片内 ISP 监控程序**自己会把 P3.1 配成推挽**。
  所以"能下载"只证明线通，**从来不证明你代码里的引脚模式是对的**。
- 官方推荐就是：UART 的 **TXD 用推挽**，RXD 用准双向（输入态）。

修法（已写进 `bsp_uart_init()`，`BSP_UART1_ON_P3` 控制）：

```c
P3M1 &= ~0x02;  P3M0 |= 0x02;    /* P3.1 / TXD1 -> 推挽 */
P3M1 &= ~0x01;  P3M0 &= ~0x01;   /* P3.0 / RXD1 -> 准双向 */
P3   |= 0x02;                    /* TXD 空闲态保持高    */
```

> 换了 `P_SW1` 改 UART1 脚位的话，这几行必须同步换引脚，否则 TXD 还是弱上拉。

**上电闪灯自诊断（`build/step1c_BL_txprobe_v0.1.hex`）**

不用电脑，灯自己把结果报出来：

```
慢闪 1 下              ← 上电标记
停 0.5 s
快闪 n 下              ← ① TI 置位次数（发 8 个字节，n 应为 8）
停 1.5 s
慢闪 1 下 / 3 下       ← ② P3.1 输出高后读回：1=拉得高，3=被拽低
```

| ① 成功字节数 | ② 引脚读回 | 结论 |
|---|---|---|
| **8** | **1 下（高）** | MCU 侧完全正常 → 问题在 CH340/上位机（换 STC-ISP 自带「串口助手」页验证） |
| **8** | **3 下（低）** | 板子上有东西拽着 P3.1：下拉电阻/指示灯/跳线帽/接错脚。查原理图或跳线 |
| **0** | 任意 | `TI` 从没置位 → 波特率发生器或 `SCON` 没生效，回去查 `AUXR`/`T2H`/`T2L` |
| 只有慢闪 1 下、没有计数 | — | 进到 `uart_init` 就失败了（`ring_init` 参数错） |

**顺便排除上位机**：直接用 STC-ISP 自带的「串口助手」页看（同一个软件、同一个 COM 口），
可以一次性排除"串口助手开错 COM / STC-ISP 还占着串口"这类干扰。
记得**先关掉外部串口助手**，一个 COM 只能被一个程序打开。

### 7.5 用工具解析串口日志（不用人眼找）

日志是滚动的，关键判据散在几十行里，人工容易漏；更麻烦的是
`[IAP] eeprom=128KB` 这种**回声行**长得很像证据，其实是代码把形参原样打一遍
（见 §7.2 的警告）。所以提供了一个解析器：

```bash
# 方式一：串口助手把收到的内容另存为 txt，然后
python tools/parse_boot_log.py 日志.txt

# 方式二：直接贴一段
python tools/parse_boot_log.py --text "[BL] selftest OK ... probe@0x14000=W ..."
```

它会做四件事：

| 动作 | 说明 |
|---|---|
| 抽取判据 | `probe@0x14000=W/NA`、`selftest OK/FAIL rc=n`、`summary: init=x selftest=y`、`p3=xx`、`txd low`、`BL CHANGED` |
| 统计活性 | `BOOT-POLL` / `[BL] boot` / `alive #n` 各出现几次 → 判断轮询路径和中断路径是否都活着 |
| 分类列出 | 分成 **[通过] / [注意] / [问题]** 三组，一眼看清 |
| 给结论 | 直接输出"P0 通过，可进第二步" / "探针不可写，先查 EEPROM 128K" / "证据不完整，再抓 20 秒" |

输出示例（有条件通过的实际情况）：

```
  抓到的行数:  BOOT-POLL x40   [BL] boot x1   alive x18
  P3 读回:     3
  BL 头字节:   2 0 2 0
  APP 头字节:  ff ff ff ff
  探针:        STAGING=W   APP=untested

[通过]
  OK  心跳 alive 出现 18 次 -> EA=1 中断发送路径正常
  OK  BL 头字节 2 0 2 0 -> IAP 读通路正常（能读到真实机器码）
  OK  flash_iap_init(128) = 0
  OK  STAGING 区（IAP 0x00000）= W -> **真的写进去了**，IAP 写通路成立
  OK  BL intact（探针没破坏自身代码）
  OK  summary 重播: init=0 selftest=0 stg=W app=untested

[注意]
  !   APP 区 = untested -> **可写性未验证**（该区非空，探针主动跳过）

结论: IAP 写通路成立（STAGING 实测可写）；APP 区可写性未验证
      这是**有条件通过**，不是完全通过。
```

工具**只认 `stg=` / `app=` 这两个实测字段**，不认 `selftest OK`。
如果日志里没有这两个字段（说明烧的是 `step1e` 或更早的固件），它会直接提示
"烧的是旧固件，那时的探针逻辑有假阳性 bug"。

> 抓日志时**至少跑 20 秒**：心跳 2 秒一行、摘要 8 秒一行，
> 抓太短会得到"证据不完整"而不是"有问题"——这两件事必须分清。

### 7.6 「有条件通过」怎么处理 + 一个差点把方案选错的假阳性

#### 先说假阳性（2026-10-25 实际踩到）

第一版探针逻辑是这样的：

```c
if (flash_iap_read_byte(OTA_APP_IAP_BASE) == 0xFF)
    writable = flash_iap_probe(OTA_APP_IAP_BASE, 0);   /* 空片才探 */
else
    writable = 1;        /* ← 非空就"跳过"，并把结果硬设成 1 */
```

于是当 APP 区 `0x14000` **非空白**时，实际日志是：

```
[IAP] 3addr-reg ok, app@0x14000 not blank, skip probe (firmware kept)
[BL] selftest OK
[BL] -> probe OK, BL intact: OTA arch (a) is valid      ← 假的
```

**`selftest OK` 和 `OTA arch (a) is valid` 都是假结论** —— 那一刻一次 Flash 写都没做过。
跳过探针的理由（"不毁固件"）本身是对的，错的是**跳过之后假装成功**。
如果照这个结论进第二步，将来 `apply-move` 写不进 APP 区才暴露，排查成本高得多。

固定做法（已进 `step1f` 和 `flash_iap.h`）：

| 原则 | 落地 |
|---|---|
| 结论只能来自**实测** | `flash_iap_probe_result(0/1)` 返回 `W` / `NA` / `untested` 三态 |
| 跳过必须显式标记 | 三态里 s 有 `UNTESTED`，绝不能用一个 `1` 代替 |
| 一定要有一处真探针 | **STAGING 区（`0x00000`）永远探** —— 它是 OTA 下载数据的真实落点，探了零风险 |
| 危险区只在安全时探 | APP 区（`0x14000`）**只有空白时才探**；非空则跳过并报 `untested` |

#### 「有条件通过」是什么

当日志显示 `stg@0x0=W` 且 `app@0x14000=untested` 时：

| 已证实 | 未证实 |
|---|---|
| ✅ IAP 能擦、能写、能回读（STAGING 区实测） | ❓ APP 区 `0x14000` 能不能写 |
| ✅ 三地址寄存器有效（能寻址到 `0x00000` 与 FF 块） | |
| ✅ 探针没动到 BL | |

**这不是失败，但也不能宣称 P0 完全通过。** 两条路：

| 方案 | 做法 | 评价 |
|---|---|---|
| **A（推荐）** | 直接进第二步：烧合并镜像把 APP 区填满，然后跑一次真实 OTA —— 能不能写立刻见分晓 | 省一次往返。而且 APP 区本来就必须有固件，绕不过去 |
| B | 用 STC-ISP「擦除」整片后重烧 `step1f`，让 APP 区变空白再拿 `W` | 干净，但多一轮烧写；且会在没有任何固件的情况下验证 |

> **为什么 APP 区会非空**：`0x14000` 不是空片，常见于
> ① 芯片出厂/之前烧过别的程序；② 上一次下载没有整片擦除、留下了旧内容。
> `app head: xx xx xx xx` 这行会打出头 4 字节 —— 若是 `02 xx xx xx`（LJMP）之类的
> 合理机器码，说明确实有固件残留；若是 `00 00 00 00` 或乱跳，
> 要反过来怀疑 IAP 读通路（对照 `bl head` 一起看）。


---

