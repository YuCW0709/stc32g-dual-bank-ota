# 一次 OTA 的完整时序

> 从“服务器上多了一份新固件”到“设备跑在新固件上”，中间有 7 个阶段、5 次校验。
> 本文按真实执行顺序展开，每一阶段都给出**判据**（日志里看什么）。

---

## 总览

```
① 触发       MQTT retained  →  版本比较（严格更高才升）
② 断链       AT+MDISCONNECT / AT+MIPCLOSE      （MQTT 与 HTTP 共用模组内存）
③ 建会话     HTTPINIT → SSL 配置 → CID → URL → BREAK/BREAKEND
④ 分片下载   HTTPACTION=0 → +HTTPACTION: 0,206,1024 → HTTPREAD → 就地落盘
⑤ 落地校验   magic / hdr_len / hdr_ver / img_len / 正文 CRC32
⑥ 搬运       BL 擦 APP 区 → 逐块复制 → 回读校验 → 置 COMMITTED
⑦ 提交       新 APP 自检通过 → app_ota_commit() → IDLE（回滚窗口关闭）
```

---

## ① 触发：MQTT 版本消息

设备订阅 `fw/0001/ver`（**必须 retain=1**），载荷格式：

```
<major>.<minor>.<patch>|<url>
例：0.8.3|https://kappa.lol/d2P0fS
```

解析要点：

| 项 | 说明 |
|---|---|
| 报文字形 | `+MSUB: "fw/0001/ver",32 byte,"0.8.3\|https://…"` |
| 载荷带引号 | 首字符是 `"`，解析要跳过 |
| `N byte` | **含两端引号**，正好可当长度校验（30 裸串 → 32） |
| 三段必须有 | 第二段后必须是 `.`，第三段至少一位数字，否则**整条丢弃** |
| URL | 必须以 `http` 开头，`"` 截止，整体 ≤ 127 字符 |
| 比较 | **严格更高**才升（防止服务器残留旧版本导致无限来回刷） |

**判据**：

```
[MQ] urc [+MSUB: "fw/0001/ver",32 byte,"0.8.3|https://kappa.lol/d2P0fS"] n=61
[MQ] srv ver 0.8
[MQ]       patch 3
[MQ] newer than me -> OTA armed
```

`n=61` 是完整行长。小于 61 说明接收路径又丢字节了（见 `PITFALLS.md` P10/P11）。

**自愈**：若版本行被判损坏（长度不符 / 三段不全），2 秒后自动**重订阅**一次
（订阅是幂等的，broker 会再补一条 retained），最多 3 次：

```
[MQ] urc len 31/32 drop
[MQ] ver line damaged, resub 2
```

---

## ② 断链：OTA 期间 MQTT 必须彻底停

**原因**：HTTP 与 MQTT 共用模组内存和 SSL 上下文。一边在下 HTTP，另一边一旦发
`MCONFIG` / `SSLMIPSTART`，会话就被拆掉，表现为：

```
[HOTA] modem silent, reopen failed   ×N
→ rerun ×2 → give up
```

**做法**：进入 `MQ_ST_HOLD`，停止一切 MQTT 动作，直到下载结束。

---

## ③ 建会话：8910 平台的 SSL 顺序

```
AT+HTTPTERM                  ← 先 TERM，否则重试必撞 +CME ERROR: 3（会话已开）
AT+HTTPINIT
AT+HTTPSSL=1
AT+SSLCFG="seclevel",153,0
AT+SSLCFG="sslversion",153,4        ← 4 = ALL
AT+HTTPPARA="CID",1
AT+HTTPPARA="URL","https://…"
AT+HTTPPARA="BREAK",1024
AT+HTTPPARA="BREAKEND",1024
```

> ★ **SSL 配置在 `HTTPINIT` 之前**（8910 平台的顺序，和 SIMCom 相反）。
> ★ `SSLCFG` 的上下文号：**HTTP = 153，MQTT = 88**。
> ★ HTTPS 还需要**系统时间**（`AT+CCLK?`）；时间没同步时 TLS 失败只表现为超时。

---

## ④ 分片下载

```
AT+HTTPACTION=0
  ↓ 等待 +HTTPACTION: <method>,<status>,<len>
  ↓ status 必须 206（Range 生效），len 通常 = 1024
AT+HTTPREAD=<start>,<size>
  ↓ 数据体直接落到 STAGING + offset
offset += len
重复，直到 offset == total
```

**为什么必须分片**：模组 HTTP 响应缓冲只有 3356 B，整包 GET 会回
`+HTTPACTION: 0,602,0`（602 = 内存不足）。

**为什么必须 206**：服务器不支持 Range 就只能整包返回 ⇒ 无解。
发布前 `check_fw_url.py` 第 7 项就是在守这条。

### 落地路径的关键设计：前缀就地落盘

`+HTTPREAD:` 这一行之后紧跟的就是二进制数据体，中间**没有空行**。
从“判定这是数据体”到“把字节写进 STAGING”，只允许有十几条指令 ——
否则 1024 B 在 115200 下要 89 ms，中间任何停顿都会丢。

实测探针（正常值）：

```
[HOTA] gap0 1 (prefix->byte0)     ← 行尾到首字节只 20 µs
[HOTA] gap 4 at 8                  ← 数据体内部无临界停顿
[HOTA] lead=0 (clean body)         ← 无前导垃圾
[HOTA] header: hdr=64 total=49152
```

### 断点续传与重连

