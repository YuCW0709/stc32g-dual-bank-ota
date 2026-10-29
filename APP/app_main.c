/*==============================================================================
 *  app_main.c  --  APP 入口（P0：上电自检）
 *  Project : Project/APP/STC32G_OTA.uvproj    CODE = 0xFF4000（FF:4000h）
 *  Platform: STC32G12K128 (Keil C251, CodeRomSize = Huge, HEX-386)
 *  Depends : BSP/*  COMMON/{ota_mcu,ota_types,log,param,crc32}  OTA/ota_partition
 *
 *  ── 本文件当前做到哪一步 ───────────────────────────────────────────────
 *  P0（APP_STAGE_P0 = 1）：
 *      · 时钟 / GPIO / UART / 看门狗初始化，串口出字
 *      · flash_iap_init(128)  + 读回 APP 区首字节，证明"我确实在 APP 区跑"
 *      · param_init() 读参数区，打印 seq / boot_cnt / 版本 / 下载断点
 *      · 500ms 心跳灯 + 喂狗
 *  未实现（等 DRV 与 ota_fsm.c 落地）：
 *      模组上电、AT 通道、MQTT 控制面、HTTP Range 下载、状态机驱动上报
 *
 *  ── ★ 为什么 P0 全程不开总中断（EA 一直为 0）─────────────────────────
 *
 *  这是从 BL 跳到 APP 之后最容易被忽略的一个坑，必须先讲清楚：
 *
 *  1) 中断向量只有一份，地址固定在 FF:0003h / FF:000Bh / FF:0023h ...，
 *     它属于 **Bootloader**（BL 定位在 0xFF0000，APP 在 0xFF4000）。
 *     硬件中断永远从 BL 的向量进，**不会**跳到 APP 里同名的那份代码。
 *
 *  2) 所以 APP 链接进来的 bsp_timer.c / bsp_uart.c 里那个中断函数，
 *     如果还写着 `interrupt 1` / `interrupt 4`，就只是一份**永远不会被执行**
 *     的副本。于是：
 *         · APP 的 s_ms 永远是 0   -> bsp_timer_delay_ms() 死等
 *         · APP 的 s_rx 永远是空   -> AT 应答一个字节都收不到
 *     而 BL 的 ISR 还在跑，往 BL 自己的变量里写 —— 看起来"中断是好的"，
 *     实际 APP 什么也拿不到。这个现象极易被误判成"模组没回数据"。
 *
 *  3) 工程里已经为此做了准备：bsp_timer.c / bsp_uart.c 用 `OTA_BUILD_APP`
 *     宏把 `interrupt` 关键字关掉，APP 侧的 ISR 编译成**普通函数**，
 *     等 BL 的向量 stub 通过 ota_isr_dispatch(n) 回调它。
 *
 *  4) 但那套跳板要求"中断分发表"放在 **APP 不会覆盖的 RAM**——APP 与 BL
 *     各自从 xdata 0x0000 开始分配，APP 一运行就会把 BL 的表踩掉。
 *     这个问题必须在 P1 一次性解决（见下），所以 P0 干脆不开中断：
 *     串口发送走轮询分支（bsp_uart.c 里 EA==0 的那条路径），完全够用。
 *
 *  ── P1 需要先定的事（阻塞项，不要跳过）───────────────────────────────
 *  [A] 中断分发表 s_isr[48]（3 字节/项 = 144B）必须放在 BL 与 APP 都不用的
 *      xdata 区间。做法：两个工程的 CLASSES 都把 XDATA 限制在 0x0000-0x2EFF，
 *      表用 `_at_ 0x2F00` 绝对定位（STC32G12K128 有 12KB xdata）。
 *  [B] BL 的四个向量 stub（T0/UART1/INT0/INT1）改成只调 ota_isr_dispatch(n)，
 *      BL 自己也在 main 里 ota_isr_bind(n, bsp_xxx_isr) 认领一份。
 *  [C] 跳转前 BL 必须 ota_isr_unbind 掉自己的，避免 APP 绑定时撞车。
 *      实际上 bind 就是覆盖，顺序上 APP 后绑即可，但要在 EA=1 之前完成。
 *============================================================================*/
#include "ota_mcu.h"

#include "app_config.h"

#include "bsp_clock.h"
#include "bsp_gpio.h"
#include "bsp_uart.h"
#include "bsp_timer.h"
#include "bsp_wdt.h"

#include "flash_iap.h"
#include "crc32.h"
#include "log.h"
#include "ota_partition.h"
#include "ota_types.h"
#include "ota_fsm.h"
#include "http_ota.h"     /* ★ v0.6.7：进度上报要读 offset/total/chunk/recover；
                           *   不包含它 C251 会按隐式 int 声明，链接器报
                           *   L25 DATA TYPES DIFFERENT（返回值 32 位被截断） */
#include "param.h"
#include "uart_at.h"
/* ★ v0.7.0 业务层：串口2 数据口 + MQTT 控制面 */
#include "uart2_data.h"
#include "app_mqtt.h"

#define TAG     "APP"
/* ★ v0.6.7：下载进度/自愈统一用这个 tag（与 ota_fsm.c 保持一致），
 *   UART1 上 grep "[OTA]" 就能只看到下载相关的行。 */
#define TAG_OTA "OTA"

/*==============================================================================
 * ★ P1-A3 提交 —— 完整版与最小验证版**共用这一份**
 *
 *  以前它写在完整版里，最小版要再抄一遍。抄两遍的代价不是多几行，
 *  而是"改了一处忘了另一处" ⇒ 账本（running_ver / boot_try / ota_state）
 *  在两个固件里语义不同，而日志长得一模一样。所以提到 #if 外面。
 *
 *  ── 交接协议（不新增字段、不需要改 PARAM_FMT_VER）─────────────────────
 *      BL 搬完  : ota_state = COMMITTED（"镜像已落地，等 APP 确认"），跳转
 *      APP 自检 : ota_state == COMMITTED ⇒ 本函数把它收尾成 IDLE
 *                 running_ver <- pending_ver，pending 清零，dl_* 清零，
 *                 boot_try = 0，ota_succ_cnt++（只在新旧版本不同时加一）
 *   掉电落在两步之间：state 停在 COMMITTED，下次上电 BL 不校验直接跳
 *   （COMMITTED != PENDING_REBOOT ⇒ 连 CRC 都不算），APP 再提交一次，幂等。
 *
 *  ── 为什么必须先清 dl_*（v0.38 上板实证）──────────────────────────────
 *      [0][APP] dl offset=49088 total=49152  ← A1 的残留从没被清
 *     param_get_download() 判据是 offset>0 && offset<=total 就算有效断点
 *     ⇒ 下次真下载会把上一次的残留当成续传起点，从 49088 接着写，直接写坏镜像。
 *============================================================================*/
static u8 app_ver_eq(const ota_ver_t *a, const ota_ver_t *b)
{
    if (a->major != b->major) return 0U;
    if (a->minor != b->minor) return 0U;
    if (a->patch != b->patch) return 0U;
    if (a->build != b->build) return 0U;
    return 1U;
}

static void app_ota_commit(void)
{
    static u8 s_new;                    /* 1 = 这次确实换了版本（要计成功数） */
    param_t *p;
    ota_ret_t rc;

    p = param_get();
    if (p->ota_state != (u8)OTA_ST_COMMITTED) return;

    /* 先判"是不是真的换了版本"：同版本重刷（A1 自己复制自己）不该计成功 */
    s_new = (u8)(app_ver_eq(&p->pending_ver, &p->running_ver) ? 0U : 1U);

    p->running_ver.major = p->pending_ver.major;
    p->running_ver.minor = p->pending_ver.minor;
    p->running_ver.patch = p->pending_ver.patch;
    p->running_ver.build = p->pending_ver.build;

    p->pending_ver.major = 0U;
    p->pending_ver.minor = 0U;
    p->pending_ver.patch = 0U;
    p->pending_ver.build = 0U;

    /* 断点必须清零 —— 否则下一次真下载会从"上一次 A1 的残留"接着写 */
    p->dl_offset    = 0UL;
    p->dl_total     = 0UL;
    p->dl_crc32     = 0UL;
    p->dl_img_crc32 = 0UL;
    p->dl_start_ms  = 0UL;

    p->boot_try  = 0U;
    p->ota_state = (u8)OTA_ST_IDLE;
    if (s_new) p->ota_succ_cnt++;

    rc = param_save(1);

    LOG_I0(TAG, "---- A3 commit: new firmware confirmed ----");
    /* 一行最多 4 个值槽，版本号正好 4 个 */
    LOG_I4(TAG, "run=%u.%u.%u.%u",
           (u16)p->running_ver.major, (u16)p->running_ver.minor,
           (u16)p->running_ver.patch, (u16)p->running_ver.build);
    LOG_I2(TAG, "commit rc=%u succ=%lu", (u16)rc, p->ota_succ_cnt);
    if (rc != OTA_OK)
    {
        /* 写不进去也无所谓：state 仍是 COMMITTED，下次上电会再提交一次 */
        LOG_E0(TAG, "commit NOT persisted, will retry next boot");
    }
}

/*==============================================================================
 *                    最小验证 APP（APP_MIN_TEST = 1）
 *
 *  验收标准只有一行：
 *      [n][APP] ==== OTA TEST OK: APP v0.2.0 ====
 *
 *  为"把失败原因收窄"而故意不做的事：
 *    · 不联网、不读模组、不碰 STAGING —— 那些是下载器与 BL 的事，
 *      目标固件再去动一次，只会让"下载失败"和"目标固件崩了"混成一个现象；
 *    · 不初始化 IAP 读写 —— 一个字节都不写，跑挂了代价为零；
 *    · 不开 EA —— 中断所有权问题（见文件头）留到 P1 解决，这里绕开。
 *
 *  但两件"收尾"必须做，否则会污染账本：
 *    ① ota_state: COMMITTED -> IDLE（app_ota_commit，否则每次上电 BL 重装）
 *    ② boot_try 清零（BL 每次跳转前 inc，不清会累积到 20 次触发回滚）
 *============================================================================*/
#if (APP_MIN_TEST != 0)

static volatile u16 xdata s_min_spin;

static void app_min_delay_ms(u16 ms)
{
    u16 n;

    for (n = ms; n != 0U; n--)
    {
        /* 空转延时：EA=0，bsp_timer_delay_ms() 靠中断自增 ⇒ 永远等不到，
         * 而且它在内部喂狗 ⇒ 表现为"卡死但复位不了"。只能这样等。 */
        for (s_min_spin = 0U; s_min_spin < APP_SPIN_PER_MS; s_min_spin++)
        {
        }
        bsp_wdt_feed();
    }
}

void main(void)
{
    param_t *p;
    u16      hb;
    u16      k;

    /* 从 BL 跳过来时 EA 可能是开的，先无条件关掉（理由见文件头） */
    EA = 0;

    bsp_clock_init();
    bsp_gpio_init();
    (void)bsp_uart_init(APP_LOG_BAUD);
    bsp_timer_init_1ms();               /* 只配置硬件；EA=0，中断不会响应 */
    bsp_wdt_init(APP_WDT_PS);
    log_init(LOG_LVL_INFO);
    EA = 0;

    bsp_led_on();
    app_min_delay_ms(120);
    bsp_led_off();

    /* ★★ 全部验收标准就是这一行 ★★
     *  版本串来自 app_config.h，与镜像头里的 ver 字段同源（打包工具也读它），
     *  所以"打出来的版本" = "头里写的版本" = "BL 认为装上的版本"，三者不会分家。 */
    LOG_I1(TAG, "==== OTA TEST OK: %s ====", APP_VER_STRING);
    LOG_I2(TAG, "app_code=0x%lx app_iap=0x%lx",
           (u32)OTA_APP_CODE_BASE, (u32)OTA_APP_IAP_BASE);

    (void)param_init();
    p = param_get();

    LOG_I2(TAG, "state=%u boot_try=%u", (u16)p->ota_state, (u16)p->boot_try);
    LOG_I4(TAG, "run=%u.%u.%u.%u",
           (u16)p->running_ver.major, (u16)p->running_ver.minor,
           (u16)p->running_ver.patch, (u16)p->running_ver.build);
    LOG_I4(TAG, "pending=%u.%u.%u.%u",
           (u16)p->pending_ver.major, (u16)p->pending_ver.minor,
           (u16)p->pending_ver.patch, (u16)p->pending_ver.build);

    app_ota_commit();

    /* commit 只在 state==COMMITTED 时动手；普通上电也得把 boot_try 清掉，
     * 否则 BL 每次跳转前 inc 的那个计数会一直累到回滚阈值。 */
    if (param_get()->boot_try != 0U)
    {
        LOG_I1(TAG, "boot_try_clear rc=%u", (u16)param_boot_try_clear());
    }

    LOG_I0(TAG, "---- minimal app running, heartbeat follows ----");

    hb = 0U;
    for (;;)
    {
        bsp_wdt_feed();
        bsp_led_toggle();
        app_min_delay_ms(APP_LED_PERIOD_MS);

        hb++;
        if ((hb % (u16)APP_HB_TICKS) == 0U)
        {
            k = (u16)(hb / (u16)APP_HB_TICKS);
            LOG_I2(TAG, "MIN OK #%u boot_cnt=%lu", k, p->boot_cnt);
            if ((hb % ((u16)APP_HB_TICKS * 2U)) == 0U)
            {
                LOG_I2(TAG, "  state=%u succ=%lu",
                       (u16)p->ota_state, p->ota_succ_cnt);
            }
        }
    }
}

