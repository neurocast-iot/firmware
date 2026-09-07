#!/bin/sh
# 分析设备上指定进程的实际内存占用 + 系统剩余可用内存
# 直接放到设备上执行: sh analyze_mem.sh
# 默认分析 appmain 和 iot_live

echo "============================================"
echo "       设备内存分析报告"
echo "============================================"
echo ""

# ---- 1. 系统总内存 ----
TOTAL=$(grep '^MemTotal:' /proc/meminfo | awk '{print $2}')
FREE=$(grep '^MemFree:' /proc/meminfo | awk '{print $2}')
AVAIL=$(grep '^MemAvailable:' /proc/meminfo 2>/dev/null | awk '{print $2}')
BUFFERS=$(grep '^Buffers:' /proc/meminfo | awk '{print $2}')
CACHED=$(grep '^Cached:' /proc/meminfo | awk '{print $2}')
SWAP_TOTAL=$(grep '^SwapTotal:' /proc/meminfo | awk '{print $2}')
SWAP_FREE=$(grep '^SwapFree:' /proc/meminfo | awk '{print $2}')

# MemAvailable 在某些老内核上不存在，用 Free+Buffers+Cached 兜底
if [ -z "$AVAIL" ] || [ "$AVAIL" = "0" ]; then
    AVAIL=$((FREE + BUFFERS + CACHED))
fi
USED=$((TOTAL - FREE))

echo "[系统内存]  单位: kB"
echo "-------------------------------------------"
printf "  总内存:       %8d kB  (%d MB)\n" "$TOTAL" "$((TOTAL / 1024))"
printf "  已用:         %8d kB  (%d MB)\n" "$USED" "$((USED / 1024))"
printf "  空闲:         %8d kB  (%d MB)\n" "$FREE" "$((FREE / 1024))"
printf "  Buffers:      %8d kB\n" "$BUFFERS"
printf "  Cached:       %8d kB\n" "$CACHED"
printf "  可用:         %8d kB  (%d MB)  <-- 真正能用的\n" "$AVAIL" "$((AVAIL / 1024))"
printf "  Swap总量:     %8d kB\n" "$SWAP_TOTAL"
printf "  Swap空闲:     %8d kB\n" "$SWAP_FREE"
echo ""

# ---- 2. 逐进程分析 ----
GRAND_RSS=0
GRAND_PSS=0
GRAND_PRIVATE=0

