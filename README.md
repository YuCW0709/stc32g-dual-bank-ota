# STC32G 双区 OTA · 从零到量产

> 8 位 8051 内核 + Keil C251 + 手工内存模型 + 4G 模组，做一套**能自愈、能回滚、能远程升级**的双区 OTA。
> 不是 demo，是真机跑通的完整链路：MQTT 收到版本 → HTTPS 分片下载 → 落地校验 → BL 搬运 → A3 提交。

**当前版本**：APP `v0.8.3` / BL `v0.2 (A1FORCE=0)` · **实测**：48 × 1024 B 分片，`0 reconnect 0 rerun`，正文 CRC32 逐位一致

---

## 一、这个项目为什么值得读

做 32 位 MCU 的 OTA，你有一堆现成库。做 **STC32G（C251 + 8051 扩展）** 的 OTA，你要自己解决：

| 难点 | 这个项目里的答案 |
|---|---|
| 没有中断（向量在 BL，APP 全程 `EA=0`） | 全轮询 + Timer0 溢出位当时基 |
| 栈只有 **256 B**，放不下任何缓冲 | 所有缓冲 `static xdata`，形参第一句就拷进 static |
| 编译器 L251 会做 overlay，形参/局部互相踩 | 两个工程都强制 `NOOVERLAY`，并有 `check_overlay.py` 守门 |
| 编译器**静默取第一个**同名宏（0 warning） | 宏命名纪律 + `check_layout.py` 在构建期校验链接结果 |
| 4G 模组 HTTP 缓冲只有 3356 B | 服务器必须支持 Range，客户端用 `BREAK/BREAKEND` 分片 |
| 模组 AT 接收只有 **1 个接收寄存器** | 主循环改成“线路静默 1 ms 才放行” |
| 烧录器不擦 STAGING/PARAM | 分区表 + 参数双备份 + 启动期状态自愈 |

如果你在写 **任何** 低资源 MCU 的 OTA，这里至少有 20 条坑是通用的 —— 见 [`docs/PITFALLS.md`](docs/PITFALLS.md)。

---

## 二、实测成果（真机日志，不是仿真）

一次完整的远程升级，从旧固件 `0.8.2` 升到 `0.8.3`：

```
[0][BL]  ==== BL v0.2 (A1FORCE=0) ====          ← 启动，不再无脑搬运 STAGING
[0][APP] ==== APP v0.8.2 ====                    ← 直接进应用
[MQ]     urc [+MSUB: "fw/0001/ver",32 byte,"0.8.3|https://…"] n=61
[MQ]     srv ver 0.8 / patch 3                   ← 服务器声明版本
[MQ]     newer than me -> OTA armed
[HOTA]   chunk 0..47 code=206 len=1024           ← 48 个 Range 分片，全 206
[OTA]    done: 49152 B, 48 chunks, 0 reconnect, 0 rerun
[OTA]    stg crc ok, body 49088 B                ← 落地即重算 CRC，坏镜像当场报
[FSM]    -> PENDING_REBOOT, reboot now
[BL]     A2: apply staging -> app (erase+copy+verify)
[APL]    APPLY OK len=49088 crc=50058959         ← 与镜像头完全一致
[APP]    ==== APP v0.8.3 ====
[APP]    ---- A3 commit: new firmware confirmed ----   ← 新固件自己确认，回滚窗口关闭
```

**判据全绿**：分片无重连、无重下、正文 CRC 与镜像头一致、安装后 CRC 仍一致、新固件自证版本号。

---

## 三、硬件与接线

| 项 | 型号 / 参数 |
|---|---|
| MCU | STC32G12K128（128 KB Flash，C251 LARGE 模型） |
| 编译器 | Keil C251 + L251（**必须** `NOOVERLAY`） |
| 4G 模组 | Air724UG（合宙 8910 平台，Cat.4，TLS 需系统时间） |
| 供电 | 5~12 V，**发射峰值 2 A**，供电弱会自己重启 |
| MQTT | EMQX Serverless，8883 / TLS |

```
        ┌──────────────┐
        │  STC32G12K128│
        │              │
UART1 ──┤ P3.0/P3.1    │  日志 115200（也被 STC-ISP 占用，烧录时拔模组）
UART3 ──┤ P0.0/P0.1    │  模组 AT 通道 115200
UART2 ──┤ P1.0/P1.1    │  业务数据口 115200（9 B 定长帧）
        └──────────────┘
              │
        ┌─────┴─────┐
        │ Air724UG  │── TLS ──> EMQX Serverless
        └───────────┘
```

