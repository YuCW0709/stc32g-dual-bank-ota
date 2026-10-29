# APP 业务层：串口2 数据采集 + MQTT over TLS + 版本触发的 OTA（v0.7.3）

本文是 v0.7.3 的业务层说明书，四件事一一对应：

1. 上电 → 联网 → 进 MQTT 会话（`app_mqtt.c`）
2. 订阅固件版本主题；周期上报 10 个数据槽（`app_mqtt.c`）
3. 数据来源 = **串口2**，9 字节定长帧（帧头 + 序号 + 4 字节数据 + CRC16）（`uart2_data.c`）
4. 服务器版本 ≠ 当前版本 → 进 OTA（复用 v0.6.19 已闭环的 HTTP 下载链）

---

## 0. 一句话看懂数据流

```
外部设备 ──UART2(115200)──> [9B 帧解析] ──> 10 个数据槽
                                              │
                         每 5 s 一次 ─────────┴──> AT+MPUB "fw/0001/up"
                                                        │
EMQX(8883/TLS) ──retain──> "fw/0001/ver" ──> [版本比较] ──> 严格更高 ⇒ OTA
```

---

## 1. 串口分工（**已固定，不要改**）

| 口 | 引脚 | 用途 | 备注 |
|----|------|------|------|
| UART1 | P3.0/P3.1 | 日志（115200） | 也被 STC-ISP 占用，下载时必须拔模组线 |
| UART3 | P0.0/P0.1 | AT 通道（模组） | `AT_UART_PORT 3` |
| UART2 | P1.0/P1.1 | **业务数据口** | 本次新增，接外部采集设备 |

### 硬约束：UART2 和 UART3 **必然同波特率**

两路都由 **Timer2** 供波特率（UART3 用 `S3ST3=0` 走 T2），只有一个重装值。
⇒ 串口2 与 AT 通道都是 **115200 8N1**，不能单独改一个。这是芯片结构决定的，不是配置遗漏。

### 硬约束：只能轮询，收不到中断

APP 全程 `EA=0`（中断向量在 BL，APP 里的同名 ISR 永远不会执行）。
`data_poll()` 因此设计成 `while (DTA_RI)` 状态机，**一个字节都不等**，
由主循环的快节拍（`APP_LED_PERIOD_MS`）反复调用。

> ⚠️ 这条约束的反面教训在 v0.6.13 已经付过代价：115200 一个字节 87 µs，
> 任何 ≥1 ms 的阻塞都会整字节丢失。所以 `data_poll()` 里不许出现 `delay`、
> 不许调 `at_cmd_wait`、不许打印日志。

---

## 2. 串口2 帧格式（9 字节定长，大端）

| 偏移 | 长度 | 字段 | 说明 |
|------|------|------|------|
| 0 | 1 | 帧头1 | 固定 `0xAA` |
| 1 | 1 | 帧头2 | 固定 `0x55` |
| 2 | 1 | IDX | 槽位号 **1..10**（0 或 >10 计为坏序号） |
| 3 | 4 | DATA | 32 位无符号，**大端**（高字节在前） |
| 7 | 2 | CRC | CRC-16/CCITT-FALSE，**大端**（高字节在前） |

**CRC 口径（必须一致，否则全部丢帧）**

```
算法   ：CRC-16/CCITT-FALSE
初值   ：0xFFFF
多项式 ：0x1021 (x^16 + x^12 + x^5 + 1)
输入反转：否      输出反转：否      结果异或：0x0000
覆盖范围：字节 0..6 共 **7 字节**（帧头 + IDX + DATA，不含 CRC 自身）
```

实现见 `DRV/uart2_data.c: data_crc()`。

**容错行为**

- `0xAA 0xAA 0x55 ...` → 重新同步，不报错
- CRC 错 → `bad_crc++`，整帧丢弃，槽位保持旧值
- IDX 越界 → `bad_seq++`，整帧丢弃
- 槽位一旦被写过就不再回到默认值（`##1229##` 只在"从未收到过"时出现）
- **数据值 0 是合法值**：收到 `IDX=4, DATA=0` 后上报的是 `D4=0`，不是 `##1229##`

---

## 3. 实例一：给串口2 发数据 → MQTT 上报什么

