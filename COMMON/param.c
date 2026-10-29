/*==============================================================================
 *  param.c  --  参数区双备份读写（A/B 两槽，seq + CRC32 选有效者）
 *  写入策略：永远写"非活动"槽 -> 回读校验 -> 切换活动槽（seq 递增）
 *
 *  ⚠ RAM 用量：param_t 约 450 B，全部用 xdata 静态变量，绝不放栈上
 *    （C251 的栈在 idata，只有 128~256 B，放大结构体必溢出）。
 *============================================================================*/
#include "param.h"
#include "flash_iap.h"
#include "crc32.h"
#include "log.h"

/* 编译期断言：参数结构体必须能塞进一个槽的可用区（512 B） */
typedef char param_size_check_t[(sizeof(param_t) <= PARAM_MAX_SIZE) ? 1 : -1];

static param_t xdata g_p;        /* RAM 镜像（460B，必须 xdata）    */
static param_t xdata s_slot_a;   /* 读槽缓存 A（避免栈上大对象）    */
static param_t xdata s_slot_b;   /* 读槽缓存 B                     */
static u8  xdata s_active;           /* 0=A 1=B                        */
static u8  xdata s_dirty;
static u8  xdata s_inited;

/* v0.27：不再走 crc32_calc（那层封装算出的值每次上电都不同）。
 * 形参先落 static（铁律 9），三步按语句分开写（与已验证正确的调用点一致）。 */
static const param_t *s_cp;
static u32 xdata s_pcrc;

static u32 param_crc(const param_t *p)
{
    u16 n = (u16)(sizeof(param_t) - 4U);

    s_cp = p;
    OTA_CRC32_OF(s_pcrc, (const u8 *)s_cp, n);
    return s_pcrc;
}

static u8 param_valid(const param_t *p)
{
    if (p->magic   != PARAM_MAGIC)        return 0;
    if (p->fmt_ver != PARAM_FMT_VER)      return 0;
    if (p->len     != (u16)sizeof(param_t)) return 0;
    if (p->crc32   != param_crc(p))       return 0;
    return 1;
}

static u32 slot_base(u8 idx)
{
    return (idx == 0) ? OTA_PARAM_A_BASE : OTA_PARAM_B_BASE;
}

void param_load_defaults(void)
{
    u8 *p = (u8 *)&g_p;
    u16  i;

    for (i = 0; i < (u16)sizeof(param_t); i++) p[i] = 0;

    g_p.magic         = PARAM_MAGIC;
    g_p.fmt_ver       = PARAM_FMT_VER;
    g_p.len           = (u16)sizeof(param_t);
    g_p.seq           = 0;
    g_p.ota_state     = 0;                  /* OTA_ST_IDLE */
    g_p.boot_try      = 0;
    g_p.active_slot   = 0;
    g_p.keepalive_s   = 60;
    g_p.clean_session = 1;
    g_p.mqtt_port     = 1883;
    g_p.log_level     = (u8)LOG_LVL_INFO;
    g_p.crc32         = param_crc(&g_p);
    s_dirty = 1;
}

ota_ret_t param_init(void)
{
    u8 xdata va, vb;
    ota_ret_t xdata rc;

    s_inited = 1;
    va = vb = 0;

    rc = flash_iap_read(OTA_PARAM_A_BASE, (u8 *)&s_slot_a, (u16)sizeof(param_t));
    if (rc == OTA_OK) va = param_valid(&s_slot_a);

    rc = flash_iap_read(OTA_PARAM_B_BASE, (u8 *)&s_slot_b, (u16)sizeof(param_t));
    if (rc == OTA_OK) vb = param_valid(&s_slot_b);

    if (va && vb)
    {
        if (s_slot_a.seq >= s_slot_b.seq) { g_p = s_slot_a; s_active = 0; }
        else                              { g_p = s_slot_b; s_active = 1; }
    }
    else if (va) { g_p = s_slot_a; s_active = 0; }
    else if (vb) { g_p = s_slot_b; s_active = 1; }
    else
    {
        param_load_defaults();
        s_active = 1;                       /* 默认值写 A 槽 */
        (void)param_save(1);
        return OTA_ERR_CRC;                 /* 首次上电或两槽损坏，已自建默认值 */
    }

    s_dirty  = 0;
    return OTA_OK;
}

param_t *param_get(void)
{
    if (!s_inited) param_load_defaults();
    return &g_p;
}

u8 param_is_dirty(void)
{
    return s_dirty;
}

/*==============================================================================
 * 临时诊断（定位完删除）
 *
 *   v0.13 上板拿到这一行（校验和自洽，可信）：
 *       pA m=50415241 c=bb701478 k=e70c5373
 *   → magic / len / fmt 全对，只有"存的 CRC"与"重算的 CRC"不符。
 *   这只可能意味着：**槽里的那 462 字节正文 ≠ 保存时算 CRC 用的那 462 字节。**
 *
 *   但 param_save 的返回码一直被调用方 `(void)` 丢掉，于是四种可能
 *   （擦失败 / 擦不干净 / 写失败 / 校验有洞）至今没被分开。这里一次全测。
 *
 *   g_pdg[0] = (擦除 rc << 8) | 擦完是否真的全 0xFF
 *   g_pdg[1] = 写入 rc（flash_iap_write 内部带整块回读校验）
 *   g_pdg[2] = 读回后与 RAM 镜像不同的字节数
 *   g_pdg[3] = 最小差异偏移（差异数为 0 时无意义）
 *   g_pdg_rc = 读回本身的 rc
 *============================================================================*/
