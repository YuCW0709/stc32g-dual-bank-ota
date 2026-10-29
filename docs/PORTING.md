# 移植方案：从 STC32G12K128 迁到 GD32F303 / STM32F103 等 Cortex-M

> 写这份文档的目的不是"再抄一遍代码"，而是先说清**哪些东西是平台债、哪些是资产**，
> 再给一条**不会把已经趟过的坑重新趟一遍**的路线。
> 当前状态：v0.6.7，STC32G 上 BL+APP 双区 OTA 已闭环到 A3，卡点只剩 4G 链路本身。

---

## 1. 先给结论

| 结论 | 依据 |
|---|---|
| **业务层可以整段搬**（`ota_fsm.c` / `http_ota.c` / `crc32.c` / `ota_verify.c` 的逻辑） | 它们只依赖 6 个原语：擦、写、读、发、收、毫秒。全部通过 `flash_iap_*` / `at_*` 调用，没有一处直接碰寄存器（除 `uart_at.c`） |
| **驱动层必须重写**（`uart_at.c` 的收发、`flash_iap.c` 全部、`bsp_*`） | 直接读 `S3CON/S3BUF/T2H/IAP_CONTR` 等 SFR |
| **最大的收益不是主频，是"丢字节问题直接消失"** | Cortex-M 上串口用 **DMA + IDLE 中断**接收，CPU 不需要在 87 µs 内响应；这条链路上过去 7 轮的返工，本质是"8051 单接收寄存器 + 轮询"的结构缺陷 |
| **第二大的收益是一批铁律作废** | 铁律 3/5/7/9/12/14 全部源自觉 C251 的 LARGE 模型 + 8 B 参数块 + code 取址 24 位。GCC/MDK 上这些约束不存在，代码可以写得像正常人写的代码 |
| **别一次全搬** | 先做"在 GD32 上把 AT 拉通 + Range 分片下载跑完"，再做 BL 双区与回滚。反过来的话，会同时调试两个未知量 |

---

## 2. 现状盘点：谁绑定平台

代码总量约 **12 000 行**，按"能不能直接搬"分成三档：

### A 档 · 几乎零改动（约 2 400 行）

| 文件 | 行数 | 说明 |
|---|---|---|
| `OTA/ota_fsm.c` | 325 | 下载状态机、进度上报。只调 `http_ota_*` / `param_*` / `flash_iap_erase_range` |
| `DRV/http_ota.c` | 1 012 | AT 命令序列、Range 分片、断线自愈。只调 `at_cmd_wait` / `at_binary_*` / `flash_iap_*` |
| `COMMON/crc32.c` | 154 | 查表 CRC32，与平台无关（注意：表不能放 code 段是**STC 专属**约束，见 §5） |
| `COMMON/log.c` 的格式化部分 | ~300 | `vsnprintf` 风格输出，只依赖一个 `putc` |
| `OTA/ota_partition.h` | 336 | **只是数字**，改常量即可 |

### B 档 · 重写但接口不变（约 2 300 行）

| 文件 | 行数 | 要重写什么 |
|---|---|---|
| `BSP/flash_iap.c` | 1 059 | 全部。STC 的 IAP 是"写命令 → 触发 → 读 32 B"的寄存器序列；Cortex-M 是 **memory-mapped + FMC 编程函数**，逻辑会缩到 200 行以内 |
| `BSP/bsp_uart.c` | 254 | UART1 日志口 → 换成 USART0 + DMA |
| `BSP/bsp_timer.c` / `bsp_gpio.c` / `bsp_clock.c` | ~350 | 换 SysTick + GPIO 时钟使能 |
| `OTA/ota_jump.c` | 135 | STC 的跳转要"绑中断向量到 APP 的向量表"；Cortex-M 只要 **设 MSP + 设 VTOR + 跳 Reset_Handler**，会简单很多 |

### C 档 · 完全是平台债（约 2 600 行，**建议重写而不是移植**）

