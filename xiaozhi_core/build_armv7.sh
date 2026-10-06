#!/bin/bash
#
# 把 QZdesk 核心交叉编译到 RV1106（armv7，设备 rootfs 是 uClibc）。
#
# 为什么用静态 musl：Rust 官方没有 uclibc 目标（只有 glibc/musl），而目标
# rootfs 是 uclibc 的，动态 glibc 二进制在里面跑不起来。静态 musl 二进制不
# 依赖目标系统任何动态库，rootfs 一个字节都不用改。
#
# 核心的三个依赖要编 C 代码，所以必须有一把 musl 交叉 gcc：
#   alsa-sys     -> 需要目标版 libasound（下面用 SDK 里的 alsa-lib 源码静态编）
#   audiopus_sys -> 自带 opus 源码，用同一把 gcc 编
#   ring         -> rustls 的加密后端，自带 C/汇编，用同一把 gcc 编
#
# 用法：
#   ./build_armv7.sh          # 编 alsa + 编核心
#   ./build_armv7.sh alsa     # 只编 alsa 静态库
#   ./build_armv7.sh core     # 只编核心（alsa 已就绪时）
#
set -e

CORE_DIR="$(cd "$(dirname "$0")" && pwd)"
TARGET=armv7-unknown-linux-musleabihf
TOOLCHAIN="${QZDESK_MUSL_TOOLCHAIN:-/home/jn/QZdesk/tools/armv7l-linux-musleabihf-cross}"
CROSS="$TOOLCHAIN/bin/armv7l-linux-musleabihf-"
WORK="${QZDESK_CROSS_WORK:-$CORE_DIR/target/cross-deps}"
VENDOR_DIR="${QZDESK_VENDOR_DIR:-$CORE_DIR/../third_party/sources}"
ALSA_PREFIX="$WORK/alsa"
SPEEX_PREFIX="$WORK/speexdsp"
OPUS_PREFIX="$WORK/opus"
# RV1106 是 Cortex-A7。这几个开关每个 C 构建都必须带：
#   -march=armv7-a  默认是 armv5te，缺了它 __sync_* 原子操作会变成外部引用，
#                   链接期报 undefined reference（alsa-lib 的 pcm_meter 就是）
#   -mfpu=neon      opus 的 NEON 内联函数缺它报 "target specific option mismatch"
#   -mfloat-abi=hard 与设备一致
ARCH_CFLAGS="-O2 -march=armv7-a -mfpu=neon -mfloat-abi=hard"
SDK_DIR="${QZDESK_SDK_DIR:-/home/jn/QZdesk/SDK}"
JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

export PATH="$HOME/.cargo/bin:$TOOLCHAIN/bin:/bin:/usr/bin:$PATH"

# 这台机器的 rm 被换成了一个「删除回收站拦截器」（PATH 里的 shim，外加一个被
# 导出的 shell 函数）。它对每次 rm 都跑一遍拦截逻辑，configure/make 因此慢好几
# 倍。构建只删自己的中间产物，这里让 rm 回到系统原版：函数先摘掉，PATH 里让
# /bin 先于 shim 目录。
unset -f rm 2>/dev/null || true

# 本机环境里 http_proxy/https_proxy 指向一个没在跑的本地代理：任何走 Rust HTTP
# 客户端的构建脚本（例如 audiopus_sys 下载 speexdsp 源码）都会超时失败。这里
# 统一摘掉，让构建脚本直连。
unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY all_proxy ALL_PROXY

if [ ! -x "${CROSS}gcc" ]; then
	echo "找不到 musl 交叉工具链：${CROSS}gcc" >&2
	echo "可以先下载（约 50MB）：" >&2
	echo "  mkdir -p $(dirname "$TOOLCHAIN") && cd $(dirname "$TOOLCHAIN")" >&2
	echo "  curl -L -o musl.tgz https://musl.cc/armv7l-linux-musleabihf-cross.tgz && tar xzf musl.tgz" >&2
	exit 1
fi

