## 9. 十个必踩的坑（按遇到概率排序）

| # | 坑 | 现象 | 处理 |
|---|---|---|---|
| 0 | **`IAP_CMD` 的 1(读)/2(写) 写反** | 编译零错误、跑起来正常，但 `bl head` 读成 0，`BL CHANGED`，**BL 复位入口被抹成 00，下次复位起不来** | 见 §8.8。v0.3 已修 + 加三道防线。**永远不要凭记忆改这两个数** |
| 0b | **重新引入可变参数日志 / LOG 参数个数写错** | 程序跑得好好的，**日志里的数字却是鬼值**：`%x` 字段变空白、`0xff4000` 打成 `0x12`、`48K` 打成 ` K`；同一份源码 BL 对、APP 全错 | 见 §8.9/§8.11。v0.7 已把 varargs 整个删掉。规矩：只用 `LOG_xN` 且 **N = 数值参数个数**、一行 ≤ 4 个值。换编译选项/工具链后先看 `chkA`~`chkE` 和行尾 `*HH` |
| 0c | **L251 的 overlay 复用（`_XDATA_GROUP_`）让参数块踩局部变量** | 功能全对、编译零警告，但日志里一个数**只错一个 bit**（`0x4000`→`0x5000`），CRC 自检恒失败，`STAGING NOT writable`，A1 复制 `rc=2`。**加一个无关的 .c 文件就可能触发** | 见 §8.14。根治 = L251 Misc controls 加 **`NOOVERLAY`**（两个工程都要），代价 xdata +46 B、code 0。守门 = `tools/check_overlay.py`（`build_local.sh` 已接入，FAIL 即 exit 7）。**删掉 `NOOVERLAY` = 回到随机崩坏** |
| 0d | **常量表加了 `code` 限定** | 编译零警告，`crc32 selftest` **恒失败**、`STAGING NOT writable`、`[VER] fail rc=12`；日志里偶发单字节乱码（`FA^L`、`[BLm`）而**行校验和自洽** | 见 §8.15。`code` = 16 位绝对地址，本工程基址 `0xFF0000+` 超出该空间，取址被截成低 16 位（`FF006F`→`006F`=STAGING）。**常量一律裸 `const`**（进 HCONST）。守门判据 0c 会拦 |
| 0e | **"擦不干净 / 写不进去"被当成代码 bug 反复改** | `param` 每次上电都说 `defaults created`，`[VER]` 永远 `rc=12 step=0`；`psv` 显示 `e=0 i=0 w=7`、`d=466 f=0`（全字节不同），而**读通路完全正常** | 见 §8.18。`w=7` 是 `OTA_ERR_CRC` ⇒ 写后首块就比对不上。**先读 `IAP_CONTR.bit4 CMD_FAIL` 分开"命令被拒"和"物理没生效"**，再查供电（4G 模组同轨拉塌 VCC）与选项字节。别再改代码——命令码/门禁/地址/TPS/CRC 都已逐项证伪 |
| 0f | **只抽查 4 个点就认定"擦干净了"** | `flash_iap_erase_sector` 报 OK，`flash_iap_is_erased` 却找到非 FF；"擦不干净"因此藏了三轮 | `flash_iap.c:258` 只查 `+0/+128/+256/+384`。**判断擦除是否干净必须全量查**（`flash_iap_is_erased`），抽样只能当快速失败用 |
| 1 | **HEX 格式没设 HEX-386** | 编译通过，烧进去不跑；map 里地址对但 hex 是 `:10...` | Output 页改 HEX Format；用第 5.4 步检查 `04` 记录 |
| 2 | **APP 用了 `interrupt` 关键字** | BL 烧完还能跑，烧了 APP 就死机/复位循环 | 删掉关键字，改 `ota_isr_bind()`。查 `Objects/*.map` 里有无 `?C_INTERRUPT` 段落在 0xFF4000 |
| 3 | **下载串口与 AT 串口抢 UART1** | 下载时好时坏；模组的 AT 数据把握手打乱 | STC 的 ISP 下载固定走 P3.0/P3.1(UART1)。**建议把 Y100E 改接 UART2/UART3**，或在 UART1 上串 100Ω + 跳线帽，下载时断开模组 |
| 4 | **EEPROM 大小不是 128K** | 探针写进去读不回来 → `probe@0x14000=NA`；★ 日志里 `eeprom=128KB` **照打**，不会有任何报错 | STC-ISP 改回 128K **并勾「本次下载需要修改硬件选项」重新下载一次**（该设置随程序烧进选项字节，不是即时生效）。见 §6.05 |
| 5 | **两个工程编译选项不一致** | 跳转后跑飞、中断乱跳 | Code Rom Size / Memory Model / 优化等级三项对齐；用同一份 `stc32g.h` |
| 6 | **Debug 页选了 Monitor-51 驱动** | 下拉看着选了 STC 驱动，但 DLL 仍是 `S251.DLL`/`DCORE51.DLL(p251)`；点调试连不上或单步就跑飞 | STC32G 是 251 核，必须用 `STC Monitor-251 Driver`；本阶段建议直接 `Use Simulator` 不调试，见 3.5 |
| 7 | **打开了 LARGE 改造前的旧 hex** | STC-ISP 一打开就报「HEX地址异常,超出可寻址范围 !」，但文件明明存在 | hex 里带着 `0x00Exxx` 的 near 常量段。**Rebuild 重生成**，或直接用 `build/step1_BL_only_v0.1.hex`。用 `--info` 看 `[ISP兼容]` 一行确认，见 §6.0 |
| 8 | **P3.0/P3.1 被配成推挽输出 / 外挂了别的器件** | 上电点下载永远停在"正在检测目标单片机..."；程序本身跑得好好的 | 下载口和 UART1 是同一对脚，上电握手的几十毫秒里被拽偏就进不去 ISP。**P3.0/P3.1 只给 UART1**，别当普通 IO；模组挂上来必须留跳线帽。见 §7.05 |