`DRV/uart_at.c`（2 066 行）里至少 60% 是**为了绕开"接收寄存器只有 1 个 + 轮询不及时就丢字节"**而写出来的补偿逻辑：

- 1:1 交错轮询（v0.6.5/v0.6.6）
- 宽松判据 `at_lenient_verdict` / 尾巴判据 / 滚动子串匹配（v0.6.0~v0.6.4）
- `at_link_test` / `at_raw_at` / `at_line_diag` / `at_listen` / `edges/low` 统计
- 波特率 12 档扫描（模组其实写死 115200）

到 Cortex-M 上，**这些全部可以删掉**：DMA 环形缓冲 + IDLE 中断之后，"收到几个字节"永远是全部，
"OK 被吃掉一个 K"这种事不会发生。保留一个**环形缓冲 + 行解析**就够了，估计 400 行。

---

## 3. 唯一需要的新东西：一层薄 HAL

移植能不能一次成功，取决于有没有把下面 6 组接口先钉死。它们现在散在各处，**先抽出来**，再动平台。

```c
/* ot_port.h —— 每个平台实现一份，业务层只 include 这个 */

/* ① Flash（地址一律"分区相对偏移"，与 ota_partition.h 口径一致） */
u32       ot_flash_page_size(void);                    /* STC 512 B / GD32 2 KB */
ota_ret_t ot_flash_erase(u32 off, u32 len);            /* len 必须是页的整数倍 */
ota_ret_t ot_flash_write(u32 off, const u8 *buf, u16 len);
ota_ret_t ot_flash_read (u32 off, u8 *buf, u16 len);
u8        ot_flash_erase_value(void);                  /* 0xFF */

/* ② 串口（AT 通道）—— ★ 必须是"DMA/中断收" */
ota_ret_t ot_uart_open(u32 baud);
void      ot_uart_set_baud(u32 baud);
void      ot_uart_putc(u8 c);
u16       ot_uart_read(u8 *dst, u16 max);              /* 非阻塞，取环里的 */
u16       ot_uart_avail(void);

/* ③ 时间 */
u32  ot_tick_ms(void);                                 /* 单调递增，中断自增 */
void ot_delay_ms(u16 ms);                              /* 内部喂狗 */

/* ④ 看门狗 */
void ot_wdt_feed(void);

/* ⑤ 跳转 / 复位 */
void ot_jump_to(u32 app_base);
void ot_reset(void);

/* ⑥ 临界区 */
u32 ot_enter_critical(void);                           /* 返回原状态 */
void ot_exit_critical(u32 st);
```

对应关系（现在 → 将来）：

| 现在 | 将来 |
|---|---|
| `flash_iap_erase_sector/range` `flash_iap_write/read` | `ot_flash_*` |
| `at_uart2_putc/getc` `at_uart2_baud` | `ot_uart_*` |
| `at_t0_poll` + `s_at_ms` | `ot_tick_ms` |
| `bsp_wdt_feed` | `ot_wdt_feed` |
| `ota_jump_to_app` | `ot_jump_to` |

**做完这一步，`http_ota.c` 与 `ota_fsm.c` 一个字符都不用改。**

---

## 4. GD32F303 具体落地方案

### 4.1 分区表（以 256 KB Flash / 2 KB 页 = 128 页为例）