for PROC_NAME in appmain iot_live; do
    # 找 PID（可能有多个）
    PIDS=$(ps | grep "$PROC_NAME" | grep -v grep | awk '{print $1}')

    if [ -z "$PIDS" ]; then
        echo "[$PROC_NAME] 未找到进程"
        echo ""
        continue
    fi

    for PID in $PIDS; do
        SF="/proc/$PID/status"
        MF="/proc/$PID/smaps"
        [ ! -f "$SF" ] && continue

        # 从 /proc/pid/status 读基础指标
        VM_SIZE=$(grep '^VmSize:' "$SF" | awk '{print $2}')
        VM_RSS=$(grep '^VmRSS:' "$SF" | awk '{print $2}')
        VM_DATA=$(grep '^VmData:' "$SF" | awk '{print $2}')
        VM_STK=$(grep '^VmStk:' "$SF" | awk '{print $2}')
        VM_LIB=$(grep '^VmLib:' "$SF" | awk '{print $2}')
        VM_SWAP=$(grep '^VmSwap:' "$SF" | awk '{print $2}')
        THREADS=$(grep '^Threads:' "$SF" | awk '{print $2}')

        # 从 smaps 读更精确的私有内存（PSS 和 Private）
        # Private = Private_Clean + Private_Dirty，这才是进程真正独占的物理内存
        PRIV=0
        SHARED=0
        PSS=0
        if [ -f "$MF" ]; then
            PRIV=$(awk '/^Private_Clean:/{a+=$2}/^Private_Dirty:/{a+=$2}END{print a+0}' "$MF")
            SHARED=$(awk '/^Shared_Clean:/{a+=$2}/^Shared_Dirty:/{a+=$2}END{print a+0}' "$MF")
            PSS=$(awk '/^Pss:/{a+=$2}END{print a+0}' "$MF")
        fi

        echo "[$PROC_NAME] PID=$PID  ($THREADS 个线程)"
        echo "-------------------------------------------"
        printf "  虚拟内存 (VmSize):   %8d kB  (%d MB)\n" "$VM_SIZE" "$((VM_SIZE / 1024))"
        printf "  物理占用 (VmRSS):    %8d kB  (%d MB)\n" "$VM_RSS" "$((VM_RSS / 1024))"
        printf "    |- 数据段 (heap):  %8d kB  (%d MB)\n" "$VM_DATA" "$((VM_DATA / 1024))"
        printf "    |- 栈:            %8d kB\n" "$VM_STK"
        printf "    |- 共享库:         %8d kB  (%d MB)\n" "$VM_LIB" "$((VM_LIB / 1024))"
        printf "    |- Swap:          %8d kB\n" "$VM_SWAP"
        echo ""

        if [ "$PRIV" -gt 0 ] 2>/dev/null; then
            echo "  smaps 精确拆解:"
            printf "    |- Private(独占):  %8d kB  (%d MB)  <-- 真正独占的物理内存\n" "$PRIV" "$((PRIV / 1024))"
            printf "    |- Shared(共享):   %8d kB  (%d MB)\n" "$SHARED" "$((SHARED / 1024))"
            printf "    |- PSS(按比例):    %8d kB  (%d MB)  <-- 共享库按进程数分摊\n" "$PSS" "$((PSS / 1024))"
            echo ""
            OVER=$((VM_RSS - PRIV))
            if [ "$OVER" -lt 0 ]; then OVER=0; fi
            printf "  RSS vs Private 差值: %8d kB  <-- 共享库被重复计算的部分\n" "$OVER"
        fi

        # 占系统内存百分比
        if [ "$TOTAL" -gt 0 ]; then
            PCT_RSS=$((VM_RSS * 100 / TOTAL))
            if [ "$PSS" -gt 0 ] 2>/dev/null; then
                PCT_PSS=$((PSS * 100 / TOTAL))
                printf "  占系统总内存: RSS=%d%%  PSS=%d%%\n" "$PCT_RSS" "$PCT_PSS"
            else
                printf "  占系统总内存: %d%%\n" "$PCT_RSS"
            fi
        fi

        GRAND_RSS=$((GRAND_RSS + VM_RSS))
        GRAND_PSS=$((GRAND_PSS + PSS))
        GRAND_PRIVATE=$((GRAND_PRIVATE + PRIV))
        echo ""
    done
done

# ---- 3. 汇总 ----
echo "============================================"
echo "[汇总]"
echo "-------------------------------------------"
printf "  两进程 RSS 合计:    %8d kB  (%d MB)\n" "$GRAND_RSS" "$((GRAND_RSS / 1024))"
if [ "$GRAND_PSS" -gt 0 ] 2>/dev/null; then
    printf "  两进程 PSS 合计:    %8d kB  (%d MB)  <-- 去重后的真实占用\n" "$GRAND_PSS" "$((GRAND_PSS / 1024))"
fi
if [ "$GRAND_PRIVATE" -gt 0 ] 2>/dev/null; then
    printf "  两进程 Private 合计: %8d kB  (%d MB)  <-- 各自独占部分之和\n" "$GRAND_PRIVATE" "$((GRAND_PRIVATE / 1024))"
fi
printf "  系统可用内存:        %8d kB  (%d MB)\n" "$AVAIL" "$((AVAIL / 1024))"

if [ "$GRAND_PSS" -gt 0 ] 2>/dev/null; then
    REMAIN=$((AVAIL - GRAND_PSS))
else
    REMAIN=$((AVAIL - GRAND_RSS))
fi
printf "  扣除后剩余:          %8d kB  (%d MB)\n" "$REMAIN" "$((REMAIN / 1024))"

if [ "$REMAIN" -lt 0 ]; then
    echo ""
    echo "  注意: 两进程 RSS 已超过系统可用内存!"
    echo "    实际不会这么算，因为共享库只占一份物理内存。"
    echo "    看 PSS 值才是真实总占用。"
fi

echo ""
echo "  说明:"
echo "    RSS = 进程报告的物理内存(含共享库，有重复计算)"
echo "    Private = 进程独占的物理内存(最准确的'真实占用')"
echo "    PSS = 共享库按进程数平摊后的值(系统视角的真实占用)"
echo "    Available = 系统当前可用于新分配的内存"
echo "============================================"