#else   /* ======================= 完整 APP ======================= */

static void app_delay_ms(u16 ms);   /* 定义在文件后部，A1 停机循环要用 */

/*==============================================================================
 * P1-A1 自测：造一个"合法镜像"放进 STAGING
 *
 *   A1 阶段 BL 只做"读懂 + 校验"，不搬运；所以得先有人往 STAGING 里放东西。
 *   最安全的放法就是**把当前 APP 区自己复制过去**：
 *     · 内容真实 —— 就是正在跑的这份固件，不是伪造数据
 *     · 格式合法 —— 头是照 ota_types.h 现造的
 *     · 后果可控 —— A2 打开搬运后，搬回去的还是同一份固件；即使搬运有 bug
 *       也不会变砖（最坏停在 BL，靠 ISP 冷启动重烧救回，不用拆片）
 *
 *   ── 触发条件与"只做一次" ──────────────────────────────────────────────
 *       ota_state == IDLE  且  rollback_flag != APP_A1_DONE_MARK
 *   做完把 rollback_flag 置 0xA1 当标记，此后每次上电都跳过。
 *
 *   ★ 借用 rollback_flag 当标记是 **A1 阶段的临时约定**，不是最终设计。
 *     A2/A3 会把"是否已提交"放进 param 的正规字段（届时递增 PARAM_FMT_VER）。
 *     现在不动 param 结构，是为了让 A1 的改动面保持在"加一个函数"这个量级 ——
 *     1 个新文件 + 1 个新函数 + 1 个分支，出问题时好回退。
 *
 *   ── 为什么正文只有 48K - 64 ───────────────────────────────────────────
 *   STAGING 只有 48 KB，镜像头占前 64 B，所以正文最多 48K-64 = 49088 B。
 *   APP 区正好 48 KB，末尾 64 B 不参与 OTA。那 64 B 永远是空片 0xFF
 *   （当前 APP 代码只到 0xFF733E，离 0xFFBFC0 还有 19 KB 空余）。
 *   ⚠ 这条是硬限制：APP 代码一旦涨过 49088 B，OTA 就会静默丢掉尾部。
 *     A3 会加编译期断言把这条卡住，现在先写在注释里。
 *
 *   ── 耗时与看门狗 ──────────────────────────────────────────────────────
 *   擦 96 个扇区 + 写 49088 B ≈ 3~5 s。擦除自己会喂狗（flash_iap_erase_range），
 *   拷贝循环里每块喂一次。中途掉电只会留下半个镜像 → 下次 BL 校验必然不过
 *   → 回到 IDLE，不会误升级。
 *============================================================================*/
/* ★ "已造过镜像"的标记值**跟着 build 号走**。
 *   这样"想再试一次"的操作就是"把 APP_VER_BUILD +1 再烧一遍"，
 *   不需要手工去清 param（param 在 IAP 区，ISP 重烧代码不会碰它，
 *   固定标记会导致重烧之后仍然跳过造镜像，白折腾一轮）。
 *   build=1 → 0xA1；build=2 → 0xA2 …… */
#define APP_A1_DONE_MARK    ((u8)(0xA0U | ((u8)APP_VER_BUILD & 0x0FU)))
#define APP_A1_IMG_LEN      (OTA_APP_IAP_SIZE - OTA_IMG_HDR_RESERVE)   /* 49088 B */
#define APP_A1_CHUNK        128U

static u8  xdata s_a1buf[APP_A1_CHUNK];       /* 拷贝缓冲           */
static u32 xdata s_a1crc;                     /* 正文 CRC32         */
static u32 xdata s_a1off;                     /* 正文内偏移         */
static u16 xdata s_a1n;                       /* 本次搬运字节数     */
static ota_img_hdr_t xdata s_a1img;           /* 现造的镜像头 42 B  */
static u32 xdata s_a1hcrc;                    /* 镜像头 CRC（v0.27） */
static u32 xdata s_a1rb;                      /* STAGING 回读 CRC（v0.28） */

static const u8 s_a1target[8] = { 'S', 'T', 'C', '3', '2', 'G', '1', '2' };

static void app_a1_make_image(void)
{
    param_t *p;
    ota_ret_t rc;
    u8 i;

    LOG_W0(TAG, "A1: build image from APP region -> STAGING");

    /* 1) 擦整块 STAGING --------------------------------------------------- */
    rc = flash_iap_erase_range(OTA_STAGE_BASE, OTA_STAGE_SIZE);
    if (rc != OTA_OK)
    {
        LOG_E1(TAG, "A1 erase rc=%u -> give up, stay IDLE", (u16)rc);
        return;
    }

    /* 2) 逐块搬运并累加 CRC ----------------------------------------------- */
    s_a1crc = crc32_begin();
    s_a1off = 0UL;
    while (s_a1off < (u32)APP_A1_IMG_LEN)
    {
        s_a1n = (u16)((((u32)APP_A1_IMG_LEN - s_a1off) > (u32)APP_A1_CHUNK)
                          ? (u32)APP_A1_CHUNK
                          : ((u32)APP_A1_IMG_LEN - s_a1off));

        rc = flash_iap_read(OTA_APP_IAP_BASE + s_a1off, s_a1buf, s_a1n);
        if (rc == OTA_OK)
        {
            rc = flash_iap_write(OTA_STAGE_BASE + OTA_IMG_HDR_RESERVE + s_a1off,
                                 s_a1buf, s_a1n);
        }
        if (rc != OTA_OK)
        {
            LOG_E2(TAG, "A1 copy rc=%u off=%lu -> give up", (u16)rc, s_a1off);
            return;                      /* state 仍是 IDLE，下次上电重试 */
        }

        s_a1crc = crc32_update(s_a1crc, s_a1buf, s_a1n);
        s_a1off += (u32)s_a1n;
        bsp_wdt_feed();
    }
    s_a1crc = crc32_final(s_a1crc);

    /* 2b) ★ v0.28：回读 STAGING，直接回答"写进去的是不是源数据" -----------
     *
     *   此前 BL 的 step=9（正文 CRC）一直不过，而"APP 写侧"和"BL 读侧"
     *   各自看代码都合理，缺的就是这**一个数**：
     *       stg == 上一行的 crc  → STAGING 内容正确 ⇒ 问题在 BL 侧（读/算）
     *       stg != 上一行的 crc  → 写进去的就错了 ⇒ 先修写侧，别动 BL
     *   顺带验证 flash_iap_write 的 49088 B 长写是否逐字节真的落了盘，
     *   以及"擦 48 KB"是否真的把整块都擦干净（未擦干净时位只能 1→0，
     *   表现正是"写入返回 OK 但内容不对"）。
     *
     *   代价：多读一遍 49088 B，日志会晚约 5 s。A1 只跑一次，值这个钱。 */
    s_a1rb  = crc32_begin();
    s_a1off = 0UL;
    while (s_a1off < (u32)APP_A1_IMG_LEN)
    {
        s_a1n = (u16)((((u32)APP_A1_IMG_LEN - s_a1off) > (u32)APP_A1_CHUNK)
                          ? (u32)APP_A1_CHUNK
                          : ((u32)APP_A1_IMG_LEN - s_a1off));

        rc = flash_iap_read(OTA_STAGE_BASE + OTA_IMG_HDR_RESERVE + s_a1off,
                            s_a1buf, s_a1n);
        if (rc != OTA_OK)
        {
            LOG_E1(TAG, "A1 readback rc=%u -> skip", (u16)rc);
            break;
        }
        s_a1rb   = crc32_update(s_a1rb, s_a1buf, s_a1n);
        s_a1off += (u32)s_a1n;
        bsp_wdt_feed();
    }
    s_a1rb = crc32_final(s_a1rb);
    LOG_I1(TAG, "A1 stg crc=%lx", s_a1rb);

    /* 3) 造镜像头（必须在正文写完、CRC 算完之后）--------------------------- */
    s_a1img.magic      = OTA_IMG_MAGIC;
    s_a1img.hdr_len    = (u16)OTA_IMG_HDR_RESERVE;   /* 64 = 正文偏移 */
    s_a1img.hdr_ver    = 0x0001U;
    s_a1img.ver.major  = (u8)APP_VER_MAJOR;
    s_a1img.ver.minor  = (u8)APP_VER_MINOR;
    s_a1img.ver.patch  = (u8)APP_VER_PATCH;
    s_a1img.ver.build  = (u8)APP_VER_BUILD;
    s_a1img.img_len    = (u32)APP_A1_IMG_LEN;
    s_a1img.img_crc32  = s_a1crc;
    s_a1img.entry      = OTA_APP_CODE_BASE;
    s_a1img.build_time = 0UL;                        /* 板子没有 RTC，留 0 */
    for (i = 0U; i < 8U; i++) s_a1img.target[i] = s_a1target[i];
    s_a1img.reserved   = 0xFFFFU;

    /* CRC 覆盖前 36 字节（magic .. target），不含 hdr_crc32 自身与 reserved。
     * 顺序上必须在 hdr_crc32 赋值之前算 —— 但那 36 字节里本来也没有它。
     * v0.27：不再调 crc32_calc（那层封装每次上电给出不同值），三步分开写。 */
    OTA_CRC32_OF(s_a1hcrc, (const u8 *)&s_a1img, 36U);
    s_a1img.hdr_crc32  = s_a1hcrc;

    rc = flash_iap_write(OTA_STAGE_BASE, (const u8 *)&s_a1img,
                         (u16)sizeof(ota_img_hdr_t));
    if (rc != OTA_OK)
    {
        LOG_E1(TAG, "A1 hdr write rc=%u -> give up", (u16)rc);
        return;
    }

    /* 4) 落参数：让 BL 下次上电去校验 -------------------------------------- */
    p = param_get();
    p->ota_state     = (u8)OTA_ST_PENDING_REBOOT;
    p->pending_ver   = s_a1img.ver;
    p->dl_total      = (u32)OTA_IMG_HDR_RESERVE + (u32)APP_A1_IMG_LEN;
    /* ★ v0.39：offset 必须和 total **同一个口径（含镜像头）**。
     *   以前写的是 APP_A1_IMG_LEN（49088），于是日志打成
     *       dl offset=49088 total=49152
     *   —— 差 64 B，永远差 64 B。这个坑不会当场报错，只会让
     *   param_get_download() 认定"有未完成断点"，等 P1-B 真下载时
     *   从错误的位置续传。改成同一个数，等价于"这次下载已经完成"。 */
    p->dl_offset     = (u32)OTA_IMG_HDR_RESERVE + (u32)APP_A1_IMG_LEN;
    p->dl_img_crc32  = s_a1crc;
    p->rollback_flag = (u8)APP_A1_DONE_MARK;         /* A1 专用"已造过"标记 */

    rc = param_save(1);
    if (rc != OTA_OK)
    {
        LOG_E1(TAG, "A1 param_save rc=%u -> state may not persist", (u16)rc);
    }

    /* 5) 报告并停机，等人工断电 -------------------------------------------- */
    LOG_I2(TAG, "A1 image ready len=%lu crc=%lx", (u32)APP_A1_IMG_LEN, s_a1crc);
    LOG_W0(TAG, "A1: POWER-CYCLE the board now");
    LOG_W0(TAG, "A1: next boot BL will verify STAGING (read-only, no apply)");

    /* 停机而不是软复位：
     *   软复位要嘛靠 WDT 超时（时间不可控）、要嘛跳 0xFF0000（不重新初始化
     *   外设），两种都会给"这次到底算不算一次干净的上电"留下争议。
     *   拔插电源是最干净的，而用户本来就要断电回传日志，零额外成本。 */
    for (;;)
    {
        bsp_wdt_feed();
        bsp_led_toggle();
        app_delay_ms(APP_LED_PERIOD_MS);
    }
}