- 每片落盘后同步 `param.dl_offset`，真掉电也能接上；
- 会话中断 → `HOTA_RECOVER` → 重建会话 → 从 `dl_offset` 继续（Range 幂等）；
- 单片重取：首片认不出 `OTAI` 就不写，`goto read_retry` 重取同一片。

**判据**：

```
[HOTA] chunk 47 code=206 len=1024
[OTA]  done: 49152 B, 48 chunks, 0 reconnect, 0 rerun
```

`0 reconnect 0 rerun` 是健康链路的标志。

---

## ⑤ 落地校验（下完那一刻就要报，别等重启）

`HOTA_DONE` 时回读 STAGING，校验 5 项：

| 项 | 失败日志 |
|---|---|
| `magic == "OTAI"` | `stg head %x %x (want 4f 54)` |
| `hdr_len == 64` | `stg hdr_len=%u (want 64)` |
| `hdr_ver == 1` | `stg hdr_ver=%u (want 1)` |
| `img_len` 在范围内 | `stg img_len=%lu out of range` |
| 正文 CRC32 | `stg crc now=%lx` / `stg crc want=%lx` |

**两个 CRC 分开打**，才能区分“头没写进去”和“正文被改写”。

通过后：

```
[OTA] stg crc ok, body 49088 B
[FSM] -> PENDING_REBOOT, reboot now
[APP] OTA image ready, POWER-CYCLE to apply
```

> 这一步的意义：坏镜像在**下完那一刻**就报，不用让用户掉电重上电再等 BL 报错。
> 失败会清下载参数并转 IDLE，外层 30 s 后自动整包重下。

---

## ⑥ BL 搬运

上电后 BL：

```
[BL] seq=21 boot_cnt=0 boot_try=0 state=3        ← state == PENDING_REBOOT 才动手
[BL] pending install -> verify staging (read-only)
[VER] STAGING ok len=49088 crc=50058959
[BL] A2: apply staging -> app (erase+copy+verify)
[APL] app crc now=8f8246c9 want=50058959 -> erase+copy
[APL] copy off=2000 / 4000 / … / a000
[APL] APPLY OK len=49088 crc=50058959
[BL] A2 DONE: app updated, booting new firmware
[BL] jump entry via-base=75                       ← 入口字节校验通过
```

搬运是**幂等**的：先擦 APP 区、再逐块复制、最后回读校验。
中途掉电 ⇒ 下次上电重新搬（此时 APP 区是半截状态，绝不会跳）。

**跳转闸门前必须校验入口首字节 == `0x75`**（C251 启动指令）。
不对就留在 BL —— 跳过去必跑飞。

---

## ⑦ A3 提交：回滚窗口在这里关闭

新 APP 启动后：

```
[APP] ==== APP v0.8.3 ====
[APP] boot_try=0 ota_state=4                     ← 4 = COMMITTED
[APP] ---- A3 commit: new firmware confirmed ----
[APP] commit rc=0 succ=0                         ← 收尾成 IDLE
```

**为什么需要这一步**：BL 只知道“我搬完了”，不知道“新固件能不能跑”。
只有新固件自己跑起来、自检通过、主动提交，才算真正成功。

### 回滚路径（A3 的反面）

```
BL 搬完 → COMMITTED，boot_try = 0
   ↓ 新固件跑飞（看门狗复位）
BL 看到 COMMITTED 且 boot_try < 20 → boot_try++
   ↓ 反复失败
boot_try >= 20 → 停止搬运，保留标记（不再消耗 Flash 寿命）
```

> `boot_try` 上限是 20 而不是 3：每轮重试要擦写 48 KB，
> 而“能重复观察”本身有价值。20 轮对 Flash 寿命无影响。

---

## 各阶段失败时会发生什么

| 阶段 | 失败表现 | 后果 | 是否砖机 |
|---|---|---|---|
| ① 收到坏版本行 | `ver line damaged` | 自动重订阅 | 否 |
| ② 断链失败 | `modem silent` | 重试 3 次后放弃，APP 继续跑 | 否 |
| ③ 建会话失败 | `601`（网络）/ `602`（内存） | 转 IDLE，30 s 后重试 | 否 |
| ④ 分片中断 | `reconnect` / `rerun` 计数上升 | 断点续传 | 否 |
| ⑤ 落地 CRC 错 | `stg crc now/want` | 清下载，重下 | 否 |
| ⑥ 搬运失败 | `apply FAIL rc=` | **留在 BL**，需重新烧录 | 是（有提示） |
| ⑦ 新固件跑飞 | 看门狗复位 | `boot_try` 累加，超限回滚 | 否 |

**唯一可能导致“必须重新烧录”的环节是 ⑥**，所以 BL 在搬运失败时**绝不能跳转**：

```c
if (rc != OTA_OK) {
    LOG_E1("BL", "apply FAIL rc=%u, stay in BL (re-flash to recover)", rc);
    bl_summary();
    return;                 /* APP 区此刻是被擦过/半截写的状态，跳过去必死 */
}
```

---

## 一次完整升级的时间参考

| 阶段 | 耗时 |
|---|---|
| MQTT 建链（含 PDP 激活） | 8~15 s（PDP 给 30 s 超时） |
| 48 片 × 1024 B 下载 | 约 60 s |
| 落地 CRC（49088 B） | < 1 s |
| BL 擦 + 搬 + 回读 | 约 18 s |
| 新 APP 启动到联网 | 8~15 s |

整轮约 **1.5~2 分钟**。日志里 `retain` 与当前版本相同时不会再触发，
所以升级完成后设备会稳定下来，不会反复刷写。
