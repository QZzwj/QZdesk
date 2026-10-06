## aarch64-unknown-linux-gnu 交叉编译说明

### 混合链接策略

本脚本采用**混合链接**方式：

- **静态链接**：alsa-lib、opus、speexdsp 编译为静态库（`.a`）直接打入二进制
- **动态链接**：保持 libc (GLIBC) 和 libdl 的动态链接

优势：
- 部署时只需拷贝单个可执行文件，无需额外 `.so` 文件
- 支持 `dlopen` 动态加载板子上的 ALSA 插件（如 PulseAudio）
- `default` 音频设备名可正常工作

### CI 自动构建

本脚本会自动下载交叉编译工具链和 C 依赖源码，无需手动配置，可直接用于 GitHub Actions CI。

```bash
# 直接运行
bash scripts/aarch64-unknown-linux-gnu/build.sh

# 输出: target/aarch64-unknown-linux-gnu/release/xiaozhi_linux_rs
```

### 工具链说明

脚本从 `Source_Mirror` Release 下载 `aarch64-linux-gnu-cross` 工具链。如需替换为其他版本的 GNU 工具链（例如使用更低版本的 GLIBC 以提升设备兼容性），修改脚本中的 `TOOLCHAIN_URL` 即可。

> **GLIBC 版本兼容性**：编译时工具链的 GLIBC 版本决定了二进制能运行的最低系统版本。使用 GLIBC 2.17 (CentOS 7) 或 2.27 (Ubuntu 18.04) 的工具链可获得最广泛的兼容性。

### 验证构建结果

```bash
# 应显示 'dynamically linked'
file target/aarch64-unknown-linux-gnu/release/xiaozhi_linux_rs

# NEEDED 中应仅出现 libc/libdl/libpthread，不应出现 libasound/libopus/libspeexdsp
readelf -d target/aarch64-unknown-linux-gnu/release/xiaozhi_linux_rs | grep NEEDED
```