### 3.1 发这 4 帧（十六进制，串口助手 / 单片机都行）

| 槽 | 十进制值 | 完整 9 字节帧 |
|----|---------|--------------|
| 1 | 1234 | `AA 55 01 00 00 04 D2 79 B0` |
| 2 | 5000 | `AA 55 02 00 00 13 88 F6 39` |
| 3 | 25 | `AA 55 03 00 00 00 19 99 D0` |
| 4 | 0 | `AA 55 04 00 00 00 00 7D 1C` |

（CRC 已按上面的口径算好，可直接抄。`AA 55 01 00 00 04 D2` 的 CRC 是 `0x79B0`，
所以帧尾是 `79 B0`。）

故意发一帧坏的做对照：`AA 55 05 00 00 00 01 00 00`（CRC 故意写错）
→ 设备侧 `bad_crc` 加 1，`D5` **仍然是** `##1229##`。

### 3.2 5 秒内设备发出的实际内容（MQTT）

主题 `fw/0001/up`，QoS 0，retain 0：

```
D1=1234,D2=5000,D3=25,D4=0,D5=##1229##,D6=##1229##,D7=##1229##,D8=##1229##,D9=##1229##,D10=##1229##
```

线上真实跑的那条 AT 命令（`AT_TX_MAX=256`，这条 147 B）：

```
AT+MPUB="fw/0001/up",0,0,"D1=1234,D2=5000,D3=25,D4=0,D5=##1229##,D6=##1229##,D7=##1229##,D8=##1229##,D9=##1229##,D10=##1229##"
```

### 3.3 一帧都没收到时

```
D1=##1229##,D2=##1229##,D3=##1229##,D4=##1229##,D5=##1229##,D6=##1229##,D7=##1229##,D8=##1229##,D9=##1229##,D10=##1229##
```

（120 B。这就是"上电没接数据源"时服务器看到的样子，属于正常。）

### 3.4 设备日志长什么样

```
[0][AT] try 1/3 best b=115200
[0][AT]        best n=22 rc=0
[0][HOTA] bearer UP                     <- 承载（MQ_ST_PDP 拉的，判据=看到真实 IP）
[0][MQ] at link ready -> mqtt start
[0][MQ] MCONFIG ok
[0][MQ] TCP/TLS up (CONNECT OK)         <- TLS 握手过了
[0][MQ] MQTT session up (CONNACK OK)    <- 用户名密码过了
[0][MQ] subscribed: fw/0001/ver
[0][DTA] uart2 data P1.0/P1.1, baud=115200 (shared T2)
[0][MQ] pub ok=1 frames=0               <- 订阅成功后立刻发第 1 条
[0][MQ] pub ok=6 frames=4               <- 之后每 6 条（30 s）一次心跳
[0][MQ]   bad crc=1 seq=0               <- 只在有坏帧时出现
```

**`pub ok=` 与 `frames=` 并排看，就是"数据没上去"这类问题的分水岭**：

| 观察 | 结论 |
|------|------|
| `pub ok` 涨、`frames` 不涨 | MQTT 通路正常，问题在串口2（没接/波特率/CRC） |
| `frames` 涨、`pub ok` 不涨 | 数据进来了但发不出去（看 `MPUB rc=`） |
| 两个都不涨 | 没进 RUN 状态，往前看 CONNACK/SUBACK |

（`frames` = 串口2 累计收到的**合法帧**数，来自 `data_frames()`。）

---

## 4. 订阅与 OTA 触发

### 4.1 连接参数

| 项 | 值 |
|----|----|
| Host | `<实例ID>.ala.cn-shenzhen.emqxsl.cn` |
| Port | `8883`（**MQTT over TLS**，必须用 `AT+SSLMIPSTART`，不能用 `AT+MIPSTART`） |
| SSL 上下文 | MQTT = **88**，HTTP = **153**（两个不能混用） |
| User / Pass | `<你的用户名>` / `<你的密码>`（放 `APP/app_secrets.h`，不入库） |
| Client ID | `stc32g_demo_0001`（必须全网唯一） |
| 订阅 | `fw/0001/ver`（固件版本） |
| 上报 | `fw/0001/up`（10 槽数据） |
| Keepalive | 60 s |
| 上报周期 | 5000 ms |

