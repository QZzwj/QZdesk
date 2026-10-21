<div align="center">
  <img width="180" src="./docs/images/logo.png" alt="Xiaozhi Linux logo">

  <h1>QZdesk Core</h1>
  <p>面向 Linux 与嵌入式设备的 Rust 小智 AI 客户端核心</p>

  <a href="https://github.com/haoyn231/xiaozhi_linux_rs/releases/latest"><img src="https://img.shields.io/github/v/release/haoyn231/xiaozhi_linux_rs?display_name=tag&sort=semver" alt="Latest release"></a>
  <a href="./LICENSE"><img src="https://img.shields.io/badge/License-MIT-green" alt="MIT License"></a>
  <img src="https://img.shields.io/badge/Core-Rust%202024-DEA584" alt="Rust 2024">
  <img src="https://img.shields.io/badge/Platform-Linux-FCC624" alt="Linux">
  <img src="https://img.shields.io/badge/Targets-x86__64%20%7C%20ARMv7%20%7C%20AArch64-informational" alt="x86_64, ARMv7 and AArch64">
  <br>
  <a href="https://github.com/haoyn231/xiaozhi_linux_rs/actions/workflows/cross-compile.yml"><img src="https://github.com/haoyn231/xiaozhi_linux_rs/actions/workflows/cross-compile.yml/badge.svg" alt="Cross Compile"></a>
  <a href="https://github.com/haoyn231/xiaozhi_linux_rs/stargazers"><img src="https://img.shields.io/github/stars/haoyn231/xiaozhi_linux_rs.svg" alt="GitHub stars"></a>

  <p>
    <a href="./README_en.md">English</a> |
    <strong>简体中文</strong>
  </p>
</div>

---

## 📖 项目简介

**QZdesk Core** 是 QZdesk AI 助手在 Linux 平台上的 Rust 实现，集成云端协议、实时音频、设备激活、业务状态机、GUI 进程通信和 MCP 工具扩展，适用于桌面 Linux、ARM 开发板以及资源受限的嵌入式设备。

项目专注于稳定、轻量的客户端核心，不内置特定 GUI。核心进程通过 UDP 与独立界面通信，可按硬件需求搭配 LVGL、Qt、Slint、TUI，或直接以无界面方式运行。

<p align="center">
  <a href="https://github.com/78/xiaozhi-esp32">小智 ESP32</a> |
  <a href="https://github.com/100askTeam/xiaozhi-linux">百问网 Linux 版</a> |
  <a href="https://github.com/haoyn231/xiaozhi_linux_rs/releases/latest">下载最新版</a> |
  <a href="https://github.com/haoyn231/xiaozhi_linux_rs/issues">问题反馈</a>
</p>

### ✨ 核心特性

- **实时音频**：支持 I2S/USB 声卡、ALSA 采集与播放、Opus 编解码，以及 SpeexDSP 降噪、AGC 和重采样。
- **云端对话**：支持 WebSocket 全双工连接、心跳保活、设备鉴权、Hello 握手、TTS、STT 和控制指令。
- **设备管理**：自动完成设备激活与绑定，持久化 Client ID / Device ID，并管理空闲、聆听、处理、说话和网络错误状态。
- **GUI 解耦**：通过 UDP IPC 同步激活码、运行状态、Toast 和 TTS 字幕，GUI 也可向核心进程发送控制指令。
- **MCP 扩展**：通过配置动态接入 Subprocess、HTTP、TCP 工具，支持同步和后台执行模式，无需修改核心代码。
- **设备工具**：网页端提供一键体检、脱敏诊断包、本地场景引擎和局域网 Skill 包互传，全部在本机或局域网内完成。
- **多架构构建**：提供 x86_64、ARMv7 GNU、ARMv7 uClibc 和 AArch64 GNU 构建脚本及 GitHub Actions 工作流。

---

## 🧩 系统架构