GD32F303 的页大小按容量分档：**≤128 KB 用 1 KB 页，256 KB / 512 KB 用 2 KB 页**
（[数据手册分档](https://blog.csdn.net/weixin_28738397/article/details/166110156)，页擦除典型 48 ms）。
下面按 2 KB 页排，**边界全部页对齐**：

| 区块 | 页 | 地址 | 大小 | 权限 |
|---|---|---|---|---|
| BL | 0 ~ 13 | `0x08000000` | 28 KB | 只读（运行期） |
| PARAM A | 14 | `0x08007000` | 2 KB | 读写 |
| PARAM B | 15 | `0x08007800` | 2 KB | 读写（A/B 双份 + 序号 + CRC） |
| STAGING | 16 ~ 63 | `0x08008000` | 96 KB | 读写（下载区） |
| APP | 64 ~ 127 | `0x08020000` | 128 KB | 只读（运行期），由 BL 写 |

要点：

- **不再需要"512 B 扇区"的概念**，擦除单位变成 2 KB。下载片大小建议直接取 **2048**（正好一页），
  与现在的 `HTTP_READ_CHUNK=1024` 不同——但**不要超过模组 3356 B 的缓冲上限**（AT 手册 §13.7）。
- 页擦除 48 ms × 24 页（48 KB）≈ **1.2 s**，所以"整包重下"前的那次全擦会明显变慢，
  `ot_flash_erase` 内部必须每页喂一次狗。
- STAGING 96 KB ≥ APP 128 KB 吗？**不满足**。两条路选一条：
  ① 把 APP 压到 96 KB（页 64~111），后 16 页留空或做备份区；
  ② 采用"**单区 + 备份页**"方案：APP 只保留当前运行的那一份，新镜像先在 STAGING 校验通过，
     再逐页搬过去；搬完前 BL 不允许跳转。这套就是现在 STC 上已经验证过的 A2 流程，**建议直接复用**。
  推荐 ①（留 16 页余量），因为 128 KB 的 APP 在这类产品里通常用不满。

### 4.2 向量表与跳转

BL 在 `0x08000000`，APP 在 `0x08020000`：

```c
/* APP 侧：main() 第一件事（或 SystemInit 里） */
SCB->VTOR = 0x08020000UL;

/* BL 侧：跳转 */
static void jump_to_app(u32 base)
{
    u32 sp = *(volatile u32 *)(base);
    u32 pc = *(volatile u32 *)(base + 4U);
    if ((sp & 0xFFF00000UL) != 0x20000000UL) return;   /* 栈顶不在 SRAM：镜像没写对 */
    __disable_irq();
    /* 关掉 SysTick、清 NVIC 使能、关 DMA —— 不然新 APP 会收到"幽灵中断" */
    SysTick->CTRL = 0;
    for (u32 i = 0; i < 8; i++) { NVIC->ICER[i] = 0xFFFFFFFFUL; NVIC->ICPR[i] = 0xFFFFFFFFUL; }
    SCB->VTOR = base;
    __set_MSP(sp);
    ((void (*)(void))pc)();
}
```

> 与 STC 的差别：STC 上是"把中断向量逐个 bind 到 APP 的向量表"（`ota_isr_*`），
> Cortex-M 上是硬件 VTOR，一眼就能看对错。**`ota_isr_table_init/bind` 这套在 Cortex-M 上可以整体删掉。**

### 4.3 串口：这是移植的重头戏

```c
/* USART0 + DMA0 CH2 环形接收，2 KB 环，半满 + 空闲双中断 */
#define RING_SZ 2048U
static volatile u8  s_ring[RING_SZ];
static volatile u16 s_head, s_tail;     /* head 由 DMA 写，tail 由主循环写 */
static volatile u16 s_half;             /* DMA 半满/满标记 */

void DMA0_Channel2_IRQHandler(void)     /* 半满：处理前半，重装后半 */
void USART0_IRQHandler(void)            /* IDLE：把 DMA 剩余计数换算成 tail，取走尾巴 */
```

关键点：

- **`s_head` 必须由 DMA 的剩余计数换算**（`RING_SZ - dma_transfer_count`），不要自己累加；
- 半满中断时**只处理前一半**，这样 DMA 指向的后一半不会被 CPU 读写撕裂；
- `IDLE` 中断里做一次"短包收尾"，这才是处理 AT 应答（不定长）的正确姿势；
- 环形缓冲给 2 KB：一条 `+HTTPREAD:` 应答最多 3.3 KB 数据体，**用 `ot_uart_read` 拉进 Flash 缓冲即可**，
  不需要更大的环。

> **不要在 Cortex-M 上照抄 `at_wait_core` 的忙等结构。** 那段代码存在的唯一理由
> 是 8051 上没有 DMA、接收寄存器只有 1 个。照抄会把这个项目最痛的一段债带进新平台。

### 4.4 Flash 编程

```c
ota_ret_t ot_flash_write(u32 off, const u8 *buf, u16 len)
{
    u32 addr = FLASH_BASE + off;
    fmc_unlock();
    fmc_flag_clear(FMC_FLAG_END | FMC_FLAG_WPERR | FMC_FLAG_PGERR);
    for (u16 i = 0; i < len; i += 2) {
        u16 hw = (u16)buf[i] | ((u16)((i + 1U < len) ? buf[i + 1U] : 0xFFU) << 8);
        if (fmc_halfword_program(addr + i, hw) != FMC_READY) { fmc_lock(); return OTA_ERR_IO; }
    }
    fmc_lock();
    return OTA_OK;
}
```

- 编程粒度：**半字（2 B）最稳**，字（4 B）更快但要求 4 字节对齐 + 长度是 4 的倍数；
- `fmc_page_erase()` 之前要 `fmc_flag_clear()`，否则第二次擦除会莫名失败（GD32 的经典坑）；
- 读 Flash **直接用指针**（memory-mapped），所以 `flash_iap_read` 那一整套
  "IAP 命令 + EA=0 + 32 B 喂狗"的代码全部消失，`ota_verify.c` 能砍掉一半。

### 4.5 让"丢字节"从架构上消失的对照表

| STC32G（现在） | GD32F303（将来） |
|---|---|
| 1 个接收寄存器，87 µs 内必须取走 | DMA 直接搬进内存，**CPU 慢 1 ms 也不丢** |
| 轮询 + 1:1 交错 | 空闲中断 + 环形缓冲 |
| 丢字节 → 宽松判据 / 尾巴判据 / 子串匹配 | 不需要 |
| 一条 `AT+HTTPREAD` 3.3 KB 要分多次轮询 | 一次 `ot_uart_read` 拉完 |
| 12 档波特率扫描（模组其实写死 115200） | 直接 115200 打开 |

**net effect：`uart_at.c` 从 2 066 行降到约 400 行，`http_ota.c` 不动。**

---

## 5. 那些铁律，哪些能带走

| 铁律 | Cortex-M 上 |
|---|---|
| 2 `NOOVERLAY` | **作废**（无 overlay 机制） |
| 3 内存模型必须 LARGE | **作废** |
| 5 局部 xdata 数组必须 static | **作废**（但要小心栈：48 KB SRAM 里给栈留 4 KB 就够，别放 3 KB 数组） |
| 7 不许用 `code` 限定数据 | **作废**（原来是因为代码基址 0xFF0000 超 16 位取址被截断） |
| 9 形参第一句拷进 static | **作废**（但"跨调用状态别放寄存器"仍是好习惯） |
| 12 多步算法不许包成薄封装 | **作废**（原来是为了绕参数块） |
| 14 不许经过形参 >8 B 的门禁函数 | **作废** |
| 10 `CLASSES XDATA(0x1000-0x2EFF)` | **换成链接脚本**：`.data/.bss` 必须落在 SRAM 区间，且 STAGING 不占 SRAM |
| 13 `static xdata` 初值不生效 | **作废**（C 启动代码会拷贝/清零），但"显式初始化"仍建议保留 |
| 17 `COMMITTED` 是 BL→APP 交接点 | **保留**，这是协议不是平台特性 |
| 18 判读纪律（和值错=抄写，和值对但怪=固件） | **保留**，这条与平台无关 |

---

## 6. 其他候选平台（一句话结论）

| 平台 | 结论 |
|---|---|
| **STM32F103C8/RC** | 与 GD32F303 几乎等价（**1 KB 页**、72 MHz、64 KB SRAM）。分区表按 1 KB 页重排即可，其余同 §4 |
| **GD32F303 / GD32F305** | 首选。国产、货源稳、2 KB 页、Flash 前 256 KB 零等待；工具链用 Keil MDK 或 GCC 都行 |
| **ESP32 / ESP32-C3** | **不要自己写 OTA 了**：`esp_ota_*` + 分区表已经把 A/B 双区、回滚、CRC 全做了，`ota_verify.c` / `ota_jump.c` / `flash_iap.c` 全部删掉。代价是启动时间（~300 ms）和功耗 |
| **STM32F4** | 扇区擦除（16~128 KB 不等），分区必须按扇区排，STAGING 至少要 128 KB 扇区对齐 —— 空间浪费比 F1/F3 大，需要重新算分区表 |

---

## 7. 建议的施工顺序（每步都能独立验证）

1. **抽 HAL**（在 STC 上做，不换平台）
   把 6 组接口抽成 `ot_port.h` + `port_stc32g.c`，保证 STC 上行为**一字不变**（同一个 hex 的 md5 应当不变）。
   这一步是纯重构，风险最低，收益是"以后换平台只写一个 port 文件"。
2. **GD32 最小系统**：时钟 + SysTick + USART0 打印 + `ot_tick_ms`。跑通 `log_printf("hello")`。
3. **AT 拉通**：DMA 环 + IDLE 中断 → 发 `AT` 看到 `OK`，再依次 `ATE0` / `CPIN?` / `CSQ`。
4. **单包下载**（不分片）：URL 指向一个 **≤ 2000 B** 的小文件，验证 `HTTPINIT/PARA/ACTION/READ` 全链路。
   （这一步在 STC 上就是因为 23104 B 直接 602 才卡了很久，先拿小文件排除变量）
5. **Range 分片**：换回 23 KB 镜像，验证 `206` + 23 片 + 头解析出的 total = 23104。
6. **`ot_flash_*` + 校验**：把下载数据写进 STAGING 页，读回算 CRC32，与头里的 `img_crc32` 比。
7. **BL 双区**：跳转 + VTOR + 搬运 + `COMMITTED` 收尾。到这一步才真正需要动 `ota_verify.c`。
8. **回滚**：STAGING 兼作备份区，`boot_try` 计数，连续 3 次起不来就回滚。

预计：1~2 天（步骤 1）、1 天（2~3）、2 天（4~6）、2~3 天（7~8）。
**其中步骤 1 建议立刻做** —— 它不依赖新硬件，而且做完之后这份代码才真正"可移植"。

---

## 8. 风险清单（按"会把事情搞砸"的程度排）

| 风险 | 后果 | 对策 |
|---|---|---|
| 照抄 `uart_at.c` 的忙等结构 | 把最痛的债带进新平台，且新平台上"丢字节"更难复现（DMA 掩盖了症状） | 重写，只保留环形缓冲 + 行解析 |
| 分区边界没按 2 KB 页对齐 | 擦除会吃掉相邻区（比如 PARAM 被擦成 0xFF） | `ot_flash_erase` 里加断言：`off % page == 0 && len % page == 0` |
| 忘记设 `SCB->VTOR` | APP 一进中断就 HardFault（跑到 BL 的向量表里） | 跳转函数和 APP 启动各设一次，双保险 |
| 跳转前没关外设中断 | 新 APP 收到"幽灵中断"（比如 DMA 还在跑） | 跳转前关 SysTick / NVIC / DMA，见 §4.2 |
| STAGING < APP 容量 | 永远装不下完整镜像 | 见 §4.1 的两条路，先定死再动手 |
| 擦除时没喂狗 | 擦 24 页（1.2 s）中途复位 | `ot_flash_erase` 每页喂一次 |
| 用 4 B 字编程但长度是奇数 | 最后一个字节写成 0x00 或 0xFF | 用半字编程，奇数长度补 0xFF |

---

## 9. 一句话总结

这次移植**不是"把 12 000 行搬过去"**，而是：
*抽一层 6 个原语的 HAL（~200 行），重写 Flash 与串口两个 port（~600 行），
把 `uart_at.c` 里为 8051 打的补丁全部丢掉（-1 600 行）。*

业务逻辑（下载状态机、自愈、校验、分区协议）**一行都不用改** —— 这些是过去
七个版本拿真机上板日志换来的，才是这个项目真正的资产。