### 4.1.1 SSL 三条配置（**少一条就连不上，v0.7.1 定案**）

`step_ssl()` 依次发三条，缺一不可：

```
AT+SSLCFG="seclevel",88,0                         不校验证书
AT+SSLCFG="sslversion",88,3                       TLS1.2（手册：0=SSL3.0 1=TLS1.0 2=TLS1.1 3=TLS1.2 4=ALL）
AT+SSLCFG="hostname",88,"<MQTT_HOST>"             ★ SNI：把域名上报给服务器
```

**第三条是最容易漏、也最难查的一条。** v0.7.0 上板时的真实故障是这样：

```
AT+SSLMIPSTART=...,8883   →  OK
                              CONNECT OK        ← TLS 握手确实成功了（532 ms）
AT+MCONNECT=1,60          →  OK
                              ERROR             ← 62 ms 后就失败
                              CLOSED            ← 链路被关
```

`CONNACK` 永远不来。而**同一套用户名密码在 PC 的 MQTT 客户端上完全正常**，
所以会误判成"凭据错""固件 bug""服务器拒绝"，全都查不到点上。

根因：TLS ClientHello 里**没有 SNI**（服务器域名没上报）。

- EMQX Serverless 是共享负载均衡，靠 SNI 把连接路由到你的实例；
- 而 `seclevel=0` **不校验证书**，于是"证书不匹配"这个本该报警的信号被掩盖了；
- 结果：握手照样成功（`CONNECT OK`），问题被推迟到 MQTT 层，表现为
  "TLS 通了、凭据没错、就是连不上"。

补上 `hostname` 后立刻 `CONNACK OK`。**判据就是它：62 ms 的 `ERROR`+`CLOSED`。**

> 另有备选写法 `AT+MCONFIG` 的第 8 个参数 `<HostNameFlag>`（1 = 上报域名，需
> 固件 **V401880+**）。本模组实测就是 `AirM2M_Air724UG_V401880_LTE_AT`，支持；
> 但它要求前 5 个可选参数全部补齐：
> `AT+MCONFIG=<id>,<user>,<pass>,0,0,"","",1`。
> 当前实现选 `SSLCFG="hostname"` 是因为它**不依赖固件版本**，且一条命令搞定。

手动验证序列（换服务器/换模组时照抄，一条条看返回）：

```
AT+CGMR                                             确认版本
AT+MDISCONNECT                                      幂等，清残留会话
AT+MIPCLOSE
AT+SSLCFG="hostname",88,"<域名>"
AT+SSLCFG="sslversion",88,3
AT+MCONFIG=<id>,<user>,<pass>
AT+SSLMIPSTART=<域名>,8883                          等 CONNECT OK
AT+MCONNECT=1,60                                    等 CONNACK OK
```

### 4.2 版本消息格式（服务器 → 设备，**必须 retain=1**）

```
<major>.<minor>.<patch>|<固件直链>
```

- 例：`0.7.4|https://kappa.lol/xoxgMU`
- 竖线前是版本号，竖线后是 OTA 直链（服务器必须支持 Range / 206）
- 载荷里**不能有逗号**（解析器取"最后一个逗号之后"，逗号会把载荷切掉）
- 直链最长 79 B（`MQTT_OTA_URL_MAX=80`）

#### ★ 线上实际格式：**载荷带双引号**（v0.7.2 定案）

服务器发的是裸串，但模组打印 URC 时会给 topic 和载荷**都加上双引号**，
而且 `N byte` 的长度**把引号也算进去**：

```
+MSUB: "fw/0001/ver",32 byte,"0.7.4|https://kappa.lol/xoxgMU"
                     ↑ 32 = 30 字符载荷 + 2 个引号
```

**手册 16.14 的示例写的是不带引号的** `+MSUB: "mqtt/topic",9 byte,SSSSddddd`，
照手册写解析器 ⇒ 载荷第一个字符是 `"` 而不是数字 ⇒ 被判成"不是版本消息"直接丢，
**表现就是"消息明明到了、设备毫无反应、日志也不报错"**。解析器现在两种格式都兼容：
跳过前导引号、拷贝 URL 时遇引号截止。