/*==============================================================================
 * 工具
 *============================================================================*/

/* P0 专用粗延时：不依赖 Timer0 中断，靠 volatile 计数器防止被优化掉。
 * P1 起请改用 bsp_timer_delay_ms()（那时中断已归属 APP）。 */
static volatile u16 xdata s_spin;

static void app_delay_ms(u16 ms);   /* A1 那段停机循环要用，先声明 */

static void app_delay_ms(u16 ms)
{
    while (ms != 0)
    {
        for (s_spin = 0; s_spin < APP_SPIN_PER_MS; s_spin++)
        {
            /* 空转，靠 volatile 让编译器不敢删 */
        }
        ms--;
        bsp_wdt_feed();                 /* 长延时期间不能断狗粮 */
    }
}

/* 打印一个版本号
 *
 * ★ 拆成两行：`who` + 4 个字段 = 5 个值，而一条 LOG 最多 4 个槽。
 *   以前这里写成一行，靠 varargs 硬塞 —— 那正是日志数字错乱的来源之一。
 *   现在超过 4 个值就必须拆行，这是编译期就能看出来的约束。 */
static void app_print_ver(const char *who, const ota_ver_t *v)
{
    if (v == 0) return;
    LOG_I1(TAG, "%s =", who);
    LOG_I4(TAG, "  %u.%u.%u.%u",
           (u16)v->major, (u16)v->minor, (u16)v->patch, (u16)v->build);
}

/* P1-A3 的 app_ver_eq() / app_ota_commit() 已上移到文件前部（#if 之外）：
 * 完整版与最小验证版共用同一份实现，避免两份提交逻辑各自漂移。
 * 协议说明与"为什么要先清 dl_*"都在那里的注释里。 */

/*==============================================================================
 * P0 自检
 *============================================================================*/
static void app_selftest(void)
{
    static u8 xdata b[4];               /* 读回缓冲：必须 xdata，栈只有 128B */
    ota_ret_t rc;
    param_t  *p;
    u8        ok;
    u32       stage_kb;
    u32       app_kb;

    LOG_I0(TAG, "---- P0 selftest ----");

    /* --- 1. 定位：确认链接地址与分区表一致 ------------------------------- */
    LOG_I2(TAG, "app_code=0x%lx  app_iap=0x%lx", (u32)OTA_APP_CODE_BASE, (u32)OTA_APP_IAP_BASE);

    /* ★ 一条 LOG 最多 4 个值槽，且参数只放变量或常量
     *
     *   以前这里写成一行：
     *     LOG_I(TAG,"stage=%luK%s", ota_part_size(...)>>10, ok?"":" (part invalid)")
     *   实测打出来是 `stage= K` —— %lu 的整个数字凭空消失。
     *   根因是 varargs 的全局参数块被 overlay 池踩了（见 log.h 文件头），
     *   v0.7 已彻底改成"固定 4 槽、非可变参数"，那个机制不再存在。
     *
     *   剩下两条仍然要守：
     *     1) 一行不超过 4 个值（超了编译器直接报参数过多，这是好事）；
     *     2) 参数里尽量不放函数调用 —— 被调函数的局部 xdata 仍可能与
     *        调用方的活跃变量共用 overlay 地址，先把结果取到变量里最稳。 */
    stage_kb = ota_part_size(PART_STAGING, &ok) >> 10;
    LOG_I1(TAG, "stage=%luK", stage_kb);
    if (!ok) LOG_W0(TAG, "  STAGING partition invalid");

    app_kb = ota_part_size(PART_APP_IAP, &ok) >> 10;
    LOG_I1(TAG, "app=%luK", app_kb);
    if (!ok) LOG_W0(TAG, "  APP partition invalid");
    LOG_I1(TAG, "bl_ro=%u", (u16)OTA_BL_IS_READONLY);

    /* --- 2. IAP 可用性（与 STC-ISP 的 EEPROM=128K 必须一致）-------------- */
    rc = flash_iap_init(128);
    {
        u32 kb = (u32)flash_iap_eeprom_kb();
        LOG_I1(TAG, "flash_iap_init(128) rc=%u", (u16)rc);
        LOG_I1(TAG, "iap size=%luK", kb);
    }

    if (rc != OTA_OK)
    {
        LOG_E0(TAG, "IAP init failed -> param/OTA unusable, heartbeat only");
        return;
    }

    /* --- 3. 读回 APP 区首 4 字节：能读到，说明 IAP 视图与代码区确实重合 --- */
    rc = flash_iap_read(OTA_APP_IAP_BASE, b, 4);
    if (rc == OTA_OK)
    {
        LOG_I4(TAG, "read app[0..3] = %x %x %x %x", (u16)b[0], (u16)b[1], (u16)b[2], (u16)b[3]);
    }
    else
    {
        LOG_E1(TAG, "read app base failed rc=%u (IAP_ADDR missing? see gate0)", (u16)rc);
    }

    /* --- 4. 参数区 ------------------------------------------------------- */
    rc = param_init();
    if (rc == OTA_OK)
    {
        LOG_I0(TAG, "param_init OK");
    }
    else if (rc == OTA_ERR_CRC)
    {
        LOG_W0(TAG, "param blank/corrupt -> factory defaults written");
    }
    else
    {
        LOG_E1(TAG, "param_init rc=%u", (u16)rc);
    }

    p = param_get();
    LOG_I2(TAG, "seq=%lu boot_cnt=%lu", p->seq, p->boot_cnt);
    LOG_I2(TAG, "boot_try=%u ota_state=%u", (u16)p->boot_try, (u16)p->ota_state);
    LOG_I2(TAG, "dl offset=%lu total=%lu", p->dl_offset, p->dl_total);
    app_print_ver("running_ver", &p->running_ver);
    app_print_ver("pending_ver", &p->pending_ver);

    /* --- 5. 本轮启动宣告成功（P0 阶段默认不落盘，见 app_config.h）-------- */
#if (APP_CLEAR_BOOT_TRY != 0)
    rc = param_boot_try_clear();
    LOG_I1(TAG, "boot_try_clear rc=%u", (u16)rc);
#endif

    /* --- 6. 结论 --------------------------------------------------------- */
    LOG_I1(TAG, "flashing OK: %s", APP_VER_STRING);
    LOG_I0(TAG, "---- paste this log back, then decide APP_STAGE_P0 ----");
}

/*==============================================================================
 * ★ 第 0 步：模组 AT 探活（APP_AT_PROBE_TEST）
 *
 * 只做一件事：证明 STC32G <-> Y100P 这条 UART2 链路通、波特率对、模组在跑。
 * **不下载、不擦写 STAGING、不动 param** —— 出任何问题都能反复上电重试。
 *
 * 八条命令的分工（每条都把**原始应答**照打出来，不做美化）：
 *   AT        链路通不通 / 波特率对不对   ← 唯一的命门（帧 = 'A' 'T' CR LF）
 *   ATI       对面到底是什么             ← 防"乱码碰巧含 OK"被误判成通
 *   AT+CGMR   模组固件版本               ← 确认是移远 AT 固件
 *   AT+IPR?   波特率模式                 ← 自适应 or 被写死
 *   ATE0      关回显                     ← 给后面的 HTTP 下载清场
 *   AT+CPIN?  SIM 卡                     ← READY 才能注网
 *   AT+CSQ    信号                       ← rssi>=10 才勉强能用
 *   AT+CEREG? 注网                       ← stat=1(本地)/5(漫游) 才算注上
 *   AT+CGATT? PDP 附着                   ← 1 才能跑 HTTP
 *
 * ★ 结论必须"周期重播"：板载 CH340 插线即上电，开机日志 3 ms 就打完了，
 *   用户不可能"先开串口再上电"。所以结论存进 s_pb_*，主循环每 2 s 重播。
 * ★ 等待期间一行日志都不打（uart_at.c 也是同理）：115200 下打 30 字符要
 *   2.6 ms，而 UART2 只有一个接收寄存器，打日志就会把应答吃掉。
 *   本文件的做法是"拿到终态之后再打"，日志里的 RX< 是**事后补打**的。
 *============================================================================*/
/*------------------------------------------------------------------------------
 * ★ 复位源诊断（RSTFLAG @ 0x7EFE99）—— 放在探活开关之外，main() 无条件调用
 *
 *   为什么必须加：v0.45 上板时日志停在 `RXD2 idle level=1`，之后**一个字都没有**，
 *   紧接着又从 BL 重启（boot_try 一路涨）。"没有日志就复位"这件事，
 *   光看串口无法区分是看门狗、掉电、还是跑飞撞了非法指令。
 *   RSTFLAG 是芯片自己记的账，读它比猜快一百倍：
 *       bit0 硬件复位（上电/复位脚）   bit1 软件复位(SWRST)
 *       bit2 看门狗复位(WDT)           bit3 低压检测复位(LVD)
 *       bit5 时钟失效复位
 *   各位会累积、"曾经被狗咬过"会一直留着 —— 正好当证据。
 *
 *   注意：RSTFLAG 属于扩展 SFR（0x7E 段），必须先开 EAXFR(P_SW2.7) 才读得到；
 *        不开的话读回来是 0，会得出"从来没复位过"的错误结论。
 *   ★ EAXFR 只动 P_SW2.7；bit0(S2_S) 保持 0 ⇒ UART2 仍在 P1.0/P1.1，不受影响。
 *----------------------------------------------------------------------------*/
static void app_log_rstflag(void)
{
    static u8 xdata rf;

    EAXFR = 1;                  /* 打开扩展 SFR 访问 */
    rf = RSTFLAG;

    /* ★ v0.6.13：原写 LOG_I2 却给了 3 个实参（C307），第 3 个值是白拿的
     *   （这次碰巧读对了，纯属运气）。槽位宏必须与实参个数一一对应。 */
    LOG_I3(TAG, "RSTFLAG=0x%x (wdt=%u sw=%u)",
           (u16)rf, (u16)((rf >> 2) & 1U), (u16)((rf >> 1) & 1U));
    LOG_I2(TAG, "  pwr=%u lvd=%u", (u16)(rf & 1U), (u16)((rf >> 3) & 1U));
}

#if (APP_AT_PROBE_TEST != 0)

/* 探活结论（static；全部 '\0' 结尾，主循环直接 %s 重播） */
static char xdata s_pb_ati[16];      /* ATI 型号                              */
static char xdata s_pb_cgmr[16];     /* AT+CGMR 固件版本                      */
static char xdata s_pb_ipr[14];      /* AT+IPR? 波特率模式（0=自适应）         */
static char xdata s_pb_csq[14];      /* +CSQ: rssi,ber                        */
static char xdata s_pb_creg[14];     /* +CEREG: n,stat                        */
static char xdata s_pb_cpin[14];     /* +CPIN: READY                          */
static char xdata s_pb_gatt[10];     /* +CGATT: 1                             */
static u8   xdata s_pb_ok;           /* 1 = AT 通了（后面几条才有意义）        */
static u32  xdata s_pb_baud;         /* 自适应得到的波特率，0 = 未确认          */
static u16  xdata s_at_replay_n;     /* AT keepalive 已重发的次数（诊断用）     */
static u16  xdata s_pb_last_rx;      /* 上一次 ping 收到的字节数（变化才打日志） */

/*------------------------------------------------------------------------------
 * 发一条 AT 命令，把原始应答压成单行存进 dst（去首尾空格、连续空格折一个）。
 * ★ 形参 3+3+1 = 7 B ≤ 8，走寄存器，不生成参数块（铁律 14）。
 * ★ 结果码不返回：探活只关心"应答长什么样"，长相本身就在 dst 里。
 *----------------------------------------------------------------------------*/
