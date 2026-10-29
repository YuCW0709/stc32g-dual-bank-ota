## 8. 第二步：合并 BL + APP，验证跳转

### 8.1 合并哪两个文件

STC-ISP 一次只能烧一个文件。第二步要同时把 **BL 放 `0xFF0000`**、**APP 放 `0xFF4000`**，
所以合并的是**两个工程的链接产物**：

| # | 来源 | Keil 路径 | 本机构建路径 | 落点 |
|---|---|---|---|---|
| 1 | Bootloader 工程 | `Project/Bootloader/Objects/Bootloader.hex` | `tools/_out_bl/Bootloader.hex` | `0xFF0000` 起 |
| 2 | APP 工程 | `Project/APP/Objects/STC32G_OTA.hex` | `tools/_out_app/APP.hex` | `0xFF4000` 起 |

**不是**"合并 hex 和 bin"、也不是"合并两个 APP"——只有 BL 和 APP 这两个。

### 8.2 前置：先把开关切到 P1

直接拿 P0 的 BL 去合并是没用的，`boot_main.c` 里那三个开关必须是这样：

| 开关 | 值 | 作用 |
|---|---|---|
| `BL_PROBE_ONLY` | **0** | 从"只跑探针"切到"读参数 → 校验 → 跳 APP"。**为 1 时烧了也不跳** |
| `BL_DIAG_WINDOW` | **0** | 关掉开机 9 秒轮询宽限期，正式启动不再白等 |
| `BL_BOOT_IAP_CHECK` | **1** | 每次上电实测一次 IAP 可写性（量产再改 0） |

改完两个工程都要 **Rebuild**（不是 Build，改的是 .h 和预处理开关）。

### 8.3 合并命令

```bash
# 合并
python tools/merge_hex.py \
    --in Project/Bootloader/Objects/Bootloader.hex \
    --in Project/APP/Objects/STC32G_OTA.hex \
    --out build/step8_BL_APP_v0.8.hex --check

# 体检合并结果（必看，确认起始地址是 0xFF0000）
python tools/merge_hex.py --in build/step8_BL_APP_v0.8.hex --info
```

`--check` 会做四件事：
1. 打印每段地址范围；
2. **按分区裁剪**：APP 文件里那 3 字节 `?CO?start251` 会被丢掉（归 BL，见 §5.5）；
3. 检查两段是否**重叠**（重叠直接报错，防止 BL 被 APP 覆盖）；
4. 检查是否越界或落进 STAGING/PARAM 区。

本机实测输出（2026-10-25，P1 第一版）：

```
[1] tools/_out_bl/Bootloader.hex     0xFF0000-0xFF2B66   11002 B
[2] tools/_out_app/APP.hex           0xFF0000-0xFF66E1    9581 B
        [裁剪] 判定为 APP 镜像，丢弃不属于它的字节：BOOTLOADER 3 B
       BOOTLOADER 0xFF0000-0xFF0002       3 B
       BOOTLOADER 0xFF000B-0xFF000E       4 B
       BOOTLOADER 0xFF0023-0xFF00AE     140 B
       BOOTLOADER 0xFF0100-0xFF2B66   10855 B
       APP        0xFF4000-0xFF4087     136 B
       APP        0xFF4200-0xFF66E1    9442 B
[check] 无重叠，同块内最大空洞 0x1499 字节，合计 20580 B
```

- BL 用到 `0xFF2B66`，16 K 槽（到 `0xFF3FFF`）还剩 **5.3 K**
- APP 用到 `0xFF66E1`，48 K 槽还剩很多

### 8.4 ★ 合并前必须过的一道守门：APP 入口必须是启动代码

这一条**决定跳转成败**，而且坏起来没有任何编译错误。

```
check_layout.py 检查：
  OK   APP ?C_C51STARTUP 正好在 0xFF4000（BL 跳转入口成立）
  OK   APP.hex 入口首字节 = 75 84 01 7E（可执行）
```

**为什么必须查**：L251 在 CODE 类内部排序时，`?CO?`（常量段）**排在 `?C_C51STARTUP` 前面**。
只要 APP 侧多一个 `const xxx code` 表，启动代码就被挤到 `0xFF4000` 之后，
BL 跳过去执行的其实是**常量数据** → 直接跑飞 → 看门狗反复复位 → "灯乱闪 + 串口全乱码"。

2026-10-25 实测踩到过：`ota_partition.c` 的 `s_parts[]` 带了 `code` 修饰，
`?C_C51STARTUP` 被挤到 `0xFF4058`。去掉 `code`（常量进 HCONST）后回到 `0xFF4000`。

```bash
python tools/check_layout.py     # 布局守门，build_local.sh 已自动调用
```

顺带纠正一个**错误的第一直觉**：APP 入口处放的是 C251 的启动代码（首字节 `75`），
**不是** `LJMP`（`0x02`）。`0x02` 是 BL 自己 `?CO?start251` 的特征。
硬件复位是 `FF:0000` 的 LJMP 跳 startup，而 BL 跳 APP 是**直达 startup**：

```
BL  ?C_C51STARTUP @0xFF0027: 75 84 01 7E 44 04 1F E4 7A 49 B0 1B ...
APP ?C_C51STARTUP @0xFF4000: 75 84 01 7E 44 04 1F E4 7A 49 B0 1B ...   ← 完全一致
```

### 8.5 烧录前：必须取消「本次下载需要修改硬件选项」

| 勾选框 | 这一步要的值 | 原因 |
|---|---|---|
| 本次下载需要修改硬件选项 | ☐ **取消** | 再次写选项字节通常伴随整片擦除，会把 STAGING（下载断点）和 PARAM（运行状态）一起清掉 |
| 上电复位自动启动看门狗 | —— | 不勾就不写，保持原样 |
| 清除EEPROM缓冲区 | ☑ 保持 | 保证工具不去碰 `FE:` 段 |

选项字节上一次已经是 128 K 了，这一步**不需要**再改，所以要把它**取消**。

然后：打开 `build/step8_BL_APP_v0.8.hex` → 冷启动下载 → 串口 115200。

### 8.6 通过判据

烧完之后串口先静默几毫秒（BL 开机日志），然后**每 2 秒一行 APP 心跳**：

> **v0.8 起每行末尾还有 `*HH`（行校验和），下面为了好读都省略了。**
> 真实输出形如 `[43][BL] chkA %u: 0 1 9 10*6b`。`HH` 随内容变，不要照抄。
> 它的用途见 **§8.12** —— 一句话：和值对说明这行没在传输中被改动，
> 和值错说明被改动过、这行作废（是线的问题，不是固件的问题）。

```
[BL] boot, bl_code=0xff0000 app_code=0xff4000
[BL] ==== BL v0.1 (P0) ====
[BL] fosc=11059200Hz
[BL] chkA %u: 0 1 9 10                             ← ★ 先确认日志没骗人
[BL] chkB %u: 15 16 255 256
[BL] chkC %u: 65535 4294967295
[BL] chkD %x: 0 ff 10000 ff4000                    ← 注意是 ff（半字节粒度抑制前导零）
[BL] chkE mix: 65537|mid|9|14000
[BL] flash_iap_init(128) -> 0
[IAP] eeprom=128KB
[IAP] bl head: 2 0 27 ff (want 2 ...)              ← ★ 必须是 2
[IAP] app head: 75 84                               ← 拆两行，每行 2 个参数
[IAP] app head: 1 7e (want 75 84 1 7e)              ← ★ APP 里是启动代码
[IAP] 3addr-reg ok, stg=W, app=untested, BL intact
[BL] selftest OK
[BL] == summary: init=0 selftest=0 stg=W app=untested ==
[BL] seq=... boot_cnt=... boot_try=0 state=0
[BL] APP head a=75 84                               ← 关键：入口可执行
[BL] APP head b=1 7e
[BL] jump entry hi=ff lo=4000                       ← ★ 必须 ff / 4000
[JUMP] entry via-base=75                            ← 两条地址算法读到的首字节
[JUMP] entry via-const=75                           ← ★ 两者必须一致
[APP] ==== APP v0.1.0 ====  (P0 build: 1)          ← ★ 跳转成功
[APP] fosc=11059200Hz  EA=0
[APP] chkA %u: 0 1 9 10                             ← APP 侧也各一遍自检
[APP] chkB %u: 15 16 255 256
[APP] chkC %u: 65535 4294967295
[APP] chkD %x: 0 ff 10000 ff4000
[APP] chkE mix: 65537|mid|9|14000
[APP] app_code=0xff4000  app_iap=0x14000
[APP] stage=48K
[APP] app=48K
[APP] bl_ro=1
[APP] flash_iap_init(128) rc=0
[APP] iap size=128K
[APP] read app[0..3] = 75 84 1 7e
[APP] enter main loop
[APP] running_ver =                               ← 版本号拆两行（v0.7 起）
[APP]   0.0.0.0                                      who + 4 个字段 = 5 个值
[APP] alive #1 boot_cnt=0                          ← 每 2 秒一行，永续
[APP] alive #2 boot_cnt=0
[APP]   state=0 ver=APP v0.1.0                     ← 每 4 秒补一行
```

> `chkA`~`chkE` 五行是**日志格式化自检**（§8.9）。它们跟上面写的完全一致，
> 才说明日志可信、后面的数字能当真。任何一行不一致，先修日志再看别的。
>
> v0.7 起自检拆成 5 行、每行最多 4 个值（接口就是 4 个槽），
> 且 `%u` 与 `%lu` 等价、`%x` 抑制前导零（所以 0 打成 `0`、0xFF 打成 `ff`）。

| 串口看到的 | 结论 | 下一步 |
|---|---|---|
| 出现 `[APP] alive #n` | ✅ **跳转成功，第二步完成** | 进 P1 功能开发（模组 / MQTT / HTTP） |
| `chkA`~`chkE` 与上面任何一处不符 | 日志格式化还在错位，**所有数字都不可信** | 见 §8.9，别拿这份日志做判断 |
| 只有 `[BL] alive #n p3=3b` | BL 没跳成，停留原因看前面几行 | 见下表逐项 |
| `jump returned rc=12` | `OTA_ERR_VERIFY`：`ota_jump` 那道"空片"闸门拦的 | 见 §8.10 —— **v0.6 已把闸门移回 `bl_boot`** |
| `via-base` 与 `via-const` 不一致 | 同一地址两条算法读出不同值，IAP 读在深栈上下文里不稳定 | 见 §8.10；这是 `ota_verify.c` 阶段必须先解决的问题 |
| `IAP READ path untrusted` + `bl head` 不是 2 | **读通路坏了**，先别管跳转 | 见 §8.8；`bl head` 全 0 就是旧 IAP_CMD bug 抹了入口，重烧本版即可 |
| `APP head=ff -> refuse to jump` | APP 区是空的 —— 烧的是 BL-only 镜像 | 换 `build/step8_BL_APP_v0.8.hex` |
| `APP head=0 -> refuse to jump` | 入口被写成 00（旧 IAP_CMD bug）或半截镜像 | 重烧 `build/step8_BL_APP_v0.8.hex`；仍不行就整片擦一次 |
| 一片没有、灯也不闪 | 复位循环或没跑起来 | 看 §7.3 |

### 8.7 跳转失败时逐项排查

| 现象 | 原因 | 修法 |
|---|---|---|
| `APP head=0xff` | 烧的是 BL-only 文件 | 烧合并镜像 |
| `jump returned rc=3` | 首字节 0xFF/0x00，被校验拒绝 | 同上 |
| 跳到 APP 后立刻复位循环（灯乱闪） | APP 入口不是启动代码 | 跑 `check_layout.py`，看 `?C_C51STARTUP` 是否在 `0xFF4000` |
| 跳到 APP 后完全静默、灯不闪 | APP 里 `bsp_timer_delay_ms()` 死等（EA=0） | 用 `app_delay_ms()` 空转，别用依赖中断的延时 |
| APP 起来了但日志乱码 | 两工程波特率/时钟宏不一致 | 对齐 `APP_LOG_BAUD` 与 `BSP_FOSC_HZ` |

> `--no-trim` 可以关掉裁剪，看原始镜像（会报"值不同"，因为两边的复位跳转
> 指向各自的启动代码，这是预期的）。

---

### 8.8 ★★★ IAP_CMD 1/2 写反事故（v0.3 修复，必读）

这是本工程踩到过**最危险**的一个坑：编译 0 error、链接 0 error、烧录成功、
串口输出一切正常，**但每读一次 Flash 就在往 Flash 里写一个字节**。

#### 现场日志（真实）

```
[17][BL] flash_iap_init(128) -> 0
[22][IAP] eeprom=128KB
[27][IAP] bl head: 0 0 0 0          ← BL 复位入口读出来是 0，本该是 02 00 27
[188][IAP] BL CHANGED after probe! stop.
[194][BL] selftest FAIL rc=6
[425][BL] APP head=... -> refuse to jump
```

#### 根因

`BSP/flash_iap.h` 里把命令码写成了：

```c
#define IAP_CMD_PROGRAM  0x01     /* ✗ 错 */
#define IAP_CMD_READ     0x02     /* ✗ 错 */
```

STC32G 的正确取值（手册例程 / 国芯官方论坛 / 逐飞 STC32G12K128 库，三处一致）：

| IAP_CMD | 含义 |
|---|---|
| 0 | 空闲 standby |
| **1** | **读出 read** |
| **2** | **编程/写 program** |
| 3 | 扇区擦除 erase |

于是：

* 每一次"读 Flash"（`cmd=0x02`）实际是**写**，写进去的是 `IAP_DATA`（=0x00）；
* 每一次"写 Flash"（`cmd=0x01`）实际是**读**，什么都没写进去。

**一次上电就干了这些事**：

| 动作 | 实际效果 |
|---|---|
| `bl head` 读 4 字节 | 把 `0xFF0000~0xFF0003` 写成 `00 00 00 00` → **BL 的 LJMP 被抹掉** |
| 探针备份 512 B | 把 STAGING 前 512 B 写成 0x00 |
| `APP head` 读 4 字节 | 把 `0xFF4000~0xFF4003` 写成 0x00 → **APP 启动代码被抹掉** |
| `param_init` 读参数区 | 把参数区写坏 → 报 `param empty/corrupt` |

下一次复位，CPU 从 `FF:0000` 取到 `00 00 00 00`，**Bootloader 起不来**。
（**但 ISP 冷启动下载仍然有效**——引导程序在独立 ROM 里，与用户 Flash 无关，
所以永远救得回来，不用拆片。）

#### 为什么极其容易误判

日志打的是 `probe@...=NA` / `selftest FAIL`，看起来完全是"第三地址寄存器没生效"，
按以前的判据会被引导到 **方案 (b)（降级布局）**——而真因跟布局毫无关系。
`BL CHANGED` 那句反而是**真话**：BL 确实被改了，只是改它的人是"读"。

#### 三道防线（v0.3 已加，缺一不可）

| 防线 | 位置 | 作用 |
|---|---|---|
| **A. 命令码白名单** | `flash_iap_raw()` 开头 | `cmd > 3` 直接返回 `OTA_ERR_PARAM`，连 `IAP_TRIG` 都不给 |
| **B. 写分区硬闸门** | `flash_iap_raw()` | 凡 `PROGRAM`/`ERASE`，先判 `ota_part_of()` + `ota_part_writable()`，落在 BL/常量区一律拒绝。**哪怕命令码再次写反，也写不进代码区** |
| **C. 读通路验证闸门** | `flash_iap_selftest()` | 先只读 `0x10000`，必须读到 `0x02`（链接器生成的 3 字节 LJMP）；读不到就打印原因并 `return`，**一个字节都不写** |

> C 的哲学：**写不可逆、读永远可逆**，所以顺序只能是"先读证，后写测"。
> 探针"备份 → 擦 → 写 → 还原"看起来安全，但备份本身就建立在"读可信"之上；
> 读不可信时，探针会拿垃圾把真数据盖掉，还返回"成功"。

#### v0.3 期望日志

```
[IAP] eeprom=128KB
[IAP] bl head: 2 0 27 ff (want 2 ...)        ← 读通路可信
[IAP] app head: 75 84 1 7e (want 75 84 01 7e) ← APP 里确实是启动代码
[IAP] 3addr-reg ok, stg=W, app=untested, BL intact
[BL] selftest OK
```

`bl head` 第一个数**必须是 2**。不是 2 就别往下看了，先修读通路。

### 8.9 ★★★ 日志格式化会骗人：varargs 参数区被 overlay 池覆盖（v0.7 才真正根治）

> **先看结论，再看下面的过程**
>
> v0.4 / v0.5 / v0.6 三次都以为修好了，实际都没有 —— 每次都只是把
> "哪几个参数被踩"换了个位置。**v0.7 的做法是把这个机制整个删掉**：
> `log_printf` 不再存在，日志接口改成"固定 4 个 32 位槽、非可变参数"，
> 并且十进制输出不再调用除法库。详见 §8.11，那是现在有效的规则；
> 下面这一节留作**病史记录**，说明为什么当初会被骗、以及为什么小修小补没用。

第二步第一次跑通时，功能是**完全成功**的（`[APP] alive #1/#2/#3` 连续出现），
但同一份日志里出现了自相矛盾的输出：

```
[IAP] app head: 75 84 1 7e (want 75 84 01 7e)   ← 正确
[BL]  APP head=    1  , jump                     ← 同一份字节，全乱
[JUMP] -> 0xff4000                               ← 正确
[BL]  app head ok, jump -> 0x12                  ← 同一个地址，鬼值
[APP] stage= K                                   ← 48 凭空消失
[APP] read app[0..3] =     1                     ← 75 84 1 7e 只剩一个 1
```

**同一份数据、同一份格式化代码，在两个文件里打出两种结果。** 这说明不是数据错，
是 `log_printf` 的**参数传递**在特定写法下错位了。

#### 规律（三条对照）

| 写法 | v0.4 现象 | v0.5 之后的解释 |
|---|---|---|
| `LOG_I("IAP","bl head: %x %x %x %x", b0,b1,b2,b3)` —— 参数是**已读好的局部变量** | ✅ 正常 | 4 个参数的**高字节全为 0**，被踩成 0 也看不出来 |
| `LOG_I("BL","APP head=%x %x %x %x", flash_iap_read_byte(a), ...)` —— 参数里**嵌套 4 次函数调用** | ❌ 前三个字段全空 | 函数调用的临时值在 overlay 里来回搬，正好把参数区冲干净 |
| `LOG_I("APP","stage=%luK%s", kb, ok?"":" (part invalid)")` —— **三目字面量指针 + 混排** | ❌ `%lu` 整个消失 | 指针是高字节非零的 4 字节值，被踩掉高位后整个字段解析错位 |

注意：**差别不在"写法"本身，而在这一行的参数落在参数区的哪个偏移上**。
所以同一行代码改动一个字符（多一个参数、少一个参数）就可能从正常变错乱，
这也是它极难复现的原因。v0.5 把参数区周围清空之后，三种写法都正常。

#### 根因（v0.5 用 `.map` 实证，不再靠猜）

v0.4 的判断（"局部 xdata 数组进 overlay"）只对了一半，所以 v0.4 烧上去之后
这几行**仍然错**（`app_code=0x4000`、`jump -> 0x12`、`APP head=    1  ,`）。
v0.5 直接查 `tools/_out_bl/Bootloader.map`，答案写在段表里：

```
_XDATA_GROUP_        010D56H - 010D9FH   74 B   XDATA UNIT   ← 链接器的 overlay 复用池
?log_printf??BYTE    010D86H                                 ← varargs 参数区就在池里
vformat/log          0D8FH 0D93H                             ← 它自己的局部变量
put_str/log          0D94H 0D95H
```

`?log_printf??BYTE` 是编译器为可变参数函数生成的**参数传递区**。链接器把它
当成一个普通的 overlay 单元，**允许它和 `vformat` / `put_str` / `put_u32` 的
局部变量共用同一段地址**。于是格式化进行到一半时，`vformat` 每轮执行的
`is_long = 0`、`put_str` 的 `n++`，写的就是参数区内部。

**为什么只错一部分行**：被踩掉的多数是参数的**高字节**，而小数值的高字节本来
就是 0，写 0 等于没写 —— 所以 `chkA`~`chkE` 全对（它们全是小常量和 0xFF4000
这类高字节恰好被容忍的值）。一旦参数是变量、地址、或高字节非零，就退化成：

| 现象 | 真相 |
|---|---|
| `app_code=0x4000` | 0xFF4000 的**高 8 位被写成 0** |
| `jump -> 0x12` | 参数被踩成指针的低位残片 |
| `APP head=    1  ,` | 4 个参数里 3 个整段被清零，`%x` 遇 0 只出一个 `0`，其余位置留空 |

#### v0.5 的修法：把参数区周围清空，不是修某个函数

思路换了：**不去猜哪个函数踩了它，而是让 `_XDATA_GROUP_` 池里除了参数区之外
没有任何别人的局部变量**。这样踩踏在物理上不可能发生。

| # | 位置 | 改法 | 改完 map 里的段 |
|---|---|---|---|
| 1 | `log.c` `put_str/put_u32/put_s32/log_now_ms/log_hexdump` | 局部变量全部 → `static` | `-----  -----` |
| 2 | `log.c` `vformat()` | 改成**无参数、无局部变量**，上下文走文件级 static。形参自己也会占 4 B 溢出槽（实测 0x0DC0-0x0DC3，正好压在参数区尾巴上） | `-----  -----` |
| 3 | `log.c` `log_printf()` | 末尾自己循环 `bsp_uart_putc`，不再调 `bsp_uart_write`（后者 2 B 形参槽也在参数区尾巴上） | — |
| 4 | `bsp_uart.c` `bsp_uart_putc/write/tx_wait_idle` | `guard` / `i` → `static` | `-----  -----` |
| 5 | `log.c` `put_ch()` | 边界 `s_pos < LOG_LINE_MAX-1` → `< LOG_LINE_MAX`。原来永远浪费最后一格，导致超长行补的 `'\n'` 写不进去、**两行粘连** | — |
| 6 | `log.c` `log_printf()` | 新增**参数区快照校验**：`va_start` 后把前 16 B 存进 `s_apchk[]`，格式化完再比一次，不一致就在行尾打 **`!!ARGX!!`** | — |

改完之后（`grep ^put_ch/log ... Bootloader.map`）：

```
log_printf?/log     0D93H 0DC3H     ← 只剩它自己的段
put_ch/log          ----- -----
put_u32/log         ----- -----
put_str/log         ----- -----
vformat/log         ----- -----
put_s32/log         ----- -----
bsp_uart_putc       ----- -----
```

参数区 `?log_printf??BYTE = 010DBBH` 从此只和自己函数的局部变量共处，
链接器再想"复用"也没有对象了。

**代价**：BL `xdata 3478`（原 3.4K，仍未超 4K），代码 +537 B。本模块本来就
声明"不可重入"，改 `static` 不引入新约束。

#### 实时自证：看到 `!!ARGX!!` 就什么都别信

```
[BL] app head ok, jump -> 0x12 !!ARGX!!
                               ^^^^^^^^^ 本行所有数字作废
```

没有这个标记 ≠ 一定对（被踩成原值也可能），但**有它就一定错**。
这比事后翻 `.map` 快得多，也是给 P1/P2 阶段留的长期哨兵。

#### ★ 规矩（写进团队约定，不要破例）

> **1. LOG 的参数列表里只放变量和常量，绝不放函数调用。**
> 需要就先算到局部变量里。
>
> **2. 一行 LOG 的参数不超过 2 个。** 多出来的信息宁可拆成第二行。
> （`APP head` 那行已按此拆成 `APP head a=` / `APP head b=` 两行。）
>
> **3. 三目运算符不要直接出现在 LOG 参数里**，尤其两边是不同长度的字符串字面量。
>
> **4. 新增任何模块，凡是要打日志的，其局部变量一律 `static`。**
> 判断标准：`grep '^ 函数名/' .map` 后面不该有地址。

#### 自检：`log_selftest()`

为了不再靠"感觉"判断日志可不可信，`log.c` 新增了自检函数。
BL 和 APP 都会在启动时各打一遍（5 行），**先确认这 5 行正确，再去相信后面任何结论**：

```
[BL] chkA %u: 0 1 9 10 15 16 255 256 65535
[BL] chkB %x: 0 1 f 10 ff 100 ffff 1000 4000
[BL] chkC %lu: 0 1 65535 65536 4294967295
[BL] chkD %lx: 0x0 0xff 0x10000 0xff4000
[BL] chkE mix: 7|65537|mid|9|0x14000
```

任何一行跟上面不一致，就说明日志还在骗人，**此时所有基于日志的判断都作废**，
先修格式化再看别的。

> 量产可以删掉这两个调用（各约 200 B code），调试期务必保留。

---

### 8.10 ★★ 两处"看起来像硬件故障"的软件缺陷（v0.6 修复）

v0.5 把日志格式化修好之后，`app_code=0xff4000` 回来了、`chkA`~`chkE` 全对，
**但 APP 反而起不来了**，而且日志里开始随机丢字符。两个独立问题，一起修掉。

#### 8.10.1 日志随机丢字符 —— UART 发送握手有竞态

现场样本（v0.5 实测）：

| 应为 | 实际 | 丢的字 |
|---|---|---|
| `STAGING` | `STAG NG` | `I` |
| `IAP write path` | `AP write path` | `I` |
| `NOT yet verified` | `NO  yet verified` | `T` |
| `writable` | `writabl` | `e` |
| `selftest` | `selftes` | `t` |
| `p3=3b` | `p3=3` / `p3=13` / `p3=bb` / `p3=33` | `b` |

**注意：这不是格式化问题** —— 被丢的字符根本没进 `SBUF`。
所以日志格式化自检（v0.7 起的 `chkA`~`chkE`）察觉不到：那几行本来就对，
一个字不少地打出来了。

根因在 `bsp_uart_putc()` 的 `EA=1` 分支：

```c
while (!s_tx_idle) { }      /* 等上一字节 */
s_tx_idle = 0;
SBUF = c;                   /* ← 写完就返回，不等本字节 */
```

两个缺口：

1. **没清陈旧 TI**。上一字节发完会把 `TI` 置位；如果 UART 中断还排在
   时基中断后面没跑，此刻 `TI=1 / s_tx_idle=0`。本函数把 `s_tx_idle` 清 0
   之后 ISR 才进来，它看到 `TI` 就把 `s_tx_idle` **又置回 1** ——
   可我们的字节**正在移位**，标志却说"空闲"。
2. **不等本字节发完就返回**，覆盖窗口留给下一次调用。

两者叠加：下一次 `putc` 立刻覆盖 `SBUF`，当前字节被拦腰截断，
接收端解出乱码或直接按帧错误丢弃。

**v0.6 修法**（`BSP/bsp_uart.c`）：

```c
ES = 0;                      /* 只关 3 条指令，不影响接收 */
if (TI) TI = 0;              /* ← 新增：清陈旧标志 */
s_tx_idle = 0;
ES = 1;
SBUF = c;
while (!s_tx_idle && --s_guard) { }   /* ← 新增：等本字节真的移出去 */
```

`EA=0` 分支同样补了 `if (TI) TI = 0;`（原来先写 `SBUF` 再等，若 `TI` 还站着
`while(!TI)` 立刻通过，本字节刚进移位寄存器就被判成发完）。

`bsp_uart_tx_wait_idle()` 的 `EA=0` 分支也改掉了原来的 `while(!TI)` ——
没有字节在发时 `TI` 永远是 0，那样会白等满整个自旋上限（约 13 ms），
而它正好在**跳转前的最后一步**被调用。

> 代价：`putc` 变成同步（一字节一字节等）。串口速率本来就是瓶颈，
> 总耗时**完全相同**，没有任何吞吐损失。

#### 8.10.2 APP 起不来：`ota_jump` 用第二次读否决了第一次读

v0.5 实测日志：

```
[493][BL] APP head a=75 84          <- bl_boot: flash_iap_read_byte(OTA_APP_IAP_BASE)
[498][BL] APP head b=1 7e           <-            常量地址，读到真机器码
[502][BL] app head ok, jump -> 0x
[517][BL] jump returned rc=12, stay in BL
```

`rc=12` = `OTA_ERR_VERIFY`（见 `COMMON/ota_types.h`），只可能来自
`ota_jump_to_app()` 里那道"空片"闸门：

```c
first = flash_iap_read_byte(app_base - OTA_PHYS_BASE);   /* 0xFF4000-0xFE0000 = 0x14000 */
if (first == 0xFF || first == 0x00) return OTA_ERR_VERIFY;
```

**同一颗芯片、同一个地址、早几十微秒前刚读出 `0x75`，这里却读成 0xFF。**

问题的本质不是"哪一次读错了"，而是**同一件事做了两遍，而且第二遍能推翻第一遍**：

| 谁 | 怎么算地址 | 结果 |
|---|---|---|
| `bl_boot()` | `OTA_APP_IAP_BASE` 常量 | `75` ✅ |
| `ota_jump_to_app()` | `app_base - OTA_PHYS_BASE` 运行时算 | `FF` ❌ → 拒绝跳转 |

两次读之间**没有任何代码改过 Flash**。哪怕其中一路的读更可信，
把闸门放在会失败的那一路上，就等于让"读不稳"直接变成"永远不跳"。

**v0.6 处置**：

1. **闸门统一放回 `bl_boot()`** —— 那里本来就有 `0xFF/0x00` 判定，
   而且实测可靠（`APP head a=75 84`）。`ota_jump_to_app()` 只负责
   "地址合法就跳"。
2. `ota_jump_to_app()` 仍**输出两条诊断**，一行一个参数：

```
[JUMP] entry via-base=75          <- 用 app_base - OTA_PHYS_BASE 算地址
[JUMP] entry via-const=75         <- 用常量 OTA_APP_IAP_BASE
```

**两者不一致 = IAP 读在"深调用栈 + 日志刚跑过"这个上下文里不稳定。**
这条线索非常重要：`ota_verify.c` 阶段的 CRC 校验就是靠连续读 Flash 做的，
读不稳则 CRC 结论也不可信。届时必须先把这件事查清。
3. 入口地址改用两个 `%x` 打印（`jump entry hi=ff lo=4000`），不再用 `%lx` ——
   v0.5 实测 `%lx` 在这一行上打不出数字，而 `%x` 在同一路径上从未出错。
4. `ota_jump_to_app()` 的局部变量全部改 `static`。它含函数指针间接调用，
   链接器对它的 overlay 分析本来就放弃，不值得把 6 字节留在栈上赌。

#### 8.10.3 顺带清掉的违规写法

`alive` 心跳行原来是：

```c
LOG_I("BL", "alive #%u p3=%x%s", (u16)(tick>>2), (u16)p3v,
      (p3v & 0x02) ? "" : " (txd low!)");       /* 三目 + 字面量指针 */
```

违反自己定的铁律第 3 条，且这行是心跳，越简单越可信。改成：

```c
LOG_I("BL", "alive #%u p3=%x", (u16)(tick>>2), (u16)p3v);
```

`p3` 稳定为 `3b` 就说明 P3.1 是高（TXD 空闲电平正确）；异常时这行本身就是证据。

---

### 8.11 ★★★ v0.7：把 varargs 整个拆掉（现在有效的日志规则）

#### 真根因（这次有实测证据，不是推测）

1. **参数区是编译器生成的全局符号，不是局部变量。**
   C251 给可变参数函数生成 `?log_printf??BYTE` 作参数块，调用方把参数写在
   **它下面**（`BYTE-2`、`BYTE-4`…，向下增长），`va_start` 拿到的正是 `BYTE`。