> 同一类坑还有一处（v0.7.2 的真凶，比引号更隐蔽）：`AT_URC_MAX` 曾经
> **同时**被定义成"URC 前缀注册表容量 8U"和"URC 行缓冲 88U"，
> 后一个定义被编译器忽略 ⇒ 行缓冲实际只有 **8 字节**，
> 61 字符的 `+MSUB` 行被反复清零撕碎，`+MSUB` 前缀永远匹配不上，
> `mqtt_parse_ver()` **根本不会被调用**。现在两个宏已拆名：
> `AT_URC_SLOT_MAX=8`（注册表）/ `AT_URC_MAX=128`（行缓冲）。
> **教训：两个不同含义的宏绝不能同名**，编译器不报错，只是静默取第一个。

> **为什么必须 retain**：设备在 `AT+MCONNECT`/`AT+MSUB` 这些命令期间，
> 到达的 URC 会被当普通应答行吃掉。retain=1 让服务器在**订阅成功那一刻补发**，
> 这条消息就一定能落在 `at_pump()` 的监听窗口里。

#### ★★ 完整性校验：用 `N byte` 当 CRC（v0.7.3）

```
+MSUB: "fw/0001/ver",32 byte,"0.7.4|https://kappa.lol/xoxgMU"
                     └─ 32 恰好等于「最后一个逗号之后到行尾」的字符数（含引号）
```

所以判据只有一条：`N == 行尾 - 载荷起点`。**不符就整条丢弃**，等 retain 重发。

为什么非要加这一条（v0.7.2 上板现场）：

```
接收路径偶发丢 1 字节（v0.6.13 起的老问题，至今未根治）
  ⇒ "0.7.1|https://kappa.lol/xoxgMU" 里丢两个字符
  ⇒ 变成 "0.71|https://kappa.lol/xogMU"       ← 版本号少一个点、URL 少一个 x
  ⇒ 版本被读成 0.71.0（比 0.7.2 大）→ 误判为"有新版"
  ⇒ URL 看起来完全正常，能过"http 开头"的检查
  ⇒ 服务器回 404 → 两个整包重跑 → give up
```

**这是最难查的一类 bug：代码没错，是运输途中掉了一个字节。**
模组白送的这个长度字段就是现成的 CRC，一条判据当场判死。
现场那两条日志现在会被这样拦下：

```
[W][MQ] urc len 30/32 drop
[W][MQ] urc len 31/32 drop
```

> 顺带：URL 在真正下发前会**原样打出来**（`[I][MQ]   url https://...`）。
> 404 长得和"文件不存在"一模一样，这一行是唯一能在下载前看出
> "线上传过来的到底是什么"的证据。

### 4.3 升级判定口径

`MQTT_OTA_ANY_VERSION = 0`（默认）⇒ **只有服务器版本严格更高才升**。

| 设备 | 服务器 | 结果 |
|------|--------|------|
| 0.7.3 | 0.7.3 | 不升 |
| 0.7.3 | 0.7.4 | **升** |
| 0.7.3 | 0.6.9 | 不升（降级会导致无限来回刷） |
| 0.7.3 | 0.7.4（无 URL） | 不升，日志打 `newer ver but no URL, ignored` |

### 4.4 实例二：一条升级消息的完整过程

**服务器侧发这一条**（MOSQUITTO 示例）：

```bash
mosquitto_pub -h "$MQTT_HOST" -p 8883 --cafile emqxsl-ca.crt \
  -u "$MQTT_USER" -P "$MQTT_PASS" \
  -t "fw/0001/ver" -r \
  -m "0.7.4|https://kappa.lol/xoxgMU"
```

**设备侧线上收到**（模组 URC 原文，载荷 30 B）：

```
+MSUB: "fw/0001/ver",32 byte,"0.7.4|https://kappa.lol/xoxgMU"
```

**设备日志序列**：

```
[0][MQ] subscribed: fw/0001/ver
[0][MQ] srv ver 0.7.4
[0][MQ]       patch 3
[W][MQ] newer than me -> OTA armed
[0][MQ] OTA download started (trigger = MQTT ver)
[0][HOTA] term -> OK
[0][HOTA] action rc=5            <- PREFIX：拿到 "+HTTPACTION: 0,206,1024"
[0][OTA] chunk 0 code=206 len=1024
[0][OTA] 10% ... 100% 23104/23104
[0][OTA] stg crc ok, body 23040 B
```