```mermaid
graph TD
    Config["配置<br/>config.toml / xiaozhi_config.json"]

    subgraph External["外部服务"]
        Cloud["小智云端<br/>WebSocket / HTTP"]
        Tools["外部 MCP 工具<br/>Subprocess / HTTP / TCP"]
    end

    subgraph Core["QZdesk Core"]
        Network["网络与协议"]
        Controller["状态机与业务控制"]
        Audio["ALSA + Opus + SpeexDSP"]
        MCP["MCP Gateway"]
        IPC["GUI Bridge"]

        Network <--> Controller
        Audio <--> Controller
        MCP <--> Controller
        IPC <--> Controller
    end

    subgraph GUI["独立 GUI 进程（可选）"]
        View["LVGL / Qt / Slint / TUI"]
    end

    subgraph Hardware["硬件"]
        Mic["麦克风"]
        Speaker["扬声器"]
        Screen["屏幕 / 触控"]
    end

    Config -.-> Controller
    Network <-->|"WSS / HTTP"| Cloud
    MCP <-->|"JSON-RPC"| Tools
    Audio <--> Mic
    Audio <--> Speaker
    IPC <-->|"UDP / JSON"| View
    View <--> Screen
```

---

## 🚀 快速开始

### 下载预编译版本

前往 [GitHub Releases](https://github.com/haoyn231/xiaozhi_linux_rs/releases/latest) 下载与设备架构和 C 运行库匹配的可执行文件：

| Release 文件 | 目标环境 |
| :--- | :--- |
| `xiaozhi_linux_rs-x86_64-gnu` | x86_64 Linux / GLIBC |
| `xiaozhi_linux_rs-aarch64-gnu` | AArch64 Linux / GLIBC |
| `xiaozhi_linux_rs-armv7-gnueabihf` | ARMv7 Linux / GLIBC hard-float |
| `xiaozhi_linux_rs-armv7-uclibceabihf` | ARMv7 Linux / uClibc hard-float，主要用于 RV1103/RV1106 |

下载后添加执行权限并运行：

```bash
chmod +x ./xiaozhi_linux_rs-x86_64-gnu
./xiaozhi_linux_rs-x86_64-gnu
```

> Release 中的 GNU 二进制基于 GCC 8.3 工具链构建，需要 GLIBC 2.28 或更高版本。请通过 `ldd --version` 检查设备环境；更旧的系统可使用仓库内脚本自行构建。

### 从源码编译

需要 Rust 1.85+、C/C++ 构建工具和 ALSA、Opus、SpeexDSP 开发库。

```bash
git clone https://github.com/haoyn231/xiaozhi_linux_rs.git
cd xiaozhi_linux_rs

# Ubuntu / Debian
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    pkg-config \
    libasound2-dev \
    libopus-dev \
    libspeexdsp-dev

cargo build --release
cargo run --release
```

程序首次启动时会在当前工作目录生成 `xiaozhi_config.json`，并自动写入设备标识。运行前请确认设备具备可用的音频输入、音频输出和网络连接。

### 配置音频设备

使用 ALSA 工具查看可用设备：

```bash
arecord -l
aplay -l
```

然后在首次运行生成的 `xiaozhi_config.json` 中找到并修改输入、输出设备字段，例如：

```json
{
  "capture_device": "plughw:0,0",
  "playback_device": "plughw:1,0"
}
```

完整的设备名格式、查询方法和配置示例见 [音频设备配置说明](./docs/音频设备配置说明.md)。

---

## ⚙️ 配置说明

项目包含两层配置：

- `config.toml`：编译期默认配置，由 `build.rs` 嵌入可执行文件，修改后需要重新编译。
- `xiaozhi_config.json`：运行时配置，首次启动自动生成；修改后重启程序即可生效。

| 配置范围 | 主要内容 |
| :--- | :--- |
| 音频 | 采集/播放设备、下发流格式、播放采样率、声道和缓冲周期 |
| GUI | Core 与 GUI 的 UDP 地址、端口和缓冲区大小 |
| 网络 | WebSocket、OTA 地址、Token、Device ID 和 Client ID |
| Hello | 上行音频格式、采样率、声道和帧时长 |
| TTS | 文字转语音（讯飞在线语音合成）的账号、发音人与参数 |
| MCP | 是否启用网关以及外部工具定义 |

当前网络下发流支持 `opus` 和 `pcm`；`mp3` 配置项已预留，但尚未实现解码。

### 文字直发（GUI / web 的文字聊天）

文字聊天默认把完整文本作为 `listen/detect/text` 直接发给云端，不经过本地 TTS。
这个字段原本也用于唤醒词，部分服务端会把它当完整用户输入处理；当前云端是否接受
长文本，以是否返回 `stt`、`llm` 或 `tts` 为准。文字聊天失败时不会改走本地 TTS。

```text
GUI 输入框 ─┐
            ├─→ chat_text / POST /api/chat/send ─→ listen/detect/text ─→ 云端
web 控制台 ─┘                                      │
                                                   └─ 超时或不支持
                                                      → abort
                                                      → 聊天与 Toast 明确报错
```

- 两个入口共用核心里的同一条路径（`CoreController::send_text_turn`），不存在两套实现：
  - GUI：`{"type":"chat_text","text":"…"}`（`qzdesk_core_send_text()` 发的就是这个）；
  - web 控制台：`POST /api/chat/send`，立刻回 `202` 并把文字转给核心。
- 默认等待 12 秒；完全没有云端响应时，先发送会话级 `abort`，再结束本轮并报告
  真实错误。不会生成或上传本地语音，避免出现“看到文字但实际问了另一句”的情况。
- 发送失败不会永久禁用后续消息。网络恢复或服务端重新接受文字后可以直接继续。
- 文字直发期间麦克风上行会被静音；用户主动按下说话会立即结束这次等待。
- 直发诊断控制：

```bash
QZDESK_DIRECT_TEXT=0                    # 关闭文字直发做诊断（不会改用 TTS）
QZDESK_DIRECT_TEXT_TIMEOUT_SECS=12      # 等待云端起手的时间，范围 3-60 秒
```

- TTS 账号密钥不必烧进二进制，运行时可用环境变量覆盖：

```bash
QZDESK_TTS_APP_ID=… QZDESK_TTS_API_KEY=… QZDESK_TTS_API_SECRET=… ./xiaozhi-linux-rs
QZDESK_TTS_VOICE=x4_xiaoyan      # 发音人（vcn）
QZDESK_TTS_SPEED=50              # 语速 0-100
QZDESK_TTS_ENABLE=0              # 关闭主动问候；文字直发仍可用
QZDESK_TTS_DUMP=/tmp/tts.wav     # 把送出去的这段音频存一份，便于排查
```

---

## 🔌 GUI 与 MCP 扩展

### 独立 GUI

Core 默认通过 UDP 与 GUI 进程交换 JSON 消息。可参考以下项目和文档完成适配：

- [LVGL GUI 示例](https://github.com/Hyrsoft/lvgl_xiaozhi_gui)
- [Slint GUI 示例](https://github.com/Hyrsoft/slint_xiaozhi_gui)
- [GUI 适配说明](./docs/GUI适配说明.md)

### MCP 工具

MCP Gateway 可从配置中动态加载外部工具，适合接入系统状态查询、屏幕亮度、远程播放器和局域网设备控制等能力。

| 传输方式 | 使用场景 |
| :--- | :--- |
| `subprocess` | 调用本地 Shell、Python 或其他可执行程序 |
| `http` | 调用远端或局域网 HTTP 服务 |
| `tcp` | 与自定义 TCP 服务或硬件网关通信 |

详细字段、执行模式和示例见 [MCP 功能说明](./docs/MCP功能说明.md) 与 [`examples`](./examples) 目录。

### 本地 Skill 检索

Rust 核心内置了三个只读 MCP 工具，用于按需加载本地 Markdown Skill：

| 工具 | 用途 |
| :--- | :--- |
| `skill_list` | 列出已安装 Skill 及元数据 |
| `skill_search` | 按关键词检索 `SKILL.md` 和 `*.md` 参考资料 |
| `skill_read` | 读取 Skill 主文件或检索结果中的参考文件 |

每个 Skill 放在独立目录中，并至少包含一个 `SKILL.md` 文件。运行时会重新扫描目录，复制或删除 Skill 后无需重新编译。默认搜索目录按优先级为：

```text
/oem/usr/share/xiaozhi/skills
/userdata/xiaozhi/skills
./skills
./xiaozhi_skills
```

也可以通过 `QZDESK_SKILL_DIR=/path/to/skills` 指定首选目录（兼容旧变量 `XIAOZHI_SKILL_DIR`）。设备上默认使用 `/userdata/xiaozhi/skills`；如果该目录不存在或当前用户不可写，模拟器和开发机自动回退到 `$XDG_DATA_HOME/qzdesk/skills` 或 `~/.local/share/qzdesk/skills`，避免出现 `Permission denied`。例如：

```bash
mkdir -p /oem/usr/share/xiaozhi/skills/my_skill
cp SKILL.md /oem/usr/share/xiaozhi/skills/my_skill/SKILL.md
```

网页管理端把安装的 Skill 先放入本地可调用池，再为每个 Skill 设置“启用/停用”和角色：最多一个主 Skill，主 Skill 的 `SKILL.md` 会在 MCP `initialize`（聊天会话开始前）加载；副 Skill 只提供名称和描述，模型需要时才调用 `skill_search` / `skill_read` 获取内容。选择状态持久化在 `/userdata/xiaozhi/skill_state.json`，修改选择后会自动重建 WebSocket 会话。

Rust 核心同时提供一个轻量网页管理端，默认监听 `8080` 端口。浏览器访问设备 IP 的 `http://设备IP:8080/`，即可上传 ZIP、粘贴 `SKILL.md`、删除 Skill 和调整主/副角色。网页端不再提供 Git 地址和分支输入。管理端写入设备的 `/userdata/xiaozhi/skills`，开发机自动使用用户可写目录；可用 `QZDESK_SKILL_WEB_PORT`（兼容 `XIAOZHI_SKILL_WEB_PORT`）修改端口，用 `QZDESK_SKILL_DIR`（兼容 `XIAOZHI_SKILL_DIR`）修改 Skill 持久化目录。删除接口只允许删除用户目录中的 Skill，不会删除 OEM 固件内置内容。通过 `run.sh` 启动时，QZdesk 会先关闭占用 `8080` 的旧服务。

Skill 列表上方的“跨设备技能包”用于离线迁移：导出全部用户自装 Skill、嵌套参考文件和 `primary` / `secondary` / `none` 角色，再在另一台设备导入。ZIP 根部是 `qzdesk-skills.json` 清单，技能内容位于 `skills/<id>/`；OEM 和源码目录中的预置 Skill 不会进入包内，隐藏文件、`__pycache__`、`node_modules`、`target` 等缓存也不会被导出。导入会先完整校验路径、体积和清单，同名 Skill 必须确认后才替换；未带 `X-QZdesk-Replace: 1` 的冲突请求不会改动现有文件。

### 一键体检与脱敏诊断包

网页“技能管理”右侧的“设备工具”中可以直接运行体检。检查项包括麦克风、扬声器、局域网、DNS、MQTT、天气、磁盘、Skill 和状态文件。体检结果只保存在当前核心进程内，服务重启后需重新运行。

诊断包是 ZIP，包含：

- `result.json`：检查项、耗时和汇总。
- `logs.txt`：经过脱敏的近期日志。
- `performance.json`：CPU、内存、磁盘和运行状态快照。
- `errors.json`：失败检查项与性能错误摘要。
- `README.txt`：诊断包内容边界说明。

诊断包不会写入密钥、聊天、记忆正文、Skill 正文或原始配置。麦克风和扬声器检查只确认设备或配置是否存在，不采样、不播放，也不录制音频。

| 接口 | 用途 |
| :--- | :--- |
| `GET /api/diagnostics/latest` | 读取当前进程最近一次体检结果 |
| `POST /api/diagnostics/run` | 立即运行体检并返回结果 |
| `GET /api/diagnostics/export` | 下载最近一次体检的脱敏 ZIP |

### 本地场景引擎

“设备工具”的“场景”页签支持新建、编辑、启停、删除、试运行和定时触发。场景保存在 QZdesk 用户数据目录的 `scenes.json`，最多 32 个场景，每个场景最多 12 个动作和 8 个触发器。

可用动作包括灯光开关/调亮度、系统音量、屏幕背光、延时提醒和切换 Skill 角色。触发器包括手动执行、每天定时和 MQTT 设备状态变化。试运行会逐项验证设备和 Skill，但不会实际发布灯光指令、写背光或设置音量。正式执行时每个动作独立运行，单个动作失败不会阻断后续动作，网页会列出每一项的成功或失败原因。

```json
{
  "name": "回家模式",
  "enabled": true,
  "actions": [
    { "type": "light", "deviceId": "客厅灯", "action": "on" },
    { "type": "volume", "percent": 35 },
    { "type": "skill_role", "skill": "home-assistant", "role": "primary" }
  ],
  "triggers": [
    { "type": "time", "hour": 19, "minute": 30, "weekdays": [1, 2, 3, 4, 5] },
    { "type": "mqtt", "deviceId": "门磁", "state": true }
  ]
}
```

`weekdays` 使用 `1` 到 `7` 表示周一至周日，空数组表示每天。MQTT 触发器至少需要 `state` 或 `brightness` 条件，亮度范围是 `0..100`。

| 接口 | 用途 |
| :--- | :--- |
| `GET /api/scenes` | 列出场景和执行限制 |
| `POST /api/scenes` | 新建场景 |
| `PUT /api/scenes/{id}` | 更新场景 |
| `DELETE /api/scenes/{id}` | 删除场景 |
| `POST /api/scenes/{id}/run` | 执行场景；请求体 `{"dryRun":true}` 表示试运行 |

核心另外注册了只读 MCP 工具 `scene_list` 和执行工具 `scene_run`，语音侧可以用同一套场景配置。

场景动作依赖以下可选环境变量：

| 变量 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `QZDESK_BACKLIGHT` | `/sys/class/backlight/backlight/brightness` | 背光 `brightness` 文件的绝对路径，同目录需有 `max_brightness` |
| `QZDESK_MIXER_CARD` | `0` | 传给 `amixer -c` 的声卡编号 |
| `QZDESK_MIXER_CONTROL` | `DAC LINEOUT Volume` | `amixer cset name=...` 使用的控件名 |
| `QZDESK_MIXER_MAX` | `30` | 控件原始最大值，用于把百分比换算成 raw 数值 |

### 局域网 Skill 互传

“设备工具”的“互传”页签用于两台 QZdesk 在同一局域网内直接传输 Skill 包，不经过云端。接收端生成六位配对码，120 秒内有效且只能使用一次；配对令牌有效 10 分钟并绑定来源 IP。发现使用 UDP，默认端口 `18766`，传输按 256 KiB 分块，支持断点续传，导入前校验 SHA-256，单个包上限 64 MiB。

传输目标只接受回环地址、IPv4 私网地址和 IPv6 ULA/回环地址。同名 Skill 会先返回冲突列表，不会自动覆盖；用户确认后以 `replaceExisting: true` 重试。

当前互传范围仅是 Skill 包，不是完整设置或整机备份，配对方式是六位码而不是二维码。

| 接口 | 用途 |
| :--- | :--- |
| `GET /api/device-sync/status` | 读取本机身份、端口和已配对设备 |
| `POST /api/device-sync/discover` | 扫描同一局域网，等待约 1.8 秒 |
| `POST /api/device-sync/code` | 生成六位配对码 |
| `POST /api/device-sync/pair` | 用 `{"address":"192.168.1.20:8080","code":"123456"}` 配对 |
| `POST /api/device-sync/send` | 发送 Skill 包，可在请求体加 `"replaceExisting":true` |

| 变量 | 默认值 | 说明 |
| :--- | :--- | :--- |
| `QZDESK_SKILL_WEB_PORT` | `8080` | 网页管理端与互传 HTTP 端口，兼容 `XIAOZHI_SKILL_WEB_PORT` |
| `QZDESK_SYNC_PORT` | `18766` | UDP 发现端口，两台设备应使用相同端口 |
| `QZDESK_SYNC_PROBE_ADDR` | 空 | 额外发现目标，逗号分隔的 `IP:port`，例如 `192.168.1.20:18766` |

---

## 💻 平台支持

状态说明：✅ 已提供构建脚本并完成设备验证　🧪 已提供构建脚本，仍欢迎更多设备测试

| Rust Target | C 运行库 | 已验证设备 | 状态 |
| :--- | :--- | :--- | :---: |
| `armv7-unknown-linux-uclibceabihf` | uClibc | Luckfox Pico、QZdesk（RV1106） | ✅ |
| `armv7-unknown-linux-gnueabihf` | GLIBC | Luckfox Lyra（RK3506） | ✅ |
| `aarch64-unknown-linux-gnu` | GLIBC | DshanPi-A1（RK3576）、红米手机 2、N1 盒子 | ✅ |
| `x86_64-unknown-linux-gnu` | GLIBC | Arch Linux 笔记本 | ✅ |
| 其他 Linux 目标 | 视平台而定 | 尚未系统验证 | 🧪 |

目标设备需要提供 ALSA 兼容的音频输入和输出。对于未列出的 Linux 开发板、虚拟机和发行版，理论上可以运行，但需要自行确认 C 运行库、`libasound.so.2` 和声卡驱动兼容性。

---

## 🛠️ 交叉编译

仓库中的脚本会下载所需工具链与依赖源码，并采用混合链接方式构建：运行时动态链接目标系统的 libc 和 `libasound`，Opus 与 SpeexDSP 静态链接进可执行文件。

| 目标 | 构建命令 | 详细说明 |
| :--- | :--- | :--- |
| ARMv7 uClibc | `bash scripts/armv7-unknown-linux-uclibceabihf/build.sh` | [README](./scripts/armv7-unknown-linux-uclibceabihf/README.md) |
| ARMv7 GNU | `bash scripts/armv7-unknown-linux-gnueabihf/build.sh` | [README](./scripts/armv7-unknown-linux-gnueabihf/README.md) |
| AArch64 GNU | `bash scripts/aarch64-unknown-linux-gnu/build.sh` | [README](./scripts/aarch64-unknown-linux-gnu/README.md) |
| x86_64 GNU | `bash scripts/x86_64-unknown-linux-gnu/build.sh` | [README](./scripts/x86_64-unknown-linux-gnu/README.md) |

以 Luckfox Pico / RV1106 为例：

```bash
rustup toolchain install nightly
rustup component add rust-src --toolchain nightly
bash scripts/armv7-unknown-linux-uclibceabihf/build.sh
```

输出文件位于：

```text
target/armv7-unknown-linux-uclibceabihf/release/xiaozhi_linux_rs
```

也可以在 GitHub Actions 中手动运行 `Cross Compile` 工作流，选择单一目标或构建全部目标并创建 Release。

---

## 🗺️ 功能边界与规划

- **IoT 与智能家居联动**：协议能力已具备，更多设备侧集成仍在完善。
- **本地离线唤醒与 AFE**：当前不计划内置。回声消除、波束成形和唤醒效果高度依赖麦克风阵列、声卡链路与硬件调校，更适合由 BSP、独立音频前端进程或专用模块提供。
- **OTA**：Linux 中的客户端是独立进程，升级应由系统服务或部署脚本完成二进制原子替换和进程重启，详见 [OTA 功能说明](./docs/OTA功能说明.md)。

---

## 🤝 贡献

欢迎测试更多 Linux 设备、完善交叉编译脚本、贡献 MCP 示例，或提交 Issue 和 Pull Request。提交代码前请阅读 [贡献指南](./docs/CONTRIBUTING.md)。

QQ群：`695113129`

---

## 🙏 致谢

- [78/xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)
- [100askTeam/xiaozhi-linux](https://github.com/100askTeam/xiaozhi-linux)
- [xinnan-tech/xiaozhi-esp32-server](https://github.com/xinnan-tech/xiaozhi-esp32-server)

---

## 📄 许可证

本项目核心代码基于 [MIT License](./LICENSE) 发布。

构建产物还包含或链接 ALSA、Opus、SpeexDSP 等第三方组件。当前构建脚本动态链接系统 `libasound`，并静态链接 Opus 与 SpeexDSP；进行二次开发或分发时，请同时遵守各第三方组件的许可证要求。
