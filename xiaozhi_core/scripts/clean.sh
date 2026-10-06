#!/bin/bash
set -e

# =============================================================================
# 清理脚本 —— 清除编译产物，保留下载的工具链
#
# 功能：
#   1. 执行 cargo clean
#   2. 清理 third_party 中所有目标的 build/ 和 sysroot/ 目录
#   3. 清理 uclibc_stub/ 目录
#   4. 保留已下载的交叉编译工具链（*-cross 目录）
#
# 用法：
#   ./scripts/clean.sh          # 清理所有
#   ./scripts/clean.sh --deep   # 深度清理（包括删除工具链）
# =============================================================================

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"
cd "$PROJECT_ROOT"

DEEP_CLEAN=false
if [ "${1:-}" = "--deep" ]; then
    DEEP_CLEAN=true
fi

echo "=== 清理编译产物 ==="

# 1. cargo clean
echo "执行 cargo clean..."
cargo clean 2>/dev/null || true

# 2. 清理 uclibc_stub
if [ -d "uclibc_stub" ]; then
    echo "清理 uclibc_stub/"
    rm -rf uclibc_stub
fi

# 3. 清理 third_party 中的 build 和 sysroot
if [ -d "third_party" ]; then
    for target_dir in third_party/*/; do
        [ -d "$target_dir" ] || continue
        target_name="$(basename "$target_dir")"

        if $DEEP_CLEAN; then
            echo "深度清理 third_party/$target_name/ (包括工具链)"
            rm -rf "$target_dir"
        else
            # 只清理 build、sysroot 和 alsa-shared，保留工具链 (*-cross)
            if [ -d "${target_dir}build" ]; then
                echo "清理 third_party/$target_name/build/"
                rm -rf "${target_dir}build"
            fi
            if [ -d "${target_dir}sysroot" ]; then
                echo "清理 third_party/$target_name/sysroot/"
                rm -rf "${target_dir}sysroot"
            fi
            if [ -d "${target_dir}alsa-shared" ]; then
                echo "清理 third_party/$target_name/alsa-shared/"
                rm -rf "${target_dir}alsa-shared"
            fi
        fi
    done

    # 深度清理后如果 third_party 为空则删除
    if $DEEP_CLEAN; then
        rmdir third_party 2>/dev/null || true
    fi
fi

echo ""
echo "=== 清理完成 ==="
if $DEEP_CLEAN; then
    echo "（深度清理：工具链已删除，下次编译将重新下载）"
else
    echo "（已保留工具链，下次编译只需重新编译 C 依赖库）"
fi