2. **链接器把它当成普通 overlay 单元。**
   它和别的函数的局部变量一起参与 `_XDATA_GROUP_` 的地址复用，位置完全由
   段大小和链接顺序决定 —— 同一份源码、同样的选项，两个工程结果不同：

   | | 参数块地址 | `_XDATA_GROUP_` 池 | 结果 |
   |---|---|---|---|
   | BL | `0x010DBF` | `0x010D56–0x010D9F` | 落在池外 → 一直正常 |
   | APP | `0x010CFA` | `0x010CCB–0x010D18` | **落在池内** → 被踩 |

3. **APP 里更糟：参数块落在 `log_printf` 自己的溢出帧里面。**
   映射表里 `log_printf?/log` 占 `0x010CD2–0x010D02`，而参数块在 `0x010CFA`。
   函数一进门写自己的溢出槽，就把刚收到的参数写坏了。
   实测症状精确对应：`chkA` 前 3 个字段对、第 4 个开始全错。

4. **机器码比对证明"不是编出来的代码不一样"。**
   从两个 hex 里把 `put_u32` 的机器码逐字节取出对比：1536 字节里只有 83 处不同，
   而且全是"+0x4100 的代码地址高字节"（正是 BL 与 APP 的代码偏移）。
   代码相同、数据不同 ⇒ 一定是运行时内存被踩。

5. **我加的 `!!ARGX!!` 自检当时抓不到，因为方向搞反了。**
   参数是写在 `BYTE` **以下**的，我却去比对 `BYTE` 以上的 16 字节 —— 恒定不相等。
   所以"没看到 `!!ARGX!!`"这件事，当时并不能证明参数没被踩。

#### v0.7 的改法（三条，缺一不可）

| # | 改法 | 解决什么 |
|---|---|---|
| 1 | **删掉 varargs**：`log_x(lvl, tag, fmt, a, b, c, d)`，4 个 32 位槽，形参立刻搬进 `static` 数组 | 全局参数块不存在了，链接器怎么排段都影响不到它 |
| 2 | **十进制输出不调用除法库**：逐字节长除 + 自写 16 位除 10（只用移位/比较/减法）；十六进制直接移位取半字节 | 不再有 `?C?ULDIV?` 调用、不再有"除数形参要跨两次库调用存活" |
| 3 | 宏名带参数个数（`LOG_I0`~`LOG_I4`），一条最多 4 个值 | 参数少写在源码里就能看出来；超过 4 个编译期直接报错 |

验收（`tools/_out_*/**.map`）：
- `log_printf` 出现 **0 次**（符号已不存在）；
- `put_ch` / `put_dec` / `udiv10` / `put_str` / `vformat` / `next_arg` /
  `put_hex32` / `log_selftest` 的 overlay 段全是 `----- -----`（0 字节）；
- 只有 `log_x` 自己占 20 字节溢出槽，而它在函数第一件事就把值搬进 `s_av[]`。

#### ★ 现在写代码必须守的规则

> **1. `LOG_xN` 的 N 必须等于数值参数个数。**
> `LOG_I2(tag, "a=%u b=%u", x, y)` ✅ ／ `LOG_I2(tag, "a=%u", x)` ❌
>
> **2. 一条日志最多 4 个值。** 多了就拆行（`app_print_ver()` 就是这么拆的）。
>
> **3. 参数优先放变量或常量。** 函数调用先取到变量再传 ——
> 不是怕 varargs 了，而是被调函数的局部 xdata 仍可能与调用方的活跃变量
> 共用 overlay 地址，取一次变量不花钱。
>
> **4. `%u` 与 `%lu` 等价、`%x` 与 `%lx` 等价**（都读一个 32 位槽），
> 不用再纠结宽度；`%x` 抑制前导零且至少 1 位，所以 0 打成 `0`。
>
> **5. 指针用 `(u32)ptr` 传，格式串用 `%s`。**
>
> **6. 行尾出现 `!!ARGN!!` 就是调用点写错了**：格式里要的值比给的多。
> 这是编译期看不出来、运行期第一次跑就能看见的自证标记。

#### 代价

BL 的 code 从 10901 涨到 12381（+1480 字节），16K 槽**只剩约 2.1 K**。
P1 往 BL 里加东西（`ota_verify.c` 的 CRC、`ota_fsm.c`）时要注意预算；
超出时优先把用不到的分支用 `#if` 裁掉，而不是回头去动日志接口。

---

### 8.12 ★★ v0.8：行校验和 `*HH` —— 让日志能自证有没有被线上打坏

#### 8.12.1 为什么还要加（v0.7 已经"全对"了）

v0.7 上板，**所有数字都对了**：两侧 `chkA`~`chkE` 全对、`app_code=0xff4000`、
`stage=48K`、`read app[0..3] = 75 84 1 7e`、`via-base`/`via-const` 都是 `75`、
跳转成功。但整份日志（约 1700 个字符）里有**两个字符变成了空格**：

```
[7 9][JUMP] entry via-const=75      <- 时间戳 [739] 的 '3' 变成空格
[689] APP head a=75  4              <- 0x84 的 '8' 变成空格
```

（同一份日志的 `[IAP] app head: 75 84` 是**对的**，所以不是同一个值被读错。）

#### 8.12.2 关键推理：这两处不可能来自格式化

`log.c` 里所有数字通路的输出字符是**封闭集合**：

| 函数 | 可能输出的字符 |
|---|---|
| `put_dec` | `'0'`~`'9'`（`'0' + s_rem`，`s_rem` 恒 ≤ 19） |
| `put_hex32` | `'0'`~`'9'` / `'a'`~`'f'`（`s_b0` 恒 ≤ 15） |
| `put_hex2` | `'0'`~`'9'` / `'a'`~`'f'`（查表，索引恒 ≤ 15） |

**0x20（空格）不在这三个集合里。** 能吐空格的只有两处：
格式串里的字面空格、`put_str()` 打的字符串。

于是只剩两种可能，而它们的修法**完全相反**：

| 可能 | 含义 | 该修哪 |
|---|---|---|
| (a) 缓冲区里就已经是空格 | 格式化算错了 | 固件（`log.c`） |
| (b) 缓冲区是对的，线上被打坏 | 传输/硬件问题 | 波特率、接线、CH340 |

**光看日志永远分不清**。2026-10-25 之前已经在这个岔路口上连错四次
（见 §8.9 的病史），所以这次不猜，直接让日志自证。

#### 8.12.3 做法：行尾 8 位累加和

`log_x()` 在补 `\r\n` 之前，对 `s_line[0 .. s_pos-1]` 做 8 位累加（mod 256），
把结果按两个小写十六进制字符附在行尾：

```
[43][BL] chkA %u: 0 1 9 10*6b
                            ^^^ = 前面所有字节之和 & 0xFF
```

- 覆盖范围**包含时间戳**（时间戳被打坏同样要能发现）；
- `log_hexdump()` 的每行同样带 `*HH`；
- 选**累加和**而不是 CRC8，是因为失败模式是"单字节被替换"，
  累加和对它 100% 敏感，而代价只有 15 字节代码、每行 3 个字符；
- 语义是**单向**的，别理解反了：

```text
和值不对  ->  这行**一定**被改动过，作废
和值对    ->  这行没被改动过（不代表内容逻辑正确，那要靠 chkA~chkE）
```

#### 8.12.4 怎么用

**自动（推荐）**：`tools/parse_boot_log.py` 已经内置校验，
坏行会被点名并从后续分析里剔除：

```
[问题]
  X   ★★ 行校验和 1/53 不符 -> 这几行在传输中被改动，已剔除
  X     第51行 记录和=fe 实算和=eb : [7 9][JUMP] entry via-const=75*fe
  X     -> 属于**传输/硬件**问题，不是固件
```

**手动**：看到可疑字符时，先看行尾 `*HH`。和值对 ⇒ 那个空格是**真的发出去了**
（固件问题）；和值错 ⇒ 是线的问题，重抓即可。别凭印象判断。

#### 8.12.5 如果确认是传输问题，下一步查什么

按代价从低到高：

1. **降波特率**：115200 → 57600（`bsp_uart_init(57600)` + 串口助手同改）。
   采样窗口翻倍，对时钟误差和线路容差的容忍度都翻倍。日志总量很小，
   2 秒一行心跳，57600 完全够用。
2. **缩短/整理 TXD 线**，别和 4G 模组的电源线、天线走一起。
3. **换 USB 口 / 换 CH340 线**（板载 CH340 在 USB 供电波动时最容易出错）。
4. 用示波器看 `P3.1` 空闲电平与字节波形（有没有毛刺、低电平被拽）。

> 判断"改好了没有"必须看**多份日志的和值统计**，不能看单次。
> 按 v0.7 的实测，损坏率约 2/1700 ≈ 0.1%，单次跑 50 行里"一次都没坏"
> 有小概率是运气。跑 5~10 次上电、累计几百行，才有说服力。

#### 8.12.6 顺带澄清一个容易看错的地方

v0.8 的 `.map` 里 `?log_x??BYTE`（`log_x` 自己的 4 个 u32 参数块）
**确实落在 `_XDATA_GROUP_` overlay 池内部**（BL `0x010D91`、
APP `0x010CCC`）。这**不是** v0.7 那个 bug，两者性质不同：

| | v0.7 的 varargs 块 | v0.8 的 `?log_x??BYTE` |
|---|---|---|
| 是什么 | 编译器造的**合成参数块**，不是任何函数的帧 | `log_x` 这个**函数的参数帧** |
| 链接器知不知道它属于谁 | **不知道**，当普通 overlay 单元随手塞 | 知道，参与调用图分析 |
| 会不会被调用方踩 | 会（实测被踩） | 不会：凡是帧和它重叠的函数，都不调用 `log_x` |

守门方法（改代码后照做）：`.map` 的 overlay 明细里，凡出现
`xxx?/yyy 0D96H ...`（有地址）的函数，**它的 `+-->` 列表里不能出现 `log_x`**。
真要在 `flash_iap_write` / `flash_iap_read` / `crc32_*` / `ota_part_contains`
里打日志，先确认它们的帧和 `log_x` 的帧不重叠，否则又是一个 v0.7。

#### 8.12.7 代价与产物

BL code 12381 → **12466**（+85 字节），16K 槽只剩约 1.9 K。

`build/step8_BL_APP_v0.8.hex`：`0xFF0000–0xFF7393`，27169 B
- BL `0xFF0000-0xFF3871`（14341 B；code=12466 const=1875 xdata=3645）
- APP `0xFF4000-0xFF4087`（136 B）+ `0xFF4200-0xFF7393`（12692 B）
  （code=11325 const=1506 xdata=3476）

---

### 8.13 ★★ P1-A1：让 BL 先"只读"证明自己读得懂 STAGING

> 这一节是 **P1 的第一刀**。在动用"擦掉 APP 区"这个不可逆动作之前，
> 必须先拿到一个结论：**BL 能把 STAGING 里的镜像完整读出来，并且校验通过。**
> 拿不到这个结论就去接 MQTT，会得到一个"能收指令、能下载、但装不上"的系统。

#### 8.13.1 为什么不能直接开始配 MQTT

P0 收官后代码里其实**只有声明，没有实现**：

```bash
$ grep -rn "ota_apply_staging" --include=*.c .
（空）
```

`ota_apply_staging()` 只在 `OTA/ota_fsm.h:234` 有一行声明。当时的 `bl_boot()`
是纯 P0 形态：自检 → `param_init` → `bl_summary` → 入口校验 → `ota_jump_to_app()`，
**根本不看 STAGING 里有没有待安装固件**。

所以那时候就算 MQTT 把固件完整下载到 STAGING、把 param 写成 `PENDING_REBOOT`，
复位后 BL 还是照样跳旧 APP —— 固件永远烂在暂存区，你只会看到"升级了但版本号没变"，
然后开始怀疑网络、怀疑信号、怀疑模组。

#### 8.13.2 三个必须先补的前提

| # | 缺什么 | 为什么卡住 |
|---|---|---|
| 1 | **搬运逻辑零实现** | 见上。OTA 本体就是 STAGING→APP 那段，它不在，其余都是摆设 |
| 2 | **APP 区写入从未验证** | `bl_probe()` 在区非空时主动跳过，日志一直是 `app=untested`。`stg=W` 只证明 IAP 写通路能用，**没证明 0x14000 以上那块能擦能写** |
| 3 | **Y100E 换型未做** | 用户当初定的时点正是"验证 OTA 之前"，现在到了。P1-A 全部做完、进 P1-B 时第一件事就是它（见 §8.13.8） |

另外 BL 的 16 KB 槽已经很紧，这就是 §8.13.5 那次瘦身的原因。

#### 8.13.3 A1 做了什么：只读校验 + 一个"自己搬自己"的镜像

**BL 侧**（`OTA/ota_verify.c`，新文件）：实现 `ota_verify_staging()`，9 步顺序校验：

```
1 读头  2 魔数  3 头长度  4 头格式版本  5 头 CRC
6 正文长度  7 机型  8 入口地址  9 正文 CRC32
```

任一步失败立刻返回，`s_step` 记住停在第几步，日志只打一行：

```
[VER] STAGING fail rc=12 step=5
```

**一份字符串覆盖 9 种失败** —— BL 只剩不到 1 KB，字符串是按字节算钱的。
`step=0` 专指"STAGING 空着"（全 FF），属正常，不算错误。

**APP 侧**（`APP/app_main.c` 的 `app_a1_make_image()`）：给 BL 造一份镜像。
做法是把**当前 APP 区自己**复制进 STAGING：

```
擦 STAGING 48 KB
→ 逐块读 APP 区(0x14000) → 写 STAGING+0x40，边写边算 CRC32
→ 造镜像头写进 STAGING+0
→ param: ota_state = PENDING_REBOOT, rollback_flag = 0xA0|build
→ 停机，等人工断电
```

为什么用"自己"当素材，而不是造一份假数据：

- 内容真实 —— 就是正在跑的那份固件；
- 后果可控 —— **A2 打开搬运后，搬回去的还是同一份固件**，即使搬运有 bug
  也不会变砖（最坏停在 BL，靠 ISP 冷启动重烧救回，不用拆片）；
- 一次实验同时验证了"读 APP 区""写 STAGING""造头""CRC 一致"四件事。

#### 8.13.4 镜像头的物理布局（与上位机打包工具的唯一约定）

```
STAGING + 0x00   magic(4) hdr_len(2) hdr_ver(2) ver(4) img_len(4)
                 img_crc32(4) entry(4) build_time(4) target(8)
                 hdr_crc32(4) reserved(2)                = 42 B
STAGING + 0x40   固件正文起点（hdr_len 恒 = OTA_IMG_HDR_RESERVE = 64）
```

| 关键点 | 值 | 说明 |
|---|---|---|
| `hdr_crc32` 覆盖范围 | **前 36 字节** | magic..target[8]，**不含** `hdr_crc32` 自身与 `reserved` |
| `hdr_len` | **64** | 它是**正文偏移**，不是头长度 |
| 正文最大长度 | **49088**（0xBFC0） | = `OTA_STAGE_SIZE - OTA_IMG_HDR_RESERVE`，正文终点正好 0xC000 |
| 结构体尺寸 | **42** | 已用编译期断言钉死（负数数组），改字段会立刻编不过 |

★ **不要用 `hdr_len - 4` 去算 CRC 范围。** `hdr_len=64` 减 4 得 60，
比真实范围多算 24 字节的 0xFF 填充，结果是"CRC 永远对不上"而且毫无提示。
这是最容易埋进去、最难查出来的一种错。代码里用独立常量 `OTA_IMG_HDR_CRC_LEN = 36` 锁定。

⚠ **硬限制：APP 代码不能超过 49088 B。** APP 区虽然 48 KB，但镜像头占前 64 B，
所以能参与 OTA 的只有 48K−64。当前 APP 代码 13772 B，离上限还有 35 KB。
A3 会加编译期断言把这条卡住。

#### 8.13.5 顺手做的瘦身：BL 从"只剩 522 B"救回 944 B

第一次编完 A1，BL 直接顶到 `0xFF3DF6` —— 16 KB 槽只剩 **522 B**，
而 A2 的搬运逻辑保守要 400~500 B，等于没路了。查 map 段表发现三块纯浪费：

| 项 | 省了 | 怎么省的 |
|---|---|---|
| `log_hexdump()` | ~410 B code + 常量 | 全项目**零调用点**，但占 512 B。加 `LOG_ENABLE_HEXDUMP`（默认 0）编成空函数 |
| `bl_tx_probe()` / `bl_pin_probe()` | ~250 B code + 约 1 s 启动时间 | P0 阶段用来区分"MCU 发不出来"和"PC 收不到"，P1 已无新信息。加 `BL_P0_DIAG`（默认 0） |
| `ota_verify` 的结构体整体赋值 | ~200 B | `*p_hdr = s_hdr` 让 C251 生成逐字段搬运，换成 8 位字节循环；同时砍掉"版本号"那条日志 |

结果：

| | 瘦身前 | 瘦身后 |
|---|---|---|
| BL code | 13642 | **13198** |
| BL const | 2112 | **2133** |
| BL 末端 | `0xFF3DF6` | **`0xFF3C4F`** |
| 到 `0xFF4000` 剩余 | 522 B | **944 B** |

> 两个开关都保留着，将来怀疑 UART 链路或要做镜像比对时改回 1 即可。

#### 8.13.6 怎么验（这一轮的完整操作序列）

1. 烧 `build/step15_iap_probe_BL_APP.hex`（**取消勾选「本次下载需要修改硬件选项」**）。
2. **第一次上电**：APP 发现 `state==IDLE` 且没造过镜像 → 开始造，约 3~5 秒，
   然后打：

   ```
   [APP] A1: build image from APP region -> STAGING
   [APP] A1 image ready len=49088 crc=xxxxxxxx
   [APP] A1: POWER-CYCLE the board now
   [APP] A1: next boot BL will verify STAGING (read-only, no apply)
   ```

   之后灯继续闪、串口不再出字 —— **这是停机等断电，不是死机**。

3. **断电 3 秒再上电**。BL 应该打：

   ```
   [BL] seq=2 boot_cnt=1 boot_try=0 state=3
   [BL] pending install -> verify staging (read-only)
   [VER] STAGING ok len=49088 crc=xxxxxxxx
   [BL] staging verified: A1 read-only, apply NOT enabled
   ```
   然后照常跳 APP，APP 因为 `rollback_flag` 已是 `0xA1` 而跳过造镜像，直接进主循环。

4. **再断几次电**，每次都应该重复第 3 步（A1 刻意不改 state，就是为了能反复观察）。
   校验耗时约 2 秒，比平时开机慢，这是预期的。

**通过判据**（缺一不可）：

| 判据 | 期望 |
|---|---|
| `[VER] STAGING ok len=` | `49088` |
| `[VER] STAGING ok crc=` | 非 `0`，且**每次上电都相同** |
| `[VER] STAGING fail` | 一次都不出现 |
| `[BL] staging verified` | 每次上电都出现 |
| APP 心跳 / 版本 | 与 P0 一致，无变化 |

失败时的处理：`step=N` 直接指出是哪一步，对照 §8.13.3 的编号表即可。
BL 会自动重试，最多 3 次（`boot_try` 计到 3 就停），日志里会说
`A1 retry n/3 armed (APP will rebuild)`。要重新来一轮：
**把 `APP/app_config.h` 的 `APP_VER_BUILD` 加 1** 再烧 —— 标记值跟着 build 变，
APP 就会重造镜像（param 在 IAP 区，ISP 重烧代码不会清它，所以标记必须随版本走）。

#### 8.13.7 产物与空间

`build/step9_BL_APP_v0.9_A1.hex`：`0xFF0000–0xFF77CB`，29233 B（**已被 v0.10 取代，见 §8.14.7**）
- BL `0xFF0000-0xFF3C4C`（15181 B；code=13195 const=2133 xdata=3826）
- APP `0xFF4000-0xFF4087`（136 B）+ `0xFF4200-0xFF77CB`（13772 B）
  （code=12102 const=1809 xdata=3651）
- `0xFF4000` 首字节仍是 `75 84 01 7E`，`0xFF0000` 仍是 `02 00 27 FF`

**A2 的预算**：BL 剩 1154 B（v0.10）。搬运循环（按 512 B 扇区）+ 回读 CRC（复用 `ver_body`
的形态）+ param 更新 + 日志，估 450~550 B，够但没有第二次机会 ——
A2 之前如果需要加东西，先回到 §8.13.5 那三个开关里找。

#### 8.13.8 P1 的整体路线（A1 之后往哪走）

| 阶段 | 做什么 | 依赖 | 风险 |
|---|---|---|---|
| **A1**（当前） | BL 只读校验 STAGING | 无 | 零（不写 STAGING/APP） |
| **A2** | BL 把 STAGING 搬回 APP 区，先做"自己搬自己" | A1 必须连续多次通过 | 擦掉 APP 区不可逆；失败停 BL，ISP 冷启动重烧即可，不用拆片 |
| **A3** | APP 启动后写 `committed`，失败回滚 | A2 通过 | 需要动 param 结构（递增 `PARAM_FMT_VER`） |
| **P1-B** | **Y100E 换型** + HTTP Range 分片下载 | A3 通过 | 见下 |
| **P1-C** | MQTT 触发 + 状态上报 | P1-B | 最低 |

**为什么 A 系列全部做完才碰 4G**：验证 OTA 不需要 4G，也不需要 MQTT。
MQTT 只是**触发手段**，OTA 本体是"STAGING → APP"那段搬运。顺序反过来的后果是
得到一个"能收指令、能下载、但装不上"的系统，然后误判成网络问题。

**P1-B 换型（Y100E AT 固件）必须实测、禁止照抄旧模组（Air780EP）结论的 5 项**：

1. 默认 AT 波特率（是否 115200、开机是否自适应速率）；
2. 指令集是否同源：`AT+QHTTP*` vs `AT+HTTP*`、`AT+MQTT*` 的名字与参数顺序；
3. HTTP Range 分片：BREAK/READ 参数顺序、单次最大读取长度、是否认 `Range` 头；
4. PWRKEY 极性/脉宽与开机完成判据（`+CPIN: READY` / `+CFUN` / URC 形态）；
5. URC 行格式（前缀、是否 `<CR><LF>` 包裹）→ 决定 `uart_at` 的行解析规则。

架构上不用改引擎：`uart_at` 只依赖"行式命令 + OK/ERROR + URC 回调"，
换模组 = 换 `drv_cmds[]` 命令字符串表 + URC 前缀表，上层 `net_dev`/`ota_fsm` 无感。

⚠ **模组不要挂 UART1**：P3.0/P3.1 是 ISP 下载口兼日志口，三者共用会互相踩。
Y100E 走 **UART2 或 UART3**。

---

### 8.14 ★★★ v0.9 翻车与 v0.10 根治：`NOOVERLAY`

#### 8.14.1 现象（v0.9_A1 上板）

`chkA`~`chkE` 五行**全对**、`via-base/via-const` 都对，但有三处不对，
而且**行校验和 `*HH` 全部自洽**（说明固件真的打出了这些字）：

```
[203][IAP] crc32 selftest FAIL
[214][BL] selftest FAIL rc=6
[586][BL] -> STAGING NOT writable: IAP cannot reach FE block
[649][BL] jump entry hi=ff lo=5000        ← 应为 lo=4000
[APP] A1 copy rc=2 off=0 -> give up
```

`hi=ff lo=5000` 里那个 `0x1000` 是最值钱的一条线索：**只差一个 bit**。
而且 `boot` 首行同一份 `OTA_APP_CODE_BASE` 打出来是 `0xff4000` 完全正确。

#### 8.14.2 根因：参数块是"孤儿段"，谁都可能压它

v0.7 把"编译器生成的可变参数块"换成了固定 4 槽形参，以为根治了。
**其实只是把参数块从调用方挪到了 `log_x` 自己身上** —— 7 个形参装不进寄存器，
C251 照样生成 `?log_x??BYTE`，而 **L251 把这个段当孤儿**：它只知道"有个
这么长的段要放"，不知道它属于谁，于是可以和其他函数的局部变量复用同一地址。

v0.9 的 map 实测：

| 段 | 地址 | 说明 |
|---|---|---|
| `_XDATA_GROUP_` | `010E4A-010E95` | overlay 复用池 |
| `?log_x??BYTE` | `010E4D` | 参数块 16 B → `010E4D-010E5C` |
| `log_x?/log` | `0E52-0E65` | log_x **自己**的局部段 |
| `crc32_selftest?/crc32` | `0E52-0E5A` | `u8 xdata v[9]` 局部数组 |
| `?crc32_update??BYTE` | `010E55` | 参数块，正压在 `v[3..8]` 上 |

于是：

- `LOG_I2` 写的第二个槽 `lo`（`0x4000`）落在 `010E51-010E54`，
  `log_x` 的局部变量把 `010E52` 从 `0x40` 写成 `0x50` → **`0x4000` 变 `0x5000`**；
- `crc32_selftest` 调 `crc32_update(crc, v, 9)`，参数直接写穿 `v[3..8]`，
  CRC 永远不等于 `0xCBF43926` → 自检失败 → `flash_iap_selftest` 提前返回
  → `3addr-reg` 检查没跑 → `s_high_ok = 0` → "STAGING NOT writable"
  → APP 的 A1 复制 `rc=2`。

**一个局部数组 + 一个孤儿参数块，拖垮整条链。**

> 为什么 v0.8 好好的，v0.9 就炸？因为加了一个 `ota_verify.c`，
> 池的**布局变了**。这不是"改错了代码"，是"改动了没人声明过的隐式契约"。
> 这类 bug 不会在编译期报警，也不会在功能上报警——它只让日志和校验的结果变成鬼值。

#### 8.14.3 根治：`NOOVERLAY`

L251 手册里有这条指令（缩写 `NOOL`），原文正好是描述我们的处境：

> The NOOVERLAY directive disables overlay analysis and data overlaying.
> When this directive is used, the linker does not overlay memory used by
> local variables and function arguments.
> ... Typically, these types of run-time problems are caused by incomplete
> overlay analysis due to indirect function calls or function calls that are
> not obvious to the linker.

**用法**：接在 `CLASSES(...)` 后面，两个工程都要加。

```
CLASSES(EDATA(0x0-0xFFF), HDATA(0x0-0xFFF), NCONST(0xE000-0xEFFF),
        HCONST(0xFF0100-0xFFFFFF), ECODE(0xFF0100-0xFFFFFF)) NOOVERLAY
```

- **uVision**：Options for Target → **L251** → **Misc controls**（不是 C251 页）
- **命令行**：`tools/build_local.sh` 的两条 `CLASSES(...)` 都已追加

**代价（实测，比想象中小得多）**：

| | 之前 | 之后 |
|---|---|---|
| BL xdata | 3861 | **3907**（+46 B） |
| APP xdata | 3675 | **3695**（+20 B） |
| code | 12686 / 11661 | 完全不变 |
| L251 警告 | 31 / 34 | **0 / 0** |

池本来只有 31~33 字节，禁用后几乎不占额外内存，还顺手清掉了全部链接警告。

**验收**：map 里 `_XDATA_GROUP_` 出现 **0 次**，XDATA 段两两不相交。

#### 8.14.4 同时做的两项配套

**① `log` 接口彻底去掉形参块**

`log_x(lvl, tag, fmt, a, b, c, d)` → `log_x2(lvl, fmt)`，只剩 **2 个形参**
（6 字节，全走寄存器，不生成参数块）：

```c
extern u32 xdata g_log_av[LOG_ARG_MAX];   /* 参数槽：调用方先填 */
u16 log_x2(log_level_t lvl, const char *fmt);

#define LOG_I2(tag, fmt, a, b) do { \
    g_log_av[0] = (u32)(a); g_log_av[1] = (u32)(b); \
    g_log_av[2] = 0UL;      g_log_av[3] = 0UL; \
    log_x2(LOG_LVL_INFO, LOG__F(tag, fmt)); } while (0)

#define LOG__F(tag, fmt)    "[" tag "] " fmt     /* 编译期拼接，零开销 */
```

- `tag` 用**字符串字面量拼接**塞进 fmt，运行时不再有 `put_str(tag)`；
  输出格式**逐字符不变**（`[时间戳][tag] 正文`），行校验和也不变；
- `log_selftest()` 因为需要运行时 tag，改成无参 + 编译期 `#if OTA_BUILD_APP`
  选 `"BL"`/`"APP"`，效果与原来调用方传参一致；
- **验收：map 里 `?log_x??BYTE` 必须 0 次**（`check_overlay.py` 会直接判 FAIL）。

**② 四个"被踩方"移出池子**

| 位置 | 原来 | 现在 |
|---|---|---|
| `crc32_selftest` | `u8 xdata v[9]` | `static u8 xdata s_v[9]` |
| `flash_iap_is_erased` | `u8 xdata tmp[32]` | `static ... s_e[32]` |
| `flash_iap_verify` | `u8 xdata tmp[32]` | `static ... s_v[32]` |
| `flash_iap_crc32_cb` | `u8 xdata tmp[64]` | `static ... s_c[64]` |

⚠ **改 `static` 后不会自动清零**：原来靠 `u16 done = 0;` 的地方必须写成
`static u16 s_done;` + 函数入口 `s_done = 0;`，否则第二次调用会接着上次的值跑。

#### 8.14.4b 另一个独立 bug：`A1 copy rc=2` 不是 crc32 的连锁，是写闸门写错了

修完 overlay 之后复查 A1 的失败点，发现 `rc=2`（`OTA_ERR_ADDR`）另有原因：

- 日志是 `A1 copy rc=2 off=0` —— **`off=0` 说明是正文第一块**，不是读 APP 区；
- 第一块的写入地址是 `STAGING + OTA_IMG_HDR_RESERVE` = **`0x000040`**；
- `flash_iap_check_range()` 里那句"写操作必须扇区对齐"直接把它判为 `OTA_ERR_ADDR`。

**这句闸门本身就是错的。** IAP 的 `PROGRAM` 命令是**按字节编程**，
写入地址根本不需要扇区对齐；只有 `erase` 是以扇区为粒度的。

修法：把 `acc` 从布尔改成三态，只有擦除才查对齐。

| `acc` | 含义 | 对齐检查 |
|---|---|---|
| 0 | 只读 | 不查 |
| 1 | 写入 | **不查**（按字节编程） |
| 2 | 擦除 | 查（512 B） |

```c
if (!acc) return OTA_OK;                    /* 0 = 只读 */
if (acc >= 2U && !flash_iap_is_sector_aligned(addr)) return OTA_ERR_ADDR;
```

调用点：`erase_sector` / `probe` 传 **2**，`write` 传 **1**，`read` / `read_byte` 传 **0**。
分区归属与跨分区检查（闸门 3、4）**完全不动** —— 那是"不擦掉 Bootloader"的保障。

> 教训：这个 bug 从 A1 落地起就在（v0.9 也是 `rc=2`），而我第一眼把它归到了
> "crc32 失败的连锁反应"里。**两个独立故障恰好同时出现时，最容易只修一个。**
> 判据是 `off=0` 和"擦除没报错"这两条：如果真是读 APP 区失败，`off` 不会是 0。

#### 8.14.5 守门：`tools/check_overlay.py`

只靠人眼看 map 不可靠（v0.9 就是这么漏的）。所以加了脚本，
**`build_local.sh` 里排在建 hex 之前，失败即 exit 7**：

| 判据 | 说明 |
|---|---|
| **0** | map 里出现 `?log_x??BYTE` / `?log_x2??BYTE` → **直接 FAIL**（不看过路图、不看池） |
| **0b** | `_XDATA_GROUP_` 仍存在 → **FAIL**（NOOVERLAY 没生效） |
| **B** | 参数块起始地址压在"**直接调用**者/被调者"的局部段上 → FAIL（双向都查） |
| **A** | 源码里出现**非 static 的局部 `xdata` 数组** → FAIL |