之后设备**停机等断电**（业务 APP 不能自己擦自己），上电由 BL 接手：

```
[4][BL] state=3 -> STAGING ok len=23040 crc=366e2347
[4][BL] APPLY OK
[0][APP] ==== APP v0.7.4 ====
```

**关键纪律：升级前必须 `AT+MDISCONNECT` + `AT+MIPCLOSE`**。
模组的 HTTP 与 MQTT 共用同一块内存，不断开会直接 `602 no memory`
（这条在 v0.6.x 已经定案，代码里 `mqtt_do_ota()` 就是按这个顺序写的）。

---

## 5. 状态机（`MQ_ST_*`）

```
OFF ──(at_get_baud()!=0)──> PDP(承载) ──> CFG(MDISCONNECT+MIPCLOSE+MCONFIG) ──> SSL(SSLCFG×3) ──> TCP(SSLMIPSTART,等CONNECT OK)
  ↑                                                                                  │
  │                                                                                  v
  └──── 连续 3 次发布失败 / 断链退避 3 s ───> BACK ──── RUN(周期 MPUB) <── CONN(MCONNECT,等CONNACK OK)
                                                         ↑                │
                                                         └── SUB(MSUB,等SUBACK)

任意状态 ──(ota_fsm_state()!=IDLE)──> HOLD（一个字节都不发） ──(OTA 结束)──> OFF
```

- ★★★ **HOLD：OTA 下载期间 MQTT 必须彻底闭嘴**（v0.7.3）
  HTTP 与 MQTT **共用模组内存和 SSL 上下文**（和 602 定案同一个根子），
  物理上不能并存。v0.7.2 现场就是这样崩的：

  ```
  16:44:24.394  HTTPPARA="URL"                 ← 下载已开始
  16:44:24.554  MDISCONNECT / MCONFIG / SSLMIPSTART   ← MQTT 同一秒从 PDP 起重连
  16:44:26.990  +HTTPACTION: 0,404,14          ← 会话已被拆，拿到的其实是坏的
  16:44:27.xxx  modem silent, reopen failed ×6 → 整包重跑 ×2 → give up
  ```

  现在只要 `ota_fsm_state() != OTA_ST_IDLE`，`mqtt_poll()` 直接 return；
  OTA 回到 IDLE 才 `HOLD → OFF → PDP` 重新起链。
- **PDP 这一步不能省**：`AT+SSLMIPSTART` 在没有活动承载时直接回 `ERROR`，
  日志只会显示 `tcp rc=1`，看着像 TLS 或用户名的错，实际是根本没联网。
  v0.7.0 第一版就是因为把"上电即下载"关掉之后承载没人拉，才补了这一状态。
  实现直接复用 `http_ota.c: hota_pdp_up()`（判据 = **看到带真实 IP 的那一行**），
  `app_mqtt.c` 里没有第二份 PDP 逻辑。
- **SSL 三条一起发，尤其不能漏 `hostname`**：漏了 SNI 会表现为
  `CONNECT OK` 之后 `MCONNECT` 立刻 `ERROR`+`CLOSED`（详见 4.1.1）。
  三条都"失败也继续"——真正决定成败的是下一步能不能拿到 `CONNECT OK`。
- **CFG 前先 `MDISCONNECT`+`MIPCLOSE`**：模组里若留着上次半开的会话/链路，
  `MCONNECT` 会**在本地**直接回 `ERROR`（不走网络、约 60 ms），
  和"服务器拒绝"现象一模一样。没连接时这两条都回 OK，幂等；
  MCU 复位而模组没断电时尤其需要。
- `TCP`/`CONN` 的"成功"是 **URC**（`CONNECT OK` / `CONNACK OK`）而不是 `OK`，
  所以用 `at_tx_mac(AT_EXP_CONNOK/AT_EXP_CONNACK, ...)` 直接等前缀（URC 先到、OK 后到都能命中）