static void app_at_once(const char *cmd, char *dst, u8 cap)
{
    /* ★ 三个形参全部先拷 static：本函数里 at_cmd_wait() 是一个大调用，
     *   之后还要继续用 cmd/dst/cap —— 留在寄存器上第二次就可能变垃圾
     *   （铁律 11：跨调用还活着的值一律不许留寄存器）。 */
    static const char * xdata s_cmd;
    static char * xdata s_dst;
    static u8   xdata s_cap;
    static char xdata raw[64];
    static u16  xdata n;
    static u8   xdata i;
    static u8   xdata j;
    static u8   xdata tr;        /* 已发几次（v0.53 重试）                  */
    static u8   xdata got;       /* 1 = 本次收到了终态 OK                  */

    s_cmd = cmd;
    s_dst = dst;
    s_cap = cap;

    /*--------------------------------------------------------------------------
     * ★★ v0.53：带重试地发命令。
     *
     *   为什么必须有：v0.51 上板实测线上**整字节丢失**——字符全对、只是少了几个
     *   （"+CPIN: READY" → "+PIN RAD"，"OK" → "O"）。一次就判"没应答"会把
     *   一条好命令的结果丢掉，而重发一次大概率能凑齐一个完整的 "OK"。
     *   这与"波特率不对"完全不同：波特率错时整片 '.'，重试也没用 ——
     *   所以判据仍然只看 at_cmd_wait 的返回值，不猜内容。
     *------------------------------------------------------------------------*/
    raw[0] = 0;
    n      = 0U;
    got    = 0U;
    for (tr = 0U; tr < APP_AT_TRY_MAX; tr++)
    {
        n    = 0U;
        raw[0] = 0;
        if (at_cmd_wait(s_cmd, 0, AT_TO_CMD, raw, 64U, &n) == AT_RESP_OK)
        {
            got = 1U;
            break;
        }
        bsp_wdt_feed();
    }

    /* 原始应答：这一行就是判据（若出现一片 '.'，说明波特率不对/线接反）
     * ★ 带上 [nB]：字节数是丢字节的定量证据（同一条命令两次跑长度不一样
     *   = 线上在丢字节，而不是模组换了说法）。 */
    LOG_I1(TAG, "TX> %s", s_cmd);
    if (raw[0] == 0)
    {
        LOG_I0(TAG, "RX< (no data)");
    }
    else
    {
        LOG_I2(TAG, "RX< [%uB] %s", n, raw);
    }
    if (got == 0U)
    {
        /* 打出来才算"证据"：没这一行，读者会以为上面那串就是全部。 */
        LOG_E1(TAG, "  ^^ no OK after %u tries", (u16)APP_AT_TRY_MAX);
    }

    /* dst 传 0 表示"只要打印、不留结论"（ATE0 这种） */
    if (s_dst == 0 || s_cap < 2U) return;

    /* 压缩成单行结论 */
    i = 0U;
    j = 0U;
    while (raw[i] != 0 && j < (u8)(s_cap - 1U))
    {
        if (raw[i] != ' ' || (j > 0U && s_dst[(u16)j - 1U] != ' '))
        {
            s_dst[j] = raw[i];
            j++;
        }
        i++;
    }
    while (j > 0U && s_dst[(u16)j - 1U] == ' ') j--;
    s_dst[j] = 0;
}

/*------------------------------------------------------------------------------
 * 探活主流程：AT 重试 -> 逐条查询 -> 打结论。返回 1 = 链路通。
 *----------------------------------------------------------------------------*/
static u8 app_at_probe(void)
{
    static ota_ret_t xdata ab;
    static u32 xdata baud;
    static u32 xdata rb;
    static u16 xdata rn;
    static u16 xdata d_ok;
    static u16 xdata d_to;
    static u16 xdata d_ed;
    static u16 xdata d_lo;
    static u8  xdata i;
    static u8  xdata n;
    static u8  xdata r;
    static u8  xdata lq;         /* 链路质量：n 次 AT 里干净答对几次（v0.53） */

    s_pb_ok    = 0U;
    s_pb_baud  = 0UL;
    s_pb_ati[0]  = 0;  s_pb_cgmr[0] = 0;  s_pb_ipr[0] = 0;
    s_pb_csq[0]  = 0;  s_pb_creg[0] = 0;
    s_pb_cpin[0] = 0;  s_pb_gatt[0] = 0;

    LOG_I0(TAG, "==== AT probe: Y100P on " AT_PORT_NAME_STR " " AT_PIN_NAME_STR " ====");

    /* baud 传 0 ="先不定档"：UART2 先按出厂 115200 配起来，
     * 真正的档位由 at_autobaud() 扫出来。
     * ★ 现场要求："确认波特率之前的数据都丢弃不处理" ——
     *   at_uart_init(0) 之后 at_get_baud() 恒为 0，自适应期间收到的任何字节
     *   都只用来判断"整行是不是 OK/RDY"，不进 URC、不置错误、不留在缓冲里。 */
    if (at_uart_init(0UL, 0, 0) != OTA_OK)
    {
        LOG_E0(TAG, "at_uart_init FAIL (uart2 setup)");
        return 0U;
    }

    /*--------------------------------------------------------------------------
     * ★★ v0.52：MCU 侧自证（P1.1 <-> P1.0 短接检测）
     *
     *   12 档全 0 B + edges=0 时，现场只有三种可能，必须先排掉第三种：
     *     A) 模组没开机/没供电
     *     B) **TX/RX 没交叉**（模组 TXD 接在 P1.1、模组 RXD 接在 P1.0）
     *     C) MCU 的 RXD2 通路坏了
     *   把 P1.1 与 P1.0 短接（拔掉模组线）后本行应打 1；再跑一轮 autobaud
     *   会在 115200 档看到 4 B（自己发的 "AT" 被自己收到）。
     *   两条都成立 ⇒ MCU 收发无误，问题一定在模组侧或接线。
     *   没短接时打 0（正常接法下就该是 0，不报错）。
     *------------------------------------------------------------------------*/
    {
        u8 lb = at_pin_loopback();
        LOG_I1(TAG, "TX<->RX shorted=%u (1 = MCU self-test HW ok)", (u16)lb);
        if (lb != 0U)
        {
            LOG_W0(TAG, "PINS ARE SHORTED - unplug module line before AT test!");
        }
    }

    /*--------------------------------------------------------------------------
     * ★★ 先等模组开机日志吐完，一个字节都别发
     *
     *   银尔达《移远AT固件HTTP测试实例》原话：
     *     "4G模块在上电、重启的时候，主动发出开机日志，这些日志一般需要过滤。
     *       然后再去执行AT命令。"
     *   所以等待不是保险，是流程要求：日志没吐完就发 AT，我们的命令会混进
     *   日志流里，回显和 OK 都可能被截断，于是这一轮被记成"0 B"，
     *   反过来误导我们以为硬件坏了。
     *------------------------------------------------------------------------*/
    LOG_I1(TAG, "waiting %lu ms for Y100P boot log to finish",
           (u32)APP_MODEM_BOOT_MS);
    at_delay_ms((u16)APP_MODEM_BOOT_MS);
    LOG_I0(TAG, "boot window over, start probing");

    LOG_I0(TAG, "at uart ready, autobaud 1200..460800 (cmd = AT + CRLF)");

    /* ★ 空闲电平自检：UART 空闲态恒为高电平。
     *   在发任何 AT 之前先读 P1.0：读到 0 就能立刻断定"模组没上电 /
     *   模组 TXD 没接过来 / 接到 GND / TX-RX 没交叉"，
     *   不用等 10 秒超时再猜。读到 1 只说明"线上没被拉死"，不代表模组在跑。 */
    LOG_I1(TAG, "RX idle level=%u (expect 1) ch=" AT_PORT_NAME_STR,
           (u16)at_uart2_rxd_level());

    /*--------------------------------------------------------------------------
     * ---- 1) 线路诊断（先分清"固件"还是"硬件"，再谈波特率）----
     *
     *   v0.47 上板的现象是"10 档全 0 B"。0 字节这件事有三个完全不同的根因，
     *   只看串口分不开，所以这里用两条**互不重叠**的判据把它劈开：
     *
     *     tx_to > 0        ⇒ UART2 时钟死（Timer2 没跑）→ 固件，与模组无关
     *     edges == 0       ⇒ 线上一次跳变都没有 → 模组没上电/没启动/TXD 没接
     *     edges > 0 但 0 B ⇒ 收通道坏（S2REN/引脚模式）
     *
     *   ★ at_line_diag() 完全绕开 UART：它只数 P1.0 有没有跳变。
     *     模组只要在应答，哪怕波特率不对（收到的是乱码），引脚就一定会动。
     *------------------------------------------------------------------------*/
    at_diag_clear();
    at_line_diag();
    bsp_wdt_feed();
    d_ok = at_diag_tx_ok();
    d_to = at_diag_tx_to();
    d_ed = at_diag_edges();
    d_lo = at_diag_low();

    LOG_I2(TAG, "diag#1 tx_ok=%u tx_to=%u", d_ok, d_to);
    /* ★ 这行是"命令到底发全了没有"的自证：
     *   at_line_diag() 发 4 次 "AT"，每次 4 字节（'A' 'T' CR LF）= 16。
     *   tx_ok == 16 ⇒ CR 和 LF 都真的被硬件移出去了（S2TI 都等到了）。
     *   若 tx_ok == 12 ⇒ 只发了 3 字节/帧，即**LF 没发**（回到 v0.49 的老行为）；
     *   若 tx_ok == 8  ⇒ CR 也没发 —— 那是别的问题，把这一行发我。 */
    LOG_I2(TAG, "       edges=%u low=%u (600ms)", d_ed, d_lo);
    LOG_I0(TAG, "       expect tx_ok=16 (4 frames x AT+CRLF)");

    if (d_to != 0U)
    {
        LOG_E0(TAG, "UART2 CLOCK DEAD: S2TI never set -> Timer2 not running");
    }
    else if (d_ed == 0U)
    {
        LOG_E0(TAG, "line is IDLE-HIGH, zero edges in 600ms");
        LOG_E0(TAG, "  module NOT answering while we send AT (still booting?)");
    }
    else
    {
        LOG_I0(TAG, "edges present on P1.0 -> something IS talking");
    }

    /*--------------------------------------------------------------------------
     * ---- 2) 自适应波特率（分轮重试，等模组启动完）----
     *
     *   ★ 为什么必须分轮重试：4G 模组上电到能应答 AT 通常要 3~15 s，
     *     而 MCU 上电后只能等固定的 APP_MODEM_BOOT_MS。若模组比预期慢，
     *     一轮扫描就会全落空，于是"模组还在启动"被误判成"链路不通"。
     *   ★ 时间账（v0.49 改过档位等待，**别按老版本算**）：
     *       单档 = 100 ms 探测 → 已提到 300 ms
     *       一轮 = 10 档 × 2 次 × 300 ms = 6 s
     *       3 轮 = 18 s（失败最坏）
     *     正常情况第一轮第一档（115200）就命中，总耗时 < 1 s。
     *     原来是 15 轮 × 2 s = 30 s；现在档位等待变长，所以轮次必须减到 3，
     *     否则失败时要干等 90 s，现场会以为死机。
     *------------------------------------------------------------------------*/
    ab = OTA_ERR_TIMEOUT;
    for (r = 0U; r < 3U; r++)
    {
        ab = at_autobaud(&baud);
        bsp_wdt_feed();
        if (ab == OTA_OK) break;

        /* 本轮无应答：打一行，让现场看到"确实在等"而不是死机 */
        LOG_I1(TAG, "autobaud: still silent, round=%u/3", (u16)(r + 1U));
    }

    if (ab != OTA_OK)
    {
        /* 30 s 都没等到 ⇒ 再诊断一次。这一轮的结论才是可信的：
         * 模组早就该启动完了，此时仍无跳变 = 硬件问题。 */
        at_line_diag();
        bsp_wdt_feed();
        d_ed = at_diag_edges();
        d_lo = at_diag_low();
        d_to = at_diag_tx_to();

        LOG_E0(TAG, "autobaud FAIL - no OK/RDY at any rate");
        LOG_I2(TAG, "diag#2 edges=%u low=%u", d_ed, d_lo);
        LOG_I1(TAG, "       tx_to=%u (0 = our TX is fine)", d_to);
        LOG_E0(TAG, "bytes seen per rate (0 = line dead / no module):");

        n = at_baud_count();
        for (i = 0U; i < n; i++)
        {
            rb = at_baud_of_idx(i);        /* 先取变量再进宏（见日志铁律） */
            rn = at_autobaud_bytes(i);
            LOG_I2(TAG, "  %lu -> %u B", rb, rn);
        }

        if (d_to != 0U)
        {
            LOG_E0(TAG, "verdict: UART2 clock dead (firmware) - Timer2/T2 reload");
        }
        else if (d_ed == 0U)
        {
            LOG_E0(TAG, "verdict: NOTHING on the wire (hardware)");
            LOG_E0(TAG, "  Y100P (Core-Y100P) checklist, by probability:");
            LOG_E0(TAG, "  1) TX/RX MUST CROSS, board TXD -> MCU RXD:");
            LOG_E0(TAG, "     " AT_PIN_NAME_STR);
            LOG_E0(TAG, "     (straight TX-TX/RX-RX = module hears nothing = this log)");
            LOG_E0(TAG, "  2) NET LED? 5s blink=SIM bad / 100ms=no network");
            LOG_E0(TAG, "  3) VIN 5-12V (12V/1A, >10W), not 3.8V VBAT?");
            LOG_E0(TAG, "  4) GND common between MCU and core board?");
            LOG_E0(TAG, "  5) Reload pressed 7s = factory reset (baud back 115200)");
        }
        else
        {
            LOG_E0(TAG, "verdict: signal present but UART RX dead (firmware)");
            LOG_E0(TAG, "  baud table may miss the module rate - tell me low/edges");
        }
        return 0U;
    }

    s_pb_ok   = 1U;
    s_pb_baud = baud;
    LOG_I1(TAG, "baud=%lu  <== confirmed (AT answered)", baud);
    LOG_I0(TAG, "  Y100P: auto-baud 1200..115200, frame = 'AT' CR LF");

    /*--------------------------------------------------------------------------
     * ★★ v0.53：先给链路打分，再问内容。
     *
     *   v0.51 上板的教训：波特率确认成功、模组也回话，但应答**整字节丢失**
     *   （"+CPIN: READY" → "+PIN RAD"，"OK" → "O"）。这种时候后面的
     *   "+CGMR"、"+CSQ" 全都会是残缺的，肉眼看不出是模组换了说法还是线在丢字节。
     *   连发 8 次 "AT" 数干净答对几次，就能一刀切开：
     *     8/8 → 线干净，后面每条命令的应答都可信；
     *     <8  → 线在丢字节，先修线（最常见：损坏的 USB-TTL TX 脚还挂在
     *            P1.0 这根线上，和模组 TXD 对拉），**此时不要读下面的内容**。
     *   代价：8 × 最多 300 ms，通了的时候每次几十毫秒，可接受。
     *------------------------------------------------------------------------*/
    lq = at_link_test(8U);
    LOG_I1(TAG, "link quality %u/8  (8 = wire clean, 0 = nothing)", (u16)lq);
    if (lq >= 8U)
    {
        LOG_I0(TAG, "  wire OK -> every reply below is trustworthy");
    }
    else if (lq >= 6U)
    {
        LOG_W0(TAG, "  wire MARGINAL -> bytes ARE being lost on RX");
        LOG_W0(TAG, "  suspect: a USB-TTL TX pin still wired onto P1.0");
    }
    else
    {
        LOG_E0(TAG, "  wire BAD -> fix the RX line before trusting any reply");
        LOG_E0(TAG, "  keep ONLY: board TXD -> P1.0, board RXD -> P1.1, GND");
        LOG_E0(TAG, "  unplug every USB-TTL TX from P1.0 while testing");
    }

    /*--------------------------------------------------------------------------
     * ★★ 先关回显（v0.53 从第 4 位提前到第 1 位）。
     *
     *   原来 ATE0 排在 AT+IPR? 之后，于是前面几条命令线上要跑两份字节
     *   （先回显、再应答）。在一条会丢字节的线上，流量减半 = 丢的概率减半，
     *   而且 ATI/CGMR 的捕获不再以回显开头，结论更干净。
     *   ★ 代价：ATE0 自己还有回显，这一条照旧要容忍。
     *------------------------------------------------------------------------*/
    app_at_once("ATE0",     0,         0U);   /* 关回显：dst 传 0 表示不存 */

    /* ---- 逐条查询：每条都把原始应答打出来 ---- */
    app_at_once("ATI",      s_pb_ati,  16U);
    app_at_once("AT+CGMR",  s_pb_cgmr, 16U);
    /* ★ AT+IPR? 的意义：
     *   回 "+IPR: 0"      ⇒ 自适应波特率
     *   回 "+IPR: 115200" ⇒ 被写死成固定波特率（之前有人 AT+IPR=x:&W 过）
     *   这两者的区别决定了"以后量产要不要固定波特率"，所以必须看见。
     *   顺带：如果 IPR=0 而扫描仍不通，那就一定不是配置问题。 */
    app_at_once("AT+IPR?",  s_pb_ipr,  14U);
    app_at_once("AT+CPIN?", s_pb_cpin, 14U);
    app_at_once("AT+CSQ",   s_pb_csq,  14U);
    app_at_once("AT+CEREG?", s_pb_creg, 14U);
    app_at_once("AT+CGATT?", s_pb_gatt, 10U);

    /* ---- 3) 结论汇总 ---- */
    LOG_I0(TAG, "---- probe summary ----");
    LOG_I1(TAG, "  ATI   %s", s_pb_ati);
    LOG_I1(TAG, "  CGMR  %s", s_pb_cgmr);
    LOG_I1(TAG, "  IPR   %s", s_pb_ipr);
    LOG_I1(TAG, "  CPIN  %s", s_pb_cpin);
    LOG_I1(TAG, "  CSQ   %s", s_pb_csq);
    LOG_I1(TAG, "  CEREG %s", s_pb_creg);
    LOG_I1(TAG, "  CGATT %s", s_pb_gatt);
    if (lq < 8U)
    {
        /* 丢字节时上面的结论会缺字符，必须当场提醒，否则会被当成"模组就这样"。 */
        LOG_E1(TAG, "^^ summary captured on a LOSSY line (%u/8) - re-check after rewiring",
               (u16)lq);
    }
    LOG_I0(TAG, "---- probe done (see heartbeat replay) ----");

    return 1U;
}