判据 B 只用**一跳直接调用**，不用传递可达：直接调用时调用者的局部变量
**一定还活着**，是真错误；而 "main → bl_boot → … → crc32_update" 这种远层
大多栈已退、局部变量早失效，报出来全是误报，会淹掉真问题。

**当前状态**：

```
[Bootloader.map] NOOVERLAY 生效，无 overlay 池；参数块 13 个各自独占 OK
[APP.map]        NOOVERLAY 生效，无 overlay 池；参数块 13 个各自独占 OK
[源码]           无非 static 局部 xdata 数组 OK
[PASS] overlay 守门全部通过
```

#### 8.14.6 三条规则（写代码时按这个来）

1. **新建工程/改链接选项，先确认 `NOOVERLAY` 在**。它是本项目的安全底线，
   删掉它 = 回到 v0.4~v0.9 的随机崩坏。
2. **不要写"非 static 的局部 `xdata` 数组"**。要缓冲区就 `static`，
   并且记得手动初始化。守门脚本会拦。
3. **一个函数形参总大小超过 8 字节，就会生成参数块**。参数块本身不可怕，
   可怕的是它和别人的局部变量重叠 —— 这正是 `NOOVERLAY` 在兜底的。
   如果哪天必须去掉 `NOOVERLAY`，就得保证每个函数的形参 ≤ 8 字节。

#### 8.14.7 产物

`build/step10_BL_APP_v0.10.hex`：`0xFF0000–0xFF774D`，28903 B。

| 段 | 地址 | 大小 |
|---|---|---|
| BL | `0xFF0000-0xFF3B7D` | 15121 B（**16K 槽剩 1154 B**） |
| APP | `0xFF4000-0xFF4087` + `0xFF4200-0xFF774D` | 136 + 13630 B |

`0xFF0000` 首字节 `02 00 27 FF`（BL 复位跳转），
`0xFF4000` 首字节 `75 84 01 7E`（APP 启动代码）—— 与 v0.7/v0.8/v0.9 一致。

### 8.15 ★★★ v0.10 上板仍失败：`code` 限定导致查表地址被截断（v0.11 根治）

#### 8.15.1 现象（v0.10，三次上电）

```
[198][IAP] crc32 selftest FAIL
[210][BL] selftest FAIL rc=6
[532][BL] == summary: init=0 selftest=6 stg=untested app=untested ==
[555][BL] -> STAGING NOT writable: IAP cannot reach FE block
[0][APP] A1 image ready len=49088 crc=5b3ba019      ← 写入成功了（acc 三态生效）
[389][VER] STAGING fail rc=12 step=0                ← 但 BL 校验不过
[407][BL] staging verify FA^L rc=12
```

`jump entry hi=V0000f; lo=4000` 这类行还有个更麻烦的特点：**文字乱码，但行尾校验和自洽**。

#### 8.15.2 先分清"固件错"还是"传输错"

行校验和是固件对 **内存里的 `s_line`** 算的，所以：

- 拿"**应该打的原文**"重算一遍和值，与收到的 `*HH` 比对；
- 不相等 → 固件真的吐了乱码（本次 12 个候选里只有 1 例巧合相等）；
- 相等 → 说明 s_line 里就是这些字节，问题在格式化之前的数据来源。

本次结论：**固件侧乱码**，不是串口/终端/粘贴。

#### 8.15.3 排除表

| 假设 | 证伪方式 | 结论 |
|---|---|---|
| overlay 没关掉 | map 里 `_XDATA_GROUP_` 0 次；BL 70 段 / APP 59 段两两重叠 = 0 | 排除 |
| 镜像内容错 | 拆 hex，CRC 查表 `@FF006F` 与期望表逐字节一致 | 排除 |
| ISR 里打日志（重入踩 `s_line`） | 两个 ISR 内无任何 `LOG_` | 排除 |
| `s_fmt` 被当成 16 位指针 | `DSD 1`（4 B），取数用 `MOV R11,@DR12` 通用指针 | 排除 |

#### 8.15.4 真因：`code` = 16 位绝对地址，而本工程代码基址在 0xFF0000 以上

把 `crc32.c` 用 `SRC()` 选项重编，看编译器给查表生成的指令：

```
; 修正前（static const u32 code crc32_tab4[16]）
ADD  WR10,#WORD0 crc32_tab4        ; 只有 WORD0，没有 WORD2

; 修正后（static const u32 crc32_tab4[16]）
ADD  WR8,#WORD2 crc32_tab4
ADD  DR8,#WORD0 crc32_tab4         ; 24 位 far 地址完整
```

- `code` 限定 = **16 位绝对地址**；`?CO?CRC32` 落在 `FF006F`(BL) / `FF4048`(APP)，
  **已超出 16 位 code 空间** → 实际读 `006F` / `4048`，正好是 **STAGING 区**。
- STAGING 刚擦完是 `0xFF`，写镜像时内容又在变 → CRC 表内容随机。
- 连锁反应：CRC 恒错 → `flash_iap_selftest` 提前返回 → `s_high_ok=0`
  → `STAGING NOT writable`；A1 与 verify 用同一张坏表、而表还从"正在被写的那片 Flash"读
  → 校验值天然不稳 → `rc=12`。
- **v0.9/v0.10 两次修 overlay 都没碰到这个真因。** 参数块重叠确实存在也确实修对了，
  但它不是 CRC 失败的原因 —— 两个独立 bug 同时存在。

#### 8.15.5 修法

1. 常量表一律裸 `const`（进 HCONST，24 位 far 寻址），**不要加 `code`**。
2. `crc32_selftest_value()` 把**算出来的值**打进日志：
   `[IAP] crc32=%x (want cbf43926)`。只打 PASS/FAIL 是这两轮各烧一次板的直接原因 ——
   值像 `cbf43926` 附近 = 算法/参数问题；完全不像 = 查表取错。
3. 守门 `tools/check_overlay.py` 新增**判据 0c**：CODE 类里除 `?CO?start251` 外
   不许出现任何 `?CO?` 段（start251 是硬件复位入口，只被 LJMP 用到，不当数据读）。

#### 8.15.6 产物（v0.11）

`build/step11_BL_APP_v0.11.hex`：`0xFF0000–0xFF77F4`，29109 B。

| 段 | 地址 | 大小 |
|---|---|---|
| BL | `0xFF0000-0xFF3C24` | 15397 B（**16K 槽剩 987 B**） |
| APP | `0xFF4000-0xFF4047` + `0xFF4200-0xFF77F4` | 72 + 13813 B |

尺寸变化：code 各 +8 B、const 各 +95 B、xdata 不变。

#### 8.15.7 留给下一次的教训

1. **"内存被踩"和"地址被截断"在日志上长得一模一样**，要先看汇编再改代码。
2. 每修一处都要问："我修的是这个现象的原因，还是另一个同时存在的 bug？"
3. 自检只打 PASS/FAIL 会浪费烧录轮次，**打实际值**。
4. 判断"固件错 vs 传输错"的可靠办法：拿原文重算行校验和去比 `*HH`。

---

### 8.16 ★★★ A1 校验失败的真正元凶：`BL_STG_PROBE` 每次上电都擦镜像头（v0.13）

#### 8.16.1 现象与破案过程

v0.11 第二次上电稳定复现：

```
[543][VER] STAGING fail rc=12 step=0
```

`rc=12` 是 `OTA_ERR_VERIFY`，`step=0` 对应 `ota_verify.c` 里
`if (s_hdr.magic == 0xFFFFFFFFUL) return OTA_ERR_VERIFY;`
—— 即 **BL 从 STAGING + 0 读到的 magic 是全 FF**。

v0.12 加了探针（逐字节读 / 批量读 / 正文对照），第二次上电的结论是：

```
[735][BL] pg hdr  byte a=4f 54   *a7   ✓校验和自洽
[763][BL] pg hdr  bulk rc=0 a=4f *7b   ✓
[778][BL] pg hdr  bulk b=54 41   *74   ✓
[807][BL] pg body byte b=1 7e    *e2   ✓
```

**镜像头 `4F 54 41 49`（"OTAI"）和正文 `75 84 01 7E` 都在，断电之后仍在。**
也就是说：A1 的写入是对的、`flash_iap_read(0)` 也是对的，
**`step=0` 不是读坏了，而是头在"被校验之前"被人擦掉了。**

#### 8.16.2 元凶

`BSP/flash_iap.h` 里 `BL_STG_PROBE` 默认 **1**，`flash_iap_selftest()` 里：

```c
#if (BL_STG_PROBE != 0)
    s_r_stg = flash_iap_probe(OTA_STAGE_BASE, s_probe_bak) ? ... ;
#endif
```

`flash_iap_probe()` 的动作是"备份 512 B → 擦 → 写 0xA5 回读 → 再擦 → 还原"，
而它作用的 `OTA_STAGE_BASE = 0x000000` **正是镜像头所在**（magic 就在 0x000000）。

还原那一半有三个缺陷，导致"赌它还回去"：

| 缺陷 | 后果 |
|---|---|
| `(void)flash_iap_erase_sector()` 与循环里的 `flash_iap_raw(PROGRAM)` **全不看返回码** | 失败静默 |
| 还原循环里**没有 `bsp_wdt_feed()`**（`flash_iap_erase_range` 里有） | 长循环有被复位打断的风险 |
| 跳过 `0xFF` 字节、写后不校验 | 等于不做还原验证 |

实测同一版固件两次上电两种结果 —— v0.11 那次还原失败（magic 变 FF），
v0.12 那次还原成功（读到 `4F 54 41 49`）。**"同一个动作两种结果" = 还原不可靠。**

#### 8.16.3 修法

```c
/* BSP/flash_iap.h */
#define BL_STG_PROBE        0
```

**只要 STAGING 里可能有镜像，这个探针一次都不能跑。**

想证明"IAP 写得进去"不要用它 —— 用 APP 侧的 A1：写 48 K + 每 128 B 整块回读校验，
那才是真实的写通路验证，而且它不碰镜像头。

#### 8.16.4 顺带查明的两件事

1. **STC-ISP 每次下载都是全片擦除**（STAGING + param 全空，只有下载区有内容）。
   所以"刷完机第一段日志"必然 `param empty/corrupt` + `state=0`，`[VER]` 根本进不去。
   **A1 的验收只能靠"刷完机之后的第二次上电"。**
2. **`param_init()` 里 `(void)param_save(1)` 把返回码吞掉了**，
   于是"BL 写默认值失败"一直是静默的 —— 这解释了为什么刷完机 BL 刚写完默认值，
   200 行之后 APP 又读到 `blank/corrupt`。

#### 8.16.5 仍未解决（v0.13 的主要目的）

同一次上电里：

```
[698][BL]  param empty/corrupt, defaults created     ← BL 说两槽都无效
[713][BL]  seq=1 boot_cnt=0 boot_try=0 state=0
[0][APP]   param_init OK / seq=2 / ota_state=3 / dl offset=49088   ← APP 读得到
```

state=0 就进不去 `[VER]`，所以这是 A1 最后一道门。已排除的（都是硬证据）：

- 两边 `param.c` 的目标码**逐字节相同**（C251 `SRC()` 反汇编 diff 只有注释差异）
- `OTA_PARAM_A_BASE=0xC000` / `B_BASE=0xC800` 来自同一份头文件
- 两工程 param 静态变量地址完全一致：`g_p@0x10001`、`s_slot_a@0x101D5`、
  `s_slot_b@0x103A7`（⇒ `sizeof(param_t)=466`），`s_probe_bak@0x1098D` 与它们不相交

所以 v0.13 用 `param_diag(idx)`（`COMMON/param.c`，只读）打印 BL **实际读回来的**
`magic / 存的 crc / 重算的 crc / len / fmt / rc`。

#### 8.16.6 v0.13 的临时开关（定位后必须关）

| 开关 | 位置 | 作用 |
|---|---|---|
| `BL_STG_PROBE = 0` | `BSP/flash_iap.h` | **长期保持 0** |
| `BL_A1_PROBE = 1` | `Project/Bootloader/boot_main.c` | param 探针，定位后删 |
| `BL_OTA_A1_FORCE = 1` | 同上 | 不管 state 都校验一次，让一份日志同时拿到 `[VER]` 结论；定位后改 0 |

尺寸：BL code 13373 / const 2695 / xdata 3924，末端 `0xFF3F70`，**16K 槽只剩 143 B**
（探针删掉后回到 ~1000 B，A2 的 450~550 B 才放得下）。

---

### 8.17 v0.13 上板结果 → v0.14 改为"测动作"而不是"看结果"

#### 8.17.1 拿到的那一行，把范围压到一个等式

```
[718][BL] pA m=50415241 c=bb701478 k=e70c5373 *57
```

`*57` 用原文反算**正好等于 57**（`[718][BL] pA m=50415241 c=bb701478 k=e70c5373`），
所以这行可信。`50415241` = `PARAM_MAGIC`，`l=466` = `sizeof(param_t)`，`f=1` = `PARAM_FMT_VER`。

**四个头判据里三个通过，只有 CRC 不符。** 而 `param_valid()` 是依次比这四项的，
前三项都过就说明槽里确实躺着"本版本写进去的默认参数"，不是残骸、不是别版本。

于是得到一个等式：

> 槽里那 **462 字节正文**（`sizeof(param_t)-4`）≠ 保存时算 CRC 用的那 **462 字节**。

要么写坏了，要么 CRC 算错了，没有第三种。

#### 8.17.2 为什么之前一直查不出来

`param_init()` 里那句

```c
(void)param_save(1);      /* 返回码被丢弃 */
```

把 `param_save` 的返回值吞掉了。而 `param_save` 内部**明明有**三道可以报错的地方：

| # | 动作 | 失败会返回 |
|---|---|---|
| ① | `flash_iap_erase_range(slot, 512)` | 门禁拒绝 / 擦不干净（`erase_sector` 只抽查 4 点） |
| ② | `flash_iap_write(slot, &g_p, 466)` | 逐字节编程失败 |
| ③ | ↳ 内部 `flash_iap_verify(slot, &g_p, 466)` | 写后整块回读不一致 |

三道全部返回了 `OTA_OK` 还是有一道报错 —— **日志里一个字都没有**。
这就是"改了三轮还是同一个现象"的原因：一直在看结果（读回来的字段），
没有看动作（擦/写/校验各自的结论）。

#### 8.17.3 v0.14 的做法：让 `param_save` 自己汇报

`COMMON/param.c` 里新增（**临时诊断，定位完删除**）：

```c
u32 xdata g_pdg[4];
u8  xdata g_pdg_rc;
```

`param_save()` 在四个位置填它：

| 字段 | 含义 |
|---|---|
| `g_pdg[0]` | `(擦除 rc << 8) \| 擦完是否真的全 0xFF`（用 `flash_iap_is_erased` 全 512 B 检，不再只抽 4 点） |
| `g_pdg[1]` | 写入 rc（含内部整块回读校验的结论） |
| `g_pdg[2]` | 读回后与 RAM 镜像**不同的字节数** |
| `g_pdg[3]` | **最小**差异偏移（倒序遍历，最后留下的就是最靠前的那个） |
| `g_pdg_rc` | 读回本身的 rc |

BL 打两行：

```
[xx][BL] psv e=0 i=1 w=0 r=0        ← e=擦rc i=擦干净 w=写rc r=读回rc
[xx][BL] psv d=0 f=0                ← d=差异字节数 f=最小差异偏移
```

**判读表（照这个走，不用再猜）：**

| 现象 | 结论 | 下一步 |
|---|---|---|
| `i=0` | **擦不干净**。残留位把正文 AND 掉 → CRC 必错，magic 可能还对 | 查 `IAP_TPS` 与擦除等待；`erase_sector` 的 4 点抽查是漏检源 |
| `w!=0` | 写后校验**当场**就发现了差异 | 看 `d`/`f` 定位从第几字节开始坏 |
| `e!=0` | 擦除被门禁拒绝（分区/对齐/容量） | 查 `ota_part_of` / `ota_part_of` 的 `contains` |
| `d=0` 且 CRC 仍不符 | 只剩"CRC 计算本身有问题" | 查 `param_crc` 的 `-4`、`sizeof`、指针宽度 |

数学上 `d=0` 与"CRC 不符"是矛盾的（`d=0` ⇒ 槽内容 == `g_p` ⇒ 重算必等于存的值），
所以只要出现 `d=0`，就百分之百是 CRC 计算这条路上的问题，不会再是 Flash。

#### 8.17.4 顺手修掉的一条"我自己制造的误读"

关掉 `BL_STG_PROBE` 之后 `s_r_stg` 只能是 `UNTESTED`，而 `flash_iap_selftest()` 结尾是

```c
if (s_r_stg != FLASH_PROBE_WRITABLE) rc = OTA_ERR_SUPPORT;   /* = 11 */
```

于是 `selftest=11`，`bl_summary` 又打出

```
[1179][BL] -> STAGING NOT writable: IAP cannot reach FE block
[1198][BL]    check EEPROM=128K first, else CAN_REACH_CODE=0
```

**这两行把排查方向直接带到"硬件到不了 FE 块"上，而实际上只是"没测"。**
v0.13 那一轮确实被它误导了一次。v0.14 加了独立分支：

```c
if (rs == FLASH_PROBE_UNTESTED) { LOG_W0("BL", "-> STAGING UNTESTED (probe off), writability unknown"); return; }
if (rs != FLASH_PROBE_WRITABLE) { /* 原来的 "NOT writable" 分支 */ }
```

**教训：诊断文案必须区分"失败"和"未测"，否则它自己就是新的假阳性源。**

#### 8.17.5 为测量腾出空间的正确方式

BL 的 16K 槽只剩 143 B，放不下新的测量代码。腾空间的顺序：

1. **删已出结论的临时探针**（`param_diag`，结论已经拿到）—— 不欠技术债
2. **关掉 `log_selftest()` 的 5 行 `chkA~chkE`** —— 新增开关 `LOG_SELFTEST`，
   `log.h` 声明与 `log.c` 定义一起用 `#if` 包住（写空函数体会报未使用参数，
   C251 对 `(void)lvl;` 报 `C138`）
3. **不要**为此删功能代码或改分区

`chkA~chkE` 关了不会失去"日志可信度"的证据：任何一条带 `%u/%x/%s` 的日志
都在持续验证同一条通路，`chk` 系列只是把已知值固定下来。要跑就把它改回 1。

#### 8.17.6 v0.14 尺寸

| | code | const | xdata | 末端 | 16K 槽余量 |
|---|---|---|---|---|---|
| BL | 12915 | 2559 | 3929 | `0xFF3D1E` | **737 B** |
| APP | 11617 | 2122 | 3717 | — | — |

产物 `build/step14_param_trace_BL_APP.hex` = `0xFF0000-0xFF775F`，29210 B。
`FF0000` 首字节 `02 00 27 ff`、`FF4000` 首字节 `75 84 01 7e`；两道守门全过，L251 0 WARNING。

---

### 8.18 v0.14 上板读数定论：Flash 写通路物理性失败（→ v0.15 测 CMD_FAIL）

> ⚠️ **本节结论已被 §8.19 推翻，仅作推理过程的存档。**
> v0.15 上板实测：擦除干净、`CMD_FAIL` 从不置位、**4 字节写+回读校验通过** ——
> Flash 擦/写/读三条通路都是活的。真正被踩的是 RAM（`g_p` / `s_line`），
> `w=7` 只是"写后回读校验拿变过的源去比"的结果。**别按本节往下查供电。**

#### 8.18.1 先给结论

**不是 param 的代码问题，也不是"BL 读不到 APP 写的"——是 Flash 写通路物理性失败，两边一样。**
BL 与 APP 每次上电都打 `defaults created` / `factory written`，因为**谁都没写成功**。
A1 的 `crc=f7589f23` 是对**源区（APP 区）**算的，从来没有证明过 STAGING 写成了 ——
所以它一直"成功"。

#### 8.18.2 三行读数的解码

日志 29 行、24 行校验和自洽；用"应打原文"反算，下面四处**全部命中**（分别得 05 / 68 / c7 / c2）：

```
[525][BL] psv e=0 i=0 w=7 r=0
[544][BL] psv d=466 f=0
```

| 字段 | 值 | 含义 |
|---|---|---|
| `e` | 0 | `flash_iap_erase_range` 返回 OK ⇒ 地址门禁/分区归属/扇区对齐**全部通过** |
| `i` | **0** | `flash_iap_is_erased(0xC000,512)` 找到非 FF ⇒ **擦不干净** |
| `w` | **7** | 是 `OTA_ERR_CRC(0x07)`，**不是** `OTA_ERR_IO` ⇒ `flash_iap_verify` 在**第一块 32 B** 内就不一致 |
| `d` | **466** | 466 字节**全部**不同 |
| `f` | **0** | 最小差异偏移是第 0 字节 |

`d=466 f=0` 这条是判据的核心：`param_t` 的默认值里**有大量 0x00、却几乎没有 0xFF**，
所以只有"整片读到 0xFF"才会正好 466/466 全不同 ⇒ **编程完全没落地**。

注意 `e=0` 与 `i=0` 并不矛盾：`flash_iap_erase_sector` 只抽查 `+0/+128/+256/+384`
四个点（`flash_iap.c:258`），四点全 FF 就报成功；`flash_iap_is_erased` 是全 512 B 检查。
**这个"四点抽查"本身就是隐患**，它让"擦不干净"整整藏了三轮。

#### 8.18.3 代码侧已经穷尽（别再回头查）

| 检查项 | 结论 | 依据 |
|---|---|---|
| IAP 命令码 1/2/3 | 正确 | `flash_iap.h:63-66` |
| 地址门禁 | 放行 | 擦除 `rc=0` |
| `0xC000` 地址截断 | 无 | 三寄存器装载，无 `code` 限定 |
| `IAP_TPS` | 11（= 主频 MHz） | 手册：`IAP_TPS = 主频MHz`，11.0592→11 |
| CRC 算法 | 正确 | `cbf43926` 已上板 |
| 两边 `param.c` 目标码 | 逐字节相同 | `SRC()` 反汇编 diff |
| `IAP_CONTR` | `0x80` 正确 | STC32G 该寄存器**没有等待位**（B7 IAPEN/B6 SWBS/B5 SWRST/B4 CMD_FAIL） |
| 触发序列 | 与官方一致 | 手册：操作完成前 **CPU 被硬件挂起**，4 个 `_nop_()` 足够 |

#### 8.18.4 v0.15：把"硬件判没判失败"测出来

`IAP_CONTR.bit4 CMD_FAIL`（`0` 正确 / `1` 失败，**需软件清零**）此前从未读过。
它正好能分开仅剩的两种可能：

- **`f=1`** → 硬件**拒绝**了命令 → 查选项字节（用户 EEPROM 大小）、地址范围
- **`f=0` 且回读 ≠ 写入值** → 命令跑了但物理没生效 → 查**供电**与芯片

实现要点（`g_iap_fail` 是**粘滞**的，OR 累积，用 `flash_iap_fail_clear()` 开窗）：

```c
IAP_TRIG = IAP_TRIG_KEY1;
IAP_TRIG = IAP_TRIG_KEY2;
_nop_(); _nop_(); _nop_(); _nop_();
if (CMD_FAIL) { g_iap_fail = 1U; CMD_FAIL = 0; }   /* 必须在 IAP_CONTR=0 之前 */
```

★ 为什么必须粘滞：`flash_iap_erase_range` 擦完还要做 4 次回读校验，若每次覆盖，
擦除那次的失败立刻被后面的读操作冲掉，等于什么都没测。

`bl_iap_probe()` 排在 `param_init` **之前**，靶子用 **B 槽 0xC800**（写坏了也不影响选槽）：

| 输出行 | 含义 |
|---|---|
| `iap tps=` | `IAP_TPS` 回读，值丢了就必然擦不净/写不进 |
| `ier1 rc= f=` | 第一次擦的返回码 + 硬件判决 |
| `ier seg=` | 4×128 B 里**第一段**含非 FF 的编号；`ff` = 全干净 |
| `ier2 f=` | 连擦第二次还能不能干净 = 弱擦除的铁证 |
| `prg rc= f= r=` | 用**真实 `flash_iap_write`** 写 4 字节再回读（`5a a5` 才对） |

尺寸：BL code 13299 / const 2627 / xdata 3935，末端 `0xFF3EE2`，**16K 槽剩 285 B**。
产物 `build/step15_iap_probe_BL_APP.hex` = 29692 B。

#### 8.18.5 两件必须同时纠正的旧判断

1. **`[VER] STAGING fail rc=12 step=0` 在"刷完机第一次上电"是必然的**，不是回归。
   STC-ISP 每次下载擦整片 → STAGING 空 → `magic` 读到全 FF。A1 的真结论只在
   **"APP 造完镜像之后的下一次上电"** 才有。
2. **日志文字乱码不是固件 bug。** 本轮 5 行坏行全是"字符被替换成空格"
   （`seq 1 boot_ nt=0`、`pen ing install`、`staging v rify FAIL`、`APP head a= 075  00084`
   ／`b= 00001 Ie`），属文本提取/粘贴环节；用原文反算，所有关键行都对得上。
   **以后不再把乱码当固件问题追。**

#### 8.18.6 零成本验证（比再烧一版更可能直接命中）

1. **把 4G 模组断电/拔掉再跑一遍。** 4G 模组峰值电流可达 2 A，与 MCU 同轨会把 VCC 拉塌；
   而 Flash **编程/擦除是最耗电的操作**（读只要 4 个时钟周期，写 1 字节 30~40 µs、
   擦 512 B 需 4~6 ms 持续大电流）。这一条同时解释"读得到、写不进"和"约 17% 的串口字符损坏"。
2. 万用表量 MCU 的 VCC 脚（IAP 要求 ≥ 4.2 V，5 V 系统建议 ≥ 4.5 V），**重点看擦写瞬间的跌落**。
3. STC-ISP「读取硬件选项」确认 **用户 EEPROM 大小真的是 128 K**、以及实际 IRC 频率。
   选项字节只在勾选「本次下载需要修改硬件选项」时才会被改写，而本流程一直让用户**取消勾选**,
   所以芯片里跑的是很早以前那一次烧录留下的设置。

---

### 8.19 ★★★ v0.15 上板推翻 §8.18：Flash 写通路是活的，坏的是 RAM（→ v0.16）

> **§8.18 的"物理性失败"结论作废。** 那一版没有 `CMD_FAIL`、也没有"短写"这个对照，
> 只有"长写整块失败"一个观测面，于是把 RAM 问题误判成了 Flash 问题。

#### 8.19.1 五行读数（57 行校验和全部自洽，全文没有一行作废）

```
[211][BL] iap tps=11                     ← 擦写脉宽参数对
[226][BL] ier1 rc=0 f=0                  ← 擦 0xC800 成功，CMD_FAIL 没置位
[289][BL] ier seg=ff                     ← 512 B 全 FF：擦得干干净净
[302][BL] ier2 f=0                       ← 连擦第二次也干净
[316][BL] prg rc=0 f=0 r=5a a5           ← ★ 4 字节写 + 写后回读校验**通过**
```

同一片上电周期里，`param_save` 写 466 字节到 **0xC000** 却 `w=7`（`OTA_ERR_CRC`）。

| 观测 | 事实 |
|---|---|
| 擦除（0xC800，两次） | 成功且干净 |
| 编程 4 B（0xC800） | **成功，回读 `5a a5` 一致** |
| 编程 466 B（0xC000） | 写后回读校验失败 |
| `CMD_FAIL` | 全程 `0` —— 硬件从没说过"我拒绝" |

⇒ **擦、写、读三条通路都是活的。**"芯片写不进去 / 供电不足"这条线到此为止（§8.18.6 的
三条零成本验证不必再做了）。

#### 8.19.2 真正的异常：`boot_cnt=4280937472`

```
[698][BL] seq=1 boot_cnt=4280937472 boot_try=0 state=0
```

`4280937472 = 0xFF29EC00`。复位后 `param_load_defaults()` 刚把整个 `param_t` 清 0，
`boot_cnt` 只可能是 0。而 `0xFF29EC` **正好是 BL 代码区里的一个地址**
（`ota_verify_staging?` 就在 `0xFF292C`）。
⇒ **一片代码地址被搬进了 `g_p` 的 RAM 镜像。**`seq=1` 却是对的（`0x10019` 没被踩），
说明踩踏是**局部、偶发**的，不是整片错位。

#### 8.19.3 乱码定案：不是线上坏的，是 RAM 里就坏了

§8.18.5 把文字乱码归到"文本提取/粘贴环节"，**这一条也是错的**。判据如下。

`put_line_sum()` 的算法是：格式化写完后，对 `s_line[0..s_pos-1]`（**已经写进缓冲的那份字节**）
做 8 位累加，再把 `*HH` 追加进去。所以：

- 和值 **不匹配** ⇒ 线上被打坏（接收 ≠ 缓冲）
- 和值 **匹配** ⇒ 缓冲里本来就是这个样子

本版 57 行全部匹配，但其中 4 行**肉眼就是乱码**。把"应该打的原文"重算和值，四条**全部对不上**：

| 收到的（和值吻合） | 和值 | 原文反算 | 结论 |
|---|---|---|---|
| `staging veriCy FAIL rc=12` | `17` | `3a` | ✗ |
| `APP head T=55 84` | `fb` | `0a` | ✗ |
| `APP head b=21 7e` | `37` | `05` | ✗ |
| ``jump entr` hi=3ff lo=704000`` | `47` | `c6` | ✗ |

⇒ **固件确实吐出的是 `veriCy` / `hi=3ff` / `lo=704000`。** 这些字符从没在源码里出现过，
且 `put_dec`/`put_hex32` 的输出集合只有 `0-9a-f`，不可能产出 `3ff`（`hi` 是 u16→`%x`，
`0x3FF` 是参数槽里的值）或 `704000`。

> **★ 校验和的适用边界（务必记牢）★**
> `*HH` 只保护"主机缓冲 → 线上 → 你的终端"这一段，**完全不保护 MCU 内部 RAM**。
> 一旦 RAM 被踩，坏字节和被算进去的和值是**同一份**，和值照样吻合。
> 所以"和值对"只能证明**线路没坏**，不能证明**内容对**。
> 反查手段仍是那一条：**拿"应该打的原文"重算一遍和值**——对不上就说明固件真的错了。

#### 8.19.4 两条线索合起来指向什么

`g_p`（466 B 的 RAM 镜像）被局部踩 + `s_line`/参数槽被局部踩，两者都在 XDATA 里，
都发生在"运行一段时间之后"。而对 `param_save` 来说这恰好是致命的：

```c
g_p.crc32 = param_crc(&g_p);                       /* 1. 先按此刻的 g_p 算 CRC */
flash_iap_erase_range(...);                        /* 2. 擦（4~6 ms）            */
flash_iap_write(..., &g_p, 466);                   /* 3. 逐字节编程（≈19~40 ms） */
      └─ 内部 flash_iap_verify(addr, buf, 466)     /*    比的是**此刻的 g_p**   */