> ⚠️ `P0.0` 上不许再挂任何第二个 TX。调试期不要“两口桥接”监听 ——
> 模组 TXD 与 USB-TTL 的 TX 会同时对拉 MCU 的 RXD，表现成“字节都在、只是少几个”，
> 会把你引向完全错误的结论（本项目为此浪费过两轮）。

---

## 四、Flash 分区

128 KB 切成 6 段，**地址是硬约束，改一个就要同步改 `build_local.sh` ×2 + 两个 `.uvproj`**：

| 段 | IAP 地址 | 大小 | 用途 |
|---|---|---|---|
| STAGING | `0x0000-0xBFFF` | 48 KB | 下载区：`64 B 头 + 49088 B 正文` |
| PARAM A | `0xC000-0xC7FF` | 2 KB | 参数主份（seq 递增，双份防写坏） |
| PARAM B | `0xC800-0xCFFF` | 2 KB | 参数备份 |
| RESERVED | `0xD000-0xDFFF` | 4 KB | 预留（回滚镜像） |
| NCONST | `0xE000-0xFFFF` | 8 KB | 常量区，BL/APP 各 4 KB |
| **BL** | `0x10000-0x13FFF` | **16 KB** | Bootloader，只读 |
| **APP** | `0x14000-0x1FFFF` | **48 KB** | 应用（代码 + 常量共用） |

> ★ APP 的 48 KB 是 **ECODE + HCONST 共用**，不是各 48 KB。
> 现在代码 40549 + 常量 6937 = 47486 B，**余 1154 B** —— 再加功能必须先砍。

---

## 五、启动链与状态机

```
上电
 ├─ BL 自检 → 读 PARAM（双份取 seq 大的）
 ├─ ota_state == PENDING_REBOOT ?
 │    ├─ 是 → 校验 STAGING（magic/hdr/img_len/正文 CRC32）
 │    │        ├─ 通过 → 擦 APP 区 + 搬运 + 回读校验 → 置 COMMITTED → 跳 APP
 │    │        └─ 失败 → 置 IDLE，记 boot_try（上限 20，防擦写磨损）
 │    └─ 否 → 直接跳 APP
 └─ 跳转前校验入口字节 == 0x75（C251 启动指令），不对就留在 BL
```

APP 侧的 OTA 状态机：

```
IDLE ──start(url)──> DOWNLOADING ──全部落地+CRC自检──> PENDING_REBOOT
  ↑                        │                                  │
  │                    失败/超时                          （复位）
  └────────────────────────┴──────────────────────────────────┘
                                                               ↓
                       BL 搬运成功 → COMMITTED → APP 自检通过 → app_ota_commit() → IDLE
```

**A3 提交是这条链的闭环点**：新固件必须自己跑起来、自检通过、把 `COMMITTED` 收尾成 `IDLE`。
如果它跑不起来（看门狗复位），`COMMITTED` 会留在参数里，`boot_try` 累加，超限由 BL 回滚 —— 这是**唯一能救回“升级后砖机”的路径**。

---

## 六、快速开始

### 1. 配置凭据（首次）

broker、用户名、密码、clientid 每个部署都不同，所以**不放在 `app_config.h` 里**：

```bash
cp APP/app_secrets.h.example APP/app_secrets.h
# 然后填自己的 broker 地址 / 用户名 / 密码 / clientid
```

`APP/app_secrets.h` 已被 `.gitignore` 排除。检查有没有误提交：

```bash
git status --short | grep app_secrets     # 只应出现 .example
```

### 2. 构建

```bash
bash tools/build_local.sh
```

一次跑完三件事，任一失败即中断：

| 守门 | 检查什么 | 失败退出码 |
|---|---|---|
| `check_layout.py` | `CLASSES XDATA(0x1000-0x2EFF)`、`?C_C51STARTUP` 落在 `0xFF4000` | 6 |
| `check_overlay.py` | 两工程都开了 `NOOVERLAY`、参数块不重叠 | 7 |
| `merge_hex.py --check` | BL+APP 合并后连续性、身份、分区占用 | 8 |

> ★ **改完代码必须构建两次比对 md5**。C251 的布局对“加一行注释”级别的改动都可能敏感，
> 两次不一致 = 有未定义行为。本项目历史上多次靠这条抓到 overlay/参数块问题。

### 3. 烧录

用 STC-ISP 烧 `build/step_dl_v0_8_2_blfix_BL_APP.hex`（BL + APP 合并包）。