> 第 4 条特别容易漏：**EEPROM 大小是硬件选项字节，改完必须重新"下载/编程"一次**才写入芯片，
> 只改下拉框不下载是没用的。

---

## 10. 分阶段验证（每步都有通过判据）

### P0 · 探针固件（最重要的 30 分钟）

目的：定死 `OTA_IAP_CAN_REACH_CODE`，顺带确认串口、时钟、看门狗都活着。

代码**已经写好**（`boot_main.c` 的 `bl_probe()` + `BSP/flash_iap.c` 的
`flash_iap_selftest()`），不需要你再改 `main()`。要做的事只有三件：

1. `boot_main.c` 第 37 行保持 `#define BL_PROBE_ONLY 1`；
2. STC-ISP 按 §6 配好（**EEPROM = 128K** 是硬前提），烧 `build/step1_BL_only_v0.1.hex`；
3. 串口 115200 看日志，判据见 **§7.2 的表格**。

`bl_probe()` 实际做的事（对照日志看）：

```
bsp_clock_init / bsp_gpio_init / bsp_uart_init(115200) / bsp_timer_init_1ms / bsp_wdt_init
  -> flash_iap_init(128)      设 IAP_TPS = 11（= fosc 的 MHz 数，不设会"偶尔写不进去"）
  -> flash_iap_selftest()     crc32 自检 + 容量比对 + APP 区首扇区探针
  -> 死循环：喂狗 + 500ms 闪灯
```

探针不会碰 `0x10000`（那是 BL 自己），只探 APP 区首扇区 `0x14000`；
而且**只有该扇区首字节是 `0xFF` 时才探**，已有固件则跳过（见 §7.0 的说明）。

### P1 · Bootloader 独立可跑 + 能跳到 APP

前提：P0 通过（`probe@0x14000=W`）。

1. 把 `boot_main.c:37` 改成 `#define BL_PROBE_ONLY 0`；
   这会把 `OTA_BL_USE_JUMP` / `OTA_BL_USE_PARAM` 一起打开。
   `ota_jump.c` 和 `param.c` **已经在 BL 工程的文件表里**，不用手动加。
2. 两个工程各 Rebuild 一次（选项见 §3 / §4.3），然后：
   ```bash
   python tools/merge_hex.py \
       --in  Project/Bootloader/Objects/Bootloader.hex \
       --in  Project/APP/Objects/STC32G_OTA.hex \
       --out build/step8_BL_APP_v0.8.hex --check
   ```
