#!/bin/bash
# 本机回归构建：复刻 uVision 的编译/链接命令行（见 Listings/*.lst 头部的 COMPILER INVOKED BY）
# 用途：不用开 Keil 就能验证"能不能编过、尺寸多少、hex 落点对不对"
#       —— 前提是本机装了 Keil（C:/KEIL_V5/C251）
set -u
CC="C:/KEIL_V5/C251/BIN/C251.EXE"
LD="C:/KEIL_V5/C251/BIN/l251.exe"
OH="C:/KEIL_V5/C251/BIN/OH251.EXE"
INC="INCDIR(COMMON) INCDIR(BSP) INCDIR(OTA) INCDIR(DRV) INCDIR(APP) INCDIR(C:/KEIL_V5/C251/INC/STC)"

build() {   # $1=名 $2=输出目录 $3=DEFINE $4=链接指令 $5..=源文件
  local name=$1 out=$2 def=$3 link=$4; shift 4
  rm -rf "$out"; mkdir -p "$out"
  local objs=() bad=0
  for f in "$@"; do
    local n; n=$(basename "$f" .c)
    "$CC" "$f" INTR2 "ROM(HUGE)" DEBUG TABS\(2\) "LARGE" "$def" $INC \
          "PRINT($out/$n.lst)" "OBJECT($out/$n.obj)" >"$out/$n.log" 2>&1
    if grep -q '\*\*\* ERROR' "$out/$n.log"; then
      echo "  [编译失败] $n"; grep '\*\*\* ERROR' "$out/$n.log" | head -3; bad=1; continue
    fi
    if grep -q 'C150\|C172\|C194' "$out/$n.log"; then
      echo "  [警告] $n"; grep 'C150\|C172\|C194' "$out/$n.log" | head -3
    fi
    objs+=("\"$out/$n.obj\"")
  done
  [ "$bad" -ne 0 ] && return 1

  local IFS=$',\n'
  { printf '%s,\n' "${objs[@]}" | sed '$ s/,$//'
    printf '\nTO %s\nPRINT(%s/%s.map) CASE\n%s\n' "$out/$name" "$out" "$name" "$link"; } > "$out/$name.lnp"
  "$LD" "@$out/$name.lnp" 2>&1 | grep -E '\*\*\* ERROR|Program Size|RUN COMPLETE'
  "$OH" "$out/$name" "HEXFILE($out/$name.hex)" H386 >/dev/null 2>&1
  echo "  hex: $out/$name.hex"
}

echo "=========== APP (CODE=0xFF4000) ==========="
build APP tools/_out_app "DEFINE(OTA_BUILD_APP=1,OTA_PTR_GENERIC=1)" \
  'CLASSES(CODE(0xFF4000-0xFFFFFF), XDATA(0x1000-0x2EFF), EDATA(0x0-0xFFF), HDATA(0x0-0xFFF), NCONST(0xF000-0xFFFF), HCONST(0xFF4200-0xFFFFFF), ECODE(0xFF4200-0xFFFFFF)) NOOVERLAY' \
  COMMON/crc32.c COMMON/log.c COMMON/param.c COMMON/ringbuf.c \
  BSP/bsp_clock.c BSP/bsp_gpio.c BSP/bsp_timer.c BSP/bsp_uart.c BSP/bsp_wdt.c BSP/flash_iap.c \
  OTA/ota_partition.c DRV/uart_at.c DRV/http_ota.c DRV/uart2_data.c \
  OTA/ota_fsm.c APP/app_mqtt.c APP/app_main.c

echo "=========== BL (CODE=0xFF0000) ==========="
build Bootloader tools/_out_bl "DEFINE(OTA_PTR_GENERIC=1)" \
  'CLASSES(XDATA(0x1000-0x2EFF), EDATA(0x0-0xFFF), HDATA(0x0-0xFFF), NCONST(0xE000-0xEFFF), HCONST(0xFF0100-0xFFFFFF), ECODE(0xFF0100-0xFFFFFF)) NOOVERLAY' \
  BSP/bsp_clock.c BSP/bsp_gpio.c BSP/bsp_timer.c BSP/bsp_uart.c BSP/bsp_wdt.c BSP/flash_iap.c \
  COMMON/crc32.c COMMON/log.c COMMON/param.c COMMON/ringbuf.c \
  OTA/ota_jump.c OTA/ota_partition.c OTA/ota_verify.c Project/Bootloader/boot_main.c

echo "=========== 布局守门（BL->APP 跳转前提） ==========="
python tools/check_layout.py || { echo "  [中止] 布局不合规，先修 check_layout 的 FAIL 项"; exit 6; }

echo "=========== overlay 守门（参数块 vs 局部变量） ==========="
python tools/check_overlay.py || { echo "  [中止] overlay 守门未通过：参数块会踩局部变量，上板必然数字错"; exit 7; }

echo "=========== 合并校验 ==========="
python tools/merge_hex.py --in tools/_out_bl/Bootloader.hex --in tools/_out_app/APP.hex \
       --out tools/_out_combined.hex --check
echo "--- 合并后体检 ---"
python tools/merge_hex.py --in tools/_out_combined.hex --info