> ⚠️ **STC-ISP 只覆盖 IAP `0x10000` 以上，从不碰 STAGING 与 PARAM。**
> 所以上一次 OTA 的镜像和参数会一直留着。要彻底干净，勾“整片擦除”。

### 4. 发布新固件（触发 OTA）

```bash
# 改版本号 → 编译 → 打镜像（一定是 --full，否则 512 对齐会裁掉超长正文）
python tools/make_img.py tools/_out_app/APP.hex build/fw_vX_Y_Z_ota.bin --full

# 上传 + 八项体检，拿到直链
python tools/publish_fw.py --upload-only --bin build/fw_vX_Y_Z_ota.bin

# 体检已有链接（长度 / ASCII / 200 / Content-Length / md5 / OTAI 头 / Range 206）
python tools/check_fw_url.py <url>
```

然后往 `fw/0001/ver` 发一条 **retain=1** 的消息：

```
payload: 0.8.3|https://kappa.lol/xxxxxx
          ↑版本号   ↑必须以 http 开头，整体 ≤ 127 字符
```

设备收到后按“严格更高才升”的规则决定是否进入 OTA。

---

## 七、文档

| 文档 | 内容 |
|---|---|
| [`docs/SETUP.md`](docs/SETUP.md) | 环境搭建、Keil 工程选项、STC-ISP 配置、烧录判据 |
| [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) | 分层、内存模型、日志系统、param 双备份、三个守门脚本、镜像格式 |
| [`docs/OTA_FLOW.md`](docs/OTA_FLOW.md) | 一次升级的完整时序：MQTT → HTTP Range → 落地校验 → 搬运 → 提交 |
| [`docs/PITFALLS.md`](docs/PITFALLS.md) | **★ 踩过的 24 条坑**，每条含现象 / 根因 / 判据 / 修法 |
| [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) | 日志判读手册 + 常见问题：看到什么就查什么 |
| [`docs/APP_MQTT.md`](docs/APP_MQTT.md) | 业务层：串口2 帧协议、MQTT 命令序列、TLS/SNI |
| [`docs/PORTING.md`](docs/PORTING.md) | 换芯片 / 换模组要改哪些地方 |
| [`docs/appendix/c251_notes.md`](docs/appendix/c251_notes.md) | C251 语言限制、编译错误对照表 |
| [`docs/appendix/rework_log.md`](docs/appendix/rework_log.md) | 按时间顺序的返工全记录（第一手调试素材，200 KB） |

---

## 八、目录

```
APP/        应用层：自检、AT 启动、主循环、MQTT 业务、A1 造镜像
BSP/        芯片外设：时钟 / GPIO / 串口 / 定时器 / 看门狗 / Flash IAP
COMMON/     与芯片无关：日志、CRC32、参数区、环形缓冲、OTA 类型
DRV/        外部器件：AT 命令引擎（uart_at）、HTTP 下载引擎（http_ota）
OTA/        OTA 业务：状态机、镜像校验/搬运、跳转、分区表
Project/    Keil C251 工程（APP 与 Bootloader 两个 .uvproj）
tools/      上位机脚本：构建守门 / 打镜像 / 发布体检 / 串口桥接 / 日志分析
build/      当前交付：合并 hex + OTA 镜像
```

---

## 九、最后：三条最贵的教训

1. **判据必须能量化，不能靠“感觉”。**
   “丢字节”这件事，本项目先后误判过三次（线上丢 / 解析错 / 抽稀能治）。
   最后靠“**线路静默时间**”这一个物理量定案 —— 因为 UART3 只有一个接收寄存器，
   115200 下一字节 87 µs，任何超过 87 µs 的循环都会整字节覆盖。
   **判据选错，后面所有努力都是错的。**

2. **临时开关必须带到期条件。**
   `BL_OTA_A1_FORCE = 1` 是个诊断开关，注释写着“定位完立刻改回 0”，忘了。
   后果：每次上电都把 STAGING 里的旧镜像搬进 APP，**新烧的固件一次都跑不起来**，
   表现极像“烧录没生效”。而它上一次没暴露，只因为当时搬进去的恰好还是同一份镜像。

3. **“消息丢了”和“消息坏了”是两件事，但都要能自愈。**
   订阅是幂等的 —— 版本行被写坏就重订阅一次，broker 会再补一条 retained。
   这条兜底第一次上板就救了一次升级。

---

MIT License · 见 [LICENSE](LICENSE)