u32 xdata g_pdg[5];
u8  xdata g_pdg_rc;

ota_ret_t param_save(u8 force)
{
    static u8  s_idx;
    static u16 s_n;
    static u16 s_d;
    static u16 s_f;
    static ota_ret_t s_rc;

    if (!force && !s_dirty) return OTA_OK;

    s_idx = (u8)(s_active ? 0 : 1);         /* 写非活动槽 */
    g_p.seq++;
    g_p.crc32 = param_crc(&g_p);

    g_pdg[0] = 0UL; g_pdg[1] = 0UL; g_pdg[2] = 0UL; g_pdg[3] = 0UL;
    g_pdg[4] = 0UL; g_pdg_rc = 0;

    s_rc = flash_iap_erase_range(slot_base(s_idx), PARAM_MAX_SIZE);
    g_pdg[0] = ((u32)s_rc << 8)
             | (u32)(flash_iap_is_erased(slot_base(s_idx), PARAM_MAX_SIZE) ? 1UL : 0UL);
    if (s_rc != OTA_OK) return s_rc;

    /* ★ cb：写之前对 RAM 镜像算一次 CRC（此刻它应等于 g_p.crc32） */
    g_pdg[2] = param_crc(&g_p);

    s_rc = flash_iap_write(slot_base(s_idx), (const u8 *)&g_p, (u16)sizeof(param_t));
    g_pdg[1] = (u32)s_rc;

    /* ★ ca：写完再算一次。cb != ca = 这 466 字节在"擦完->写完"这段窗口里
     *   被别的东西改过，于是 flash_iap_write 内部的"写后回读校验"必然失败
     *   （它比的是变过的 g_p）—— 那样 w=7 就不是 Flash 的错，是 RAM 的错。 */
    g_pdg[3] = param_crc(&g_p);

    /* 读回逐字节比对。倒序遍历，循环结束时 s_f 留下的是**最小**差异偏移，
     * 一眼能看出"从第几个字节开始坏" —— 直接指向崩溃点所在的字段。 */
    s_d = 0; s_f = 0;
    g_pdg_rc = (u8)flash_iap_read(slot_base(s_idx), (u8 *)&s_slot_a, (u16)sizeof(param_t));
    for (s_n = (u16)sizeof(param_t); s_n != 0U; )
    {
        s_n--;
        if (((u8 *)&s_slot_a)[s_n] != ((u8 *)&g_p)[s_n])
        {
            s_d++;
            s_f = s_n;
        }
    }
    g_pdg[4] = ((u32)s_d << 16) | (u32)s_f;

    if (s_rc != OTA_OK) return s_rc;

    s_active = s_idx;
    s_dirty  = 0;
    return OTA_OK;
}

ota_ret_t param_factory_erase(void)
{
    ota_ret_t rc = flash_iap_erase_range(OTA_PARAM_BASE, OTA_PARAM_SIZE);
    if (rc == OTA_OK)
    {
        param_load_defaults();
        s_active = 1;
    }
    return rc;
}

/*==============================================================================
 * 断点续传
 *============================================================================*/
ota_ret_t param_update_download(u32 offset, u32 total, u32 crc_running)
{
    if (offset > total) return OTA_ERR_PARAM;
    g_p.dl_offset = offset;
    g_p.dl_total  = total;
    g_p.dl_crc32  = crc_running;
    s_dirty = 1;
    return param_save(0);                   /* 允许节流 */
}

ota_ret_t param_clear_download(void)
{
    g_p.dl_offset = 0;
    g_p.dl_total  = 0;
    g_p.dl_crc32  = 0;
    s_dirty = 1;
    return param_save(1);
}

u8 param_get_download(u32 *p_offset, u32 *p_total, u32 *p_crc)
{
    if (g_p.dl_offset == 0) return 0;
    if (g_p.dl_offset > g_p.dl_total) return 0;
    if (p_offset) *p_offset = g_p.dl_offset;
    if (p_total)  *p_total  = g_p.dl_total;
    if (p_crc)    *p_crc    = g_p.dl_crc32;
    return 1;
}

/*==============================================================================
 * 启动计数
 *============================================================================*/
u8 param_boot_try_inc(void)
{
    if (g_p.boot_try < 0xFF) g_p.boot_try++;
    g_p.boot_cnt++;
    s_dirty = 1;
    (void)param_save(1);                    /* 必须立即落盘，否则启动即死会死循环 */
    return g_p.boot_try;
}

ota_ret_t param_boot_try_clear(void)
{
    g_p.boot_try = 0;
    s_dirty = 1;
    return param_save(1);
}

/*==============================================================================
 * 临时诊断已上移到 param_save()（g_pdg[] 的定义在那里）。
 * 旧的 param_diag() 已删除：它只证明了"槽里 magic/len/fmt 对、CRC 不对"，
 * 结论已经拿到，留着白占 BL 的 16K 槽。
 *============================================================================*/