#endif /* APP_AT_PROBE_TEST */

/*==============================================================================
 * 网络命令组取证（APP_NET_DIAG）
 *
 *   ── 为什么需要它（v0.3.5 上板日志）──────────────────────────────────────
 *     [HOTA] sapbr contype rc=4      ← rc=4 是 AT_RESP_TIMEOUT，**一个字节都没回来**
 *   这一条把两种完全不同的病因压在同一个现象里：
 *     ① 模组真的不认 AT+SAPBR（合宙 Air 系固件可能只认 CSTT/CIICR 或 QICSGP/QIACT）
 *     ② at_cmd_wait() 自己的 16 B 形参参数块把入参踩了（铁律 14 隐患）
 *         —— 它在本工程**从未成功执行过一次**，第一次登场就是 SAPBR
 *   靠读代码分不出，只能让模组自己开口：把应答原文打出来。
 *
 *   ── 判读口径（下轮照这个看，别再猜）──────────────────────────────────
 *     `[AT+CPIN?] rc=0 ...`            ⇒ at_cmd_wait 正常 ⇒ 病因是 ①
 *     `[AT+CPIN?] rc=4 rx=`（空）      ⇒ at_cmd_wait 坏了 ⇒ 病因是 ②，先修它
 *     `[AT+CGATT?] ... CGATT: 1`       ⇒ 已附着，PDP 该能起来
 *     `[AT+CGATT?] ... CGATT: 0`       ⇒ 没附着 ⇒ 先解决注网/天线，命令组无从谈起
 *     `[AT+CSTT...] rc=0`              ⇒ 合宙风格可用，PDP 改用 CSTT+CIICR+CIFSR
 *     `[AT+QICSGP...] rc=0`            ⇒ Quectel 风格可用，PDP 改用 QICSGP+QIACT
 *
 *   ── 纪律 ───────────────────────────────────────────────────────────────
 *     · 应答原文含 CR/LF，原样打印会把日志行截断、行尾 *HH 立刻错 ⇒ 必须净化
 *     · 一行只放 3 个值槽（LOG_I3），行宽 < 160
 *     · 每条命令各自一行，失败不中断，全部跑完（一次上板拿全证据）
 *============================================================================*/

/* 净化：不可见字符 -> '.'，末尾补 0（形参 5 B，不生成参数块）
 * ★ v0.7.0：本函数与下面的 app_at_ev() 现在被**主路径**用着（ATE0 关回显），
 *   所以不能再圈在 APP_NET_DIAG 里面 —— 条件编译边界已下移到 app_net_diag()。 */
static void app_san(char *b, u16 n){
    static u16 xdata i;
    static u8  xdata c;

    for (i = 0U; i < n; i++)
    {
        c = (u8)b[i];
        if ((c < 0x20U) || (c > 0x7EU)) b[i] = '.';
    }
    b[n] = 0;
}

/* 发一条命令，把 rc 和应答原文打成一行（形参 7 B，安全）
 *
 * ★ v0.6.0：带 3 次重试。现场那条线成片吞字节，同一条命令第一次 rc=4、
 *   第二次就回来是常态（上板日志 try1 rc=4 / try2 rc=0）。
 *   ERROR / +CME 不重试 —— 那是模组的明确态度，再发一百次也一样。
 *   日志形如 `[AT+XXX] rc=0/2 rx=...`，斜杠后面是**实际发了几次**。 */
static void app_at_ev(const char *cmd, u16 to_ms)
{
    static at_resp_t xdata r;
    static u16  xdata n;
    static u16  xdata ovf;
    static char xdata b[40];

    /* 长时间静默会让人以为卡死：每条命令前喂狗 + 翻灯，板上有可见节奏。
     * 不等 at_wait_core 内部的喂狗点 —— 那条命令只要 1 s 就走完了，
     * 15 s 的 SAPBR=1,1 靠的是循环里的 at_wdt_tick()（每 64 圈一次）。 */
    bsp_wdt_feed();
    bsp_led_toggle();

    r = (at_resp_t)AT_RESP_TIMEOUT;
    (void)at_take_overflow();          /* 清掉上一条的计数，只统计本条 */
    do
    {
        n = 0U;
        b[0] = 0;
        r = at_cmd_wait(cmd, 0, to_ms, b, 38U, &n);
        if (r != AT_RESP_TIMEOUT) break;
        bsp_wdt_feed();
    } while (0);

    /* ★★★ v0.6.5：k 换成 **ovf（本命令期间环形缓冲满过几次）**
     *   这是"丢字节到底丢在哪"的判决位：
     *     ovf > 0  ⇒ 我们自己的 2 KB 环被填满过 ⇒ 丢在**固件**里（解析太慢）
     *     ovf == 0 ⇒ 字节根本没进环          ⇒ 丢在**硬件/线路**上
     *   以前只有一个 n，两种病看起来一模一样，白跑了好几轮。
     *   ★ 同时把"仅超时才重发"的 3 次重试去掉：重发会把两条应答混在一起，
     *     让 n 失去"这一条收到多少字节"的物理含义（本条就是要量它）。 */
    if (n > 38U) n = 38U;
    app_san(b, n);
    ovf = at_take_overflow();

    /* rc: 0=OK 1=ERROR 2=+CME 3=+CMS 4=超时 7=残片(宽松判据) */
    LOG_I4(TAG, "[%s] rc=%u n=%u ovf=%u", cmd, (u16)r, (u16)n, ovf);
    if (n > 0U) LOG_I1(TAG, "   rx=%s", b);
}