- URC 走独立通道 `at_pump()` + `at_urc_take()`，**不能用阻塞等待**：
  `at_wait_*` 进出都清行缓冲，短窗口反复调用会把跨窗口的 `+MSUB` 行撕成两半
- PDP 通过后会把"宽松判据"关回去（`at_set_lenient(0)`）：承载阶段允许拿 OK 残片当成功，
  **MQTT 这几条不允许** —— 宁可重试，也不要拿残片当成功。
  OTA 流程自己会重新打开宽松判据。

---

## 6. 资源占用（48 KB 硬墙，改代码前必读）

APP 代码区 = `0x14200-0x1FFFF` = **48640 B**，`ECODE`（代码）与 `HCONST`（常量/字符串）
**共用**这一个 48 KB。v0.7.0 第一次构建就是因为这个爆的（15 个 `L107 ADDRESS SPACE OVERFLOW`）。

| 项 | v0.6.13 | v0.7.3 | 上限 |
|----|---------|--------|------|
| ECODE | 33105 | 40850 | — |
| HCONST(const) | 5681 | 6764 | — |
| ECODE+HCONST | 38786 | **48236** | 48640（余 **404**） |
| XDATA | 7629 | **7540** | 7936（余 **396**） |
| edata+hdata | 256 | 256 | 1024 |

腾地方的手段（一次性的、可逆的）：`AT_DIAG_LEGACY = 0`（见 `uart_at.h`），
把 v0.47~v0.6.6 的物理层取证工具整段下线，省 **约 3.9 KB 代码 + 0.6 KB 常量**。
保留的判据只有真正定过案的两条：`at_rx_probe()`（定长应答字节数）与 `at_take_overflow()`。

> ⚠️ 想再加功能，先看 `build/step_dl_v0_7_3_BL_APP.hex` 的余量：代码 **404 B** / xdata 396 B。
> 已经到「再加一个模块就得砍旧功能」的地步了。
> 平均一个 `LOG_I2` 加一条短字符串 ≈ 30~40 B。**别指望能再加一个模块**。

---

## 7. 编译与烧录

```bash
bash tools/build_local.sh          # APP + BL + 三道守门 + 合并 hex
```

当前交付：`build/step_dl_v0_7_3_BL_APP.hex`
md5 `76c41123d2ed247340b141f224cba53f`（两次构建一致）

烧录用 STC-ISP 整包（含 BL）。**下载时必须拔掉模组线**（P3.0/P3.1 被 ISP 占用）。

> STC-ISP 每次全片擦除 ⇒ 刷完**第一次上电**必然 `param empty`，
> 验收只看**第二次上电**。

---

## 8. 上板验收顺序（照这个顺序看，不要跳）

```
1) [0][AT] try 1/3 best b=115200 / best n=.. rc=0     ← 模组在应答
2) [0][HOTA] bearer UP                                ← PDP 承载（MQ_ST_PDP 拉的）
3) [0][MQ] MCONFIG ok                                 ← 客户端 ID / 用户名密码已写
4) [0][MQ] TCP/TLS up (CONNECT OK)                    ← TLS 握手成功
5) [0][MQ] MQTT session up (CONNACK OK)               ← 用户名密码通过
6) [0][MQ] subscribed: fw/0001/ver                    ← SUBACK
7) [0][DTA] uart2 data P1.0/P1.1, baud=115200        ← 数据口就绪
8) [0][MQ] pub ok=1 frames=0                          ← 上报通路
9) 服务器侧看到 D1..D10（未接数据源时全是 ##1229##）
10) 发 4 帧示例 → 下一周期变成 D1=1234,D2=5000,D3=25,D4=0
11) 发 retain 版本消息 0.7.4|https://... → newer than me -> OTA armed
```

任何一步没到，**不要往下看**：后面每一步都以前一步成立为前提。
第 2 步失败时日志会直接给出根因（`SIM not inserted` / `csq rssi=99` / `att 10/10 = 0`），
不需要再猜。

---

## 9. 常见故障对照