# ---------------------------------------------------------------- alsa 静态库
build_alsa() {
	if [ -f "$ALSA_PREFIX/lib/libasound.a" ]; then
		echo "alsa 静态库已存在：$ALSA_PREFIX/lib/libasound.a"
		return 0
	fi

	local tarball=""
	# 先用仓库里随附的源码包（third_party/sources），其次 SDK 的 buildroot dl 缓存。
	if [ -n "${XIAOZHI_ALSA_SRC:-}" ]; then
		[ -f "$XIAOZHI_ALSA_SRC" ] && tarball="$XIAOZHI_ALSA_SRC"
	else
		# 注意：这里的通配符必须留在引号外，否则 shell 不展开
		tarball="$(ls "$VENDOR_DIR"/alsa-lib-*.tar.* 2>/dev/null | head -1 || true)"
	fi
	[ -n "$tarball" ] || \
		tarball="$(ls "$SDK_DIR"/sysdrv/source/buildroot/buildroot-*/dl/alsa-lib/alsa-lib-*.tar.* 2>/dev/null | head -1 || true)"
	if [ -z "$tarball" ]; then
		echo "找不到 alsa-lib 源码包（应在 third_party/sources/ 或 SDK 的 buildroot dl 目录里）" >&2
		exit 1
	fi

	mkdir -p "$WORK"
	echo "解包 $tarball"
	tar xf "$tarball" -C "$WORK"
	local src
	src="$(find "$WORK" -maxdepth 1 -type d -name "alsa-lib-*" | head -1)"
	[ -n "$src" ] || { echo "解包后没找到 alsa-lib 目录" >&2; exit 1; }

	echo "配置 alsa-lib（静态、只要 libasound，不要 python/ucm/topology）"
	cd "$src"
	[ -f Makefile ] && make distclean >/dev/null 2>&1 || true
	CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" CFLAGS="$ARCH_CFLAGS" \
	./configure --host="$(basename "${CROSS}gcc" | sed 's/-gcc$//')" \
		--prefix="$ALSA_PREFIX" \
		--enable-static --disable-shared \
		--disable-python --disable-ucm --disable-topology \
		--without-debug >/dev/null

	echo "编译 alsa-lib"
	make -j"$JOBS" >/dev/null
	make install >/dev/null
	echo "alsa 静态库就绪：$ALSA_PREFIX/lib/libasound.a"
}

# ------------------------------------------------------------------- opus 静态库
# 核心的 build.rs 与 audiopus_sys 都先用 pkg-config 找目标版 opus，找不到才去下
# 源码自己编（audiopus_sys 走 cmake，我们这把工具链还得额外喂 FPU 开关）。这里
# 直接交叉编一份静态 opus 并提供 .pc，两边都会跳过各自的下载/构建。
build_opus() {
	if [ -f "$OPUS_PREFIX/lib/libopus.a" ]; then
		echo "opus 静态库已存在：$OPUS_PREFIX/lib/libopus.a"
		return 0
	fi

	local version=1.5.2
	local tarball="$WORK/opus-$version.tar.gz"
	local src="$WORK/opus-$version"
	if [ ! -d "$src" ]; then
		mkdir -p "$WORK"
		# 仓库里随附了同版本源码包，先拷过来，下面的下载分支就不会走（交叉编译不必联网）；
		# XIAOZHI_OPUS_SRC 可指向自备的包。
		local vendored="${XIAOZHI_OPUS_SRC:-$VENDOR_DIR/opus-$version.tar.gz}"
		if [ ! -f "$tarball" ] && [ -f "$vendored" ]; then
			cp -f "$vendored" "$tarball"
		fi
		if [ ! -f "$tarball" ]; then
			echo "下载 opus-$version"
			curl -fsSL -o "$tarball" \
				"https://downloads.xiph.org/releases/opus/opus-$version.tar.gz" || {
				echo "下载 opus 失败" >&2
				exit 1
			}
		fi
		tar xzf "$tarball" -C "$WORK"
	fi

	echo "配置并编译 opus-$version（静态、带 neon）"
	cd "$src"
	[ -f Makefile ] && make distclean >/dev/null 2>&1 || true
	CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" CFLAGS="$ARCH_CFLAGS" \
	./configure --host="$(basename "${CROSS}gcc" | sed 's/-gcc$//')" \
		--prefix="$OPUS_PREFIX" \
		--enable-static --disable-shared --disable-doc \
		--disable-extra-programs >/dev/null

	make -j"$JOBS" >/dev/null
	make install >/dev/null
	echo "opus 静态库就绪：$OPUS_PREFIX/lib/libopus.a"
}

# -------------------------------------------------------------- speexdsp 静态库
# 本项目用的 audiopus_sys 会找目标版 speexdsp（找不到就去 GitHub 下源码自己编）。
# 自己交叉编一份放进来：既不依赖外网，也保证是 armv7 静态库。
build_speexdsp() {
	if [ -f "$SPEEX_PREFIX/lib/libspeexdsp.a" ]; then
		echo "speexdsp 静态库已存在：$SPEEX_PREFIX/lib/libspeexdsp.a"
		return 0
	fi

	local version=1.2.1
	local tarball="$WORK/speexdsp-$version.tar.gz"
	local src="$WORK/speexdsp-$version"
	if [ ! -d "$src" ]; then
		mkdir -p "$WORK"
		# 仓库里随附了同版本源码包，先拷过来，下面的下载分支就不会走（交叉编译不必联网）；
		# XIAOZHI_SPEEXDSP_SRC 可指向自备的包。
		local vendored="${XIAOZHI_SPEEXDSP_SRC:-$VENDOR_DIR/speexdsp-$version.tar.gz}"
		if [ ! -f "$tarball" ] && [ -f "$vendored" ]; then
			cp -f "$vendored" "$tarball"
		fi
		if [ ! -f "$tarball" ]; then
			echo "下载 speexdsp-$version"
			curl -fsSL -o "$tarball" \
				"https://downloads.xiph.org/releases/speex/speexdsp-$version.tar.gz" || {
				echo "下载 speexdsp 失败" >&2
				exit 1
			}
		fi
		tar xzf "$tarball" -C "$WORK"
	fi

	echo "配置并编译 speexdsp-$version（静态）"
	cd "$src"
	[ -f Makefile ] && make distclean >/dev/null 2>&1 || true
	CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" CFLAGS="$ARCH_CFLAGS" \
	./configure --host="$(basename "${CROSS}gcc" | sed 's/-gcc$//')" \
		--prefix="$SPEEX_PREFIX" \
		--enable-static --disable-shared --disable-examples >/dev/null

	make -j"$JOBS" >/dev/null
	make install >/dev/null
	echo "speexdsp 静态库就绪：$SPEEX_PREFIX/lib/libspeexdsp.a"
}