3. 烧 `build/step8_BL_APP_v0.8.hex`，串口应看到：

```
[BL] boot, bl_code=0xff0000 app_code=0xff4000     <- v0.5 起 app_code 必须是 ff4000
[BL] seq=... boot_cnt=... boot_try=0 state=0     <- param_init 自建默认值
[BL] jump entry hi=ff lo=4000                     <- 入口 = 0xFF4000（v0.6 起不用 %lx）
[JUMP] entry via-base=75                          <- 两条地址算法读到的首字节
[JUMP] entry via-const=75                         <- ★ 两者一致才说明 IAP 读稳定
[APP] ... APP 的 P0 自检日志 ...                   <- APP 真的跑起来了
```

| 现象 | 含义 |
|---|---|
| 停在 `[BL] APP head=ff -> refuse to jump` | APP 区首字节是 `0xFF` → 合并时 APP 没被写进去，查 `--check` 输出 |
| `[JUMP] bad entry` + `hi/lo` 不是 ff/4000 | `OTA_APP_CODE_BASE` 不是 `0xFF4000`，看 `ota_partition.h` 是否被改过 |
| `jump returned rc=12` | **v0.5 及更早有这个 bug**（§8.10.2）：`ota_jump` 重读首字节失败后拒绝跳转。v0.6 已把闸门移回 `bl_boot`，重烧即可 |
| `via-base` 与 `via-const` 不一致 | IAP 读在深栈上下文里不稳定 —— `ota_verify.c` 的 CRC 校验会受影响，必须查清 |
| 打印了 `[JUMP] -> 0xFF4000` 但后面没了 | 跳到空地址或 APP 的时钟/串口没初始化 → 先单独验 APP（见 P2） |
| BL 日志都没有 | P1 的 BL 比 P0 多了 `param.c`，若 data 爆了会链接失败——看 `Program Size` |

> ⚠ P1 的 `bl_boot()` **还没有镜像校验**（等 `ota_verify.c`）。它只检查
> "APP 区首字节不是 `0xFF`" 就直接跳。所以这一步只能用来验证跳转链路，
> **不能当成正式启动流程**。
>
> 另外 `param_init()` 会在第一次运行时写参数区（`0x0C000`）。若你反复烧录调试，
> 注意参数区不会因为重新烧录而被擦掉（STC-ISP 只写 hex 覆盖到的地址）。

### P2 · APP 独立可跑

- 先**不经过 BL**：把 APP 的 `CLASSES` 里 `CODE(...)` 临时改成 `0xFF0000`、`ECODE` 改成
  `0xFF0200`，**单独烧 APP**（会覆盖 BL，测完必须重烧合并镜像），确认 APP 自身能跑。
- 再改回 `0xFF4000 / 0xFF4200`，由 BL 跳转进入，确认中断正常
  （关键是串口收字节不丢——这是验证向量跳板是否生效的最快方法）。

### P3 · 整链路 OTA

- 判据：云端下发 → `DOWNLOADING`(进度上报) → `VERIFYING` → `PENDING_REBOOT` → 重启 → `COMMITTED`。
- 必做的破坏性测试：**下载到 50% 拔电**，上电后续传；**校验阶段拔电**，上电后应回到 `IDLE` 且旧固件可跑；
  **新固件故意改坏 CRC**，应回滚到 `ROLLED_BACK`。

---

## 11. 常用快捷操作备忘

| 操作 | 位置 |
|---|---|
| 改优化等级 | Options for Target → C251 |
| 看代码/变量占用 | Build Output 的 `Program Size` 行；或打开 `.map` |
| 强制重编全部 | `Project → Rebuild all target files` |
| 查看某函数地址 | `.map` 里搜函数名 |
| 清空 STC 选项字节 | STC-ISP 硬件选项里全部按默认，重新下载一次 |
| 恢复出厂（清 EEPROM） | STC-ISP 勾「清除EEPROM缓冲区」后下载一次空程序 |


---

## 附录 B · C251 的语言限制（写代码前必须知道）

这三条不是"风格问题"，是**会直接编译失败**的硬限制，踩一次浪费半天：

### B.1 保留字当变量名 → 满屏语法错误