```

**第 3 步要花 19~40 ms**（466 × 30~40 µs，CPU 每字节被硬件挂起）。如果 `g_p` 在这段窗口里
被改了一个字节，写进去的是旧值、`verify` 比的是新值 ⇒ **必然 `w=7`，而 Flash 完全无辜。**
4 字节短写窗口只有约 0.15 ms，几乎不可能被命中 —— **这正好解释了"短写通、长写不通"。**

所以本版要量的第一件事就是：**`g_p` 在写窗口内到底变没变。**

#### 8.19.5 v0.16 探针（`build/step16_psv_BL_APP.hex`）

三项改动，全部放进已有的两处探针里，不新增函数：

1. **短写搬到 A 槽**（`prg` → `pra`，靶子 `0xC800` → `0xC000`）。
   零代码代价，把"地址"和"长度"两个变量分开：

   |           | 466 B @0xC000 | 4 B @0xC000 |
   |-----------|---------------|-------------|
   | 结果      | 已知 `w=7`    | **★ 本版**  |
   | 对照      | 466 B @0xC800 未测 | 4 B @0xC800 → `rc=0`（已知） |

2. `param_save` 里**在写前后各算一次 `g_p` 的 CRC**：
   `g_pdg[2]=cb`（擦完、写之前）、`g_pdg[3]=ca`（写完）。
3. 恢复 `d`（差异字节数）/ `f`（最小差异偏移）：`g_pdg[4] = (d<<16)|f`。

新增两行日志：

```
[BL] psv e=%u i=%u w=%u r=%u                  ← 与 v0.14/v0.15 同语义
[BL] psv cb=%x ca=%x d=%u f=%u                ← 本版新增
[BL] pra rc=%u f=%u r=%x %x                   ← 短写落在 A 槽
```

**判读表（按优先级从上往下）**

| 现象 | 结论 | 下一步 |
|---|---|---|
| `cb != ca` | **写窗口内 `g_p` 被踩** ⇒ 根因是 RAM 被踩，`w=7` 是它的表现 | 改成"每 32 B 一写一回读"缩短窗口，同时抓踩 RAM 的凶手 |
| `cb == ca` 且 `d=466 f=0` | 466 B 一个都没落地，**跟源缓冲无关** | 看 `pra`：短写在 A 槽也失败 ⇒ 地址问题；短写成功 ⇒ 长度/供电/时序 |
| `cb == ca` 且 `d=0` | 写进去了也读得回 | 那 `w` 就不该是 7 ⇒ 查 `flash_iap_verify` 自身 |
| `cb == ca` 且 `0<d<466`、`f` 指向某个字段 | 只有尾巴坏 | `f` 的值直接对应 `param_t` 的字段偏移，按图索骥 |
| `pra rc=0 r=5a a5` | A 槽短写也通 ⇒ **与地址无关** | 问题锁定在"长度/写窗口" |
| `pra rc=7 r=ff ff` | 连 4 B 都写不进 A 槽 | 回去查 `0xC000` 与 `0xC800` 在门禁/分区/选项字节上的差别 |

> **BL 空间告警**：本版 `code=13463 const=2661 xdata=3943`，末端 `0xFF3FA8`，
> **16 KB 槽只剩 88 字节**。下一版如果还要加探针，必须先删旧的（`flash_iap_selftest()`
> 里 `bl head` / `app head a` / `app head b` / `jump entry` 四条已长期稳定，是首选删除项）；
> 或者把 `BL_A1_PROBE` 整体关掉。**A2（搬运）约需 450~550 B，届时必须腾出空间。**

---

### 8.20 v0.16 上板读数：把嫌疑从"地址"移到了"长度"，并抓到一个更硬的事实

#### 8.20.1 校验和这轮的分布变了：3 行作废、55 行可信

`tools/_last_log.txt` 里 58 行，核出 3 行和值对不上：

| 行 | 收到 | 重算 | 性质 |
|---|---|---|---|
| `[857][BL  staging verify FAIL rc=12` | `84` | `f8` | **作废** |
| `[874][B ] A1 retry 1/3 armed` | `f4` | `6a` | **作废** |
| `[1045] BL] jump entry hi=U0000f8 lo=4000` | `16` | `98` | **作废** |

这三行的共同形态是**时间戳前缀掉了一个字符**（`[BL]`→`[BL `、`[BL]`→`[B ]`、
`[1045][BL]`→`[1045] BL]`），是典型的**传输/粘贴**损坏，与固件无关，判读时必须先剔掉。

反过来，另外三行**看着像乱码、和值却完全自洽**（用"应打原文"反算差值正好等于被插入字符的和）：

| 收到 | 应打原文 | 差 |
|---|---|---|
| `psv cb=c8b2**o**b74` | `psv cb=c8b20b74` | `0x6F-0x30 = 0x3F`，`0x47-0x08` = 同值 ✓ |
| `APP head a=**U000**75 84` | `APP head a=75 84` | `'U'+'0'+'0'+'0' = 0xE5`，`0x10-0x2B` = 同值 ✓ |
| `APP head b=**f00**1 7e` | `APP head b=1 7e` | `'f'+'0'+'0' = 0xC6`，`0xEF-0x29` = 同值 ✓ |

⇒ **固件真的吐了这几串字符**，损坏点在 MCU 内部，不在线上。
`'U'` 与 `'o'` 都是**十六进制字符表之外**的字符，而 `put_hex32` 只会吐
`'0'~'9'`/`'a'~'f'`——`'a'+21='U'`（`s_b0=31`）、`'a'+14='o'`（`s_b0=24`），
即 **`put_hex32` 的 `s_b0`（半字节）越界了**。`s_b0`/`s_started` 是
`log.c` 里的 `static u8 xdata`，说明**格式化途中它们被异步改写**。

#### 8.20.2 `psv cb` / `ca` 只差低 16 位 ⇒ 这一格不能当证据

`cb=c8b2ob74 ca=c8b22b72`：CRC32 有雪崩性，输入差一个字节就会全 32 位都变，
**共享高 16 位在数学上几乎不可能**；而 `ob74` 里的 `o` 又是渲染损坏。
⇒ 这一格已被 8.20.1 证明不可信，**不要用它推"`g_p` 被踩"**。

#### 8.20.3 真正的新事实：`128 B 通、466 B 不通`

同一次上电里，APP 的 A1 搬运是 **128 B 一块、384 块**，
日志里**一条 `A1 copy rc=` 都没有** ⇒ 384 次 `flash_iap_write(…,128)` 全部
写后校验通过；随后 42 B 的镜像头也写成功。而 `param_save` 的 **466 B** 必失败
（`w=7` + `d=466 f=0`），BL 里 4 B 也通（`pra rc=0 r=5a a5`）。

**⇒ 唯一还站得住的变量是"长度"**。466 > 255、128 < 255，这是第一嫌疑
（`u16`/`u8` 混用、循环上界截断一类）；其次是"写窗口长度不同导致撞上中断"。

> ⚠️ 顺带作废一条旧结论：**A1 的镜像 CRC 在不同版本之间不同 ≠ 读不稳**。
> 本轮同版本两次 A1 都是 `crc=e5996b9a`，完全一致；v0.16 的 `9b305bff`
> 是因为 APP 编译产物本身变了（`code` 11674→11717）。**长读是稳的。**

#### 8.20.4 v0.17 探针（`build/step17_len_ea_probe_BL_APP.hex`）

只问一件事：断点在哪一档长度。源数组成 `i ^ 0x5A`，读回值自证：

| 长度 | `[0]` 应为 | 末字节应为 |
|---|---|---|
| 128 B | `5a` | `[127] = 25` |
| 466 B | `5a` | `[465] = 8b` |

三条新日志：

```
[BL] sh0 %x %x          ← 写之前读 STAGING[0..1]（镜像造好应是 4f 54）
[BL] sw128=%u r=%x %x   ← 128 B + 写后校验 rc + 回读 [0] / [127]
[BL] sw ea1=%u r=%x %x  ← 466 B，EA=1（param_save 的真实环境）
[BL] sw ea0=%u r=%x %x  ← 同一个 466 B 写，全程关中断
[BL] sh1 %x %x          ← 写之后 STAGING[0..1]
```

**判读表**

| 现象 | 结论 |
|---|---|
| `sw128=0 r=5a 25` 且 `sw ea1=7` | **确认是长度**：128 通、466 不通 ⇒ 查循环上界/计数类型 |
| `sw128=7` | 连 128 也失败 ⇒ 不是长度（APP 那 384 次成功值得回头复核） |
| `sw ea1=7` 但 `sw ea0=0 r=5a 8b` | **中断是根因**（16 ms 窗口撞 Timer0 节拍）⇒ 长擦写全程 `EA=0` |
| `sw ea1=0 r=5a 8b` 且 `psv d=466` | 从 `s_sw` 写通、从 `&g_p` 写不通 ⇒ **源指针**问题，不是 Flash |
| `sh0=4f 54` 但 `sh1=5a ..` | **写跑到 0x0000 去了**（地址高位丢失）⇒ 抓 `flash_iap_set_addr` |
| `sh0=ff ff` | APP 的镜像头根本没落进 STAGING（APP 侧 42 B 写失败） |

> **BL 空间**：本版删掉了 `iap tps` / `iera rc` / `iera seg` / `iera2` / `pra` /
> `psv e i w r` 六条已定论的日志，换成上面的探针；
> `code=13460 const=2632 xdata=4450`，末端 `0xFF3F88`，**16 KB 槽剩 119 B**。

---

### 8.21 v0.17 上板定论：**中断打断 IAP 序列**（不是长度、不是 Flash、不是指针）

这一轮把 v0.16 的"长度"嫌疑彻底否掉了。同一次上电、同一份代码、
同一源数组 `s_sw[i]=i^0x5A`、同一地址 `0xC000`，只有 EA 一个变量：

| 操作 | EA | 读数 | 判读 |
|---|---|---|---|
| 4 B 写 | 1 | `rc=0` | 0.15 ms，一个 1 ms 节拍都打不中 |
| 128 B × 384（APP 的 A1 搬运） | **0** | 一条 `A1 copy rc=` 都没有 | 全程关中断 ⇒ 全通过 |
| 128 B 写 | 1 | `sw128=7 r=5a 25` | 4~5 ms，能被节拍打断 |
| 466 B 写 | 1 | `sw ea1=7 r=5a 8b` | 16 ms，打断十几次 |
| 466 B 写 | **0** | `sw ea0=0 r=5a 8b` | 同一次上电，同源同址 ⇒ **通过** |

⇒ **长度不是变量，中断才是。** `128 < 255`、`466 > 255` 这个"u16/u8 截断"的
假设到此作废——同一个 128 B 换个 EA 状态就一好一坏，长度解释不了。

#### 8.21.1 为什么 `rc=7` 但数据其实落地了

`sw ea1=7` 同时读到 `r=5a 8b`——首字节和末字节**都对**。也就是说：
`flash_iap_write` 的写循环把 466 B 全写进去了，报错的是它尾部的
`flash_iap_verify`。**坏的是"读"，而且只在多字节序列里坏**：
`flash_iap_read_byte`（单次 raw）在 EA=1 下一直是对的。

这解释了 `param_save` 的 `d=466 f=0`：它自己再 `flash_iap_read` 466 B 读回，
**读回的是全 0xFF**，与 `g_p` 逐字节比对自然 466 全不同、最小差异偏移 0。
Flash 里其实有数据，只是 EA=1 的多字节读者读不出来。

#### 8.21.2 根因边界

`flash_iap_raw()` 原本就 `EA=0`，但它只保护**一次命令**。
466 字节编程 = 466 次 raw = **465 个开中断窗口**，每个都能被 Timer0 打进来。
单次 raw 是原子的，**序列不是**。

#### 8.21.3 修法（v0.18）

`flash_iap_write()` / `flash_iap_read()` 把**整个循环**包进
`flash_iap_critical_enter/exit`，循环内每 `IAP_FEED_EVERY`(=32) 字节
`bsp_wdt_feed()`（32 B ≈ 1.1 ms，远短于 `WDT_PS_32` 的数百 ms 起）。
喂狗只写 `WDT_CONTR`，不依赖中断，EA=0 下照样有效。

**必须同时改 read，不能只改 write**：`verify` / `is_erased` / `crc32` /
`param_init` / BL 校验 STAGING 全都走这条读通路。只修 write 等于
"写得进去却读不出来"，`[VER] rc=12` 会继续复现。

> **不要动的地方**：`flash_iap_raw` 自己的 EA=0 必须保留。
> 外层已经关中断时它读到 `ea=0`、退出写 `EA=0`，是无害的幂等行为；
> 但只要有一处调用方漏了外层保护，它是最后一道防线。

#### 8.21.4 v0.18 探针与判读（`build/step18_ea_fix_BL_APP.hex`）

探针从 5 行缩到 2 行（`sh0`/`sh1`/`sw128`/`sw ea0` 全部删掉，已定论，白占 16 K 槽）：

```
[BL] sw466=%u r=%x %x    ← 466 B，EA=1（param_save 的真实环境）
[BL] pra2 %x %x          ← param_save 之后 A/B 两槽的首字节
```

| 现象 | 结论 |
|---|---|
| `sw466=0 r=5a 8b` | **修好了**：466 B 在 EA=1 下也通过，r 自证按序写入 |
| `sw466=7` | 关中断没解决 ⇒ 回头查 `s_i`/`s_dat` 与 `?flash_iap_write??BYTE` |
| `psv d=0` | param 写进去也读得回 ⇒ A1 校验应该跟着好 |
| `psv d=466 f=0` 但 `pra2 41 ff` | 写落地了、读回比对错 ⇒ 查 `s_slot_a` 与 `g_p` |
| `pra2 ff ff` | 两个槽都没有 magic `0x41` ⇒ 写真没落地 |

> **BL 空间**：`code=13198 const=2572 xdata=4452`，末端 `0xFF3E46`，
> **16 KB 槽剩 441 B**（v0.17 是 119 B）。A2 估 450~550 B，**还差约 100 B**。

---

#### 8.22 v0.18 上板：`rc=2` 是**不可能值** ⇒ 先修"错误码说谎"，再谈诊断

v0.18 只加了一条读数 `sw466=2 r=ff af`。`2` = `OTA_ERR_ADDR`。

**先把"2 从哪来"彻底排除掉**（这次没有靠猜，是照着 `.lst` 逐条核对的）：

| 判据 | 实测语义 | `0xC000 / 466 / acc=1` 下的结果 |
|---|---|---|
| gate1 容量 | `addr < s_eeprom_size` 且 `len <= size-addr` | `0xC000 < 0x20000`、`466 <= 0x14000` ✔ |
| `ota_part_of(0xC000)` | 遍历 `s_parts`，`s_parts[i].id` 在结构体 **偏移 9** | STAGING 段 `0xC000<0xC000` 不成立而跳过 → **2 (PART_PARAM)** ✔ |
| `ota_part_writable(2)` | 硬拒只拦 `0/3/4`，再按 id 查 `writable`（偏移 8） | **1** ✔ |
| `ota_part_contains(2,0xC000,466)` | `0 > (0x800-466)` 不成立 | **1** ✔ |
| 扇区对齐 | 只在 `acc>=2` 时检查，`acc=1` 直接跳过 | 不参与 ✔ |

⇒ `flash_iap_check_range(0xC000,466,1)` **必为 0**，`flash_iap_write` 不可能返回 2。
同时 `tools/check_overlay.py` 也从 map 侧独立确认：**"参数块 13 个各自独占 OK"**，
`addr/len/ea` 正好占满 `?XD?FLASH_IAP_WRITE??FLASH_IAP`(7 B)、
`?flash_iap_read`(7 B)、`?flash_iap_verify`(6 B)、`?flash_iap_check_range`(4 B)
四个段，**没有一个字节重叠**。"参数块被局部变量压住"这条老路在 v0.18 已经不成立。

**那 2 是哪来的？两个"错误码说谎"的洞（本版已修）：**

1. `flash_iap_read()` 原来 `buf[i]=0xFF` 之后**忽略 raw 的返回码**，
   读失败就等价于"数据确实是 FF"。`param_save` 的 `d=466 f=0` 正是
   **"读不出来"被伪装成"写不进去"** —— 方向从一开始就错了。
   → 现在任一字节 raw 失败即整次返回 `OTA_ERR_IO(6)`。
2. `flash_iap_verify()` 原来把读通路的任何失败**改写成 `OTA_ERR_ADDR`**：
   `if (flash_iap_read(...) != OTA_OK) return OTA_ERR_ADDR;`
   → 于是 "IO(6)" 和 "ADDR(2)" 在日志里长得一模一样。
   **v0.18 那句 `sw466=2` 是否被这一行改写过身份，现在无法排除。**
   → 现在原样上抛。

**结论性判据（本版重写探针就是为了它）**：不要再读"合成结论"，
把四道闸门分别念出来。`build/step19_gate_probe_BL_APP.hex`：

```
[BL] gate of=%u wr=%u ct=%u rg=%u     ← part_of / writable / contains / check_range
[BL] ers=%u a=%x b=%x                 ← 擦 512B 的 rc，及 0xC000、0xC1D1 是否为 ff
[BL] w4=%u r=%x %x                    ← 4 B 短写（v0.15/16 确实通过过的那条）
```

| 现象 | 结论 |
|---|---|
| `gate of=2 wr=1 ct=1 rg=0` | 门禁全通 ⇒ 之前那个 2 是错误码说谎，按 `w4` 继续看 |
| `gate` 里出现某个 0 | 就是那一步拒的，且能直接定位到 `part_of` / `writable` / `contains` |
| `ers=0 a=ff b=ff` | 擦除正常覆盖整个扇区，地址无偏移 |
| `ers=0` 但 `b≠ff` | **地址系统性偏移**：擦只擦到一部分 ⇒ 查 `flash_iap_set_addr` 三寄存器 |
| `w4=0 r=5a a5` | 短写仍通 ⇒ 问题只在"长序列"（长度/时长），不是地址或门禁 |
| `w4=6` | 读通路 raw 失败（新错误码）⇒ 直接查 IAP 读命令与 `IAP_TPS` |
| `w4=2` | 连 4 B 都被门禁拒 ⇒ 与长度无关，按 `gate` 行定位 |

> **BL 空间**：`code=13321 const=2538 xdata=3947`，末端 `0xFF3E9F`，
> **16 KB 槽剩 352 B**（删掉 `stg probe disabled` / `app not blank` 两行腾出 203 B）。
> A2 估 450~550 B，**仍差约 150 B**。

> **这一轮的方法论教训**：当读数给出一个"源码不可能产生"的值时，
> 正确的动作不是继续猜机制，而是先检查**这个值是不是被某行代码改写过的**。
> `verify` 里那句 `return OTA_ERR_ADDR` 就是典型的"改写者"。

---

### 8.22 v0.19 上板 + map 定论：**C251 参数块只隔 7 字节，嵌套调用会把调用者的活指针踩掉**

#### 8.22.1 v0.19 上板读数（校验和 57/57 全自洽）

```
[170][BL] gate of=2 wr=1 ct=1 rg=0      ← 四道闸门逐条念出来，全部通过
[190][BL] ers=6 a=ff b=ff               ← 擦除返回 OTA_ERR_IO(6)
[207][BL] w4=7  r=5a a5                 ← 4 B 写：rc=7(CRC) 但首两字节读回全对
```

- `gate` 四值 ⇒ `check_range(0xC000,466,1) = 0`。**地址门禁彻底无罪**，
  v0.18 那个 `sw466=2` 是 `flash_iap_verify` 里 `return OTA_ERR_ADDR` 改写出来的假身份。
- `w4=7` 且 `r=5a a5`：**数据真的落地了，报错的是比对**。
- map 段表 + `check_overlay.py` 双双确认 39 个 xdata 段零重叠 ⇒「静态变量共址」这条线关闭。

#### 8.22.2 真正的根因（map 地址直接给出）

C251 给每个函数的形参块 `?函数名??BYTE` 只分配 **7 字节**，实际要用 **10 字节**
（`buf` 4 + `addr` 4 + `len` 2，第 3 个形参在块内偏移 **+8**，见汇编
`MOV DPTR,#WORD0 ?flash_iap_read??BYTE+8`）。于是相邻参数块按 7 字节步进、
彼此重叠 3 字节。v0.19 实测：

| 参数块 | 地址 | 块内布局 |
|---|---|---|
| `?flash_iap_write??BYTE`  | 0x10F0E | buf@+0, addr@+4, len@+8 |
| `?flash_iap_read??BYTE`   | 0x10F15 | buf@+0, addr@+4, len@+8 |
| `?flash_iap_verify??BYTE` | 0x10F1C | buf@+0, addr@+4, len@+8 |

`write → verify → read` 是三层嵌套。**read 一被调用，写自己的块
（addr@0x10F19、len@0x10F1D）就把 `verify` 还活着的 `buf`（0x10F1C–0x10F1F）
改掉三个字节**；read 返回后 verify 拿这个被改过的指针逐字节比对 ⇒ 恒不相等
⇒ **恒返回 `OTA_ERR_CRC(7)`**。

这一次性解释了自 v0.18 起的全部现象：`w4=7 但数据对`、`psv w=7`、
`A1 param_save rc=7`、`[VER] rc=12`。APP 工程布局不同（A1 的 384×128 B 写+校验全过）
不是它没问题，只是没踩上。

**顺带作废 §8.21 的结论**：中断（EA）确实要关（长序列必须原子），但它**不是** `w=7` 的成因。
v0.17 那次 `ea0=0` 之所以成功，是因为当时 `verify` 的布局还没被踩。

#### 8.22.3 修法：形参入口即拷进模块级 static

模块级静态变量住在 `?XD?FLASH_IAP`（**INSEG** 类，链接器只把 UNIT 类排进可覆盖区），
参数块永远压不到它。三个函数各自在入口第一件事就 `s_xxx = 形参`，此后只用副本：

```c
static u16 s_wlen;  static u32 s_waddr;      /* write  */
static u16 s_rlen;  static u32 s_raddr;      /* read   */
static const u8 *s_vsrc; static u16 s_vlen; static u32 s_vaddr;   /* verify */
```

从此与链接布局无关：再加变量、再移位，这三条通路都不会重新踩上。
**新代码凡"形参要在嵌套调用之后还活着"，一律照此办理。**

#### 8.22.4 同时修掉一个"错误码说谎"的洞

`flash_iap_raw()` 原来**永远返回 `OTA_OK`**（CMD_FAIL 只置粘滞 `g_iap_fail`）。
于是 v0.19 给 `flash_iap_read` 加的 `s_bad` 永远为 0 —— **读失败继续被伪装成"数据全 FF"**。
现在 raw 把本次 CMD_FAIL 如实变成 `OTA_ERR_IO(6)`，`read` 的 `s_bad` 才真正生效。

#### 8.22.5 v0.20 上板判读表（`build/step20_param_fix_BL_APP.hex`）

新日志两行，源数组 `s_sw[i] = i ^ 0x5A`，读回自证（`[0]=5a`、`[465]=8b=0x1D1`）：

| 行 | 含义 |
|---|---|
| `ers=%u %x %x`  | 擦 0xC000 的 rc；0xC000 与 0xC1D1 是否 ff |
| `w466=%u %x %x` | **466 B 长写**（param 的真实长度）的 rc；首字节与尾字节 |

| 现象 | 结论 |
|---|---|
| `w466=0 a=5a b=8b` | **修复成立**：466 B 全落地、写后校验也通过 |
| `w466=7` | 比对仍错 ⇒ 按 §8.22.2 重新查新 map 的三个参数块地址 |
| `w466=6` | 某次 IAP 命令被硬件拒（CMD_FAIL，现在会如实上报） |
| `w466=0` 但 `b=ff` | 只有尾巴没落地 ⇒ 喂狗间隔或看门狗窗口问题 |
| `ers=6` 且 `a≠ff` | 擦除没擦净 ⇒ 回头看 IAP_TPS 与选项字节 |

已删日志（都已长期稳定，腾出 16K 槽）：`gate …`、`APP head a/b`（APP 自己会打
`read app[0..3]`）、两处 `stg probe disabled` / `app not blank`，另删掉 `app_head[4]` 数组
与其 4 次读。BL 码 13114 B，末端 `0xFF3D7E`，**16K 槽剩 641 B**（v0.19 只有 352 B）。

### 8.23 v0.20 上板：**写通路无罪了**（`w466=0`），但 `param_save` 那次写仍然没落地

日志 51 行，`tools/check_log_sum.py` 判 **51/51 全自洽**（含时间戳重算），
所以下面每个数都是固件真的写进 `s_line` 的字节，不是粘贴问题。

#### 8.23.1 三条硬读

| 行 | 读法 |
|---|---|
| `ers=6 ff ff` | 擦 0xC000 之后 rc=6，但 `0xC000` 与 `0xC1D1` 都是 ff ⇒ **扇区确实擦干净了**，6 来自 `flash_iap_erase_sector` 那 4 点回读里**某一次读失败**（`dat` 初值 0x00、读失败不改它 ⇒ 报"不干净"）。这是"错误码说谎"的第三处，已记在案 |
| `w466=0 5a 8b` | ★★ **466 B 长写整段通过**，首字节 0x5A、尾字节 0x8B（=465^0x5A，自证）⇒ §8.22 的"形参入口即拷副本"修复成立。**地址门禁 / 长度>255 / CMD_FAIL / 擦除时序 / 供电 这一整串嫌疑人全部出局，别再回头查** |
| `psv cb=c8b22b74 ca=c8b22b74 d=466 f=0` + `pra2` A=ff | `param_save()` 写同样 466 B 到同样 `0xC000`：`g_p` 在写窗口里**没变**（cb==ca），可 flash 里那 466 字节和 `g_p` 一个都对不上，且 A 槽首字节仍是 ff ⇒ **这一次写根本没落地** |

⇒ 两个调用的 addr、len、被调函数完全相同，自变量只剩两个：
**(a) 源缓冲是 `g_p` 还是静态数组 `s_sw`**；**(b) 是本次上电的第几次擦-写**。

#### 8.23.2 两处"真乱码"（固件侧，不是传输）—— 结论是：小数值别用 `%x`

| 收到 | 应该是 | 判读 |
|---|---|---|
| `armed TAPP will rebuild)` | `armed (APP will rebuild)` | 丢了一个 `(`，行尾和值仍自洽 ⇒ **缓冲区里就已经少了这个字节** |
| `pra2 ff 800ff` | `pra2 ff ff` | 第二个 `%x` 打出 5 位十六进制 = 参数高 16 位漏进来了，`(u32)(u16)` 的截断没生效 |

已用 hex 反查确认格式串本身是对的（`armed (APP will rebuild)` 在 `0xFF3605`、
`pra2 %x %x` 在上一版 `0xFF36C9`）⇒ 坏在运行时"填缓冲区"这一段，不在链接产物。
**处置**：诊断行里的小数值一律用 `%u`（长除通路），漏位会显示成一个大数，
一眼能认出是"参数脏"；不写 `%x`。这两处只影响 1~3 个字符，**不足以推翻任何数值结论**，
但足以让人误读字符串 —— 所以 §8.23.3 之后的探针不再靠字符串表达含义。

#### 8.23.3 本段日志**不是** A1 的判据

这是"刷完机第一次上电"那一段（`param empty/corrupt`、`seq=1`、STC-ISP 全片擦除后
STAGING 必然为空）⇒ `[VER] STAGING fail rc=12 step=0` 是**必然结果，不是回归**。
A1 的判据只能是**断电再上电的第二段**：BL 读 STAGING 镜像头 + 48 K 正文 CRC。
（`BL_OTA_A1_FORCE=1` 让它不看 `state` 也校验，所以第二段一定跑得到。）

#### 8.23.4 v0.21 探针（`build/step21_abprobe_BL_APP.hex`）

同一上电周期内顺序做两次写，只改"源缓冲"这一个变量：

| 行 | 内容 |
|---|---|
| `P1 e%u r%u f%u b%u` | 源 = `param_get()`（`g_p` 默认值，magic='PARA'，首字节 0x41=**65**）。**复刻 `param_save` 的那次写**：擦 rc / 写 rc / CMD_FAIL 粘滞 / 写完 `0xC000` 首字节 |
| `P2 e%u r%u f%u b%u` | 源 = `s_sw`（首字节 0x5A=**90**）。v0.20 已验证能过的那种写 |
| `tps=%u sz=%u` | `IAP_TPS`（应为 11）与 `sizeof(param_t)`（应为 466） |
| `psv e%u i%u w%u d%u` | `param_save` 四格：擦除 rc / 擦完是否全 FF / 写入 rc / 读回差异字节数（cb/ca 已证明稳定，让位） |
| `pra2 %u %u %u` | A 槽首字节 / B 槽首字节 / 读回自身的 rc |

| 现象 | 结论 |
|---|---|
| `P1 r0 b65` 且 `P2 r0 b90` | 写通路彻底无罪 ⇒ 问题在 `param_save` 多做的两步（`is_erased` / `param_crc`），下一版只测它们 |
| `P1 r7 f1` | 硬件拒了 PROGRAM（CMD_FAIL）⇒ IAP 状态/时序，看 `e` 与 `tps` |
| `P1 r7 f0` | 硬件说成功、比对不过 ⇒ 只剩"读回来的数据不对" |
| `P1` 失败、`P2` 通过 | 变量是**源缓冲**（`g_p` vs 静态数组） |
| `P1` 通过、`P2` 失败 | 变量是**第几次写**（擦-写节奏 / IAP 没恢复） |

BL 码 13237 B、const 2482 B，末端 **`0xFF3E13`** ⇒ **16K 槽剩 492 B**。
A2（`ota_apply_staging`）估 450~550 B ⇒ **做 A2 之前必须把探针整段删掉腾地方**。

### 8.24 v0.21 上板：**P1/P2 双双通过，写通路彻底无罪；元凶是"形参本身"**

#### 8.24.1 上板读数（v0.21，55 行里 49 行校验和自洽，下面 4 行全在自洽里）

```
[204][BL] P1 e0 r0 f0 b80     源 = g_p（param_get），466 B 写 + 内部校验全过，首字节 0x80 落地
[257][BL] P2 e0 r0 f0 b90     源 = s_sw，同上，首字节 0x5A 落地
[450][BL] param empty/corrupt, defaults created
[486][BL] psv e0 i1 w7 d466   擦除 rc=0 / 擦净=1 / 写入 rc=7 / 读回 466 B 全不同
[508][BL] pra2 255 255 0      A 槽首字节 ff、B 槽 ff、读回自身 rc=0
```

推论链：

1. `P1` 用的是 `g_p`（`param_save` 的同一个源、同一地址、同一长度）⇒ **"源缓冲是 g_p 还是静态数组"这个变量被消掉**，不是它。
2. `P2` 也过 ⇒ **第几次擦-写也不是变量**（P1/P2 连续两次擦写都成）。
3. 同一次上电里 `param_save` 写同址同长却 `w=7` 且 **A 槽一个字节都没落地**（`pra2` 首字节 `ff`，而它刚在 `P2` 里被写成过 `5a`）⇒ 那次擦除生效了、写完全没生效。
4. `pra2` 第三格 `0` = `g_pdg_rc`，即"读回 466 B"这一步的返回码是 **OK** ⇒ 读通路没报错，`d=466` 是真实差异，不是读坏了的假象。

`b80` 而不是 `b65` 是另一件事：`param_get()` 依赖 `s_inited` 初值，而 **C251 不保证 `xdata` 静态变量上电清零**（v0.16 的 `boot_cnt=0xFF29EC00` 已证）。探针在 `param_init` 之前调 `param_get()`，`s_inited` 是残值 ⇒ 没走 `param_load_defaults()`，`g_p` 还是残值，所以首字节是 0x80。**这不影响结论**（P1 只用来证明"从 g_p 出发的 466 B 写得进"），但记一笔。

#### 8.24.2 map 定论：`?func??BYTE` 形参块的步进是 **2 字节**，而且被局部段直接压在下面

v0.22 的 BL map（`tools/_out_bl/Bootloader.map`）：

```
?flash_iap_write??BYTE    = 0x10F84      ← 形参块：addr@+0(4B)、len@+8(2B) ⇒ 实占 0x10F84..0x10F8D
?flash_iap_read??BYTE     = 0x10F86      ← 只隔 2 字节！
?flash_iap_verify??BYTE   = 0x10F88      ← 再隔 2 字节
?crc32_update??BYTE       = 0x10F8A

?XD?FLASH_IAP_SELFTEST??FLASH_IAP = 0x10F84..0x10F86   ← 正压在 write 块的头上
?XD?MAIN??BOOT_MAIN               = 0x10F87..0x10F89   ← 正压在 read/verify 块上
?XD?BSP_UART_WRITE??BSP_UART      = 0x10F8A..0x10F8B   ← 正压在 crc32_update 块上
```