#if (APP_NET_DIAG != 0)
static void app_net_diag(void)
{
    LOG_I0(TAG, "---- net cmd probe (rc: 0=OK 1=ERR 2=CME 4=TO 7=PART) ----");

    /* ★ v0.6.0：这条线在丢字节 ⇒ 全套取证开宽松判据，否则每条命令都被
     *   一颗丢掉的 'K' 判死，一条日志也读不出来（原理见 at_set_lenient）。 */
    at_set_lenient(1U);
    LOG_I0(TAG, "lenient mode ON (line drops whole bytes)");

    /* 先自证链路：8/8 = 线干净，之后的 rc 才可信 */
    LOG_I1(TAG, "link %u/8 before probe", (u16)at_link_test(8));

    /*==========================================================================
     * ★★★ v0.6.12：定长应答数字节 —— 本轮唯一"判决性"的判据
     *
     *   前面所有判据（link 8/8、rx= 里有几个字符、rc=4/7）都是定性的，
     *   八轮下来一直在"线路 vs 固件"之间来回。这一条给出一个数：
     *
     *       ATE0 下  AT+IPR?  ->  "\r\n+IPR: 115200\r\n\r\nOK\r\n"  = 22 B
     *       ATE1 下  （带回显）                                     = 30 B
     *
     *   ⇒ `rxprobe exact 6/6` + `n min=22 max=22` = 这条链路一个字节都没丢，
     *     接下来任何失败都只能出在协议/服务器侧；
     *   ⇒ `exact 0/6` + `n min=13` = 每次丢 9 个字节 ⇒ **线路/驱动丢的**，
     *     改固件一个字都没用（固件侧只有环满才丢，见下一行 ovf）。
     *   ★ 这条必须打在 net probe **之前**：probe 里那十几条命令会把
     *     模组状态弄乱（PDP/HTTP 会话），届时 n 的期望值就不再固定。
     *------------------------------------------------------------------------*/
    /* ★★★ v0.6.14：判据改为**与模组型号无关**的形式。
     *   旧写法把基准钉在 "AT+IPR? = 22/30 B"，那是 SIMCom 专属值；换成 Air724UG
     *   之后这条命令的应答长度不同（甚至可能不支持），会打出 exact 0/6，把
     *   "链路完全正常" 报成 "每次都在丢字节"，整轮判读方向就全错了。
     *   新判据：同一条命令的应答必然定长 ⇒ **min==max 即零丢字节**，不需要知道是多少。
     *   探针命令也换成 AT+CGMR（3GPP 标准命令，各家都有，应答 40 B 量级）。 */
    LOG_I0(TAG, "---- rx fidelity: AT+CGMR, N runs must be same length ----");
    at_rx_probe(6U);

    /* ★ 换模组后第一件要知道的事：型号与 AT 固件版本（合宙回
     *   `+CGMR: "AirM2M_Air724UG_V409_LTE_AT"`）。这一条不判生死，只看原文。 */
    app_at_ev("AT+CGMR", 2000U);

    /* ★ v0.6.14：Air724UG 上 https 的第三个前提是**系统时间**。
     *   时间没同步（还停在 2000 年）时，TLS 握手会失败，而我们只会看到一个
     *   超时的 HTTPACTION —— 看起来像"服务器不响应"，实际是证书有效期判不过。
     *   这一条不判生死，只把时间原文打出来：`+CCLK: "26/10/28,..."` = 已同步。 */
    app_at_ev("AT+CCLK?", 2000U);

    /* ★★ v0.6.2 标尺（先看这两行，再看别的）
     *   ATE0 生效后 "AT" 的应答**恰好 6 字节**：CR LF O K CR LF。
     *     n=6 ⇒ 线干净，下面每条的 rc 都可当真；
     *     n=3 ⇒ 丢一半（本轮现场就是这个量级）；
     *     n=0 ⇒ 一个字节都没来，改固件无用，去查线。
     *   有这条标尺，"n 到底算多还是算少"就不用猜了。 */
    app_at_ev("AT", 1000U);
    app_at_ev("AT", 1000U);

    /* ── 第一组：现状对照（上一轮全部成功，用来判断这一趟线有没有变差）── */
    app_at_ev("AT+CPIN?", 1000U);
    app_at_ev("AT+CGATT?", 1000U);

    /* ── 第二组：承载链路。★ AT 家族已定案（v0.5.0 现场）─────────────
     *   SAPBR=3,1,"CONTYPE" → OK    CSTT → OK    CIICR → OK
     *   QICSGP             → ERROR   QIACT=1 → ERROR
     *   ⇒ 本模组是 **SIMCom/SAPBR 风格**，移远那两条根本不存在。
     *     这一版**不再发 QICSGP/QIACT**：必然 ERROR，每条还要空等 15 s，
     *     只会把日志和上板时间都浪费掉，并且很可能就是模组随后复位的原因。 */
    app_at_ev("AT+SAPBR=3,1,\"CONTYPE\",\"GPRS\"", 2000U);
    app_at_ev("AT+SAPBR=3,1,\"APN\",\"\"", 2000U);   /* 空串 = 按网络自动取 APN */
    app_at_ev("AT+SAPBR=1,1", 15000U);               /* 激活 PDP，实测 2~10 s  */
    app_at_ev("AT+SAPBR=2,1", 8000U);                /* ★ 必须见 +SAPBR: 1,1,"<ip>" */
    /* ★ CIFSR 已删（v0.5.1 现场实证）：SAPBR 承载下它固定回 +CME ERROR: 3，
     *   它只对 CSTT/CIICR 那套上下文有效。要看 IP，看上一行的 +SAPBR 应答原文。 */
    app_at_ev("AT+HTTPINIT", 3000U);                 /* 验证 HTTP 命令组在不在 */
    app_at_ev("AT+HTTPTERM", 3000U);                 /* 立刻收摊，不留脏状态   */

    /* 跑完再打一次链路质量：与开头那个 8/8 对比，就能看出"跑一趟有没有把线拖坏" */
    LOG_I1(TAG, "link %u/8 after probe", (u16)at_link_test(8));

    /* ★★★ v0.6.13：`poll gap` 已整体删除（见 uart_at.c 里的说明）。
     *   判"固件漏读"现在只有一个数：上面 rxprobe 的 exact k/6 与 n（22 = 收全）。
     *   这里只留环形缓冲溢出计数（现在只可能发生在 TX 期间）。 */
    LOG_I1(TAG, "ring ovf=%u (tx-time only)", at_take_overflow());


    LOG_I0(TAG, "---- net cmd probe done ----");
}
#endif /* APP_NET_DIAG */

/*==============================================================================
 * 入口
 *============================================================================*/