C251 把存储器空间和修饰符都做成了关键字。**不能用作标识符**：

```
data  code  xdata  idata  edata  pdata  bdata  bit
sfr   sbit  interrupt  using  reentrant  alien
small  compact  large  near  far  at  inline  task
```

典型症状：`const u8 *data` 会让编译器认为你在写"指向 data 空间的指针"，
于是参数表解析崩溃，报 `syntax error near ','` + 同一函数 `too many actual
parameters` + 后面 `redefinition of ...`。

本工程已把全部 `data` 参数改名为 `src`。以后新增函数请避开上表。

### B.2 `#if` 里不能用 `enum` 常量

```c
typedef enum { LOG_LVL_INFO = 3 } log_level_t;
#if (LOG_LEVEL >= LOG_LVL_INFO)     /* ✗ 预处理器不认识，当成 0 */
```

预处理器只认宏。症状是 `warning C322: 'xxx': unknown identifier`，
而且比较结果全错（要么全开要么全关，静默失效）。

本工程日志级别已改为 `#define LOG_LVL_INFO 3`。

### B.3 `#if` 里的宏不能含类型强转

```c
#define OFFSET(a)  ((u32)(a) + 0xFE0000)   /* 含 (u32) */
#if (OFFSET(0x14000) != 0xFF4000)          /* ✗ invalid integer const expression */
```

预处理器没有类型系统。宏里出现 `(u32)`、`(u8 *)` 都会让 `#if` 报
`error C308`。被 `#if` 求值的宏必须是**纯常量表达式**。

### B.4 存储空间必须显式声明（本项目已踩过）

本工程的 Memory Model 把**未限定存储空间的对象放进 `data`**，而 8051 的
`data` 段只有 **128 字节**。所以：

```c
static u8 s_rx_mem[1024];          /* ✗ error C172: ?DT? 段太大 (act=1035, max=128) */
static u8 xdata s_rx_mem[1024];    /* ✓ 放 xdata，12KB SRAM 里 */
```

规则（本项目已全部按此整理）：

| 对象 | 放哪 | 说明 |
|---|---|---|
| 缓冲区 > 32 B、结构体参数表 | `xdata` | 12KB SRAM 全在 xdata |
| 局部临时数组（如 64 B 读块） | `xdata` | 放 data 会把段撑爆 |
| 计数器、标志位（< 10 B） | 默认空间 | 无所谓 |
| 常量表 | `code` | 不占 RAM |

代价是调用 `u8 *`（far 通用指针）参数时会有 `C150: 'xdata' pointer converted
to 'edata' pointer` 警告——**这是合法的空间转换，可以忽略**，不要为了消警告去
把参数写成 `u8 xdata *`（会换来 C194 obsolete 警告）。

---

## 附录 A · 首次编译错误对照表

把 Build Output 的第一条错误对上号，比从头猜快得多。**C251 的报错有强烈的级联性：
一条 `u8` 未定义会让后面 50 行全部报错，所以永远只看第一条。**