| 现象 | 原因 | 处理 |
|------|------|------|
| `module no response` | 模组没上电 / TX-RX 没交叉 / 波特率不对 | 看 `rx idle=%u`：0 = 供电或 TXD 没接 |
| `pdp failed (xN), retry in 5 s` | 承载起不来（没卡 / 没信号 / 没附着） | 往前看 `SIM not inserted` / `csq rssi=99` / `att 10/10 = 0` |
| `tcp rc=1` 反复失败 | **承载没起来**（最常见），或 host/port 写错 | 先确认上面有 `bearer UP`；再看 `broker`/`port` 两行 |
| **`CONNECT OK` 之后 `MCONNECT rc=1`，模组线上回 `ERROR`+`CLOSED`（约 60 ms）** | **SSL 少发 `hostname`（无 SNI）**，v0.7.0 踩过 | 见 4.1.1；补 `AT+SSLCFG="hostname",88,"<域名>"`。凭据没问题时优先查这条 |
| 同上，但改 SNI 也没用 | 模组里留着上次半开的会话/链路 | `MCONNECT` 前先 `AT+MDISCONNECT`+`AT+MIPCLOSE`（v0.7.1 起内置） |
| `action rc=4` 一直超时 | PDP 没起（`bearer UP` 没打印） | 先查 SIM / 天线 / `AT+CGATT?` |
| `602 no memory` | HTTP 与 MQTT 同时占用模组内存 | 升级前必须 `MDISCONNECT`+`MIPCLOSE` |
| 上报里永远全是 `##1229##` | 串口2 没数据 / 波特率不符 / 帧 CRC 算错 | 先看 `frames=` 与 `bad_crc=` |
| 收到数据但服务器看不到 | MPUB 超时（模组忙 / 主题错） | 看 `pub fail=` 计数，连续 3 次会自动重连 |
| 版本消息一直没反应 | 没设 retain | `mosquitto_pub` 加 `-r` |
| **版本消息到了（桥接里能看到 `+MSUB`）但设备一行日志都没有** | **URC 行缓冲太小，整行被撕碎**（v0.7.2 真凶：宏同名导致只有 8 字节） | 查 `AT_URC_MAX`（应为 128）与 `AT_URC_SLOT_MAX`（8）是否被弄混 |
| 有 `urc payload not a version` | 载荷带引号未处理（v0.7.2 前） | 见 4.2，现已兼容 |
| **`urc len %u/%u drop`** | **接收路径丢了字节**（v0.6.13 起的老问题），长度字段对不上 | 这是**保护动作**，不是故障：丢掉坏消息等 retain 重发即可。频繁出现说明 RX 真的不稳 |
| **`armed` 之后 `+HTTPACTION: 0,404`** | URL 少字符 / 文件确实不存在 | 看 arming 后紧跟的那行 `url %s`，逐字符对数；v0.7.2 现场就是 `xoxgMU`→`xogMU` |
| **`AT+HTTPTERM` 后面跟着 `AT+HTTPTERMAT`、`AT+HTTPTERMATAT`…** | 发 `AT` 前没 `at_tx_reset()`，把上一条命令带上了 | v0.7.3 已修（`hota_reopen`）；`at_tx_mac` 发完**不清**缓冲，清不清是调用方的事 |
| **`armed` 后 `modem silent, reopen failed` 连打 + `rerun` 两次 + `give up`** | 下载期间 MQTT 又在重连，把 HTTP 会话拆了 | v0.7.3 已修（`MQ_ST_HOLD`）：OTA 期间 `mqtt_poll()` 直接 return |
| 升级完又要求升级 | 服务器版本比设备低（残留旧消息） | 这是 `MQTT_OTA_ANY_VERSION=0` 在保护，改服务器版本号 |

---

## 10. 后续可扩展点（当前未实现）

- `mqtt_ctrl.c`：下行控制命令（服务器 → 设备动作）
- `net_dev.c`：网络设备抽象（MQTT / HTTP 统一调度）
- `app_report`：上报内容按槽位类型做缩放（比如温度 ×10）
- A3 回滚：`boot_try` 计数 + 旧镜像备份（当前只有"新固件自检失败 → BL 拒绝搬运"这一层）

槽位扩容：改 `uart2_data.h` 的 `DATA_SLOT_N`（同时注意 payload 长度，
`AT_TX_MAX=256` 的天花板，10 槽已用 147 B，**最多到 14 槽左右**）。