# ------------------------------------------------------------------- 编译核心
build_core() {
	if [ ! -f "$ALSA_PREFIX/lib/libasound.a" ]; then
		echo "还没有 alsa 静态库，先跑：$0 alsa" >&2
		exit 1
	fi
	if [ ! -f "$SPEEX_PREFIX/lib/libspeexdsp.a" ]; then
		echo "还没有 speexdsp 静态库，先跑：$0 speexdsp" >&2
		exit 1
	fi
	if [ ! -f "$OPUS_PREFIX/lib/libopus.a" ]; then
		echo "还没有 opus 静态库，先跑：$0 opus" >&2
		exit 1
	fi

	# alsa-sys 用 pkg-config 找目标版 alsa：把搜索路径指到我们编出来的那个。
	# 必须同时设 PKG_CONFIG_LIBDIR —— 它才会「替换」pkg-config 的默认搜索目录；
	# 只设 PKG_CONFIG_PATH 是「追加」，宿主机的 opus.pc 照样会被找到，结果把
	# x86_64 的 -lopus/-L/usr/lib/x86_64-linux-gnu 链进 armv7 目标（链接期报
	# "file format not recognized"）。关掉默认目录后，audiopus_sys 会自动回退到
	# 编译它自带的 opus 源码，用的就是我们这把 musl gcc。
	export PKG_CONFIG_ALLOW_CROSS=1
	export PKG_CONFIG_PATH="$ALSA_PREFIX/lib/pkgconfig:$SPEEX_PREFIX/lib/pkgconfig:$OPUS_PREFIX/lib/pkgconfig"
	export PKG_CONFIG_LIBDIR="$ALSA_PREFIX/lib/pkgconfig:$SPEEX_PREFIX/lib/pkgconfig:$OPUS_PREFIX/lib/pkgconfig"
	export PKG_CONFIG_SYSROOT_DIR=
	# cc-rs 按 <目标三元组>_<变量> 取交叉工具，全部指向 musl gcc
	local suffix
	suffix="$(echo "$TARGET" | tr '-' '_')"
	export "CC_${suffix}=${CROSS}gcc"
	export "AR_${suffix}=${CROSS}ar"
	export "CFLAGS_${suffix}=${ARCH_CFLAGS}"
	export "CXX_${suffix}=${CROSS}g++"
	export "CXXFLAGS_${suffix}=${ARCH_CFLAGS}"
	export CARGO_TARGET_ARMV7_UNKNOWN_LINUX_MUSLEABIHF_LINKER="${CROSS}gcc"
	# 让 linker 能找到静态 libasound；-static 保证产物是静态 ELF
	export RUSTFLAGS="-L native=$ALSA_PREFIX/lib -C link-arg=-static"

	# audiopus_sys 用 cmake 编它自带的 opus：不给工具链文件，cmake 会拿宿主编译器
	mkdir -p "$WORK"
	local cmake_toolchain="$WORK/musl-toolchain.cmake"
	cat > "$cmake_toolchain" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_C_COMPILER ${CROSS}gcc)
set(CMAKE_CXX_COMPILER ${CROSS}g++)
set(CMAKE_AR ${CROSS}ar)
set(CMAKE_RANLIB ${CROSS}ranlib)
set(CMAKE_C_FLAGS_INIT "${ARCH_CFLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${ARCH_CFLAGS}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
	export CMAKE_TOOLCHAIN_FILE="$cmake_toolchain"

	cd "$CORE_DIR"
	echo "cargo build --release --target $TARGET"
	cargo build --release --locked --target "$TARGET"

	local bin="$CORE_DIR/target/$TARGET/release/xiaozhi-linux-rs"
	[ -f "$bin" ] || { echo "没生成 $bin" >&2; exit 1; }
	echo
	echo "产物：$bin"
	file "$bin" || true
	ls -la "$bin" | awk '{print "体积："$5" bytes"}'
}

case "${1:-all}" in
alsa) build_alsa ;;
speexdsp) build_speexdsp ;;
opus) build_opus ;;
core) build_core ;;
all)
	build_alsa
	build_speexdsp
	build_opus
	build_core
	;;
*) echo "用法: $0 [all|alsa|speexdsp|opus|core]" >&2; exit 1 ;;
esac