void main(void)
{
    /* 从 BL 跳过来时 EA 可能是 BL 打开的，先无条件关掉（原因见文件头）。 */
    EA = 0;

    bsp_clock_init();
    bsp_gpio_init();
    (void)bsp_uart_init(APP_LOG_BAUD);
    bsp_timer_init_1ms();               /* 只配置硬件，不使能中断的响应 */
    bsp_wdt_init(APP_WDT_PS);

    log_init(LOG_LVL_INFO);             /* 量产级别；调试可临时改 LOG_LVL_DBG */

    app_log_rstflag();                  /* ★ 复位源：区分看门狗/上电/软件复位 */

    /* 注意：这里**仍然**不打开 EA —— P0 全程轮询。 */
    EA = 0;

    bsp_led_on();
    app_delay_ms(120);
    bsp_led_off();

    LOG_I2(TAG, "==== %s ====  (P0 build: %u)", APP_VER_STRING, (u16)(APP_STAGE_P0 ? 1 : 0));
    {
        u32 fosc = bsp_clock_fosc();        /* 先取变量，再传参（见 log.h 说明） */
        LOG_I2(TAG, "fosc=%luHz  EA=%u", fosc, (u16)EA);
    }

    /* ★ 先用已知值确认"日志没骗人"，再去看自检结论。
     *   LOG_SELFTEST=0 起不再编译（省约 200 字节 code）。 */
#if (LOG_SELFTEST != 0)
    log_selftest();
#endif

    app_selftest();

    /*--------------------------------------------------------------------------
     * P1-A3：提交（必须紧跟自检 —— 自检通过才允许宣布"新固件可用"）
     *
     *   条件由 state 决定：只有 BL 搬运成功才会把 state 写成 COMMITTED。
     *   普通上电（state=IDLE）走不到任何写 Flash 的分支，零开销。
     *
     *   ★ 顺序很重要：必须在下面 A1 造镜像之前。
     *     A1 的进入条件是 `state == IDLE && rollback_flag != MARK`，
     *     提交会把 state 变成 IDLE；如果 rollback_flag 还没打上标记，
     *     A1 就会立刻再造一次镜像、又要求断电 —— 死循环。
     *     本轮 A1 的标记一定在（同一个 build 号），但以后
     *     APP_A1_SELFTEST 关掉之后就彻底没有这个耦合了。
     *------------------------------------------------------------------------*/
    app_ota_commit();

    /*--------------------------------------------------------------------------
     * P1-A1：造镜像（只做一次，见上面 app_a1_make_image 的说明）
     *
     *   条件很严：必须 state==IDLE 且 rollback_flag 还不是 0xA1。
     *   正常运行时（比如你反复按电源键）这条分支不会进第二次。
     *   真要做第二次，用 ISP 重新烧录整包（会重建 param）即可。
     *------------------------------------------------------------------------*/
#if (APP_A1_SELFTEST != 0)
    {
        param_t *p0 = param_get();

        if (p0->ota_state == (u8)OTA_ST_IDLE &&
            p0->rollback_flag != (u8)APP_A1_DONE_MARK)
        {
            app_a1_make_image();            /* 正常不会返回：内部停机等断电 */
        }
    }
#endif

    /*--------------------------------------------------------------------------
     * ★ 第 0 步：模组 AT 探活（只验连接，不下载、不写 STAGING）
     *
     *   放在 A1 之后：A1 一旦跑起来就不返回（内部停机等断电），所以要让探活
     *   跑到，A1 必须先自然跳过（烧完机的第二次上电即是这样）。
     *   探活失败**不阻断**启动：结论进 s_pb_*，主循环照常跑并周期重播。
     *------------------------------------------------------------------------*/
#if (APP_AT_PROBE_TEST != 0)
    (void)app_at_probe();
#endif

    /*--------------------------------------------------------------------------
     * P1-B：HTTP 直拉 OTA 测试（MQTT 之前的命脉验证）
     *
     *   上电即触发一次下载：AT 初始化 -> 等模组就绪 -> 下载镜像到 STAGING
     *   -> 置 PENDING_REBOOT -> 停机等断电 -> BL 接手校验+搬运。
     *   下载循环在下面主循环里由 ota_fsm_poll() 驱动。
     *------------------------------------------------------------------------*/
#if (APP_NET_ENABLE != 0)
    {
        static u32 xdata baud;
        static u8  xdata at_ok;
        /* v0.4.1 自证用：k=循环号 e=扫描前跳变数 rc8=autobaud 返回码 */
        static u8  xdata k;
        static u8  xdata rc8;
#if (AT_DIAG_LEGACY != 0)
        static u16 xdata e;         /* 扫描前跳变数（只给老探针用） */
        static u16 xdata raw_n;     /* v0.6.6 裸采样字节数 */
#endif

        /* ★★ 为什么不能"无条件 at_uart_init + 盲发 AT"（v0.3.4 修，上板日志实证）
         *   `at_uart_init()` 结尾把 `s_cur_baud` 置 0 —— 那是"波特率尚未确认，
         *   收到的字节一律不认"的闸门（见 uart_at.c），只有 at_autobaud() 命中
         *   才会打开。于是有两种必然失败：
         *     ① 只 init 不 autobaud ⇒ 模组的 OK 全被丢弃 ⇒ "module no response"
         *        （v0.3.3 上板日志正是这一行，20 秒空等，与线好坏无关）
         *     ② 探活已确认过波特率，OTA 块又 init 一次 ⇒ 闸门被关回去，同样失败
         *   正确顺序：已有确认值就复用；没有才 等开机 → autobaud → 拿确认值。
         *
         *   另外：这里不用 at_cmd_wait("AT",0,500,0,0,0) —— 6 形参合计 >8 B
         *   会生成参数块（铁律 14），at_autobaud 的 1 形参更干净。 */
        baud = at_get_baud();
        if (baud != 0UL)
        {
            at_ok = 1U;
            LOG_I1(TAG, "module AT OK (link from probe) baud=%lu", baud);
        }
        else
        {
            at_ok = 0U;
            if (at_uart_init(115200UL, 0, 0) != OTA_OK)
            {
                LOG_E0(TAG, "at_uart_init fail");
            }
            else
            {
                /* ★ 通道自报（v0.5.0）：日志必须能独立证明"这版跑的是哪个口、
                 *   哪两根脚"。字符串在编译期拼接，运行期零传参 ⇒ 不会因为
                 *   %s 指针宽度问题打错（铁律 7 的老坑），也对不上账时更好查。 */
                LOG_I0(TAG, "==== AT ch: " AT_PORT_NAME_STR " " AT_PIN_NAME_STR " ====");

                /* ────────────────────────────────────────────────────────────
                 * ★★★ v0.4.1：先自证"这根线收不收得到东西"，再去猜波特率
                 *
                 *   前三轮都跳过前两问直接猜波特率，等于每次都在猜同一个空。
                 *   现在按"由近到远"排三个互不重叠的问题：
                 *     ① P1.0 这根线收不收得到东西？  → 环回自证 / 纯监听
                 *     ② 模组自己会不会说话？          → 纯监听（一个字节都不发）
                 *     ③ 我们的话送到它耳朵里没有？    → autobaud 的应答
                 * ──────────────────────────────────────────────────────────── */

                /* ★★★ v0.6.4：**先盲切 9600，再谈 autobaud**
                 *
                 *   前三版都卡在同一处：要改波特率得先让模组收到 AT+IPR，
                 *   而"确认它收到了"依赖回执 —— 回执正是丢字节的那个方向
                 *   （step59 实测：四次 try 全 rc=4，同一时刻桥接日志里模组
                 *   每次都回了完整的 OK）。⇒ 旧写法永远迈不出第一步。
                 *
                 *   新打法利用一条已证实的结论：**MCU→模组 方向 0 丢字节**
                 *   （模组把我们那 3 秒 autobaud 乱码原样回显，一字不缺）。
                 *   ⇒ AT+IPR 可以**盲发**，不需要看到 OK；发完切过去再验。
                 *
                 *   为什么非要降速：115200 一字节 87 us，9600 是 1042 us
                 *   （12 倍余量），且 11059200/4/9600 = 288 整、无分频误差。
                 *   若降速后仍丢 ⇒ 不再是余量问题，只剩硬件对拉/引脚损坏。
                 *   这一步既是修复，也是判据。
                 *
                 *   失败会自动把两边拨回 115200，然后落回原来的 12 档扫描，
                 *   所以最坏只是"和上一版一样"，不会锁死。 */
                at_delay_ms((u16)APP_MODEM_BOOT_MS);

                /* ★★★ v0.6.5：**删掉"盲发 AT+IPR=9600 再降速"这一整段**
                 *
                 *   v0.6.4 的设想是"发完 IPR 切过去再验"，靠的是"MCU→模组
                 *   方向 0 丢字节"。step60 上板结果把它彻底否掉了：
                 *     MCU 侧  9600 blind -> 0 (AT n=0)
                 *     桥接侧  20:46:17 三次 AT+IPR=9600 全都回 OK，
                 *             20:46:22 又发 AT+IPR=115200 仍回 OK
                 *   ⇒ ① 模组收得到，也回得出 OK；
                 *     ② 但它**并没有真的换到 9600**（否则回 OK 那条也会跟着变）；
                 *     ③ 白白多花 ~15 s，还让后面 12 档扫描的第一档落在错的状态里。
                 *   结论：这条模组在 115200 上是**写死/自适应**的，IPR 降速这条路
                 *   走不通，别再试。降到 9600 的实验价值也已被取代 ——
                 *   丢字节的真身是"接收轮询太慢"（见 at_wait_core v0.6.5 注释），
                 *   那件事只能在固件里修，不能靠降波特率绕。
                 *   ⇒ 直接进 115200 探活（at_autobaud 档位表第一档就是 115200）。 */

#if (AT_DIAG_LEGACY != 0)
                    if (at_ok == 0U)
                    {
                        /* 老取证链（v0.47~v0.6.6）保留在此，见 uart_at.h 的 AT_DIAG_LEGACY */
                        rc8 = at_pin_loopback();
                        LOG_I1(TAG, "loopback shorted=%u (1 = jumper in)", (u16)rc8);
                        if (rc8 != 0U)
                        {
                            LOG_I0(TAG, "TX<->RX SHORTED, uart self test");
                            at_diag_clear();
                            (void)at_ping(500U);
                            LOG_I1(TAG, "uart loopback rx=%u (want 4)", at_ping_bytes());
                            app_at_ev("AT+CPIN?", 800U);
                            LOG_I0(TAG, "jumper off, module on, power cycle");
                        }
                        else
                        {
                            LOG_I1(TAG, "own RX idle=%u (expect 1)",
                                   (u16)at_uart2_rxd_level());
                            e = at_listen_port(2U, 400U);
                            LOG_I2(TAG, "x-listen UART2 RX edges=%u low=%u", e, at_diag_low());
                            e = at_listen_port(3U, 400U);
                            LOG_I2(TAG, "x-listen UART3 RX edges=%u low=%u", e, at_diag_low());
                            for (k = 0U; k < 3U; k++)
                            {
                                e = at_listen(500U);
                                baud = 0UL;
                                rc8  = (u8)at_autobaud(&baud);
                                LOG_I3(TAG, "try %u pre-edges=%u rc=%u",
                                       (u16)(k + 1U), e, (u16)rc8);
                                if (rc8 == (u8)OTA_OK) { at_ok = 1U; break; }
                                at_delay_ms(800U);
                            }
                            if (at_ok != 0U)
                            {
                                LOG_I1(TAG, "module AT OK baud=%lu", baud);
                            }
                            else
                            {
                                at_diag_clear();
                                at_line_diag();
                                LOG_I2(TAG, "FAIL edges=%u low=%u",
                                       at_diag_edges(), at_diag_low());
                                LOG_I2(TAG, "     tx_ok=%u tx_to=%u",
                                       at_diag_tx_ok(), at_diag_tx_to());
                                raw_n = at_raw_at(600U);
                                LOG_I1(TAG, "raw AT n=%u", raw_n);
                                if (raw_n > 0U) LOG_I1(TAG, "   raw=%s", at_raw_line());
                                LOG_E0(TAG, "module no response, skip OTA");
                            }
                        }
                    }
#else  /* AT_DIAG_LEGACY == 0 —— v0.7.0 精简路径 */
                    /*--------------------------------------------------------------------------
                     * ★ v0.7.0：APP 只有 48 KB 代码区，加进 MQTT 之后 ECODE+HCONST
                     *   已顶到 16 位空间尽头（L107 ADDRESS SPACE OVERFLOW）。
                     *   取证用的老探针整段下线（AT_DIAG_LEGACY=0，源码仍在 uart_at.c），
                     *   这里只留**能定位、能验收**的那几个数：
                     *
                     *     best b= / n=   每一档收到几个字节 —— 0 = 线死；>0 但 rc!=0 = 判据不过
                     *     rc=            0 表示这一档凑出了 OK/RDY
                     *     rx idle        失败后读 P1.0 空闲电平：0 = 模组没供电 / TXD 没接
                     *
                     *   三条加起来已经能分开"硬件 / 引脚 / 波特率"三类，不再需要
                     *   edges / listen / raw 那一整套。真要复查老问题：把
                     *   AT_DIAG_LEGACY 改回 1（同时得先腾出 ~3.9 KB，否则链接不过）。
                     *------------------------------------------------------------------------*/
                    for (k = 0U; k < 3U; k++)
                    {
                        baud = 0UL;
                        rc8  = (u8)at_autobaud(&baud);
                        LOG_I2(TAG, "try %u/3 best b=%lu", (u16)(k + 1U),
                               at_sweep_best_baud());
                        LOG_I2(TAG, "       best n=%u rc=%u", at_sweep_best_n(), (u16)rc8);
                        if (rc8 == (u8)OTA_OK) { at_ok = 1U; break; }
                        at_delay_ms(800U);
                    }

                    if (at_ok != 0U)
                    {
                        LOG_I1(TAG, "module AT OK baud=%lu", baud);
                    }
                    else
                    {
                        LOG_I1(TAG, "rx idle=%u (expect 1)", (u16)at_uart2_rxd_level());
                        LOG_E0(TAG, "module no response, skip OTA");
                    }
#endif /* AT_DIAG_LEGACY */
            }
        }

        if (at_ok != 0U)
        {
            /* ★★ v0.6.2：**先关回显**，这是本版性价比最高的一条改动。
             *
             *   模组默认 ATE1，它把收到的命令原样回送一遍 ⇒ 每条应答在线上
             *   要跑**两份字节**。而 step56 的实测规律是：
             *     应答越短越容易活下来 —— 10 B 的 "AT" 返通常成功，
             *     32 B 的 "AT+CPIN?" 只剩 16 B，52 B 的 "AT+SAPBR=2,1" 必失败。
             *   ⇒ 关掉回显 = 线上字节数直接减半 = 长度指数上的那一段直接省掉。
             *
             *   ★ 发两次是幂等的：这条命令自己还带着回显，第一次若因丢字节
             *     没生效，第二次补上；第二次即使回 +CME 也无害。
             *   ★ ATE0 / ATE1 不写 NV，复位即恢复默认 ⇒ 每次上电都要发。 */
            app_at_ev("ATE0", 1500U);
            app_at_ev("ATE0", 1500U);

            /* ★★★ v0.6.7：**删掉"降到 9600"这一步（遗留调用点）**
             *
             *   v0.6.5 已经把这对函数的"主路径调用"删掉了，但漏掉了这一处，
             *   于是只要 at_ok 成立就会先跑一遍 at_baud_down()：
             *     · 它盲发 AT+IPR=9600 再把 MCU 切到 9600；
             *     · step60 的桥接日志证明**模组根本没换档**（之后 115200 仍回 OK）
             *       ⇒ 赌赢了是白花 ~15 s，赌输了 MCU 停在 9600 而模组在 115200，
             *         后面每一条命令都是乱码 —— 一个纯负期望的分支。
             *   ⇒ 整段拿掉。丢字节的真身是接收轮询节奏（已在 v0.6.5/v0.6.6 修），
             *     不是"余量不够"，降速既不能治也不用它来证明。
             *   ★ at_negotiate()/at_baud_down() 仍留在 uart_at.c 供以后
             *     "换了一块真能改 IPR 的模组"时复用，见 uart_at.h 的弃用说明。 */

#if (APP_NET_DIAG != 0)
            /* 链路刚确认，此刻链路上的每一个 rc 都可信 —— 先取证再干活 */
            app_net_diag();
#endif
            /*--------------------------------------------------------------------------
             * ★★★ v0.7.0：联网成功后的分岔
             *
             *   ota_fsm_init() 无论走哪条路都要跑（它把 param 里的断点/状态读进
             *   状态机，MQTT 触发的下载也依赖它）。区别只在"谁去发起下载"：
             *     APP_HTTP_OTA_TEST=1：上电即下载（v0.6.x 的行为，台架用）
             *     APP_HTTP_OTA_TEST=0：**只有 MQTT 的版本消息能触发**（量产）
             *------------------------------------------------------------------------*/
            (void)ota_fsm_init();

#if (APP_HTTP_OTA_TEST != 0)
            if (ota_fsm_state() == OTA_ST_IDLE)
            {
                if (ota_fsm_start_download(OTA_TEST_FW_URL) == OTA_OK)
                {
                    LOG_I0(TAG, "OTA download started");
                }
            }
#endif

            /* 串口2 数据口：必须在上面的 at_uart_init 之后（它才把 Timer2 跑起来
             * 并定好 115200；串口2 的波特率源就是这个 Timer2）。 */
            data_init();

#if (APP_MQTT_ENABLE != 0)
            mqtt_init();
#endif
        }
    }
#endif

    /*--------------------------------------------------------------------------
     * 主循环：喂狗 + 心跳灯 + ★ 周期心跳日志
     *
     *   心跳灯只能证明"芯片有电"。心跳日志才能证明"这份代码在这个地址上跑"：
     *   板载 CH340 的板子没法先开串口再上电，开机那十几行 3ms 就发完了，
     *   所以状态必须**周期重播**，否则任何一次"串口里什么都没有"都要重新猜。
     *   见 app_config.h 第 4 节。
     *------------------------------------------------------------------------*/
    LOG_I0(TAG, "enter main loop");
    {
        u16     n   = 0;
        param_t *p  = param_get();      /* 永不返回 0，未初始化时会自动填默认值 */

#if (APP_AT_PROBE_TEST != 0)
        /* AT keepalive 的圈数计数（见下面的重发块，v0.51） */
        u16     at_rt = 0U;
#endif

        for (;;)
        {
            bsp_wdt_feed();

#if (APP_NET_ENABLE != 0)
            /* 驱动下载状态机（下载期间会阻塞在 AT 等待上） */
            {
                static ota_state_t xdata st;
                static ota_state_t xdata st_prev;
                static u8  xdata o_retry;   /* 外层已重试次数 */
                static u16 xdata o_tick;    /* 距下次重试的 tick 数 */
                static u8  xdata o_init;    /* ★ 铁律 13：xdata 上电是 0xFF，
                                             *   计数器必须亲手清，不能靠隐式零 */

                st = ota_fsm_poll();

                if (o_init != 0xA7U)
                {
                    o_init  = 0xA7U;
                    o_retry = 0U;
                    o_tick  = 0U;
                    st_prev = OTA_ST_DOWNLOADING;   /* 抑制第一圈的"刚失败"判定 */
                }
                if (st == OTA_ST_PENDING_REBOOT)
                {
                    LOG_W0(TAG, "OTA image ready, POWER-CYCLE to apply");
                    for (;;)
                    {
                        bsp_wdt_feed();
                        bsp_led_toggle();
                        app_delay_ms(APP_LED_PERIOD_MS);
                    }
                }

                /*--------------------------------------------------------------
                 * ★★ v0.6.7 外层兜底重试（"重新下载"那条退路的最后一层）
                 *
                 *   内层 http_ota 已经把"单片重取 3 次 → 重连续传 6 次 →
                 *   整包重下 2 次"全用完之后才会回 IDLE。到这一步说明
                 *   网络/模组不是"抖一下"，而是真的没了。
                 *   外层不立刻重试，而是等 APP_OTA_RETRY_TICKS（默认 60 s）
                 *   —— 这一分钟是给"人插回天线 / 网自己回来"用的。
                 *   判据只在**状态从 DOWNLOADING 掉回 IDLE** 的那一刻触发，
                 *   所以不会在正常下载途中误触发。
                 *
                 *   ★★★ v0.7.0：这条退路**只在"上电即下载"模式下才允许存在**。
                 *     业务版的下载是由 MQTT 的版本消息发起的，直链来自服务器；
                 *     如果这里还按老逻辑去重下 OTA_TEST_FW_URL，就等于把业务
                 *     固件刷成那份测试镜像。⇒ 关闭模式下只记账，重试交给
                 *     "retain 的版本消息在下次重连时补发"（天然是分钟级）。
                 *------------------------------------------------------------*/
#if (APP_HTTP_OTA_TEST != 0)
                if ((st == OTA_ST_IDLE) && (st_prev == OTA_ST_DOWNLOADING))
                {
                    if (o_retry < APP_OTA_RETRY_MAX)
                    {
                        o_tick = APP_OTA_RETRY_TICKS;
                        LOG_W2(TAG_OTA, "download stopped at %lu, retry in %u s",
                               http_ota_get_offset(),
                               (u16)(APP_OTA_RETRY_TICKS / 2U));
                    }
                    else
                    {
                        LOG_E1(TAG_OTA, "gave up after %u retries", (u16)o_retry);
                    }
                }

                if ((st == OTA_ST_IDLE) && (o_tick != 0U))
                {
                    o_tick--;
                    if (o_tick == 0U)
                    {
                        o_retry++;
                        LOG_I1(TAG_OTA, "retry whole download, attempt %u",
                               (u16)o_retry);
                        if (ota_fsm_start_download(OTA_TEST_FW_URL) != OTA_OK)
                        {
                            LOG_E0(TAG_OTA, "retry start failed");
                            if (o_retry < APP_OTA_RETRY_MAX)
                            {
                                o_tick = APP_OTA_RETRY_TICKS;
                            }
                        }
                    }
                }
#else
                if ((st == OTA_ST_IDLE) && (st_prev == OTA_ST_DOWNLOADING))
                {
                    /* ★ v0.7.3：原来这里打的是 http_ota_get_offset()，下载失败后
                     *   那个值是垃圾（现场打出 3096670560），既吓人又没用。
                     *   报错码才是能查的东西（8 = 链路/模组，4 = 409，3 = 404…）。 */
                    LOG_E1(TAG_OTA, "download stopped, err=%u, wait MQTT ver",
                           (u16)ota_fsm_last_error());
                }
                o_retry = 0U;
                o_tick  = 0U;
#endif
                st_prev = st;
            }
#endif

            bsp_led_toggle();

            /*--------------------------------------------------------------------------
             * ★★★ v0.7.0：主循环的"等待"必须是**快循环**，不能是 app_delay_ms(500)
             *
             *   串口2 一字节 87 us（115200）。任何一次 >= 1 ms 的死等都会丢 10 个
             *   字节，一帧 9 字节的报文就废了 —— 而 app_delay_ms(500) 等于每
             *   500 ms 才看线路一眼，数据口根本不可能工作。
             *
             *   ⇒ 把这一拍切成"尽快循环 + 软定时"：
             *       data_poll()  收串口2 的字节（必须最频繁，它是唯一的实时性要求）
             *       at_pump()    收 MQTT 的 +MSUB（非阻塞；命令期间自动让路）
             *       mqtt_poll()  状态机与定时上报（没到点就一条 return）
             *   一圈几微秒，87 us 的窗口轻松盖住。
             *
             *   ★ 代价：AT 命令（发布/建链，最长 8 s）期间不跑 data_poll，
             *     这期间的数据帧会丢。帧是"全量快照"语义，丢了下一帧就是新值，
             *     不影响正确性；下行版本消息则靠 retain 补发兜住。见 docs/APP_MQTT.md。
             *------------------------------------------------------------------------*/
            {
                static u32 xdata t_next;
                static u8  xdata t_init;
                static u8  xdata spin;
                u32 t0;             /* 内层循环：“这次真收到字节”的时刻  */

                if (t_init != 0x3CU) { t_init = 0x3CU; t_next = at_ms(); spin = 0U; }
                t_next = (u32)(t_next + APP_LED_PERIOD_MS);

                /*--------------------------------------------------------------
                 * ★ v0.8.0 历史（已被 v0.8.2 取代，留这一段是为了不重踩）
                 *
                 *   v0.7.8 现场：服务器连发 6 条 `+MSUB`，桥接侧逐条完整，
                 *   MCU 侧只有 `urc b=270 l=11 m=0` —— 收到 11 行、没一行头是 +MSUB。
                 *   当时的结论是“一圈太慢”，于是按 1:8 抽稀。结果：
                 *   轮头回来了，但每隔 8 圈丢一个字节。
                 *   → “抽稀”治不了本，因为它假设“慢的只有那一圈”。
                 *   v0.8.2 改成盯线路的静默时间（见下面的内层循环）。
                 *------------------------------------------------------------*/
                for (;;)
                {
                    /*----------------------------------------------------------
                     * ★★★ v0.8.2：接收优先 —— 判据是"静默了多久"，不是"这圈干什么"
                     *
                     *   v0.8.0 的 1:8 抽稀把丢头治好了，但现场又出现了新形态
                     *   （18:08:26）：行头在、行内每 4~10 字节掉一个，
                     *       +MSUB "fw/000/ver,32byte,"0..9|htps://kapa.lol/R6ky
                     *   61 字节到 51 字节。丢点间隔正好落在"8 圈一次"的
                     *   节拍上 ⇒ 元凶是**每 8 圈那一次的 mqtt_poll**
                     *   （它自己只要超过 87 us 就吃掉一个字节）。
                     *
                     *   盯着"谁慢"永远治不完：以后任何一处新加的活都会再犯。
                     *   改成盯**线路**：只要 AT 口在动，主循环就不许离开
                     *   这个内层循环；静默到 APP_AT_QUIET_MS（1 ms）才放行。
                     *   一字节 87 us ⇒ 11 倍余量，而且判据和循环快慢无关。
                     *
                     *   data_poll 没有整段让路：AT 字节之间有空档，它就填在
                     *   空档里跑（见 else 分支）。所以串口2 的数据帧不会因为
                     *   一条 URC 被丢。
                     *
                     *   ★ 不许把 `bsp_wdt_feed()` 塞进内层循环里"多喂几次"：
                     *     喂狗是 SFR 写，一次几微秒，但它会打断"字节空档
                     *     全给 data_poll"的时序。放到循环外，一圈一次足够
                     *     （WDT 1.1 s，最坏一圈 6 ms）。
                     *--------------------------------------------------------*/
                    t0 = at_ms();
                    for (;;)
                    {
                        if (at_pump() != 0U)
                        {
                            /* ★ 真从 SFR 取到了字节：线路还在动，
                             *   静默计时重新开始，不许离开。 */
                            t0 = at_ms();
                        }
                        else
                        {
                            data_poll();    /* 线路没动，才轮得到数据口 */
                            if ((u32)(at_ms() - t0) >= APP_AT_QUIET_MS) break;
                        }
                    }

                    bsp_wdt_feed();

                    spin++;
                    if ((spin & 0x07U) == 0U)
                    {
#if (APP_MQTT_ENABLE != 0)
                        mqtt_poll();
#endif
                        if ((u32)(at_ms() - t_next) < 0x80000000UL) break;
                    }
                    else
                    {
                        /* ★ 让时基照样推进：TF0 是标志不是计数器，
                         *   一条 6 ms 的 URC 之间只推一次会漏掉 5 ms。 */
                        at_ms();
                    }
                }
            }

#if (APP_AT_PROBE_TEST != 0)
            /*------------------------------------------------------------------
             * ★ AT keepalive（v0.51，现场逼出来的）
             *
             *   原实现只在启动时探活一次：3 s 等开机 + 18 s 扫 3 轮，之后
             *   UART2 **永久静默**。于是只要串口工具不是"先开好再给 MCU 上电"，
             *   就永远只能看到 0 B —— 15:18 那次 30 秒全 0 正是这个原因，
             *   跟线接没接半点关系。
             *
             *   现在：没连通（s_pb_ok == 0）时每 APP_AT_RETRY_TICKS × 500 ms
             *   重发一帧 "AT\r\n"。工具任何时候打开都能立刻看到波形/字节，
             *   一眼判断链路通不通。一旦模组回了 OK 就置 s_pb_ok 并停发，
             *   不影响后续业务。
             *
             *   波特率：at_autobaud() 失败时会主动拨回 115200（见 uart_at.c），
             *   所以这里发出去的确定是 115200 下的标准 AT 帧，工具按 115200
             *   开就能看到可读文本，不会是乱码。
             *----------------------------------------------------------------*/
            if (s_pb_ok == 0U)
            {
                u8  pr;
                u16 pn;

                at_rt++;
                if (at_rt >= APP_AT_RETRY_TICKS)
                {
                    at_rt = 0U;
                    s_at_replay_n++;

                    /* ★ v0.52：用 at_ping() 而不是 at_cmd_wait("AT")。
                     *   at_ping 会把**本轮实际收到几个字节**回填出来
                     *   （at_ping_bytes），这是"线到底有没有动静"的唯一
                     *   硬指标。只看"有没有 OK"会把"收到乱码"和"什么也
                     *   没收到"混成同一个结论 —— v0.47 的"10 档全 0 B"
                     *   就是这么被误读的。
                     *   ★ 只在"结果有变化 / 首次 / 每 10 次"时打印，
                     *     否则 3 s 一条会把心跳日志淹掉。 */
                    pr = at_ping(APP_AT_PING_MS);
                    pn = at_ping_bytes();

                    if ((pn != s_pb_last_rx) || (s_at_replay_n == 1U) ||
                        ((s_at_replay_n % 10U) == 0U))
                    {
                        LOG_I2(TAG, "ping #%u rx=%u B (isolated AT, 115200)",
                               (u16)s_at_replay_n, pn);
                        s_pb_last_rx = pn;
                    }

                    if (pr == AT_PROBE_RDY)
                    {
                        s_pb_ok = 1U;
                        LOG_W0(TAG, "ping got RDY -> module trained, baud=115200");
                    }
                    else if (pr == AT_PROBE_OK)
                    {
                        s_pb_ok = 1U;
                        LOG_I0(TAG, "ping got OK -> link alive (115200)");
                    }
                }
            }
#endif

#if (APP_HEARTBEAT_LOG != 0)
            n++;
            if ((n % APP_HB_TICKS) == 0)
            {
                /* 单行最多 2 个参数（见 log.h）。boot_cnt 是识别
                 * "看门狗复位循环"的唯一依据，所以每行都带。 */
                LOG_I2(TAG, "alive #%u boot_cnt=%lu", (u16)(n / APP_HB_TICKS), p->boot_cnt);
                if ((n % (APP_HB_TICKS * 2U)) == 0)
                {
                    /* ★ v0.7.3：这两行（state/succ/run）原来每 4 s 重播一次，
                     *   而开机已经完整打过一遍。48 KB 代码区只剩两百来字节，
                     *   把周期重播换成代码余量；boot_cnt 那条 alive 保留 ——
                     *   它是识别"看门狗复位循环"的唯一依据。 */

#if (APP_AT_PROBE_TEST != 0)
                    /* ★ 探活结论周期重播（每 4 s）：板载 CH340 插线即上电，
                     *   开机日志必然错过，所以模组状态必须能随时看到。
                     *   这是"实际 OTA 测试"第 0 步的唯一判据来源。 */
                    if (s_pb_ok != 0U)
                    {
                        LOG_I1(TAG, "  probe baud=%lu", s_pb_baud);
                        LOG_I1(TAG, "  probe ati=%s",  (const char *)s_pb_ati);
                        LOG_I1(TAG, "  probe ipr=%s",  (const char *)s_pb_ipr);
                        LOG_I1(TAG, "  probe cpin=%s", (const char *)s_pb_cpin);
                        LOG_I1(TAG, "  probe csq=%s",  (const char *)s_pb_csq);
                        LOG_I1(TAG, "  probe creg=%s", (const char *)s_pb_creg);
                        LOG_I1(TAG, "  probe cgatt=%s",(const char *)s_pb_gatt);
                    }
                    else
                    {
                        LOG_I1(TAG, "  probe AT=FAIL baud=%lu", s_pb_baud);
                    }
#endif
                }
            }
#endif
        }
    }
}

#endif  /* APP_MIN_TEST：0 = 完整 APP（上），1 = 最小验证 APP（下） */