L251 给 `?func??BYTE` 只按 **2 字节**步进（v0.19 时是 7 字节），而编译器实际要用 **10 字节**（`addr@+0`、`len@+8`）。于是两块必然重叠，而且**第三方函数的局部段还会被排在块中间**。

⇒ **`len` 存在 `block+8`，那个位置在链接器的账本里"不属于这个块"**，随时可能是别人的局部变量。返回值 `w=7` 而不是 `w=2`、数据没落地却能读回 OK，全部由此而来。

#### 8.24.3 修法（v0.22）：形参第一条语句就落地，此后一个字都不许再读形参

`BSP/flash_iap.c` 里所有"形参在被调函数返回之后还被读"的函数，全部改成：

```c
ota_ret_t flash_iap_write(u32 addr, const u8 *buf, u16 len)
{
    s_waddr = addr;     /* ← 第一条语句，之前不许调用任何函数 */
    s_wlen  = len;
    s_wbuf  = buf;      /* ← buf 也必须拷：原来它活在 DR12，跨 466 次 raw 调用 */
    ...
    s_rc = flash_iap_check_range(s_waddr, (u32)s_wlen, 1);
```

覆盖清单：`write` / `read` / `verify` / `check_range` / `erase_sector` / `erase_range` /
`read_byte` / `is_erased` / `crc32_cb`。其中 `check_range` 是**安全关键**：它在
`ota_part_of()`（会生成 12 字节块）返回后还要用 `addr`/`len`，一旦被压就变成"拿错地址判门禁"。

副产物（汇编实测，`tools/_asmA/flash_iap.lst`）：

```
;---- Variable 'buf'  assigned to Register 'DR28' ----
;---- Variable 'addr' assigned to Register 'DR4'  ----
```

`buf`/`addr` 从此**根本不进形参块**（只 `len` 进），块内只剩一条 u16 ⇒ 暴露面从 10 字节降到 2 字节。

#### 8.24.4 v0.22 交付与判据

`build/step22_param_abi_fix_BL_APP.hex`（81804 B）。`bl_iap_probe()` 已删除（P1/P2 的问题回答完了），
新增一行判据：

| 新日志行 | 含义 |
|---|---|
| `wdbg a=%u l=%u nf=%u f=%u` | 写函数内部真正拿到的 `addr` / `len` / 被拒掉的 PROGRAM 次数 / 粘滞 CMD_FAIL |

| 现象 | 结论 |
|---|---|
| `psv e0 i1 w0 d0` | **修法成立**，`param_save` 落地了 —— A1 可以往下走 |
| `wdbg a=49152 l=466 nf=0` 但仍 `w7` | 466 次 PROGRAM 全被接受却读不回 ⇒ 硬件/时序 |
| `wdbg a=49152 l=466 nf=466` | 每次都被门禁拒 ⇒ `s_eeprom_size` / `ota_part_of` 在窗口内变了 |
| `wdbg a=49152 l=0` | `len` 槽仍被踩，修法未生效 |
| `wdbg a=0` | `addr` 槽也被踩 |

BL code 13192 B、const 2447 B，末端 **`0xFF3DC3`** ⇒ **16K 槽剩 572 B**（比 v0.21 多 80 B）。

### 8.25 v0.22 上板：**读通路整段塌了**（`bl head: ff ff ff ff`），形参修复的结论被推翻

#### 8.25.1 上板读数（55 行，44 行校验和自洽）

```
[46][BL] flash_iap_init(128) -> 0
[58][IAP] eeprom=128KB
[72][IAP] crc32=cbf43926 (want cbf43926)
[85][IAP] bl head: ff ff ff ff (want 2 ...)          ← ★ 读 IAP 0x10000 得全 FF
[103][IAP] IAP READ path untrusted (bl head ff != 2), NO WRITE DONE
[141][BL] selftest FAIL rc=6
[274][BL] ps…                                        ← 被串口丢字节截断，不可用
[292][BL] pra2 255 255 0                             ← A/B 槽首字节 FF，读回 rc=0
[313][BL] wdbg a=192 l=0 nf=0 f=0                    ← ★ 不是 (49152, 466)
[335][BL] pending in tall -> verify staging ( ead-only)   ← 丢 2 个字符
[353][VER] STAGING fail rc=12 step=0
[564][BL] jump entry hi=fV lo=4000                   ← 'V' 不在十六进制字符表内
```

三条硬事实：

1. **`bl head` 是本版第一个 IAP 操作，它读的是 BL 自己正在执行的代码入口 0xFF0000
   （IAP 0x10000），复位后 3 字节必为 `02 00 27`。读到 `FF` 说明"读"没落到那个地址，
   或者命令根本没生效。** v0.21 的同一行是 `2 0 27 ff`。
2. `pra2 255 255 0`：第三格 `g_pdg_rc = 0` ⇒ `flash_iap_read` **返回成功**，
   只是 466 字节内容全是 FF。不是 `CMD_FAIL`，也不是门禁拒绝。
3. `wdbg a=192 l=0`：`a` 是 `g_iap_dbg[0]`（全 u32 打印），`l` 是 `(u16)g_iap_dbg[1]`。
   按 8.24 的构造，`param_save` 一定传 `(0xC000, 466)`。拿到 `(192, 0)` 只有两种可能：
   **写函数没被调用、`g_iap_dbg` 读到的是未初始化 xdata**；或 **`param_save` 提前 return
   了（擦除那步失败），压根没走到写**。

#### 8.25.2 定论：**这一版不能再靠读代码推断**

v0.22 只动了 `BSP/flash_iap.c` 的 static 布局（9 个函数"入口即拷"），
`set_addr` / `raw` 的汇编**一个字节都没变**。可读通路从"正常"变成"全 FF"。
这说明：**`?XD?<func>??<module>` 局部段的地址一变，就会换一个函数踩上参数块。**
"修好一处、坏掉一处"是这套 ABI 的固有行为，不是可以用心推理出来的。

所以 v0.23 不再改功能，只做一个**决定性实验**。

#### 8.25.3 v0.23 决定性实验：`bl_iap_alias()`

在 `bl_iap_check()` 之后、`param_init()` 之前执行：

```c
s_e  = flash_iap_erase_range(OTA_STAGE_BASE, 512);        /* 擦 STAGING 首扇区 */
s_a  = 0xA5;
s_w  = flash_iap_raw(IAP_CMD_PROGRAM, OTA_STAGE_BASE, &s_a);   /* 不擦写上层，只测原语 */
s_t0 = flash_iap_read_byte(0x00000);        /* 应为 165 */
s_a  = 0xEE;                                 /* 预置：没写回出参时读到 238 */
s_r1 = flash_iap_raw(IAP_CMD_READ, 0x10000, &s_a);
s_v1 = s_a;                                  /* 应为 2（BL 入口 LJMP 操作码） */
s_v2 = flash_iap_read_byte(0x14000);         /* 应为 117（APP 入口 0x75） */
```

| 日志 | 结论 | 下一步 |
|---|---|---|
| `alias e0 w0` / `t0=165 r1=0` / `v1=2 v2=117` | **读通路完全正常**，v0.22 的塌陷只是布局运气；0~64K 与 0x10000/0x14000 都读得对 | 直接看 `ps1/ps2`，回到 `param_save` 落地问题 |
| `v1=165`（等于 `t0`） | **IAP 的 bit16 地址位失效，0x10000 别名到 0x00000** | 查 STC-ISP「EEPROM=128K」选项 / `IAP_ADDRE`（0xF6）。OTA 架构的地址前提在这块板上不成立，与形参块无关 |
| `r1=0 v1=238` | `raw` 执行了但**没写回出参**（`p_dat` 被踩） | 形参/指针通路问题，走 8.26 的重构 |
| `r1=6` | 硬件 `CMD_FAIL` | 供电/时序 |
| `t0 != 165` | 连 0~64K 都写不进/读不出 ⇒ **写通路也是坏的** | 先查 EEPROM 选项与 `IAP_TPS` |
| `e != 0` | 擦除就被门禁拒了 | 看 `ota_part_writable(PART_STAGE)` |

`alias` 会擦掉 STAGING 首扇区（A1 镜像头）。该镜像在 v0.21/v0.22 都已被判 `rc=12`，
擦了不丢信息；APP 侧重建镜像时会自己擦整个 STAGING。

#### 8.25.4 v0.23 交付

`build/step23_iap_alias_BL_APP.hex`（82776 B）。BL code 13527 B、const 2466 B，
末端 **`0xFF3F25`** ⇒ **16K 槽只剩 218 B** —— 这是最后一次放探针，
**A2（`ota_apply_staging`）之前必须把 `bl_iap_alias()` + `bl_param_probe()` 整段删掉**。

顺带把 `psv` 拆成 `ps1 e%u i%u` / `ps2 w%u d%u` 两行：v0.22 那行 4 个 `%u`
被串口截成 `ps`，拆短能降低单行被截断的概率，也便于逐行用和值判定。

## 8.26 v0.24：探针把自己要验的东西挡住了（A1 前的收尾）

### 8.26.1 v0.23 日志读数（51 行中 49 行自洽，判据行全部自洽）

```
alias e0 w0            擦除 + 单字节编程都成功
alias t0=165 r1=0      写 0xA5 到 STAGING[0]，读回 165 ⇒ 0~64K 读写都通
alias v1=2   v2=117    raw 直读 0x10000 = 2（BL 入口 LJMP
                       read_byte(0x14000) = 0x75（APP 入口首字节）
ps1 e6 i1              擦除 rc=6，但擦净标志=1
ps2 w0 d0              写 rc=0，466 B 读回零差异
wdbg a=49152 l=466 nf=0 f=0   写函数拿到的正是 (0xC000, 466)
pra2 255 255 0         读 A/B 槽首字节都是 FF
[VER] STAGING fail rc=12 step=2
```

**结论一（最重要）：IAP 读通路 100% 正常，bit16 完全生效。**
`v1=2` 读的是 BL 自己此刻正在执行的入口（`02 00 27`），`v2=117` 是 APP 入口
`75 84 01 7e` 的首字节。0x10000 没有别名到 0x0000 —— §8.25 那张判读表里的
"(A) bit16 失效"整条路关闭，"查 EEPROM 选项 / 换头文件"不必再试。

**结论二：`bl head: ff ff ff ff` 是假读数，不是读通路故障。**
汇编实测（`flash_iap.lst` §761）：

```
MOV R15,R11   ; b0
MOV R12,R11   ; b1
MOV R13,R11   ; b2
              ; b3 直接留在 R11(=A)
```

编译器把 4 个结果全放通用寄存器，而它们要跨 4 次 `flash_iap_read_byte()`
活着；那条链 `read_byte → check_range → ota_part_of/ota_part_contains → raw`
只保存 R13 与 WR14(R14:R15)，**R12 / R11 不保存**。于是 b0 已经不是 0x02
⇒ 走进 `b0 != 0x02` 分支 ⇒ 返回 6 ⇒ **后面两处探针全被跳过**（`stg/app` 永远 untested）。

同一上电稍后的 `bl_iap_alias()`（变量全 static）读同一条地址得到正确的 2 / 0x75
—— 这是"读通路无罪"的直接反证。**铁律 5/9 的同一个道理：跨调用还活着的值，
一律不许留在寄存器/栈上。**

**结论三：`step=2` 是探针自己造成的。**
`step=2` = 魔数不对，而 `step=1`（读头）已经通过 ⇒ BL 读 STAGING 的链路是通的。
魔数之所以不对，正是因为 `alias` 往 `STAGING[0]` 写了 `0xA5`。
**探针把要验的东西（A1 镜像头）破坏了** —— 这就是它必须删掉的原因。

**结论四：`ps1 e6` 是真 bug，而且会阻断 `param_save`。**
`flash_iap_erase_sector` 的 4 点回读写法是

```c
dat = 0x00;
flash_iap_raw(IAP_CMD_READ, ..., &dat);   /* 返回码被丢掉 */
if (dat != 0xFF) return OTA_ERR_IO;       /* 读失败 ⇒ dat 仍是 0x00 ⇒ 误报 */
```

只要那一下**读**失败，就必然得到"dat=0x00≠0xFF"。而 `param_save` 里
`if (s_rc != OTA_OK) return s_rc;` 会因为假 6 **直接跳过整个写动作**
⇒ 参数永远存不下来。这是错误码说谎的第三处，但它是唯一会**阻断功能**的一处。

### 8.26.2 v0.24 改动（三条，全部是"不再自欺"）

| 改动 | 文件 | 为什么 |
|---|---|---|
| 回读失败不再判成"擦不干净"：`dat` 初值改 `0xFF`，`raw != OK` 就 `continue`；只有**读成功且不是 FF** 才报 `OTA_ERR_IO` | `BSP/flash_iap.c` `flash_iap_erase_sector` | 假 6 会阻断 `param_save` 的写 |
| `bl_first` / `b0..b3` / `rc` 全部改 `static` | `BSP/flash_iap.c` `flash_iap_selftest` | 让 `bl head` 与 `selftest` 可信；它们现在落在 `?XD?FLASH_IAP`（**INSEG**），参数块压不到 |
| 删 `bl_iap_alias()`；`pra2` 的两次 `read_byte` 先落 `static` 再打 | `Project/Bootloader/boot_main.c` | 探针会污染 STAGING；`LOG_I3` 参数表里直接调函数会被求值踩踏（v0.23 的 `pra2 255 255 0` 与同一上电的 `ps2 w0 d0` 互相矛盾，只能是这个原因） |

验证修法生效：`?XD?FLASH_IAP` 从 `0x312` 涨到 `0x319`（+7 = 5×u8 + ota_ret_t），
`?XD?FLASH_IAP_SELFTEST` 段消失 —— static 局部变量归**模块段**（INSEG），
不归函数局部段（UNIT 可覆盖）。这就是"压不到"的结构性保证。

### 8.26.3 v0.24 判读表

`build/step24_selftest_static_BL_APP.hex`（82092 B）。BL code 13282 / const 2400，
末端 **`0xFF3DEE`** ⇒ **16K 槽剩 529 B**（删探针腾出 311 B）。

另外把 A1 的失败重试上限从 **3 提到 20**（`boot_try < 20U`）。原因：以前 `param_save`
存不下来，`boot_try` 永远是 0，"3 次放弃"从未触发过；v0.24 起它会真的累加，
3 次上限立刻变成"只能试 3 次"，而 A1 的价值恰恰是能重复观察十几次。

| 日志 | 结论 |
|---|---|
| `bl head: 2 0 27 ff` | selftest 修好，读通路可信；`selftest = 0` |
| `ps1 e0 i1` | 擦除不再误报（`e` 必须 e0；若仍 e6 说明读真的失败） |
| `ps2 w0 d0` | 写 + 整块回读校验通过 |
| **`pra2 65 255 0`** | **`param_save` 真的落地了**（A 槽 magic 低字节 `'A'`=0x41=65） |
| `pra2 255 255 0` | 仍没落地 ⇒ 回到 8.24 的形参块路线，但这次读数可信 |
| **断电重上电后 `[VER] STAGING ok len=49088 crc=...`** | **A1 达成** |

★ 抓日志顺序（非常重要）：
1. 烧 v0.24（ISP 会全片擦除）
2. **第一段**：刷机后第一次上电 —— 看 `selftest` / `bl head` / `ps1` / `ps2` / `pra2`，
   以及 APP 侧的 `A1 image ready`。这一段 `[VER] STAGING fail rc=12 step=0` 是必然（STAGING 空）。
3. **断电，重新上电，抓第二段** —— 这一段才是 A1 的判据（`[VER] STAGING ok`）。
   **在抓完之前不要再烧任何 hex**（烧录会擦掉 STAGING）。

### 8.27 v0.24 上板：**A1 的卡点收敛到一处 —— 镜像头 CRC（`step=5`）**

两段日志（刷机后 / 断电重上电），各 46 行自洽，坏行 5 条（`seq=` 截断、`pending ins all`、
`A1 retry 3795462748`、`j mp entry`、`wd g`）—— 一律不看。

**先看好的那一半（v0.24 的三条修复全部生效）：**

| 日志 | 含义 |
|---|---|
| `bl head: 2 0 27 ff` + `3addr-reg ok` | selftest 的 5 个字节改 static 生效，读通路可信 |
| `selftest=11` | 11 = "STAGING untested（探针关闭）"，不是失败。**CRC 自检通过**（失败会是 6） |
| `ps1 e0 i1` | `erase_sector` 回读误报的假 6 修好了，擦除干净 |
| 第二段 **BL 没有** `param empty/corrupt`，APP 打 `param_init OK` | **A 槽里躺着有效 param** |

**⇒ 结论一：`param_save` 其实一直都是成功的。**
`ps2 w7 d0`（写报 CRC 错、差异 0 字节）和 `pra2 80 0 0` / `pra2 80 255 0`（A 槽首字节 128，
B 槽一会儿 0 一会儿 255）**全是假读数**：下一段上电 `param_init` 认了 A 槽，说明首字节确实是
`0x41`。两个值自相矛盾（B 槽 0 / 255）只有一种解释——坏的是 `flash_iap_read_byte`，
不是 Flash。**这也就是"诊断说 A、功能说 B"必须靠"功能侧能不能用"来裁决的原因。**

**⇒ 结论二（唯一的卡点）：`[VER] STAGING fail rc=7 step=5`。**
`step` 定义在 `ota_verify.c`：1 读头 / 2 魔数 / 3 hdr_len / 4 hdr_ver / **5 头 CRC** / 6 长度 /
7 机型 / 8 entry / 9 正文 CRC。走到 **5** 意味着：头读出来了、魔数对、hdr_len=64 对、
hdr_ver=1 对 —— **镜像头真的在 STAGING 里，而且前 8 个字节完全正确**。
（对比 v0.21 那次是 `step=0` = STAGING 空，现在这一步的进展是实的。）

而 `crc32=9f4db109` / `crc32=84d14afa` 两次上电给出两个不同值（应为 `cbf43926`）
⇒ **CRC32 的计算在某个工程里不稳**，而头 CRC 和正文 CRC 都建在 `crc32_update` 上。

**本地算出基准值**（Python，按 C251 大端，magic/hdr_len/hdr_ver/ver=0.1.0.1/img_len=49088/
img_crc32=b723d64c/entry=FF4000/build_time=0/target="STC32G12"，前 36 字节）：

```
hdr_crc32 应为 aabc4c0e
```

### 8.27.1 v0.25 三处改动

1. **`crc32_update` 的形参 → 模块级 static**（铁律 9 的最后一处）。
   它是全工程唯一"形参留在参数块里、且在循环里反复回读"的函数。BL v0.24 汇编实测：
   `?crc32_update??BYTE = 0x10F92`，`len` 的家 = `0x10F9A` = `?XD?CRC32_UPDATE`；
   而这块 8 字节正好覆盖 `?XD?BSP_UART_WRITE` / `?XD?FLASH_IAP_WRITE` /
   `?XD?FLASH_IAP_READ` / `?XD?FLASH_IAP_VERIFY` 四个别的函数的局部段。
   `crc`/`src` 走寄存器所以暂时没炸，但 `len` 一旦在循环里被写掉，
   表现就是**同一个自检向量两次上电两个值**（正是本版看到的）。
   修法后 `?XD?CRC32` 从 9 B 涨到 21 B（+12 = 4 份副本），循环里只读 `s_ulen`/`s_ui`。
2. **`flash_iap_read_byte` 改走 `flash_iap_read`**（不再直接调裸 `raw`）。
   让诊断读数和所有功能通路走同一条路，否则"pra2 说 128、param_init 说有效"会继续耗轮次。
3. **`step=5` 失败时把两个值都念出来**：`hdr crc now=%lx want=%lx`。
   一次分清三种情况，不再靠推理：
   | 日志 | 结论 |
   |---|---|
   | `now=aabc4c0e` 而 `want` 不是 | **APP 侧 crc32 错**（写进头的 CRC 是错的） |
   | `want=aabc4c0e` 而 `now` 不是 | **BL 侧 crc32 错** |
   | 两个都不等于 `aabc4c0e` | 头里的字段本身被写歪了（回去查 APP 的 `s_a1img` 赋值） |
   另外 `crc32=%x` 改 `%lx`，并且**只算一次**（原来 LOG 调一次、`if` 又调一次，
   等于给"日志说谎"留口子）。

### 8.27.2 v0.25 交付与判读

`build/step25_crc32_fix_BL_APP.hex`（83146 B）。BL code 13493 / const 2432，
末端 **`0xFF3EE1`** ⇒ **16K 槽只剩 286 B**。入口 `02 00 27` / `75 84 01 7e` / `75 84 01 7e`
已复验，布局守门 + overlay 守门全过（参数块 13 个各自独占），L251 0 WARNING。

| 日志 | 结论 |
|---|---|
| `crc32=cbf43926` | CRC 核心修好（这是第一个要看的数） |
| `hdr crc now=aabc4c0e want=aabc4c0e` 但不该再有这一行 | 头 CRC 通过，流程继续往后走 |
| `[VER] STAGING ok len=49088 crc=...` | **A1 达成** |
| `[VER] ... rc=7 step=9` | 头有了、正文 CRC 不对 ⇒ `crc32_update` + 正文搬运的一致性 |
| `pra2 65 255 0` | `read_byte` 修好，与"param 有效"不再矛盾 |
| `ps2 w0 d0` | `flash_iap_verify` 不再假报 7 |

★ **286 B 已经不够 A2 了**（`ota_apply_staging` 估 450~550 B）。A2 之前必须删掉
`bl_param_probe()`（`ps1/ps2/pra2/wdbg` 四行）与 `g_iap_dbg` 相关写法。

★ 抓日志顺序不变：烧 → **第一段**（刷机后首次上电，看 `crc32=` / `hdr crc` / `pra2`）
→ **断电重上电 → 第二段**（A1 判据）。第二段抓完之前不要烧任何 hex。

## 8.28 v0.26：**真凶找到了 —— XDATA 被链到 16 位之外，同一变量有两个家**

### 8.28.1 v0.25 上板读数（两段，关键行自洽）

```
crc32=84d14afa (want cbf43926)   crc32 selftest FAIL     ← 两段各一个值，互不相同
crc32=c6f39ea9 (want cbf43926)   crc32 selftest FAIL
ps1 e0 i1   ps2 w7 d0            pra2 80 255 0
hdr crc now=6ab6b2d5 want=6607673e    [VER] STAGING fail rc=7 step=5
APP: A1 image ready len=49088 crc=46031df2（两段一致）
```

这段日志**第一段还算规矩、第二段全是乱数**（`ps1 e57622 i192`、`wdbg a=756077772 l=29758`），
正是"同一个变量有两个物理位置"的典型表现：读到哪一份取决于之前跑过什么。

### 8.28.2 离线核算：APP 侧完全正确，BL 侧算错

用交付的 hex 直接算，不依赖任何日志：

| 量 | 独立复算（Python binascii.crc32 / 大端打包） | 结论 |
|---|---|---|
| APP 区 49088 B 的正文 CRC | `46031df2` ← 与日志 `crc=46031df2` 一致 | **APP 的 crc32 通路完全正确** |
| 镜像头前 36 B 的 CRC | `6607673e` ← 与日志 `want=6607673e` 一致 | **STAGING 里的镜像是好的** |
| 同一段字节用 BL 的 crc32 算 | BL 打 `now=6ab6b2d5` | **只有 BL 侧算错** |

⇒ 这不是"镜像写坏了"，也不是"算法错"，而是 **BL 里 `crc32_update` 读到的数据和它以为的不是同一份**。

### 8.28.3 根因：XDATA 类没有写在 CLASSES 里，L251 默认把它放到 0x010000

`tools/_out_bl/Bootloader.map` 的类表（v0.25）：

```
010000H  010000H  01FFFFH  000FB8H  XDATA      ← 起点 0x010000，已经超出 16 位
```

而 C251 对 xdata 有**两种**访问指令，寻址能力不同：

| 指令 | 位宽 | 地址怎么来 |
|---|---|---|
| `MOVX @DPTR`（`F0` 等） | **16 位** | 只有 DPTR，页字节不参与 |
| `MOV @DRk` / 通过 far 指针 | 24 位 | 完整 24 位地址 |

同一个数组，如果一处按"绝对符号"写、一处按"指针"读，编译器会分别生成这两条指令：

```
crc32_selftest_value:  MOV DPTR,#0F46 ; MOVX @DPTR,A     → 实际写到 0x0F46
crc32_calc:            MOV WR2,#WORD0 s_v / WORD2 s_v    → 指针是 0x010F46
                       crc32_update 里 MOV R5,@DR0       → 实际读 0x010F46
```

**写在 0x0F46，读在 0x010F46** ⇒ 读到的是从没写过的 RAM ⇒
每次上电的值都不同（`9f4db109` / `84d14afa` / `c6f39ea9`）—— 一个纯函数不可能这样，
这就是"结果依赖内存布局"的铁证。同一个机制顺带解释了：

- `param_save` 通过指针把 `g_p` 写进 Flash，而别处对 `g_p` 的更新落在另一个家
  ⇒ 写进去的是**旧内容**，`flash_iap_verify` 拿它比对必然不等 ⇒ 恒 `w7`；
- `pra2 80 255 0` 里的 `80` 其实是 `0x50`＝`'P'`（`PARAM_MAGIC` 大端首字节），
  **param 一直是有效的**，坏的是读数；
- 每改一次布局就换一个函数受害 —— 因为"哪些访问被编译成 16 位"取决于局部代码形状。

### 8.28.4 修法（一处配置，零代码开销）

`CLASSES` 里显式写出 XDATA，并让起点避开 `?STACK`（0x0008-0x0107）与 reg bank：

```
XDATA(0x1000-0x2EFF)
```

三处必须同时改，缺一处就会"IDE 编出来的和脚本编出来的不是同一个固件"：

1. `tools/build_local.sh` 的 APP 行与 BL 行
2. `Project/APP/STC32G_OTA.uvproj` → L251 → Misc controls
3. `Project/Bootloader/Bootloader.uvproj` → L251 → Misc controls

修后 map：

```
000008H  000107H  ?STACK          256 B
000108H  000FFFH  GAP             栈溢出还有 3.8 KB 余量
001000H  001596H  ?XD?PARAM       g_p @ 0x1015
001F3AH  001F4EH  ?XD?CRC32
```

指令侧复核（从 hex 直接读）：

```
FF3039: 90 1F 46 F0    MOV DPTR,#1F46 ; MOVX @DPTR,A   ← s_v[0]
```

`0x1F46` 正是 `s_v` 的符号地址 ⇒ 两条指令从此指向同一个字节。

**佐证**：改动前后，两工程入口第 3 个字节从 `01` 变成 `00`
（`75 84 01 7E` → `75 84 00 7E`）—— 那是启动代码在写 xdata 页寄存器，
编译器原来认为 xdata 在第 1 页，现在认为是第 0 页。**这条变化本身就是"原来真的在第 1 页"的旁证。**

### 8.28.5 新增构建守门（第 6 项，`check_layout.py`）

```
FAIL <工程> 的 XDATA 类起址 = 0x010000（>= 0x10000），超出 16 位可寻址范围
  -> 同一个变量会被两条指令访问到两个不同地址
```

以后任何人删掉 `XDATA(...)`、或改 CLASSES，构建会直接 exit 6 拦下来，
不会再用八轮上板去换这个结论。

### 8.28.6 v0.26 交付

`build/step26_xdata16_fix_BL_APP.hex`（83146 B）。BL code 13493 / const 2432，
末端 `0xFF3EE1` ⇒ **16K 槽剩 286 B**（与 v0.25 完全相同：这次修复零代码开销）。
入口 `02 00 27` / `75 84 00 7E` / `75 84 00 7E`，三道守门全过，L251 0 WARNING。

| 日志 | 结论 |
|---|---|
| `crc32=cbf43926 (want cbf43926)` | **布局修好了**（第一个要看的数，且不再有 `selftest FAIL`） |
| 不再出现 `hdr crc now/want` 那行 | 头 CRC 通过，流程继续往后走 |
| `[VER] STAGING ok len=49088 crc=46031df2` | **A1 达成** |
| `ps2 w0 d0` / `pra2 80 255 0` | `flash_iap_verify` 不再假报 7（`80`=`'P'` 本来就对） |
| 仍是 `crc32` 随机 | 说明还有第三处地址来源，回去查 `?BYTE` 参数块 |

## 8.29 v0.27：`crc32_calc` 这层"薄封装"是最后一个坏点 —— 直接取消它

### 8.29.1 v0.26 上板读数（四段，全部行尾和值自洽）

先把 90 行日志逐行核了 `*HH`（`tools/check_log_sum.py` 的同一套算法）。
**只有 6 行和值不符，且全部是用户转贴时掉了字符的行**（`efaults`/`boo_try`/
`verfy`/`wll`/`hi=T00000 f`/`1 20`）—— 即"传输丢字"，不是固件问题。
其余每一行（含所有 `crc32=`、`wdbg`、`[VER]`、APP 段）都自洽，可以当证据用。

| 事件 | 读数 | 说明 |
|---|---|---|
| 第一次上电 BL | `crc32=8270e960` + `selftest FAIL rc=6` | **仍然错**，XDATA 搬到 0x1000 并没有救回来 |
| 第一次上电 BL | `wdbg a=49152 l=466 nf=0` | 写函数拿到的形参完全正确（49152=0xC000, 466） |
| 第一次上电 APP | `A1 image ready len=49088 crc=e546ad98` | 与离线 `binascii.crc32` 复算 **一模一样** |
| 第二次上电 BL | `crc32=395bd4b4` | 同一个固定向量，换一次上电换一个值 |
| 第二次上电 BL | `[VER] hdr crc now=da5e7f41 want=5766d1bf` | 两侧都不等于离线期望 `bf66f5af` |

顺带把两个"看着像故障"的读数结清了：

* **`ps1/ps2/pra2/wdbg` 那 4 行在第二次上电变成垃圾**（`e11302` / `w1682` /
  `a=1386217503`）—— 不是新故障。这 4 个数来自 `param_save()` 与
  `flash_iap_write()` **内部才会写**的 `g_pdg[]`/`g_iap_dbg[]`；第二次上电
  param 有效 ⇒ 根本没调 `param_save`/`flash_iap_write` ⇒ 读到的是**未初始化的
  xdata**（C251 启动代码不清 xdata，铁律 5）。这行探针从此只会误导，已关掉。

### 8.29.2 离线定位：把"每一步都能自证"的路径和"算错"的路径摆在一起

| 调用点 | 写法 | 结果 |
|---|---|---|
| `app_main.c` 正文 CRC（49088 B） | `begin()` → 循环 `update()` → `final()`，**分语句** | ✅ 与 Python 逐位吻合 |
| `flash_iap_crc32_cb` / `ota_verify` 正文 CRC | 同上，分语句 | ✅（同一条路） |
| `crc32_selftest_value`（9 B） | `crc32_calc()` | ❌ 每次上电不同 |
| `param_crc`（462 B） | `crc32_calc()` | ❌ ⇒ `param_save` 回读校验永远不等 ⇒ `rc=6` |
| `app_main.c` 镜像头 CRC（36 B） | `crc32_calc()` | ❌ ⇒ 写进头的 `hdr_crc32` 是错的 |
| `ota_verify.c` step=5 头 CRC（36 B） | `crc32_calc()` | ❌ ⇒ `now != want`，两侧都对不上离线值 |