| 报错 | 根因 | 处理 |
|---|---|---|
| `error C318: can't open file 'ota_types.h'` / `'ringbuf.h'` / `'bsp_clock.h'` | 没加 Include Paths | 见 §3.0，加 4 个目录 |
| `warning C34: 'u32': missing declaration specifiers` + `error C42: 'xxx': not in formal parameter list` 成片出现 | `stc32g.h` 里没有 `u8/u16/u32` | 确认 `#include "ota_mcu.h"`（本工程已统一），且 COMMON 在 Include Paths 里 |
| `error C231: 'u8': redefinition` | 你的 `stc32g.h` 自带这些 typedef | C251 → Define 加 `OTA_HAS_STD_TYPES` |
| `error C29: formal/parameter has illegal storage class` 出现在 `static const xxx code tbl[]` | 上一行的类型未定义导致级联 | 先修类型，通常自动消失 |
| `warning C194: obsolete declaration: use '<type> xdata buf'` | 指针上写了 `xdata` 限定（旧语法） | 本工程已全部去掉，改成通用指针；Huge 模式通用指针 3 字节可跨区 |
| `error C308: invalid integer const expression` / `C322: 'u32': unknown identifier`（在 `#if` 行） | `#if` 里的宏含 `(u32)` 强转，预处理器求不了 | 宏必须是纯常量表达式；本工程已改为 `(a) + 0xFE0000` 无强转 |
| `error C67: 'u8': undefined identifier` | 该 .c 没包含任何带类型定义的头 | 每个 .c 第一行包含自己的 .h |
| `warning C140: 'flash_iap_init' undefined; assuming 'extern int ...'` | 头文件没找到（C318 的连带后果） | 先修 C318 |
| `error C53: redefinition of 'rc'` / `C142: illegal initialization` | 级联噪声 | 先修上面的类型/头文件 |
| `error C25: syntax error near '}'` 出现在结构体末尾 | `u8` 未定义 | 同第 2 条 |
| `*** WARNING L1: UNRESOLVED EXTERNAL SYMBOL` | 少加了 .c | 见 §4.1 的文件状态表；未实现的文件不要 Add |
| `*** ERROR L104: MULTIPLE PUBLIC DEFINITIONS` | 同一个 .c 被两个 Group 各加了一次 | 删掉重复项 |
| `*** ERROR L250: CODE SIZE LIMIT IN RESTRICTED VERSION` + `RESTRICTED VERSION WITH 0800H BYTE CODE SIZE LIMIT` | Keil C251 是评估版，限 2 KB 代码 | 装正式授权。见 §6.1 ① |
| `*** ERROR L107: ADDRESS SPACE OVERFLOW, SPACE: DATA` | `data` 段只有 128 B，装不下 | Memory Model 改 Large。见 §6.1 ② |
| APP 的 `.lnp` 里 CODE 落在 `0xFF0000`（而且带 224 B 启动代码） | `CODE(...)` 写在 CLASSES 外面，被静默忽略 | 写进 CLASSES 里，见 §3.3 / §3.4.2 |
| `*** WARNING L16: UNCALLED SEGMENT, IGNORED FOR OVERLAY PROCESS`（几十条） | P0 阶段只用了少数函数，其余没被调用 | 正常，可忽略。函数被调用后自动消失 |
| `L251: CODE SPACE MEMORY OVERLAP` | 两个工程 CODE 段重叠 | BL `CODE(0xFE0000)`、APP `CODE(0xFE2000)` |
| `IAP_ADDRE: undefined identifier` | 换了一份没有第三地址寄存器的 `stc32g.h` | 把 `BSP/flash_iap.c` 顶部 `#define OTA_IAP_ADDR_REG IAP_ADDRE` 注释掉。闸门 0 会拒绝 >0xFFFF 的地址（明确报错，不会写错地方） |
| `WDT_CONTR: undefined identifier` | 头文件未声明 | 取消 `BSP/bsp_wdt.c` 顶部 `sfr WDT_CONTR = 0xC1;` 的注释 |
| `IAP_TPS: undefined identifier` | 该版本 `stc32g.h` 没有 IAP 时序寄存器 | 本工程的 `C251\INC\STC\STC32G.H` 有（0xF5），换头文件后需同步适配 |
| `error C25: syntax error near ','` + 同函数 `C95: too many actual parameters` | 参数名用了 C251 保留字（`data`） | 改名为 `src`。见附录 B.1 |
| `warning C322: 'LOG_LVL_INFO': unknown identifier` | `#if` 里用了 enum 常量 | 日志级别改成 `#define`。见附录 B.2 |
| `error C172: '?DT?xxx': segment too big (act=1035, max=128)` | 未限定存储空间的对象默认进了 `data`（只有 128B） | 给该对象加 `xdata`。见附录 B.4 |
| `warning C150: 'xdata' pointer converted to 'edata' pointer` | `xdata` 缓冲区传给 `u8 *`（far 通用指针）参数 | **安全转换，可忽略**。若要消掉，得把参数写成 `u8 xdata *`，会换来 C194 警告，不值 |
| `warning C150: 'code' pointer converted to 'edata' pointer` | `code` 常量表传给通用指针 | 同上，忽略 |
| `warning C47: 'i': unreferenced local variable` | 声明了没用 | 删掉该变量 |
| `warning C174: 'xxx': unreferenced 'static' function` | 条件编译下某函数未被引用 | 用同一个宏把它整体包住（`#if ... #endif`） |