**唯一正确的写法与唯一出错的写法，区别只有一个**：出错的都走
`crc32_calc()` 这层封装，而它是全工程唯一把三步塞进**一条 return** 的地方：

```c
return crc32_final(crc32_update(crc32_begin(), src, len));   /* ← 已删除 */
```

用 `C251 ... CODE` 重新编译出汇编逐指令核对（`tools/_asmZ/crc32.lst`，
0xFF2FB2 处的机器码与源码逐字节对上，`CA3B / 90 1FB0 / 1BEA30 / 7F30 / ...`），
单看指令看不出破绽 —— 所以本轮**不再继续猜 ABI**，而是直接把这个函数从
工程里删掉，让所有调用点都写成已经验证正确的那种形态。

### 8.29.3 修法：宏代替函数（`OTA_CRC32_OF`）

`COMMON/crc32.h`：删除 `u32 crc32_calc(const u8*, u16)`，换成

```c
#define OTA_CRC32_OF(dst, src, len)                      \
    do {                                                 \
        (dst) = crc32_begin();                           \
        (dst) = crc32_update((dst), (src), (u16)(len));   \
        (dst) = crc32_final(dst);                         \
    } while (0)
```

**约束：`dst` 必须是模块级 `static u32 xdata`**（不许栈上局部量）。四个调用点：

| 文件 | 变量 |
|---|---|
| `BSP/flash_iap.c` 自检 | `s_stval`（static） |
| `COMMON/param.c` `param_crc` | `s_pcrc`（新增 static），形参先落 `s_cp`（铁律 9） |
| `OTA/ota_verify.c` step=5 | `s_crc_hdr`（本来就是 static） |
| `APP/app_main.c` 头 CRC | `s_a1hcrc`（新增 static） |

汇编复核：新代码在两次 `ECALL` 之间**把中间值落到 xdata**（与 APP 正文 CRC
那条已验证的路径完全同形），不再依赖"返回值在 DR4 里跨调用活着"。

### 8.29.4 本轮自证探针：一行两个 32 位量，输入字节完全相同

```
[IAP] crc32=<xdata 向量> cst=<HCONST 向量>
```

* `crc32=` 的向量是 `static u8 xdata s_v[9]`，由 `MOVX @DPTR` 写进去（老形态）
* `cst=`  的向量是 `static const u8 s_vc[9]`，住 HCONST，和查表同一个空间

判读：

| 读数 | 结论 |
|---|---|
| 两个都 `cbf43926` | **修好了**，往下只看 `[VER]` |
| `cst` 对、`crc32` 不对 | 算法无罪，坏在"xdata 写进去 / 按指针读出来"这条通路 ⇒ 下一步改用远指针写入 |
| 两个都不对 | 算法/调用通路还有别的问题 |

### 8.29.5 v0.27 交付与离线预言值

`build/step27_crc_macro_BL_APP.hex`（82818 B，两次构建 md5 一致 `53b97ca6…`）

* BL `code=13298` / `const=2349`，末端 `0xFF3DCB` ⇒ **16K 槽剩 564 B**（原 286 B）
  —— 关掉 `bl_param_probe()` 腾出来的空间，够 A2（估 450~550 B）用了
* APP `code=12404` / `xdata=3832` / `const=2043`
* 入口 `02 00 27` / `75 84 00 7E`，三道守门全过，L251 0 WARNING
* **离线预言值**（`binascii.crc32` 复算本版 APP hex 的 0xFF4000 起 49088 B）：
  * 正文 CRC = **`aa5229d6`** ⇒ 应同时出现在 APP 的 `A1 image ready crc=` 和
    BL 的 `[VER] STAGING ok ... crc=`；**两处相等且等于它 = A1 达成**
  * 镜像头 36 B CRC = `5ed01653` ⇒ 只在 step=5 失败时才打印，成功时不出现

### 8.30 v0.27 上板：**step=5 首次通过，失败点推进到 step=9**

#### 8.30.1 读数（36 行全部行尾和值自洽 —— 这次转贴没掉字，全部可当证据）

| 行 | 值 | 离线期望 | 判读 |
|---|---|---|---|
| `[IAP] crc32=aa343e9a c1=…`（v0.27 时任是 `cst=a63fda69`） | 两值都不对且互不相等 | `cbf43926` | 9 字节自检仍错 |
| `[VER] STAGING fail rc=7 step=9` | — | — | **step=5/6/7/8 全过**，卡在正文 CRC |
| `[APP] A1 image ready len=49088 crc=aa5229d6` | 与 Python 复算本版 hex **逐位一致** | `aa5229d6` | **APP 侧 CRC 通路已证明正确** |
| `[APP] A1 stg crc=` | v0.28 新增 | — | 回读 STAGING，判"写侧 vs 读侧" |
| `[BL] param empty/corrupt`（第一段） | 必然 | — | 下载擦整片 |
| `[APP] param_init OK`（第二段） | param 有效 | — | 466 B 长写 + 校验确实能成功 |

关键转折：**step 从 0/5 推进到 9**，说明头 CRC 与头里所有字段（含 `img_crc32`）
至少是**两侧自洽**的；`step=9` 是"BL 的 49088 B 正文 CRC ≠ 镜像头里的
`img_crc32`"。

#### 8.30.2 v0.27 那种"两个值都不对"为什么不能继续猜 ABI

这一轮把**静态层面能查的全查完了**，全部正确，所以继续读代码不会有新信息：

| 查什么 | 手段 | 结论 |
|---|---|---|
| `crc32_update` 的代码生成 | `C251 … CODE` 取汇编 | 正确（含 `ADD WR2,WR6`：C251 **大端**，`WR2` 才是低 16 位） |
| BL / APP 两个工程是否编出同一段 | 用两套 `DEFINE` 各编一次 `crc32.c` 后 diff | **逐字节相同**，只差时间戳 |
| 查表内容 | 从 hex 直接转储 `?HC?CRC32` | `FF3D6D` 起 16 项表，大端，完全正确 |
| 两个向量的地址立即数 | 从 hex 读 `MOV WRk,#imm16` | `s_v=0x1F52 / WORD2=0x0000`、`s_vc=0xFF3D64 / WORD2=0x00FF`，均正确 |
| `crc32_update` 里 5 个槽位 | 从 hex 读 5 条 `MOV DPTR,#imm16` | `s_ucrc=1F46 / s_ulen=1F4A / s_usrc=1F4C / s_ui=1F50 / len=1FBE` 全对 |
| 结果变量落点 | map 符号表 | `s_stval=0x1CCC`、`s_stval_c=0x1CD0`，都在 `?XD?FLASH_IAP` 内，不与他人重叠 |
| 日志参数装配 | `flash_iap_selftest` 汇编 | 正确（`g_log_av[0]=s_stval`，`g_log_av[1]=` 上一调用留在 `DR4` 的返回值） |

⇒ **结论：不能再靠"读代码"推进，必须改用能一刀分割故障面的探针。**

#### 8.30.3 v0.28 的三个探针（一个数定一个岔路口）

1. `[IAP] crc32=%lx c1=%lx`
   * `c1` = **1 字节**向量 `"A"`，期望 `d3d99e8b`。`len==1` 时循环只跑 `i=0`，
     **完全不经过 `p[i]` 的地址计算**。
   * `c1` 对 / `crc32` 错 ⇒ 坏在"按索引取第 i 个字节"，算法与查表无罪。
   * `c1` 也错 ⇒ 连一个字节都算错，坏在查表/初值/取反。
2. `[IAP] cst=%lx` 单独一行（HCONST 向量，走代码空间取数）。
   * 若 `c1`/`crc32` 都对了而 `cst` 仍错 ⇒ 这是**代码空间通用指针取数**的
     平台限制，与 CRC 无关，别再拿它当金丝雀（铁律 7 的同一片雷区）。
3. `[VER] body crc now=%lx want=%lx`（step=9 失败时）
   * `now` 与 APP 报的正文 CRC 相等 ⇒ BL 的算法与读通路都对，**坏在 STAGING
     里的内容（写侧）**；
   * `now` 完全不像 ⇒ BL 侧算/读错，别再怀疑写侧。
4. `[APP] A1 stg crc=%lx`：APP 写完 49088 B 后**回读 STAGING** 再算一次。
   * 与本行上面的 `A1 image ready crc=` 相等 ⇒ 写进去的就是源数据 ⇒ 问题在 BL；
   * 不等 ⇒ **写侧**就错了（未擦净时位只能 1→0，正是"写返回 OK 但内容不对"）。
   * 代价：多读一遍 49088 B，日志晚约 5 s。

#### 8.30.4 v0.28 交付与离线预言值

`build/step28_probe_1byte_BL_APP.hex`（85072 B，两次构建 md5 一致 `6c0ad04a…`）

* BL `code=13523` / `const=2394`，末端 `0xFF3ED9` ⇒ **16K 槽只剩 294 B**
  —— 探针是临时的，验证完必须摘掉才能做 A2（A2 需 450~550 B）
* APP `code=12889` / `xdata=4049` / `const=2394`
* 入口 `02 00 27` / `75 84 00 7E`，三道守门全过，L251 0 WARNING
* **离线预言值**（本版 APP hex 0xFF4000 起 49088 B）：
  * 正文 CRC = **`95e314a4`** ⇒ 应同时出现在 `A1 image ready crc=` 与
    `A1 stg crc=`；**两处相等且等于它 = 写侧无罪**
  * 镜像头 36 B CRC = `949dcadc`（只在 step=5 失败时打印）
  * `crc32("123456789") = cbf43926`、`crc32("A") = d3d99e8b`

### 8.31 v0.29：v0.28 上板判读 —— CRC 引擎彻底结案，故障面收敛到"地址门禁"

**v0.28 三探针的读数（两次上电完全一致，36 行日志行尾 `*HH` 全部自洽）**

| 读数 | 含义 |
|---|---|
| `crc32=cbf41f48 c1=d3d99e8b` | `c1`（1 字节 xdata 向量）**正确** = `d3d99e8b`；`crc32`（9 字节 xdata 向量）错 |
| `cst=cbf43926` | HCONST 9 字节向量 **正确** |
| `A1 stg crc=95e314a4` + `A1 image ready len=49088 crc=95e314a4` | 与 Python 复算本版 hex **逐位一致** ⇒ **STAGING 内容正确，写侧无罪** |
| `[VER] STAGING fail rc=2 step=9` | 失败点从 step=5 推进到 step=9，**错误码从 7（CRC）变成 2（地址）** |

**关键推理：CRC 引擎无罪，别再怀疑它**

`cst` 对 ⇒ 查表 / 初值 / 最终取反 / 9 字节循环 / 3 字节通用指针 全部正确。
同期 4 条**真正判生死**的通路，值都与 `binascii.crc32` 逐位一致：

| 通路 | 长度 | 数据位置 | 结果 |
|---|---|---|---|
| APP 造镜像正文 | 49088 B | 128 B xdata 缓冲 | `95e314a4` ✓ |
| APP 回读 STAGING | 49088 B | 128 B xdata 缓冲 | `95e314a4` ✓ |
| `param_crc` | 462 B | `&s_p`（xdata） | `param_init OK` ✓ |
| 镜像头 | 36 B | `&s_hdr`（xdata） | step=5 通过 ✓ |

⇒ **"xdata + 索引取数"这条路是好的。** 唯一算错的只有那个 9 字节 xdata
自检向量本身，而它**不在任何功能路径上**（正文 / 参数 / 镜像头都不经过它）。

**v0.29 的处理**：删掉 `crc32_selftest_value()` 与 `crc32_selftest_value_1()`，
只保留一直准确的 `crc32_selftest_value_cst()`。日志从三行变一行
`[IAP] crc32=<值>`，正确值仍是 `cbf43926`。

**顺带根除一句自相矛盾的日志**

v0.28 同一屏出现了：
```
[BL] == summary: init=0 selftest=6 stg=untested app=untested ==
[BL] -> STAGING NOT writable: IAP cannot reach FE block
```
这两行在 `bl_summary()` 里是**互斥**的（`rs == UNTESTED` 会提前 return）。
能同时出现只有一种可能：`rs` 是 `0xFF`。链条是：

1. crc32 自检 FAIL → `flash_iap_selftest()` 提前 `return OTA_ERR_IO`；
2. 而 `s_r_stg = UNTESTED` 写在那之后，没被执行；
3. `static u8 s_r_stg = FLASH_PROBE_UNTESTED;`（=0）的 **xdata 初值在 BL 里没生效**
   —— C251 启动代码不搬运 `?XD?` 初值数据，xdata 上电是 `0xFF`；
4. `bl_pstr()` 对 `0xFF` 走兜底打 `untested`，而判据是 `rs == 0`，
   于是继续走到 `rs != WRITABLE` 分支，打出 `cannot reach FE block`。

**新铁律**：`static xdata x = 常量;` 的初值**不可信任**，一律在函数里显式赋值；
判据比较只能用显式赋值后的值。v0.29 已把两个探针的 `UNTESTED` 赋值提到
自检函数最前面。

⚠️ 由此带来的**日志变化**（不是新故障）：
- `[IAP] cst=...` 那一行消失（合并进 `crc32=`）；
- `crc32 selftest FAIL` 消失；
- summary 里 `selftest=6` 会变成 **`selftest=11`**（`OTA_ERR_SUPPORT`，
  意思是"STAGING 未实测"，因为 `BL_STG_PROBE=0`），同时会打
  `selftest: STAGING untested (probe off), not a fault` —— **这是预期的**。

**v0.29 的新探针（只为一件事：rc=2 到底是哪条判据）**

`flash_iap_check_range()` 的闸门 1 有两条判据，却共用一个 `OTA_ERR_ADDR(2)`：
- `addr >= s_eeprom_size` —— 地址超容量
- `len > s_eeprom_size - addr` —— 长度越界

静态排查已穷尽（`flash_iap_read` 的入口汇编、`len` 落在 `?flash_iap_read??BYTE+8`
= 0x1FB5、`s_ca/s_cl/s_eeprom_size` 全部落在干净的独立段、`acc` 走 `A` 寄存器、
全 map 符号无重叠），所以改成让门禁**自报**它看到的值：

```c
u32 xdata g_cr[3];        /* [0]=addr  [1]=len  [2]=s_eeprom_size  */
```

失败时打两行（仅 `rc == OTA_ERR_ADDR` 时）：

| 日志 | 判读 |
|---|---|
| `[VER] chk a=%lx l=%lx` | 门禁看到的 addr / len |
| `[VER] chk sz=%lx off=%lx` | 门禁看到的容量 / 正文内偏移 |

判读表：

| 现象 | 结论 |
|---|---|
| `a=40 l=80 sz=20000 off=0` | 三个值全正常 ⇒ **rc=2 不是门禁给的**，去查 ver_body 的调用链/返回值传递 |
| `a=40 l=80 sz=40` | 容量被写小了（`s_eeprom_size` 被踩），顺着 0x1BF1 查谁写的 |
| `a` 远大于 `sz` | 偏移被踩大（`s_off` / `s_n`），或地址计算错 |
| `l` 远大于 `sz - a` | 长度被踩大（`len` 槽 = 0x1FB5） |

**v0.29 交付**：`build/step29_checkrange_probe_BL_APP.hex`（84364 B，
md5 `23401811…`，两次构建一致）
- BL `code=13477` `const=2420`，末端 `0xFF3EC5` ⇒ **16K 槽剩 315 B**
- 入口 `02 00 27` / `75 84 00 7E`，三道守门全过，L251 0 WARNING
- 布局：`?XD?FLASH_IAP` 0x19B3-0x1CDB（`g_cr`@0x1BD6，`s_eeprom_size`@0x1BF1）、
  `?XD?OTA_VERIFY` 0x1E90-0x1F4D（`s_off`@0x1E94、`s_buf`@0x1E99、`s_n`@0x1F48）、
  `?XD?CRC32` 0x1F4E-0x1F5D

**离线预言值（本版 APP 0xFF4000 起 49088 B 复算）**：
正文 CRC = **`dc98ec88`**（APP 的 `A1 stg crc` / `A1 image ready crc` 与 BL 的
`[VER] STAGING ok crc` 三处都应是它），镜像头 36 B CRC = `be66c5d0`。

### 8.32 v0.30：读通路绕开地址门禁调用 + 删掉 CRC 自检

**v0.29 上板读数（30 行行尾和值全对，可当证据）**

| 行 | 值 |
|---|---|
| `[IAP] crc32=` | `69d39b4b`（v0.28 同一份源码是 `cbf43926`，一变布局就变） |
| `[VER] STAGING fail` | `rc=2 step=9`（仍是第 99 块） |
| `[VER] chk` | `a=3140 l=42ff57` / `sz=20000 off=3100` |
| APP | `A1 stg crc` == `A1 image ready crc` == `dc98ec88` == 离线复算 ✓ |

**读数怎么解**

1. `0x3140 = 0x3100 + 0x40` ⇒ 地址是按 `s_off` 正确算出来的；
   `0x3100 / 0x80 = 98`，而 `[1334][VER]` 距开始约 1160 ms、单块实测 12 ms
   （v0.27 全程 4637 ms / 384 块）⇒ **`s_off=0x3100` 是正常的循环进度**，
   不是被踩；真正坏的是同一块里的 `len`。
2. `l=42ff57` 高 16 位非零。但这条链上 `len` 是 u16，调用点又用
   `XRL WR0,WR0` 显式清零高半字（**镜像 0xFF0655 实测，全镜像只此一处**，
   `9A FF 02 A6` 的四个调用点里 0xFF0658 就在 `flash_iap_read` 内）
   ⇒ **按代码它不可能大于 0xFFFF**。
3. 镜像侧能查的全查了，全对：`s_vc` 内容（0xFF3E5E = `31..39`）、指针立即数
   （`MOV WR2,#3E5E` / `MOV WR0,#00FF` @0xFF321A）、`crc32_update` 查表槽位、
   `?XD?FLASH_IAP` 符号无重叠（`s_cl`@0x1BFB / `g_cr`@0x1BD6 只有
   `check_range` 自己引用）。⇒ **读代码不再有新信息**。

**结论与修法**：静态分析给出了字面上的矛盾，说明"再猜一次"没有出路。
而 `acc == 0` 时 `flash_iap_check_range()` 的全部动作就是
`闸门1（容量）→ return OK`（分区 / 对齐 / 可写三道只服务擦写），
**但它的形参是 4+4+1 = 9 B > 8 B，必然生成一个 `?BYTE` 参数块** ——
正是本工程反复出事的结构（铁律 9）。

⇒ v0.30 让读通路**不再调用它**，就地判三条等价条件：

```c
if (s_rlen == 0U)      return OTA_ERR_PARAM;
if (!s_inited)         return OTA_ERR_STATE;
if (s_raddr >= s_eeprom_size ||
    (u32)s_rlen > (s_eeprom_size - s_raddr))
{
    g_cr[0] = s_raddr; g_cr[1] = (u32)s_rlen; g_cr[2] = s_eeprom_size;
    return OTA_ERR_ADDR;
}
```

`g_cr` 的语义随之从"门禁看到的"变成"**读通路看到的**"，日志文字不变：

| 看到什么 | 结论 |
|---|---|
| `[VER] STAGING ok len=49088 crc=<离线值>` | **A1 达成** |
| `[VER] body crc now/want` 两值都像 | 读通路通了，问题落在 CRC 引擎（少见） |
| 仍 `rc=2` + `chk a/l` | 读通路自己也判越界 ⇒ `s_eeprom_size`/`s_raddr` 被踩，按 `chk` 三值定位 |
| `rc=6` | 门禁已绕开，是 IAP 逐字节读失败（硬件/EA 层面），按 `rc=6 step=9` 查 |

**同时删掉 CRC 自检（`crc32_selftest` / `crc32_selftest_value_cst`）**

它从 v0.25 起连续 6 版都打 FAIL，读数还不可复现（v0.28 `cbf43926` →
v0.29 `69d39b4b`，而向量内容与指针立即数复查都对），并且**一 FAIL 就提前
return**，把真正有效的判据 —— `[IAP] bl head: 2 …`（读 BL 入口 4 字节，
真固件真地址）—— 和 STAGING/APP 两处探针全部跳过。删掉后这行会重新出现，
它才是"读通路是否可信"的实测证据。

**v0.30 交付**：`build/step30_read_bypass_gate_BL_APP.hex`（83128 B，
md5 `534a158c…`，两次构建一致）
- BL `code=13303` `const=2369`，末端 `0xFF3DE4` ⇒ **16K 槽剩 540 B**
  （v0.29 只有 315 B，A2 估需 450~550 B，现在基本够）
- 入口 `02 00 27` / `75 84 00 7E`；三道守门全过，L251 0 WARNING

**离线预言值（本版 APP 0xFF4000 起 49088 B 复算）**：
正文 CRC = **`4cd879eb`**、镜像头 36 B CRC = `9e423412`。

**预期日志变化（不是故障）**：`[IAP] crc32=` 与 `crc32 selftest FAIL` 消失；
`selftest=6` → **`selftest=11`**（= STAGING untested，因 `BL_STG_PROBE=0`），
并新增 `[IAP] bl head: 2 …`。

### 8.33 v0.31：改打地面证据（`d40` / `dbl0`）

**v0.30 上板读数**（30 行行尾和值全对，仅 3 行因转贴掉字不符）：

```
[IAP] bl head: ff ff ff ff (want 2 ...)
[IAP] IAP READ path untrusted (bl head ff != 2), NO WRITE DONE
[BL] selftest FAIL rc=6
[VER] body crc now=121a9955 want=4cd879eb
[VER] STAGING fail rc=7 step=9        ← rc=2 消失，读通路绕开门禁成功
[APP] A1 stg crc=4cd879eb  ==  A1 image ready crc=4cd879eb  （与 Python 一致）
```

`rc=2` 消失、`step=9` 第一次算出了 CRC，说明 v0.30 的"读通路绕开 9 B 形参调用"
生效了。但 CRC 值错，同时 `bl head` 退化成全 FF。

**这一版把静态分析做穷尽了，以下每一项都逐字复核、全部正确：**

| 核对项 | 结论 |
|---|---|
| `ver_body` 镜像机器码（0xFF274F） | `ADD DR4,#040H` 加了头偏移；`s_n` 送 `?crc32_update??BYTE+8`；累加 `ADD DR4,DR0`；判据 `CMP DR4,DR0 / JNC` —— **全对** |
| `flash_iap.c` 在 BL / APP 两工程 | 汇编指令 **逐字节相同**（只差时间戳） |
| `?XD?OTA_VERIFY` 190 B | `s_crc`4+`s_off`4+`s_step`1+`s_buf`128+`s_hdr`42+`s_crc_hdr`4+`s_i`1+`s_n`2+`s_crc_run`4 = 190 **恰好铺满** |
| `?XD?CRC32` 12 B @0x1F4A | 紧接 `s_crc_run`(0x1F46-0x1F49) 之后，**无重叠** |
| 13 个 `?BYTE` 参数块 | 全部 ≥0x1F56，**碰不到 `s_buf`(0x1E95-0x1F14)** |
| `s_eeprom_size` | `init(128)` 算出 0x20000，日志打 128 —— **不是容量门禁的锅** |
| 耗时 | 4395 ms ÷ 49152 次逐字节读 ≈ 89 µs/次 ⇒ **循环真的跑满了 384 块** |
| `flash_iap_set_addr` | 确实写 `IAP_ADDRE = addr>>16`，0x10000 与 0x14000 的 ADDRE 都是 1 |

⇒ 「代码对、地址对、布局对、读也没报错」但结果错。这时只能上板取原始字节。

**新增两条探针（都放在 `flash_iap_selftest` 容量检查之后、`bl head` 闸门之前，
所以不会被提前 return 吞掉）：**

```
[PRB] d40=75 84 0 7e          ← STAGING 正文起点 4 字节（= APP 入口）
[PRB] dbl0=2 r=0              ← IAP 0x10000 首字节 + **返回码**
```

`dbl0` 的 `r` 是关键：`flash_iap_read_byte()` 任何错误都返回 0xFF，把 r 丢掉了，
所以 v0.30 的 `bl head: ff ff ff ff` 分不清"读失败"和"硬件真给 FF"。

**判读表**

| `d40` | 含义 | 下一步 |
|---|---|---|
| `75 84 0 7e` | 正文第一块数据是好的 ⇒ 故障在"重复 384 次"这条路上 | 加 64 B 分块对照趟 |
| 其他 | 读通路在 0x40 就已错 ⇒ 与 `s_off`/累加无关 | 查 0x40 与 0x0 的差别（头 42 B 是读对的） |

| `dbl0` 的 `r` | 含义 |
|---|---|
| `0` 而字节是 `ff` | 硬件在这个地址**真给 FF** ⇒ 地址映射 / 读保护 |
| `2` (ADDR) | 被容量检查拒 ⇒ `s_eeprom_size` 不是 0x20000 |
| `6` (IO) | `CMD_FAIL` ⇒ IAP 时序/参数问题 |
| `12` (STATE) | `s_inited` 丢了 |

**离线预言值（本版 APP 0xFF4000 起 49088 B 复算）**
- 正文 CRC = **`66c21819`**（APP `A1 stg crc` / `A1 image ready crc` 与 BL
  `[VER] STAGING ok crc` 三处都应是它）
- 镜像头 36 B = `6c7837da`
- `d40` 期望 `75 84 0 7e`；`dbl0` 期望 `2` 且 `r=0`

**尺寸**：BL `code=13509` `const=2410`，末端 `0xFF3EDB` ⇒ 16K 槽剩 **292 B**。
（原计划的 64 B 分块对照趟要 ~380 B，放不下，先砍掉。）
三道守门全过，`NOOVERLAY` 生效、13 个参数块各自独占，两次构建 md5 一致。

⚠️ **预期日志变化（非故障）**：新增 `[PRB] d40=` / `[PRB] dbl0=` 两行；
其余与 v0.30 相同（`[IAP] crc32=` 仍不出现、`selftest` 仍是 `6`）。

## 8.34 v0.32 上板：**A1 达成**；v0.33 打开 A2（搬运生效）

### 8.34.1 v0.32 的日志：三相一致的 66c21819

第二段（断电重上电）关键三行，行尾和值全部核对通过：

```
[4838][VER] STAGING ok len=49088 crc=66c21819*6d     ← calc=6d ✓
[4865][BL]  staging verified: A1 read-only, apply NOT enabled*35
[4885][BL]  == summary: init=0 selftest=6 stg=untested app=untested ==*d2
```

三方对账（全部 `binascii.crc32`）：

| 谁算的 | 值 |
|---|---|
| APP 写 STAGING 时算的（并回读校验） | `66c21819` |
| BL 从 STAGING 读 49088 B 算的 | `66c21819` |
| Python 对同一份 hex 的 `0xFF4000..` 49088 B | `66c21819` |

⇒ **P1-A1 成立**：BL 能完整、正确地读懂 APP 造出的镜像。
`[VER] STAGING ok` 这一行是 A1 唯一的通过判据，它出现了。

### 8.34.2 但仍然要记住：这个"对"是**布局相关**的

同一段代码在 v0.31（仅多两处赋值）算出 `bac835a5`，v0.32 就对了。
这与 v0.16~v0.23 的历史完全同型 —— **"某版对、某版错、改布局就变"**。
所以正确的结论不是"读通路从此没问题"，而是：

* A1 的**功能判据**已拿到（可以用来做 A2 的输入前提）；
* 但**同一族缺陷还在**，证据是 `bl head: ff ff ff ff` 至今仍在打。

### 8.34.3 `bl head` 的定论与修法

v0.31 的两条地面证据把范围夹死了：

| 探针 | 通路 | 读数 |
|---|---|---|
| `d40` | `flash_iap_read(STAGING+0x40, buf, 4)` | `75 84 0 7e` ✓ |
| `dbl0` | `flash_iap_read(0x10000, buf, 4)`，rc=0 | `2` ✓ |
| `bl head` | 4 次 `flash_iap_read_byte(0x10000+i)` | `ff ff ff ff` ✗ |

同一个地址、同一个底层 `flash_iap_raw`，**只有 len=1 的封装不行**。
而 `flash_iap_read_byte(0x14000)`（跳转前那一处）却是好的 —— 所以它不是
"0x10000 不可读"，而是**这条封装本身在特定调用上下文里读不到**。

**v0.33 的处置**：不再用它当判据。自检要证明的是"功能通路的读可信"，
那就必须用**功能通路**去证：

```c
rc = flash_iap_read(OTA_BL_IAP_BASE, s_pr, 4U);     /* 走 prove 过的那条路 */
LOG_I4("IAP", "bl head: %x %x %x %x (want 2 ...)", s_pr[0], s_pr[1], s_pr[2], s_pr[3]);
```

期望：`bl head: 2 0 27 ff`
（`02 00 27` = 0xFF0000 的 LJMP 到 0xFF0027，第 4 字节 FF 是链接器留的空洞）
—— 这三个字节是**结构性常量**，读到它们就等于读到真固件。

### 8.34.4 v0.33：A2（`ota_apply_staging()`）的三个设计要点

**① 幂等。** BL 的 `param_save` 目前是坏的（APP 侧实测 `A1 param_save rc=6`，
下一次上电 BL 立刻报 `param empty/corrupt`）⇒ **"装好了"这个状态持久化不了**。
不幂等的话每一轮上电都要擦 96 个扇区 + 写 49 KB。
所以第一步不是擦写，而是先算 APP 区正文 CRC：等于镜像头的 `img_crc32` 就
一个字节都不动。

**② 自愈。** 先擦后搬、源永远在 STAGING（只读）⇒ 中途掉电/复位后，
下一次上电必然发现 CRC 不符，自动整段重来。**不需要任何进度标记。**

**③ 可回退。** `BL_OTA_A2_APPLY` 改回 0 就回到 A1 的"只看不动"。

流程（全部走 `flash_iap_read/write/erase_range` 唯一通路，每 128 B 喂狗）：

```
[VER] STAGING ok len=49088 crc=<X>            ← 镜像本身可信
[APL] FORCE copy: idempotent check skipped    ← 本轮调试开关
[APL] copy off=2000 ... （每 8 KB 一行）
[APL] APPLY OK len=49088 crc=<X>              ← 回读校验通过
[BL]  A2 DONE: app updated, booting new firmware
```

失败时**绝不跳转**（APP 区此刻是被擦过/半截写的状态），留在 BL 打心跳：
`[APL] copy fail off=<偏移> rc=<码>` 或 `[APL] app crc now=.. want=..` ——
偏移能定位断在哪一块。

### 8.34.5 本轮调试开关 `BL_OTA_A2_FORCE_COPY = 1`

A1 的镜像是**从 APP 区自己复制出来的**，内容与现场完全相同 ⇒ 幂等判定必然命中
⇒ 真正的擦写路径一次都跑不到（白烧一版）。所以本轮强制跳过幂等判定，
专门验证"擦 + 搬 + 回读"。镜像内容与现场一致，哪怕搬运写坏，
代价也只是"APP 区被写成它自己的副本"，ISP 冷启动随时可救。
**搬运被证实之后必须改回 0。**

### 8.34.6 为腾空间删掉的东西（BL 16 K 槽从 −1267 B 变成 +898 B）

| 删除项 | 理由 |
|---|---|
| `flash_iap_probe()` + `s_probe_bak[512]` | ★风险大于收益：它在"首字节读到 0xFF"时才探 APP 区，而 `flash_iap_read_byte` 正是那条会误报 FF 的封装 ⇒ 可能在有效固件上擦掉 APP 首扇区（复位入口）再"还原"。且 A1/A2 的真业务写证比"写一个 0xA5"强 |
| 自检里的两处探针调用 + `probe_str()` | 同上；`s_r_stg/s_r_app` 一律 `UNTESTED`，如实播报"未测" |
| `d40` / `dbl0` 探针 + `s_prc` | 使命完成（见 8.34.3） |
| `bl_summary()` 里 `rs != UNTESTED` 之后的分支 | 已成死代码（`s_r_stg` 恒为 UNTESTED），连字符串一起删 |
| `ota_verify` 的 `ck1` / `ck192` 检查点 | A1 已通过，判据不再需要 |

### 8.34.7 离线预言值（v0.33，改动后 APP 也变了）

| 量 | 期望值 |
|---|---|
| 镜像正文 CRC（49088 B） | **`25d6d29f`** |
| 镜像头 CRC（36 B） | `9fb99eb4` |
| `bl head` | `2 0 27 ff` |
| `selftest` | `11`（= OTA_ERR_SUPPORT，"未测"不是失败） |
| BL 入口 / startup / APP 入口 | `02 00 27 ff` / `75 84 00 7e` / `75 84 00 7e` |
| BL 尺寸 | code=13269 const=2044，末端 `0xFF3C7D` ⇒ **槽余 898 B** |

⚠ `A1 stg crc=` / `A1 image ready ... crc=` 本轮也应是 `25d6d29f`
（APP 源码删了探针，正文变了；**不再是上一版的 66c21819**）。

## 8.35 v0.33 上板：**A1 退回去了，但挖出了它的真形状**

### 8.35.1 这一版先说好消息：两条预言精确命中

```
[71][IAP] bl head: 2 0 27 ff (want 2 ...)*ca     ← 逐字节正确！预测值命中
[88][BL]  selftest: STAGING untested (probe off), not a fault*29
[5443][BL] == summary: init=0 selftest=11 stg=untested app=untested ==*f5
```

`bl head` 改成走**批量读**之后当场读到 `2 0 27 ff` —— 正是 0xFF0000 的
`LJMP 0x0027`。v0.31 那条 `ff ff ff ff` 是**逐字节封装**独有的毛病，与功能通路无关，
现在这个结论被正面证据钉死了。`selftest=11`（未测）也与预言一致。
**这些缺陷（读通路整体）从此可以放下。**

### 8.35.2 坏消息：正文 CRC 又不对，而且这次可以算出它错在哪

```
[5197][VER] body crc now=34824ce1 want=25d6d29f*55
```

先把"是不是写侧"彻底排掉：APP 的 `A1 stg crc=`（源码第 197 行）打的是 **`s_a1rb`**，
即**回读 STAGING 49088 B 算出来的**值，不是源区算的：

| 谁 | 值 | 含义 |
|---|---|---|
| APP 累加（源区）`s_a1crc` | `25d6d29f` | 与 Python 对 hex 的正文 CRC 一致 |
| APP **回读 STAGING** `s_a1rb` | `25d6d29f` | **STAGING 里躺着的内容是对的** |
| BL 从 STAGING 读算 | `34824ce1` | 只能是 BL 这一侧 |

⇒ STAGING 无罪、APP 写侧无罪（它的写 + 回读都自洽）。**是 BL 的读/算错。**

### 8.35.3 ★ 数值穷举得到唯一解：第 38 块被第 37 块顶替 ★

以 `34824ce1` 为目标，对**本版正文**扫了约 2260 个"整段搬家"型假设
（起点偏移、长度 ±64/±128 与 0xC000/0xBFC0/0xC040/0x1000…、单块跳过 / 单块重复 /
单块置 FF / 单块置 0 / 单块换 chunk0 / 单块换上一块 / 单块倒序、相邻块交换、
全反转、偶奇拆分、2/4 字节序互换）——**只有一个命中**：

```
正文偏移 4864..4991（第 38 块，IAP 地址 0x1340）的内容 == 第 37 块（0x12C0）
```

唯一性做了双向验证：

| 固定 | 扫描 | 命中 |
|---|---|---|
| `chunk38 := chunk j` | j = 0..383 | **仅 j = 37** |
| `chunk k := chunk37` | k = 1..383 | **仅 k = 38** |

即 `c0..c37, c37, c39 .. c383`（长度仍是 49088，第 38 块**整块消失**、
第 37 块**出现两次**）。约 4600 个向量落在 2^32 空间里，随机撞中的概率约 **1e-6**。

**这解释了此前所有"看不懂"的现象**：错值每版都不同、改布局就换一个函数受害 ——
因为受害的是**随机某一块**，不是某个固定的结构。

同时它也**否掉了三条老猜测**：

* 不是"丢了一次 `s_off += s_n`"：那会让循环跑 385 次、总长变 49216，
  字节流是 `c0..c37, c37, c38..`（第 38 块还在），与命中结果不同；
* 不是"读到全 FF"：那块不是 FF，是**上一块的真实内容**；
* 不是"哪一块被替换"：只有 37→38 这一对，不是任意块。

### 8.35.4 代码层面找不到第二个地址来源（map 逐段核过）

| 量 | 地址 | 与谁可能冲突 |
|---|---|---|
| `s_buf[128]` | `0x1BD0-0x1C4F` | 紧邻 `s_hdr`（`0x1C50`），段内无重叠 |
| 13 个 `?BYTE` 参数块 | 全部 `≥0x1D9D` | 离 `s_buf` 还有 334 B |
| `?XD?OTA_VERIFY` | `0x1BC5-0x1C9E`（218 B） | 与 `?XD?OTA_JUMP`（`0x1C9F` 起）首尾相接、不重叠 |
| `s_raddr` | `0x19E3` | 在 `?XD?FLASH_IAP`（`0x19B3-0x1AD6`）内，独占 |

而且 `flash_iap_read` 的读循环**整段 `EA=0`**（`critical_enter` 包住 for），
1 ms 的 Timer0 中断根本进不来 ⇒ **ISR 不是嫌疑**。
`flash_iap_read` 的形参第一条语句就落进 `s_raddr/s_rlen/s_rbuf`（铁律 9）。

⇒ "读到上一块"只能是 `s_raddr` 在那一次调用里是旧值，而**代码里找不到它会变旧的理由**。
按本项目的规矩：**不许用读代码代替硬件证据**，那就让板子自己念三个数出来。

### 8.35.5 v0.34 的三条自证探针（都只在 `rc == OTA_ERR_CRC` 时跑）

写在 `ota_verify_staging()` 里、`ver_body()` 之后：

| 日志行 | 来源 | 期望值 | 判读 |
|---|---|---|---|
| `[VER] self crc=` | `apl_crc(0x10000, 16K)` | **`26c0ad90`** | 读自己的代码区。它若不等 ⇒ BL 的"读+算"整体不可信，后面两条不用看 |
| `[VER] ck38 crc=` | `apl_crc(0x1340, 128)` | `13320cd8` | 读出 **`ee21861d`** ⇒ 陈旧地址读**当场复现**（那是第 37 块的真值） |
| `[VER] stgA crc=` | `apl_crc(0x40, img_len)` | `25d6d29f` | 与 `now` 比：`=now` 确定性错；`=want` 首趟是偶发；第三个值 ⇒ 读通路不稳 |

`self` 的期望值 **跟着固件走**：v0.33 是 `c499c33e`，本版加了探针后是 `26c0ad90`。
换版本必须用同一份 hex 重算 —— 这正是"注释里的数也要来自同一份产物"。

**顺带当兜底**：第 2 趟若正好等于 `want`，就把 `rc` 改回 OK 继续往下走（A2 照常），
并打 `[VER] body RETRY ok crc=`。`now` 与 `stgA` 两个值都留在日志上 ——
"两趟不一致"这件事藏不住。

### 8.35.6 本轮判读表（一屏分完）

| 看到什么 | 结论 | 下一步 |
|---|---|---|
| `self` ≠ `26c0ad90` | BL 的读+算整体不可信 | 查 IAP 时序（`IAP_TPS` / `IAP_CONTR` 使能后是否要等） |
| `self` 对、`ck38` = `ee21861d` | 陈旧地址读**复现** | 直接打这块的地址通路 |
| `self` 对、`ck38` = `13320cd8`、`stgA` = `now` | 确定性错误，同一错法 | 加逐块指纹，找哪一块是"偶然"的 |
| `self` 对、`ck38` 对、`stgA` = `want` | **首趟是偶发**，重算即对 | `RETRY ok` 生效 → A2 继续跑 |
| 三个值都不同 | 非确定性读 | 读通路必须加重试/双读校验 |

### 8.35.7 ⚠ 空间警告

BL `code=13621 const=2129`，末端 `0xFF3E32` ⇒ **16 K 槽只剩 461 B**。
探针证完必须摘掉（`self`/`ck38`/`stgA` + `s_abase…s_p_crc` 那组只能在 BL 编译的量）。
A2 已经实现，所以这 461 B 就是往后所有改动的全部余地。

## 8.36 v0.34 上板：**A1 的故障形状被钉死；A2 第一次真写就被门禁拒了**

### 8.36.1 三条预言全部精确命中（行尾和值逐行核过）

```
[5109][VER] body crc now=5768849c want=25d6d29f*32    ← 首趟错（第三个数了）
[6771][VER] self crc=26c0ad90*ba                      ← 预言 26c0ad90 ✓
[6798][VER] ck38 crc=13320cd8*21                      ← 预言 13320cd8 ✓
[11744][VER] stgA crc=25d6d29f*d8                     ← 第二趟 = 正确答案 ✓
[11759][VER] body RETRY ok crc=25d6d29f*ad
[11777][VER] STAGING ok len=49088 crc=25d6d29f*ff     ← A1 判据（第二趟给的）
```

三条结论一次成立：

| # | 结论 | 依据 |
|---|---|---|
| ① | **读通路 + CRC 引擎按需调用时完全正确** | `self` / `ck38` 两个离线预言一字不差 |
| ② | 错的是**"首趟这一趟"**，不是某个地址 | 同一份代码、同一地址范围，首趟错、第二趟对 |
| ③ | 受害块是**随机**的 | v0.33 唯一解是 `chunk38:=chunk37`，v0.34 那块 `ck38` 却是对的 |

⇒ 与 v0.33 的离线穷举（2260 个假设、唯一解、概率 1e-6）互相印证：
**每次上电随机坏一块**，v0.32 恰好没中，所以"单次通过不算通过"。
**不再继续反推是哪一块 —— 已经证明这条路走不通（v0.34 又扫了一遍单块替换矩阵，零命中）。**

### 8.36.2 A2：第一次真写，被地址门禁当场拒绝

```
[11804][BL]  A2: apply staging -> app (erase+copy+verify)*2f
[11824][APL] FORCE copy: idempotent check skipped (debug build)*20
[11845][APL] erase app fail rc=2*01
[11862][BL]  apply FAIL rc=2, stay in BL (re-flash to recover)*34
```

**先给一个让人安心的结论：一个扇区都没擦。**
`erase_range -> erase_sector` 的门禁在 `flash_iap_raw(ERASE, ...)` **之前**，
所以 rc=2 意味着"命令还没发出去"。APP 区此刻和上电前**逐字节相同**，
没有任何数据损失，只是 BL 按设计拒绝跳转（它不知道 APP 是好的）。

rc=2 = `OTA_ERR_ADDR`。而这次擦的是 `0x14000`，三条判据静态看全对：

| 判据 | 值 | 结论 |
|---|---|---|
| 容量 | 0x14000 < `s_eeprom_size`=0x20000（`eeprom=128KB` 就是它右移 10 位打出来的） | ✓ |
| 扇区对齐 | 0x14000 & 0x1FF = 0 | ✓ |
| 分区可写 | 表里 `{0x14000, 0xC000, 1, PART_APP_IAP}`，`ota_part_writable(5)` 应为 1 | 待证 |

⇒ 静态层面已经到极限（读代码第 7 次了）。按本项目规矩：**上板让门禁自己说**。

### 8.36.3 v0.35 的处置：擦除不再经过 `check_range`，失败时自报闸门

与 v0.30 把读通路的 `check_range` 内联掉**同一个理由**（铁律 14）：
它 4+4+1 = 9 B 形参 > 8 B，必然生成参数块；本工程凡是经过它的路径都反复出现
rc=2，而读通路内联后**从此再没出过**。所以擦除也内联，并且失败时打一行：

```
[IAP] erase g=<gate> p=<part> a=<addr>
```

判读（一次上板就能定案）：

| 看到 | 含义 | 下一步 |
|---|---|---|
| 没有这行 ⇒ 出现 `[APL] copy off=2000 ...` | **擦除通过了** ⇒ 病根就是 `check_range` 这条调用链 | A2 继续跑，看 APPLY OK |
| `g=4 p=0` | `ota_part_of(0x14000)` 没查到 — 分区表（HCONST 0xFF3D2C）在 BL 里读不对 | 改用宏直接判（不查表） |
| `g=4 p=5` | 查到了，是 `ota_part_writable()` 说不可写 | 查该函数的表读/枚举比较 |
| `g=2` | 容量判据挂了 | 查 `s_eeprom_size` 的实际值 |
| `g=3` | 对齐判据挂了 | 查 `is_sector_aligned` |

### 8.36.4 顺带删掉的东西（16K 槽：461 B → 445 B）

`self` / `ck38` 两条探针已答完（见 8.36.1），删掉；`stgA`（第二趟）保留 ——
它现在同时承担 A1 的兜底与"两趟不一致"的证据。擦除侧新增的一行日志吃掉
了腾出来的空间，净剩 445 B。

### 8.36.5 v0.35 离线预言

| 量 | 期望值 |
|---|---|
| 镜像正文 CRC（49088 B） | **`bcff27b7`** |
| 镜像头 CRC（36 B，只在 step=5 失败时打） | `5fbf1dca` |
| `bl head` | `2 0 27 ff` |
| BL 入口 / startup / APP 入口 | `02 00 27 ff` / `75 84 00 7e` / `75 84 00 7e` |
| BL 尺寸 | code=13647 const=2119 末端 `0xFF3E42` ⇒ 槽余 **445 B** |

★ APP 也变了（`flash_iap.c` 两工程共用）⇒ `A1 stg crc=` 本轮应是 `bcff27b7`，
**不再是 25d6d29f**。

★ `BL_OTA_A2_FORCE_COPY=1` 仍然开着 ⇒ 一旦 A2 跑通，**每次上电都会重擦重搬一遍**
（因为 A1 造的镜像就是 APP 自己的副本，幂等判定永远命中）。看到 `A2 DONE`
就断电，别让它反复跑。

## 8.37 v0.35 上板：**擦除通了，写入栽在同一道门禁上**

### 8.37.1 这一轮最要紧的一行是"没出现的那行"

```
[9127][BL]  A2: apply staging -> app (erase+copy+verify)*04
[9146][APL] FORCE copy: idempotent check skipped (debug build)*f4
[9349][APL] copy fail off=0 rc=2*e9        ← 失败点从"擦"挪到了"写"
```

**全程没有 `[IAP] erase g=` 那一行** —— 而那一行是 v0.35 新加的、只在门禁拒绝时才打。
没打，说明四条内联判据（未初始化 / 容量 / 对齐 / 分区可写）一条都没拒，
`0x14000` 那一擦**真的发出去了**。

时间也对得上：`9127 → 9349` 共 222 ms，96 个扇区 × 约 2 ms ≈ 192 ms，
剩下的 30 ms 正好是读第一块 + 写第一块的耗时。⇒ **擦除这项，通了。**

> 这是第 3 次同型证据：把 `flash_iap_check_range()` 从某条通路上摘掉，
> 那条通路立刻正常（v0.30 读、v0.35 擦、v0.36 写）。而它内部四道闸门
> 单独看全是对的。**病根不在判据，在"9 B 形参必然生成参数块"这件事上。**

### 8.37.2 A1 这一侧已经完全稳住

```
[4615][VER] body crc now=110cb6f2 want=bcff27b7   ← 首趟又错（第 4 个不同的值）
[9069][VER] stgA crc=bcff27b7                     ← 第二趟永远对
[9084][VER] body RETRY ok crc=bcff27b7
[9101][VER] STAGING ok len=49088 crc=bcff27b7     ← A1 判据每轮都拿到
```

错值已经集齐 5 个互不相同的数（`121a9955` / `bac835a5` / `34824ce1` / `5768849c` / `110cb6f2`），
而**第二趟每次都对**。这就是 v0.34 定下的结论在反复复现：
坏的是"某一趟的读取"，不是某个固定地址；"算两趟、对上就继续"是有效的工程对策。
A1 从此不再是路障，**根因留到 A2 通了再收**。

### 8.37.3 写入的真身：`ota_part_contains()` 自己踩自己（map 实证）

`copy fail off=0 rc=2` 的 `rc=2` 只可能来自 `flash_iap_check_range()` 的
`OTA_ERR_ADDR` —— 写链上再没有第二个 ADDR 来源（`flash_iap_raw` 的拒绝只计数不上抛，
`flash_iap_verify` 上抛的是 CRC/IO）。而擦除刚刚证明前四道闸门里…
擦除只用前三道，**写比擦多用的那道正好是「整段不得跨分区」**，也就是：

```c
if (!ota_part_contains(s_p, s_ca, s_cl)) return OTA_ERR_ADDR;
```

v0.35 的 BL map 把它的死因照得清清楚楚：

```
?ota_part_contains??BYTE              = 0x1DCE    ← 1+4+4 = 9 B 的形参块起点
?XD?OTA_PART_CONTAINS??OTA_PARTITION  = 0x1DD4    ← 局部变量 u8 i 住在 0x1DD4
```

L251 给形参块只按 **6 字节**排（0x1DCE → 下一个块 0x1DD4），而它实际要 **9 字节**。
于是 `i` 正好落在自家形参块的尾巴里。而原实现是：**`len==0` 判完之后，
进循环第一句就写 `i = 0`，然后在循环里反复读 `part` / `iap_addr` / `len`。**

```
i = 0  →  清掉形参块尾 4 字节  →  被清掉的可能是 iap_addr 的高位
     →  iap_addr 变成天文数字  →  iap_addr < base 命中 continue
     →  返回 0  →  判"跨分区"  →  OTA_ERR_ADDR(2)
```

**这就是"改一次布局就换一个函数受害"的机制**：被压住的永远是那个
"局部段刚好排在形参块尾巴上"的函数，而 L251 的排布随代码量抖动。

**修法（铁律 9 的标准形）**：形参第一条语句就拷进**模块级** static
（落 `?XD?OTA_PARTITION`，与形参块无关），局部量也一律 static。
拷完之后形参块被谁踩都无所谓 —— 这才是真正的免疫。

### 8.37.4 v0.36 做了三件事

**① 写入门禁内联**（`BSP/flash_iap.c`，与 v0.30 读、v0.35 擦同一手法）
四条判据内联，"整段不跨分区"改用**首字节与末字节同区**来判
（分区在 IAP 空间里是连续区间，首末同区 ⇔ 整段在内，等价且零函数调用）：

```
[IAP] wr g=<gate> p=<part> a=<addr>
```

| 看到 | 含义 |
|---|---|
| **没有这一行**，直接出现 `[APL] copy off=2000 …` | 写入门禁全过 ⇒ A2 在跑，等 `APPLY OK` |
| `g=1` | 未初始化（`s_inited`=0） |
| `g=2` | 容量/长度判据（`a` 会告诉你它实际看到的地址） |
| `g=4 p=0` | `ota_part_of(0x14000)` 没查到 |
| `g=4 p=5` | 查到了是 `PART_APP_IAP`，但 `ota_part_writable()` 判 0 |
| `g=6` | 首末字节不同区（跨分区） |

**② `flash_iap_check_range()` 整段删除**
它已经没有调用者了（读/擦/写全部内联），而保留它只会让人下次又顺手调。
`BSP/flash_iap.h` 里那一大段"唯一门禁"改成四条内联判据的清单 +
一句"不要再造多形参门禁函数"。

**③ 把"命令被拒"和"写不进去"分开**

```c
s_rc = flash_iap_verify(...);
if (s_rc != OTA_OK && g_iap_dbg[3] != 0UL) return OTA_ERR_IO;
```

于是三种结果各有各的码，不用再猜：

| `copy fail … rc=` | 含义 | 下一步 |
|---|---|---|
| **6** | `flash_iap_raw(PROGRAM)` 被拒（分区/容量/CMD_FAIL），命令没生效 | 查 IAP 时序与分区表 |
| **7** | raw 说成功，Flash 却不认 | 只剩 `IAP_TPS` / 供电 / 写保护 |
| **2** | 还是门禁（新 `wr g=` 行会指出是哪一道） | 对号入座 |
| **0** | 真的写进去了 | 等 `[APL] APPLY OK` |

### 8.37.5 ⚠ 空间与开关

BL `code=13745 const=2144`，末端 `0xFF3EBD` ⇒ **16 K 槽余 322 B**（删掉
`check_range` 换来了 ~60 B，内联与日志吃掉一些）。
`BL_OTA_A2_FORCE_COPY = 1` 仍然开着：A1 造的镜像就是 APP 自己的副本，
幂等判定必然命中 ⇒ 一旦 A2 跑通，**每次上电都会重擦重搬**。
看到 `[APL] APPLY OK` / `[BL] A2 DONE` 就断电，然后把它改回 0。

### 8.37.6 v0.36 离线预言（APP 也变了）

| 量 | 期望值 |
|---|---|
| `A1 stg crc=` / `A1 image ready` / `[VER] STAGING ok` / `[APL] APPLY OK` | **`77f35870`** |
| 镜像头 CRC（只在 step=5 失败时打） | `2b429498` |
| `bl head` | `2 0 27 ff` |
| BL 入口 / startup / APP 入口 | `02 00 27 ff` / `75 84 00 7e` / `75 84 00 7e` |
| BL 尺寸 | code=13745 const=2144 末端 `0xFF3EBD` ⇒ 槽余 **322 B** |

## 8.38 v0.36 上板：**A1 再次靠 RETRY 兜住；A2 卡在 `p=0`（分区判定恒 NONE）**

### 8.38.1 本版真正的新信息只有三行

```
[4776][VER] body crc now=ff1b951d want=77f35870
[9369][VER] stgA crc=77f35870
[9384][VER] body RETRY ok crc=77f35870      ← 第 6 次，RETRY 仍然每次都救回来
[9470][IAP] erase g=4 p=0 a=14000           ← 门禁 4（分区）+ p=0（PART_NONE）
[9490][APL] erase app fail rc=2             ← 96 个扇区一个都没擦
```

外加第一段的两处：

```
[265][BL]  [IAP] wr g=4 p=0 a=c000          ← BL 写 param
[0][APP]   [IAP] wr g=4 p=0 a=c000          ← APP 写 param
```

**三个调用点、两个工程、两个不同地址（0x0C000 / 0x14000），`p` 一律是 0。**

### 8.38.2 为什么这次能定性（两条硬证据）

1. **表本身是好的。** 同一份 hex 里 `[APP] stage=48K / app=48K` 是正确的 ——
   它们是 `ota_part_size(PART_STAGING/APP_IAP)` 查**同一张 `s_parts[]`** 得到的，
   说明 HCONST 寻址、下标计算、结构体字段读取都没问题。
2. **同一个构建里，同一个地址，两种结果。** `param_save(0xC000)` 的**擦除**过了
   （否则会打 `erase g=`），而它的**写入**没过（`wr g=4 p=0 a=c000`）。
   同一个地址、同一个 `ota_part_of()`，一次返回 2、一次返回 0
   ⇒ 问题不在"表/地址/分区"，而在**调用与返回值这一层**。

配合 v0.35（同一处擦除是**过的**）⇒ 这是老病：**布局一挪，受害函数就换一个。**
原 `ota_part_of()` 在循环里**每轮读两次 u32 形参**，而中间夹着从 HCONST 取
`s_parts[i].base/.size` 的动作（`?C?LIB_CODE?` 里那几支 24 位取数 helper）。
形参若停在 helper 不保存的寄存器里（铁律 11：R11/R12 不保存），第一轮之后
`iap_addr` 就是天文数字 ⇒ 全部比较为假 ⇒ 恒返回 PART_NONE。

### 8.38.3 v0.37 的修法：把"属于哪个分区"变成**纯立即数比较**

分区表在 IAP 空间里是**首尾相接、无缝隙**的一维铺排（`ota_partition.h`
里已有 6 条 `#error` 断言守着），所以"属于哪个分区"完全等价于
"跟下一条分区的起点比大小"。于是：

```c
ota_part_t ota_part_of(u32 iap_addr)
{
    static u32 s_a;                    /* 铁律 9/11：只读的临时值也落 static */
    s_a = iap_addr;                    /* 第一条语句就落地，此后不再读形参 */
    if (s_a <  OTA_PARAM_BASE)     return PART_STAGING;
    if (s_a <  OTA_RESERVED_BASE)  return PART_PARAM;
    if (s_a <  OTA_NCONST_BASE)    return PART_RESERVED;
    ...
}
```

- **零内存访问、零函数调用、零形参复读** —— 无论病根是"形参被踩"还是
  "局部 `i` 被踩"还是"返回值丢"，这个形状都没有可踩的东西。
- `ota_part_writable()` 同样改成常量集合判断（可写 = STAGING/PARAM/RESERVED/APP）。
- `ota_part_contains()` **整段删除**（它唯一的调用方 `flash_iap_check_range`
  已在 v0.36 删除；它自己就是"局部 `i` 压在自家 9 B 形参块尾"的那个）。
- map 实证：`?PR?OTA_PARTITION` 里 `ota_part_of` 的符号块**只有** `s_a`（0x1DB8），
  不再出现任何 HCONST 引用；`?XD?OTA_PARTITION` 从 11 B 缩到 **5 B**。

### 8.38.4 顺手加的一行自证探针（在 A2 开头）

```
[APL] pt a=%u w=%u      预期 a=5 w=1
```

- `a=5` ⇒ `ota_part_of(0x14000)` 正常；`a=0` ⇒ 返回值仍然丢（换写法再战）。
- `w=1` ⇒ `ota_part_writable(PART_APP_IAP)` 正常；`w=0` ⇒ 是它坏了。
- 后面每行 `erase g=` / `wr g=` 的 `p` 都拿这一行当基准对照，
  一次上板就能把"分区判定 / 可写判定 / 擦 / 写"四段切开。

### 8.38.5 v0.37 离线预言（`tools/predict_img_crc.py` 已入库，v0.36 复现无误）

| 量 | 期望值 |
|---|---|
| `A1 stg crc=` / `A1 image ready` / `[VER] STAGING ok` / `[APL] APPLY OK` | **`6322856a`** |
| 镜像头 CRC（只在 step=5 失败时打） | `7a4318f8` |
| `[APL] pt a= w=` | `5` / `1` |
| `[BL] bl head` | `2 0 27 ff` |
| BL 入口 / APP 入口 | `02 00 27 ff` / `75 84 00 7e` |
| BL 尺寸 | code=13684 const=2163 末端 `0xFF3E93` ⇒ 槽余 **364 B** |
| 交付 hex | `build/step37_part_of_const_BL_APP.hex`（md5 `414c43a0…`，两次构建一致） |

`tools/predict_img_crc.py` 用法：`python tools/predict_img_crc.py build/xxx.hex`
—— 正文 = APP 区 0x14000 起 49088 B（未写字节按 0xFF），头按 C251 **大端**拼 36 B。
拿 v0.36 的 hex 跑它得到 `77f35870` / `2b429498`，与当时的日志逐位一致，
所以这一版的 `6322856a` 可以当**判据**用，不是猜的。

### 8.38.6 验收判据（按顺序看这四行）

1. `[APL] pt a=5 w=1` —— 分区判定通了。
2. **没有** `[IAP] erase g=` 这一行 —— 96 扇区擦干净（约 222 ms）。
3. `[APL] copy off=` 每 8 KB 一行，一直到 `7fc0`（≈49152→实际最后一次是 0xbfc0 前一档）。
4. `[APL] APPLY OK len=49088 crc=6322856a` + `[BL] A2 DONE` ⇒ **A2 通了**，
   立刻断电，把 `BL_OTA_A2_FORCE_COPY` 改回 0。

## 8.39 v0.37 上板：**分区判定修好了（`pt a=5 w=1`），A2 从"擦都不动"推进到"搬了 28 KB"**

### 8.39.1 一次上板解决了四件事

| # | 证据 | 结论 |
|---|---|---|
| 1 | `[4760][APL] pt a=5 w=1` | `ota_part_of` / `ota_part_writable` 纯常量比较**成立** |
| 2 | 两段日志里**再无** `[IAP] wr g=4` / `[APP] param_init OK` / `seq=2→3` / `boot_try=1` | param 双槽写入恢复正常（此前 A 槽一直被门禁拒） |
| 3 | `[APP] A1 stg crc=6322856a` + `A1 image ready ... 6322856a` | 与离线预言**逐位一致**（`tools/predict_img_crc.py`） |
| 4 | 第二段 `[4714][VER] STAGING ok` 前**没有** `body crc now=` 失配行 | **A1 第一次首趟就对**（第 1~6 次都靠 RETRY 兜） |
| 5 | 无 `[IAP] erase g=`，`copy off=2000/4000/6000` 正常推进 | **擦除第一次真通**（96 扇区），搬运跑了 225 个 128 B 块 |

也就是：铁律 15 那条"分区查询不许查表、不许复读形参"生效了。

### 8.39.2 新的断点：`copy fail off=7080 rc=6`

```
[6839] copy off=6000      ← 已搬 24 KB，全对
[7178] copy fail off=7080 rc=6
```

先做排除法：

- **不是地址**：`0x7080` → IAP `0x1B080`，就在 APP 区中间；同一段地址前 225 个块全过。
- **不是门禁**：门禁只会给 `rc=2`（ADDR）。`rc=6` 只有三个来源：
  ① `raw(PROGRAM)` 被硬件拒（`CMD_FAIL`）；② 写后回读校验时 `raw(READ)` 失败；
  ③ 搬运时**源读**（STAGING）失败。
- **不是 CRC 层**：`rc=6` 不是 `OTA_ERR_CRC(7)`，数据比对还没开始就返回了。

⇒ 与 A1 那个"首趟算错、第二趟就对"是**同一家族**：某一次 IAP 命令没成功，
而 `flash_iap_raw()` 是一次性的 —— 失败就失败，没有第二次机会。

### 8.39.3 v0.38 的修法：单字节重试 + 整块重试，把"偶发"吃掉

`BSP/flash_iap.c`：

```c
#define IAP_RETRY   3U      /* 单字节 IAP 命令重试次数 */

/* 写：只在 CMD_FAIL 时重试才安全（命令被拒 ⇒ 位没动过 ⇒ 可以再写一次） */
for (s_try = 0; s_try < IAP_RETRY; s_try++)
    if (flash_iap_raw(IAP_CMD_PROGRAM, s_waddr + (u32)s_i, &s_dat) == OTA_OK) break;
if (s_try >= IAP_RETRY) g_iap_dbg[3]++;     /* 三次都没成才记账 */

/* 读：幂等，失败无条件重试 */
for (s_try = 0; s_try < IAP_RETRY; s_try++)
    if (flash_iap_raw(IAP_CMD_READ, s_raddr + (u32)s_i, &s_rbuf[s_i]) == OTA_OK) break;
if (s_try >= IAP_RETRY) s_bad = 1U;
```

读那一处同时是 **A1 首趟 CRC 偶发算错的正面修法**：以前靠上层再算一遍兜住，
现在在出问题的最小单元（一个字节）上就重试。

`OTA/ota_verify.c`（A2 搬运）：整块（源读 + 目标写 + 写后回读）失败就**整块重来 3 次**
—— 底层已按字节重试过，走到这里说明整块真没成；目标块里已写成功的字节再写同值是
幂等的（Flash 只能 1→0），所以整块重试安全。

失败时多打一行 `[APL] rd=%u rej=%lu`：

| 现象 | 含义 | 下一步 |
|---|---|---|
| `rd=0 rej>0` | PROGRAM 三次都被拒 | 只剩 IAP 时序 / 供电 —— 查 `IAP_TPS`、VCC |
| `rd=6 rej=0` | **源读**（STAGING）三次都失败 | 查 STAGING 那一带的擦写历史 / IAP 读时序 |
| 仍 `rc=7` | 写进去了但读回不一样 | 真·写不进（位没擦干净） |

### 8.39.4 v0.38 离线预言（APP 也变了）

| 量 | 期望值 |
|---|---|
| `A1 stg crc=` / `A1 image ready` / `[VER] STAGING ok` / `[APL] APPLY OK` | **`00b13ea4`** |
| 镜像头 CRC | `565ce791` |
| `[BL] bl head` | `2 0 27 ff` |
| BL 入口 / APP 入口 | `02 00 27 ff` / `75 84 00 7e` |
| 尺寸 | BL code=13768 const=2164 末端 `0xFF3EE8` ⇒ **槽余 279 B**；APP code=11760 |
| 交付 hex | `build/step38_iap_retry_BL_APP.hex`（md5 `6c389bcc…`，两次构建一致） |

> v0.37 的 `pt a= w=` 探针已完成使命（上板 `a=5 w=1`），v0.38 起删除腾空间；
> 分区判定的守门仍由擦除门禁的 `erase g=/p=` 承担。

### 8.39.5 这一轮的验收动作（**别急着重烧**）

1. 烧 v0.38 → 抓第一段（刷完机首次上电）。
2. **断电重上电** → 抓第二段：应看到 `[VER] STAGING ok crc=00b13ea4` → `copy off=` 一路到
   `[APL] APPLY OK len=49088 crc=00b13ea4` → `[BL] A2 DONE`。
3. 若又停在某个 `copy fail off=…`：**先别再烧，直接断电再上电一次**。
   `state` 仍是 3（PENDING_INSTALL），A2 会自动整段重来：
   - 断点**换了位置** ⇒ 确认是偶发，重试就能过，继续加厚重试即可；
   - 断点**还在 0x7080** ⇒ 是确定性故障，按 8.39.3 那张表查 rd/rej。
4. 看到 `APPLY OK` 立刻断电，把 `BL_OTA_A2_FORCE_COPY` 改回 0。

## 8.40 v0.38 上板：**A2 闭环成功 —— 双区 OTA 第一次真正生效**

```
[12630][APL] copy off=a000
[17728][APL] APPLY OK len=49088 crc=b13ea4
[17934][BL] A2 DONE: app updated, booting new firmware
[17982][BL] jump entry ...
[0][APP] ==== APP v0.1.0 ====  (P0 build: 1)
```

`APPLY OK` 之后 BL 跳进了**刚搬进去的那份固件**，APP 又重新跑了一遍自检并进入主循环。
**A1（造镜像）→ A2（擦/搬/回读校验）→ 跳转 → 新固件运行** 这条链第一次完整闭合。

### 8.40.1 日志核对：93/95 行和值成立

`tools/check_log_sum.py`（本版改成接受文件参数）逐行核对：

| 行 | 和值 | 结论 |
|---|---|---|
| 93 行 | 全部成立 | 可用作判据 |
| `10603][APL] copy off=4000` | want=22 got=23 | **丢了行首 `[`**，抄写损坏；内容由 2000/6000/8000/a000 互证 |
| `[17982][BL] jump entry hi=ff lo= 4000` | want=c4 got=4d | 某一位被换掉，抄写损坏；功能判据看 `via-base=75` / `via-const=75`（两行都成立） |

> 结论：**除了那两行，本段日志每一个数字都可当证据**。这也再次印证了那条规矩 ——
> `*HH` 对只证"线路没坏"，错就一定不能用来推理。

### 8.40.2 这一版钉死的五件事

| 证据 | 结论 |
|---|---|
| `pt a=5 w=1` 之后没有 `erase g=` / `wr g=` | 分区判定 + 擦 + 写三道门禁**同时**正常 |
| `copy off=2000…a000` 一路推进 | 搬运按 128 B 稳定走，块内"写+回读校验"没再被拒 |
| `APPLY OK len=49088 crc=b13ea4` | 整段回读校验通过 ⇒ **APP 区内容 = STAGING 内容** |
| `A2 DONE` 后 `jump`，APP 自检全绿 | 搬运后的固件**能跑**（不是"写对了但跑不起来"） |
| `seq=4 boot_cnt=0 boot_try=0`、`read app[0..3]=75 84 0 7e` | param 双槽写入持续正常；APP 入口字节没被搬坏 |

### 8.40.3 三条遗留账（本版收掉两条，第三条记账不修）

1. **版本账本从来没闭合**（本版修）
   ```
   [0][APP] running_ver = 0.0.0.0       ← 新固件在跑，账本还是旧版本
   [0][APP] pending_ver = 0.1.0.1       ← 待生效版本从没被消费
   [0][APP] dl offset=49088 total=49152 ← 永远差 64 B
   ```
   第三条最危险：`param_get_download()` 的判据是 `offset>0 && offset<=total`
   就算"有有效断点" ⇒ P1-B 真下载时会把 A1 的残留当续传起点，**从 49088 接着写**。
   根因有两条，都是"口径不一致"：
   - BL 搬完把 state 写成 `IDLE`，"我装好了"在参数里没留任何痕迹；
   - `dl_total` 含镜像头（64+49088），`dl_offset` 却只算正文（49088）。

2. **A1 首趟又错了**（本版用"合并调用点"治，见 8.40.4）
   `[4634][VER] body crc now=d6a47614 want=b13ea4` → `stgA crc=b13ea4` → RETRY ok。
   这是第 7 个互不相同的错值，v0.37 首趟对、v0.38 首趟错 ⇒ **随机**，不是确定性。

3. **时间戳会回退**（记账，不修）
   `[17934] A2 DONE` 之后紧跟 `[17454] == summary`，两行和值都成立 ⇒ 固件真的打了
   这两个数。`log_now_ms()` 已用 `EA=0` 取时间戳，所以不是撕裂读。
   疑点在"`param_save(1)` 那 480 ms 里 Timer0 的计数值"，但**它不影响任何功能判据**，
   而且改时间基准会牵动看门狗与所有超时 —— 留到 P1-B 的定时器重构一起看。

### 8.40.4 v0.39 改动（A3 提交 + 生产路径）

| # | 文件 | 改动 |
|---|---|---|
| 1 | `OTA/ota_verify.c` | `apl_crc()` 从"仅 BL"提升为**两工程唯一的读循环**；`ver_body()` 删掉自带的 90 行循环改调它 |
| 2 | `OTA/ota_verify.c` | `BL_OTA_A2_FORCE_COPY` → **0**（回到幂等：已是目标镜像就不擦不写） |
| 3 | `Project/Bootloader/boot_main.c` | `BL_OTA_A1_FORCE` → **0**（只有 `state==PENDING_REBOOT` 才校验） |
| 4 | `Project/Bootloader/boot_main.c` | 搬运成功后 `ota_state = OTA_ST_COMMITTED`（不再写 IDLE） |
| 5 | `APP/app_main.c` | 新增 `app_ota_commit()`：`COMMITTED` ⇒ `running_ver←pending_ver`、`pending` 清零、`dl_*` 清零、`boot_try=0`、`ota_succ_cnt++`、`state=IDLE` |
| 6 | `APP/app_main.c` | A1 落参数时 `dl_offset` 与 `dl_total` 统一成 `64+49088=49152` |
| 7 | `APP/app_main.c` | 心跳把 `ver=%s`（编译期字符串）改成从 param 念 `state/succ` 与 `run=x.y.z.b` |

**第 1 条为什么能治 A1 首趟**：两趟算的是同一段 Flash、同一个 CRC 引擎、同样的分块与
喂狗节奏，唯一差别只剩调用点形状 —— 首趟在 `ver_body`（形参对象 `s_off/s_n`），
二趟在 `apl_crc`（`s_aoff/s_an`）。`crc32_update(u32,u8*,u16)` 合计 9 B > 8 B，
**必然生成 ?BYTE 参数块**，而 L251 只按 6~7 B 步进 ⇒ 谁的局部段排在块尾谁中招。
两个调用点只有一个中招，正好解释"首趟错、二趟对"。
⇒ 不再猜哪一块坏了，**让首趟走那条已被证明正确的调用点**；RETRY 从"另一份实现"
降级成"同一条路的第二次尝试"。

**第 4/5 条为什么不需要新字段**：`COMMITTED` 在 `ota_fsm.h` 里本来就是
"已生效、待 APP 确认"的语义，直接拿来当交接点。掉电落在 BL 保存与 APP 提交之间也安全 ——
state 停在 `COMMITTED`，BL 下次上电不校验直接跳（`COMMITTED != PENDING_REBOOT`，
连 CRC 都不算），APP 再提交一次，幂等。

### 8.40.5 v0.39 的期望值

| 期望值 | 出现位置 |
|---|---|
| **`571d1b2b`** | `A1 stg crc=` / `A1 image ready` / `[VER] STAGING ok` 三处同值 |
| `fbc8d5f8` | 镜像头 CRC（只在 step=5 失败时打） |
| `app already current crc=571d1b2b` | **新的一行**：幂等命中，说明这次**没擦没写** |
| `ota_state=4` | APP 自检读到的 state（= COMMITTED，BL 刚写的） |
| `run=0.1.0.1` / `commit rc=0 succ=1` | A3 提交成功 |
| `state=0 succ=1` / `run=0.1.0.1` | 心跳，账本闭合后的稳态 |

### 8.40.6 验收动作（三步上电，全程不许跨步烧 hex）

1. **烧 v0.39**（ISP 全片擦除 ⇒ param 必为空）。抓第一段：
   `param empty/corrupt` → `seq=1 … state=0` → 跳 APP → `param_init OK` →
   `A1: build image` → `A1 image ready len=49088 crc=571d1b2b` → **停机等断电**。
2. **断电再上电**。抓第二段，按顺序看这五行：
   ```
   [BL] pending install -> verify staging (read-only)      ← state=3，BL 校验
   [VER] STAGING ok len=49088 crc=571d1b2b                 ← 若前面没有 body crc now=，说明"合并调用点"生效
   [APL] app already current crc=571d1b2b                  ← 幂等命中，这次没擦没写
   [BL] A2 DONE: app updated, booting new firmware
   [APP] ---- A3 commit: new firmware confirmed ----        ← A3
   [APP] run=0.1.0.1 / commit rc=0 succ=1
   ```
3. **再断电上电一次**（抓第三段，这一段的全部意义是"什么都不发生"）：
   没有 `pending install`、没有 `A1: build image`、没有 commit，
   直接 `boot_try=0 ota_state=0` + `dl offset=0 total=0` + 心跳 `state=0 succ=1 run=0.1.0.1`。
   这一段正常才说明"生产路径"真的干净 —— 之前 `BL_OTA_A1_FORCE=1` 时永远看不到它。

> 第 3 步之后就可以把 `APP_A1_SELFTEST` 改回 0（A1 造镜像的使命结束，
> 以后造镜像交给真正的 HTTP 下载器）。

## 8.41 v0.39 上板：**A3 提交成功 —— 版本账本闭合；首趟 CRC 的"调用点"解释被证伪**

### 8.41.1 两段日志（`step39_a3_commit_BL_APP.hex`，md5 `a60b1020…`）

第一段（刷完机首次上电，STC-ISP 全片擦除 ⇒ `param empty` 正常）：

```
[APP] A1 stg crc=571d1b2b
[APP] A1 image ready len=49088 crc=571d1b2b      ← 与离线预言逐位一致
```

第二段（断电重上电，A2 + A3 一次走完）：

```
[152][BL] seq=2 boot_cnt=0 boot_try=0 state=3
[174][BL] pending install -> verify staging (read-only)
[4724][VER] body crc now=b8b173ec want=571d1b2b   ← 第 8 个互不相同的错值
[9270][VER] stgA crc=571d1b2b
[9285][VER] body RETRY ok crc=571d1b2b
[9302][VER] STAGING ok len=49088 crc=571d1b2b
[9328][BL] A2: apply staging -> app (erase+copy+verify)
[13872][APL] app already current crc=571d1b2b     ← 幂等命中，不擦不写（4544 ms = 一次全量读）
[14074][BL] A2 DONE: app updated, booting new firmware
[0][APP] boot_try=0 ota_state=4                    ← 4 = COMMITTED（BL 的交接痕）
[0][APP] dl offset=49152 total=49152               ← 口径已统一
[0][APP] ---- A3 commit: new firmware confirmed ----
[0][APP] run=0.1.0.1
[0][APP] commit rc=0 succ=1
[0][APP]   state=0 succ=1 / run=0.1.0.1            ← 心跳里账本已闭合
```

**A1 造镜像 → BL 校验 → 幂等判定 → COMMITTED 交接 → APP 自检 → A3 提交 → IDLE**
整条链闭合。`running_ver=0.0.0.0 / pending_ver=0.1.0.1 / dl offset=49088` 那三个遗留账
（见 §8.40.3）全部收掉：现在 `run=0.1.0.1`、`dl offset=49152=total`、`state=0`。

### 8.41.2 行尾和值核对：90 行成立，5 行抄写损坏

| 行 | 判读 |
|---|---|
| `[93[BL] selftest: …` | 掉了 `]`（两段同样坏法），内容与 §8.40 一致，不影响判据 |
| `[BL] jump entry hi=g0000 ff lo=U04000` | 时间戳与竖线丢失；功能判据看 `via-base=75` / `via-const=75` |
| `[ 4095][BL] == summary: …` | `14095` 掉了一位，且丢了 `[BL]` 前的分隔 |
| `[14124][BL] jump entry hi=v00000ff lo= 04000` | 同上 |

> **新增观察**：第一段的 APP 从 `[APP] param blank/…` 起到 `A1: next boot…` 这一整段
> **全部丢了 `[ms]` 前缀**（和值仍然逐行成立 ⇒ 是真的没打前缀）。这属于日志层
> 已知缺陷的第三种表现（另两种：时间戳回退、单字符错位），**不影响功能判据**。
> 注意：和值是在固件内算的，所以"和值错"只可能来自抄写；"少了前缀"则说明
> 固件真的没打 —— 这两种必须分开看。

### 8.41.3 ★ v0.39 的核心假说被证伪

v0.39 的判断是：首趟错、第二趟对 ⇒ 两个调用点只有一个被 `?BYTE` 参数块踩。
第一段日志里 `body crc now=b8b173ec` 说明**改成同一个调用点之后首趟照样错**。

⇒ 真正的分界是 **"本上电周期的第几次 `apl_crc` 调用"**：

| 调用序号 | 位置 | 结果 |
|---|---|---|
| 第 1 次 | `ver_body()` → `apl_crc` | **错**（8 次采样中 7 次错） |
| 第 2 次 | RETRY → `apl_crc` | 对 |
| 第 3 次 | `ota_apply_staging()` 幂等判定 | 对 |

同一个函数、同一段 Flash、同一个上电周期，第 3 次也一字不差 ⇒ 不是调用点，
而是"第一次"这个属性本身。

### 8.41.4 离线把"结构性损坏"全部排除（`tools/explain_bad_crc.py`）

拿 `bad=b8b173ec`、`good=571d1b2b` 和目标 hex，穷举了这些形态，**全部零命中**：

| 类 | 范围 |
|---|---|
| 单块 := 前/后邻块 | 384 块 |
| 单块 := 首块 / 末块 / 全 FF / 全 00 | 384 块 × 4 |
| 单字节改值 | 49088 B × 2 |
| 整段滞后 / 跳过 W 块 | 约 15 万组合 |
| 起址整体平移 | ±600 B（覆盖"减去 64 B 头"这类错算） |
| 长度 ±2 KB（多算按 FF 补 / 少算截断） | 4096 组合 |
| 初值换成陈旧值（0 / FFFFFFFF / 头 CRC 等） | 5 种 |
| 连续多块 := 全 FF | 约 15 万组合 |

> 校验：本机 `zlib.crc32` 实测 3.4 GB/s，上述穷举确实跑满（标定已记在脚本注释外）。
> ⇒ 错法**不是**"某一块被换成别的数据"，必须上板把"第一处偏离"夹出来。

### 8.41.5 v0.40：首趟 4 KB 分段自证探针

`OTA/ota_verify.c` 的 `apl_crc()` 里，只在**本上电周期首趟**（`s_acall == 0`）打印
每个 4 KB 边界的累计 CRC：

```
[4724][VER] body crc now=???????? want=571d1b2b
[  x][VER] p1 o=1000 c=05f65c99
[  x][VER] p1 o=2000 c=cfe40c15
...
[  x][VER] p1 o=b000 c=7f374f7f
```

期望值由 `tools/predict_img_crc.py` 直接给出（新增「4 KB 累计 CRC」一节）。
**第一个与期望不符的行 ⇒ 坏数据就在它前一个 4 KB 窗口里**（32 个 128 B 块）。
拿到窗口后，用该窗口的累计 CRC 当靶子只在这 32 块里穷举，就能定位到具体块。

★ 本版 APP 一个字没改 ⇒ 镜像 CRC 仍是 **`571d1b2b`**，与 v0.39 完全相同。
于是这一轮还顺带回答一个关键问题：
**同样的数据、不同的 BL，如果首趟又打出 `b8b173ec` ⇒ 确定性；打出别的值 ⇒ 随机。**

尺寸：BL `code=13826 const=2207` 末端 `0xFF3F4D` ⇒ **槽余 179 B**（探针花了 120 B）；
APP `code=12353`（未变）。交付 `build/step40_p1_ckpt_BL_APP.hex`，md5 `089e5017…`（两次构建一致）。



## 8.42 v0.40 上板：探针**自己被自己关了**（静变量上电是 0xFF，不是 0）

### 8.42.1 日志事实（和值 78 行成立 / 7 行抄写损坏）

```
[4803][VER] body crc now=da3e5fe want=571d1b2b      ← 第 9 个互不相同的错值
[9430][VER] stgA crc=571d1b2b
[9445][VER] body RETRY ok crc=571d1b2b
[14121][APL] app already current crc=571d1b2b
[14325][BL] A2 DONE / [0][APP] A3 commit run=0.1.0.1 / commit rc=0 succ=1
```

`body crc now` 那行和值 `*9f` 核对通过，是真值（`%lx` 抑制前导零，真值 `0da3e5fe`）。
A3 又一次闭环 ✔。

**但 `p1 o=…` 一行都没有。**

### 8.42.2 根因：`static` 计数器的上电值不是 0

v0.40 的探针用 `s_acall == 0` 判"首趟"，而 `s_acall` 是**没写初值**的
`static u8 xdata`。本工程 BL 侧静态变量的上电值是 **0xFF**（铁律 13 的另一面：
不只是"带常量的初值不生效"，**连隐式的零初始化也不生效**）。
⇒ 第一次调用时 `s_acall` 已经是 0xFF，`s_acall == 0` 为假 ⇒ `s_afirst = 0` ⇒
探针静默关闭。日志里"什么都没多打"就是这个原因，不是探针逻辑错。

**判据**：凡"跨调用要保持"的计数器/标志，一律在**确定的入口**显式清零，
不许依赖零初始化。

### 8.42.3 这一版顺带拿到的对照结果

v0.39 与 v0.40 的 **A1 镜像完全相同**（APP 一字未改，都是 `571d1b2b`），
而 BL 不同 ⇒ 首趟错值 `b8b173ec`（v0.39）≠ `0da3e5fe`（v0.40）。

这只说明"错值不是数据的函数"，**不能**说明随机 —— 因为 BL 布局也变了。
要区分"确定性/随机"必须**同一份 hex 重复取样**，所以 v0.41 把
`BL_OTA_A1_FORCE` 临时改回 1：每上一次电都跑一遍首趟，一分钟能拿 3~4 个样本。

### 8.42.4 v0.41 改动

1. `ota_verify_staging()` 入口 `s_acall = 0U;`（显式清零，见 8.42.2）。
2. 探针条件加一条 `s_aoff == 128`：把"第一处偏离在不在 0..4096"再切成两半。
   `o=0080` 就错 ⇒ 第一次读整段就已经脏；`o=0080` 对而 `o=1000` 错 ⇒ 坏在中间。
3. `BL_OTA_A1_FORCE = 1`（★临时，只为同构建重复取样，定位完立刻改回 0）。

尺寸：BL `code=13824 const=2207` 末端 `0xFF3F4B` ⇒ **槽余 181 B**；APP 未变
（`code=12353`，镜像 CRC 仍 `571d1b2b`）。
交付 `build/step41_p1_ckpt2_BL_APP.hex`，md5 `d1a5c0d7…`（两次构建一致）。

### 8.42.5 期望值（`tools/predict_img_crc.py` 直接给出）

```
p1 o=0080 c=4f17d50c     p1 o=5000 c=c1d05e1e     p1 o=9000 c=a057da48
p1 o=1000 c=05f65c99     p1 o=6000 c=4f828115     p1 o=a000 c=62f1e81e
p1 o=2000 c=cfe40c15     p1 o=7000 c=ef113d04     p1 o=b000 c=7f374f7f
p1 o=3000 c=3bcca8a8     p1 o=8000 c=fa1cee34
p1 o=4000 c=95d0f00c
```

**第一处与上表不符的行 ⇒ 坏数据就在它前一个 4 KB 窗口里（32 个 128 B 块）。**
拿到窗口后，用该窗口自己的累计 CRC 当靶子只在那 32 块里穷举即可定位到块。

### 8.42.6 这一次的判据顺序

1. 烧 v0.41 → 第一段（STC-ISP 全片擦除，`param empty` 正常；FORCE=1 会先对一个空的
   STAGING 校验一次，打 `STAGING fail rc=12 step=0`，属正常）。
2. APP 造完镜像（`A1 image ready crc=571d1b2b`）→ **断电重上电**：拿到 13 行 `p1`。
3. **再断电上电 2 次**，每次再拿一组 `p1` + 一行 `body crc now=`。
   **判据：3 个首趟错值是否相同。** 相同 ⇒ 确定性（离线穷举有效）；
   不同 ⇒ 随机（时序/硬件，别再穷举）。

## 8.43 v0.41 上板：首趟偶发错的**真身**被夹出来了（偏移 0x2800 单块未刷新）

### 8.43.1 日志事实（和值 58/60 成立，`p1` 那 9 行**全部可信**）

```
[241] [VER] p1 o=80   c=4f17d50c
[622] [VER] p1 o=1000 c=5f65c99      ← %lx 抑制前导零，真值 0x05f65c99
[1012][VER] p1 o=2000 c=cfe40c15
[1403][VER] p1 o=3000 c=b254f1b      ← 真值 0x0b254f1b，**第一处偏离**
[1793][VER] p1 o=4000 c=547c7e6e
[2193][VER] p1 o=5000 c=5522ed5a
[2583][VER] p1 o=6000 c=42fdd2e8
[2975][VER] p1 o=7000 c=9d218dd      ← 真值 0x09d218dd
[3375][VER] p1 o=8000 c=98a95e8f
（之后 BL 自己复位，重新 `[0][BL] boot`，`seq=3` 不变 ⇒ param 没落盘）
```

两行 BAD 都是已知的日志层缺陷，内容由邻行互证：
`[BL] para  empty/corrupt, defaults created`（丢 `[ms]` 前缀 + 双空格）、
`[BL] jump entry hi= 00ff lo=v00040T0`（串头错位，功能判据看 `via-base=75`）。

★ **`[14{[BL] ==== BL v0.1 (P0) ====*6d` 的和值是对的** ⇒ 固件真的打出了 `{`。
这是"和值对但内容怪 ⇒ 怀疑固件（`s_line` 内部）"的又一例，不是抄写。

### 8.43.2 先纠正一个误读：**不是上电 5 次，是第 9 行自己重启**

一眼看上去像"同一份 hex 上电五次、结果一字不差"，实际上是**一次上电内反复重启**：
BL 在 `o=8000` 之后跑飞 → WDT 复位 → `seq` 仍是 3、`state` 仍是 3 → 又走
`pending install` → 又打一遍同样的 9 行。

判据：**v0.40（无探针）的 `body crc now=da3e5fe` 打在 4803 ms**，而整段 12 个
4 KB 窗口需要约 4550 ms ⇒ 那一版**跑完了整段**。v0.41 只到 3375 ms。
⇒ **复位是"在 `apl_crc` 里打日志"这个动作引入的**，不是原有 bug。

### 8.43.3 离线定位：一个 4 KB 窗口 + 唯一解

上一行对（`o=2000`）、这一行错（`o=3000`）⇒ 坏数据在 **0x2000..0x3000**。
新工具 `tools/locate_bad_window.py <hex> <good_prefix_crc> <bad_window_crc> [lo] [hi]`：

```
窗口 0x2000..0x3000（4096 B / 32 块）
自检：good 窗口算出的末值 = 3bcca8a8   ← 与离线靶子一致，语义正确
--- 命中 1 条 ---
  A 块#16（偏移 0x2800）:= 前一块
```

把它当真：`body[0x2800:0x2880] := body[0x2780:0x2800]`，重算九个探针：

| 假设 | o=3000 | o=4000 | o=5000 | … | o=8000 | 合计 |
|---|---|---|---|---|---|---|
| 只有 0x2800 := 0x2780 | ✔ | ✔ | ✔ | ✔ | ✔ | **9/9 全中** |
| 从 0x2800 起整段滞后一块 | ✘ | ✘ | ✘ | ✘ | ✘ | 6/9 不符 |
| 完全正确（对照） | ✘ | ✘ | ✘ | ✘ | ✘ | 6/9 不符 |

⇒ **整段 49088 B 里，只有第 80 块（偏移 0x2800）读回来的是前一块的内容，其余 382 块全对。**

### 8.43.4 为什么"读到前一块"只能是"没写进 s_buf"

`flash_iap_read()` 的循环里第一句是 `s_rbuf[s_i] = 0xFF;`（前置填充）。
只要循环跑过一次，`s_buf` 就不可能是**完整的上一块**（会至少带 0xFF）。
所以只剩两种可能，而它们都指向同一处：

1. 循环一次都没跑（`s_rlen` 那一次是 0）—— 但 `if (s_rlen == 0U) return PARAM` 会拦；
2. 数据写到了别处（`s_rbuf` 那一次不是 `s_buf`）。

无论哪种，**函数都返回了 OTA_OK**（`ver_body` 检查过 rc，日志里没有 `body` 之外的错误行）。
⇒ 与铁律 14 三次验证（v0.30 读门禁 / v0.35 擦门禁 / v0.36 写门禁）**完全同型**：
**形参 4+3+2 = 9 B > 8 B 的那个函数，就是出事的那个。**
`flash_iap_read()` 是整条链上最后一个这样的中间层，而且它的
`s_raddr / s_rlen / s_rbuf` 是**跨函数共享、不可重入**的静态量 ——
首趟前面刚跑完 param 装载 + 8 步头校验（几十次 read），
只要有一次把这仨写歪，整块就静默变成上一块。

### 8.43.5 v0.42 改动：这一条通路不再经过它

1. `apl_crc()` 的读块**就地内联**：直接调 `flash_iap_raw(IAP_CMD_READ, addr, &s_buf[i])`，
   只用本模块自己的 `s_i / s_an / s_arc / s_rt`，不共享任何别的函数的静态量。
   重试语义与 v0.38 一致（读幂等，单字节最多 3 次），每 32 B 喂狗。
2. 地址门禁**一次性**做在入口（`s_asz = flash_iap_eeprom_size()`，铁律 15 的立即数比较），
   循环内不再判 —— 原来这条检查是 `flash_iap_read` 替我们做的。
3. **撤除全部逐窗口探针**（v0.40/v0.41 的 `p1 o=…`）。理由见 8.43.2：
   13 行日志在 BL 深调用链上是有代价的。正常路径回到**零打印**。

尺寸：BL `code=13862 const=2186` 末端 `0xFF3F5C` ⇒ **槽余 164 B**；APP `code=12353`
**一字未改** ⇒ 镜像 CRC 仍 `571d1b2b`、头 `fbc8d5f8`。
交付 `build/step42_aplcrc_inline_BL_APP.hex`，md5 `0bd46e0a…`（两次构建一致）。

### 8.43.6 这是一个干净的对照实验

**同一张镜像、不同的 BL**：如果 v0.42 首趟就对了 ⇒ 内联确实修掉了它；
如果还错 ⇒ 根因在 `flash_iap_raw` 的读时序（那时错块位置若仍是 0x2800，
说明与 BL 布局无关，是地址/数据相关）。

**判据只有一条：`[VER] STAGING ok` 之前没有 `body crc now=`。**
（RETRY 网仍在，别删 —— 它每轮都在替我们保住功能判据。）

## 8.44 v0.42 内联读块反噬：漏了外层临界区（→ v0.43）

### 8.44.1 v0.42 上板现象：RETRY 网第一次被击穿

v0.42 把 `apl_crc` 的读块从 `flash_iap_read()` 改成**内联逐字节 `flash_iap_raw()`**，
想绕开"0x2800 单块未刷新"的中间层。结果更糟：

```
[9679] body crc now=2afa96e9 want=571d1b2b   ← 首趟错
[19144] stgA crc=e4db7301                    ← 第二趟也错！
[19161] STAGING fail rc=7 step=9             ← RETRY 没救回来
```

连续两次上电，四个错值全不同（`2afa96e9/e4db7301/c79dc6c1/b2eca1bf`）⇒ 随机。
这是 v0.38 起"第二趟必对"的铁律**第一次失效**。

### 8.44.2 根因：内联循环漏了 flash_iap_read 的外层临界区

`flash_iap_raw()` 内部**每次调用**都自己 `critical_enter()`（读 EA→EA=0）再
`critical_exit()`（恢复 EA）。`flash_iap_read()` 之所以稳，是因为它**外层先
enter 一次**，整块 128 字节循环期间 EA 恒为 0，内层 raw 的 exit 只会"恢复成 0"，
1ms 定时器中断全程插不进来。

v0.42 的内联循环**漏了这个外层 enter** ⇒ 每读一个字节，raw 的 exit 就把 EA
恢复成 1 ⇒ 下一字节之前 Timer0 中断插入，打断 IAP 读时序 ⇒ 读回的 IAP_DATA 是
随机错误值，且**散布在多个字节**。

这解释了全部四个现象：错值随机、第二趟也错、单块/单字节穷举全不命中（多字节
散布，不是"某块被换掉"）。

### 8.44.3 v0.43 修复：恢复整块一个外层临界区

在内联循环外加 `s_ea = flash_iap_critical_enter();` … 循环 …
`flash_iap_critical_exit(s_ea);`，与 `flash_iap_read()` 语义等价。内层 raw 的
enter 读到 EA=0、exit 恢复 EA=0，无副作用。失败路径也先 `exit` 再上抛。

### 8.44.4 铁律 19：多字节 IAP 读必须整段一个 EA=0 临界区

`flash_iap_raw()` 的一次 enter/exit 只保护**一次命令**；任何"循环调 raw 做多字节
读/写"的代码，都必须**外层再包一道 enter/exit**，否则字节之间中断会插入 IAP
操作。这不是新问题——铁律 8 已写过"多字节 IAP 序列必须整段 EA=0"，v0.42 是
内联重写时把它漏了，属于同一铁律的又一次复现。

### 8.44.5 交付

`build/step43_iap_critical_BL_APP.hex`（md5 `000fe851…`，两次一致）；BL
code=13894 末端 `0xFF3F7C` ⇒ 槽余 132 B；APP 未改 ⇒ 镜像 CRC 仍 `571d1b2b`、
头 `fbc8d5f8`。判据仍是唯一一条：`STAGING ok` 前没有 `body crc now=`。

