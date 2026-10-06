<div align="center">
<img alt="License" src="https://img.shields.io/badge/license-MIT-blue.svg"><img alt="Version" src="https://img.shields.io/badge/version-1.0.0-informational.svg"><img alt="UI" src="https://img.shields.io/badge/UI-LVGL%209.2.3-9cf.svg"><img alt="Core" src="https://img.shields.io/badge/core-Rust-orange.svg"><img alt="Platform" src="https://img.shields.io/badge/platform-RV1106%20%2F%20Linux-lightgrey.svg"><img alt="Build" src="https://img.shields.io/badge/build-CMake%20%2B%20Cargo-success.svg">
</div>

<h1 align="center">QZdesk</h1>

<p align="center">A 480×320 landscape embedded AI desktop terminal: local LVGL UI + Rust voice core + importable skills</p>

<p align="center">
🇨🇳 <a href="./README.md">简体中文</a> | 🇺🇸 <a href="./README.en.md">English</a>
</p>

---

## Introduction

QZdesk is an AI desktop terminal running on an embedded Linux board. It consists of two processes that talk over local UDP (the UI spawns the core):

- **`qzdesk_screen`** (C + LVGL) — the 480×320 landscape UI that owns the touch panel, fbdev, Wi-Fi, backlight and volume;
- **`xiaozhi_linux_rs`** (Rust) — the voice core: real-time speech through the XiaoZhi cloud, plus an MCP gateway, local skills, smart-home control, weather, performance monitoring and a web console.

A skill is a directory with a `SKILL.md`, imported from the web console; once imported it changes the assistant's persona, tone and output format with **no code change and no reflash** — a complete, trimmable solution for "a desktop assistant that talks", with swappable personas and local device control.

## Features

| Capability | Description |
| --- | --- |
| Real-time voice chat | WebSocket to the XiaoZhi cloud; Opus encode/decode and ALSA capture/playback locally, with push-to-talk and interruption |
| Local skills | Import a `SKILL.md` (or a ZIP containing it): a primary skill changes the persona directly, secondary skills are retrieved on demand, and imports take effect immediately |
| MCP gateway | Exposes 13 tools to the cloud: 4 configurable external tools + 7 skill tools + 2 smart-home tools, over subprocess / HTTP / TCP |
| Smart home | A built-in MQTT client talks to zigbee2mqtt or hand-written topics directly; controllable from both the web console and voice |
| Text chat | The GUI input box and the web console share one path: text is synthesized locally and sent upstream in exactly the same frames as the microphone |
| Web console | A single-page console served by the device (default `:8080`): skills, live chat, weather, performance, smart home |
| Device data | Weather comes straight from Open-Meteo (no API key) and performance samples `/proc` and `statvfs` every 2 seconds; the device UI and the web page read the same snapshot |
| Board-free development | The same code runs on a PC through the LVGL SDL simulator (480×320 window); Wi-Fi / backlight / volume / timezone use the real interfaces and can all be overridden by environment variables |
| Presence detection | Frame differencing on the camera decides "someone is here": it wakes the screen and has the assistant say hello (5-minute cooldown); plug in an RKNN model to turn it into face recognition |
| Reminders & pomodoro | Stored in the core: voice, device UI and the web page read and write one copy, and a restart does not lose it; when a reminder fires it lands in the chat record |
| Core logs | The last few hundred lines stay in memory; the web `logs` card and the device read the same source, so no ssh needed to debug |

## Architecture

```text
qzdesk_screen (LVGL 480×320 UI)
      ↕ UDP 5678 / 5679
xiaozhi_linux_rs (Rust voice core)
      ↕ HTTP :8080 ──── browser console
      │
      ├─ WSS audio ───→ XiaoZhi cloud (ASR · LLM · TTS)
      ├─ subprocess ──→ MCP tools (system status / timer / pomodoro / robot)
      ├─ MQTT ────────→ zigbee2mqtt / LAN devices
      └─ HTTPS ───────→ Open-Meteo (weather snapshot)
```

| Channel | Port / protocol | Notes |
| --- | --- | --- |
| UI ↔ core | UDP `5678` (core) / `5679` (UI) | Chat history, state, volume & backlight, weather snapshot, performance data |
| Browser ↔ core | HTTP `8080` | The web console and its JSON API (port configurable) |
| Core ↔ cloud | WSS | XiaoZhi protocol messages: hello / listen / stt / tts (default `wss://api.tenclass.net/xiaozhi/v1/`) |
| Core ↔ LAN devices | MQTT 3.1.1 (QoS0) | Smart home |
| Core ↔ MCP tools | subprocess / HTTP / TCP | Declared in the `[mcp]` section of `config.toml` |

## Getting Started

Requirements: CMake ≥ 3.12.4, a C/C++ and a Rust toolchain (the core uses edition 2024), SDL2 (simulator only), ALSA development libraries with opus/speexdsp, and Python 3 (two MCP tool scripts). LVGL **9.2.3** ships with the repository under `third_party/` — nothing to prepare.

> [!NOTE]
> In `third_party/`, `lvgl/`, `lv_conf.h` and `conf/dev_conf.h` must stay side by side (`lv_conf.h` does `#include "conf/dev_conf.h"`). The opus / speexdsp / alsa-lib sources the cross build needs ship there too, under `third_party/sources/`, and `build.rs` plus `build_armv7.sh` prefer them, so **those steps need no network access** (override with `XIAOZHI_OPUS_SRC` / `XIAOZHI_SPEEXDSP_SRC` / `XIAOZHI_ALSA_SRC`). The cross toolchain itself is not vendored (~288MB); the scripts only download it when it is missing locally.

### Run on a PC (simulator)

```bash
./run.sh      # 480×320 window, mouse acts as touch
```

`run.sh` only drives the simulator and builds for the host (its output is an x86 binary, useless on the device). It frees TCP 8080 from any previous service, configures and builds `qzdesk_screen` plus the Rust core, then starts them under a small supervisor loop. Useful switches:

| Environment variable | Effect |
| --- | --- |
| `QZDESK_BUILD_CORE=ON` | Force a core rebuild (otherwise `target/release` is reused) |
| `QZDESK_BUILD_DIR=...` | Build directory (default `build`) |
| `QZDESK_JOBS=N` | Number of parallel build jobs |
| `QZDESK_REPLACE_PORT_8080=0` | Do not kill the process holding port 8080 |
| `QZDESK_PANEL=320x240` | Build and run for a different panel size (default `480x320`) |

### Building the device image (RV1106)

The device is an ARMv7 / RV1106 board: **nothing is compiled on it and host binaries are useless on it**. Both the UI and the core come out of the Rockchip SDK (a separate repository, not shipped here — board configuration, panel timings and the partition layout live in it):

```bash
cd <Rockchip SDK>
./build_qzdesk.sh       # builds the UI + cross-compiles the core, then packs output/image/update.img
```

The board configuration has to be picked once beforehand (the script reminds you): `./build.sh lunch` → RV1106_QZdesk + SPI_NAND.

- UI `qzdesk_screen`: built by the SDK's cross toolchain through this repository's `CMakeLists.txt` (the SDK's `project/app/qzdesk/src` points at this repository);
- Core `xiaozhi_linux_rs`: built by `xiaozhi_core/build_armv7.sh`, statically linked against musl, so the target rootfs needs no extra shared libraries;
- Output lands in the SDK's `project/app/out/bin/` and is packed into `output/image/update.img` for flashing; on boot `S99qzdesk` starts the UI, which then starts the core.

## Usage

### First activation

On start, the core performs an OTA activation check against the cloud. For the first run, open the **AI assistant** page on the device and enter the six-digit activation code shown in the chat area at `xiaozhi.me` on your phone.

### UI tour

| Page | Contents |
| --- | --- |
| Home | Status bar (time / device address / Wi-Fi / battery) + AI assistant card + weather, settings and apps cards |
| AI chat | Chat view ⇄ full-screen face, two modes; the chat view is bubbles plus an input bar, and the face switches between 5 expressions with the core state |
| Apps | Skills / system status / timers / pomodoro / presence detection / device control |
| Skills | Shows whether each skill is primary, secondary or off |
| Settings | WLAN scan and connect, sound, backlight, time and timezone, about |

### Using skills

A skill is a directory containing `SKILL.md`, and it defines the assistant's persona, tone and output format:

1. Open `http://<device-ip>:8080` in a browser;
2. Paste the `SKILL.md` content, or upload a ZIP containing it;
3. A newly imported skill **becomes the primary skill**, effective from the next turn; switch it to secondary or off later on the device's **Skills** page or in the web list;
4. **You can switch mid-conversation too**: say "switch to ×× mode" / "answer with the ×× skill" / "drop that persona" and the assistant calls `skill_use` to swap the primary skill on the spot (the old one becomes secondary), with the body delivered in the tool result — **no session rebuild, no dropped connection**.

| Role | Behaviour |
| --- | --- |
| Primary | Its body goes straight into the model context and shapes persona and format from the first sentence |
| Secondary | Costs no context; retrieved by `skill_search` / `skill_read` only when relevant |
| Off | Takes no part in the conversation |

> [!NOTE]
> The primary skill body travels in the tool descriptions of MCP `tools/list` (sent once per session), not in `initialize.instructions` — the latter is ignored by some cloud gateways, whereas tool descriptions always reach the model context. The core rebuilds the session when skills change.

### Web console

Default address `http://<device-ip>:8080`; change the port with `QZDESK_SKILL_WEB_PORT` (or `XIAOZHI_SKILL_WEB_PORT`). The page offers skill management and editing, live chat (pushed over SSE), weather, performance monitoring and smart-home control.

## Configuration

Configuration lives in two layers: `xiaozhi_core/config.toml` holds compile-time defaults (embedded into the binary by `build.rs`), `xiaozhi_config.json` is the runtime configuration (generated on first start; restart to apply). Frequently used environment variables (they take precedence over the config files):

| Environment variable | Effect |
| --- | --- |
| `QZDESK_SKILL_WEB_PORT` / `XIAOZHI_SKILL_WEB_PORT` | Web console port (default 8080) |
| `QZDESK_SKILL_DIR` / `QZDESK_SKILL_SOURCE_DIR` | Skill directory / read-only skill source directory |
| `QZDESK_CORE_AUTOSTART=0` | Do not let the UI spawn the core |
| `QZDESK_CORE_BIN` | Path to the core executable |
| `QZDESK_CORE_HOST` / `QZDESK_CORE_PORT` / `QZDESK_CORE_GUI_PORT` | Local IPC addresses (defaults `127.0.0.1`, `5678`, `5679`) |
| `QZDESK_TTS_APP_ID` / `_API_KEY` / `_API_SECRET` / `_VOICE` / `_SPEED` / `_VOLUME` / `_PITCH` / `_ENABLE=0` | Online text-to-speech |
| `QZDESK_SMARTHOME_ENABLE` / `_BROKER` / `_USER` / `_PASSWORD` / `_Z2M_TOPIC` | Smart-home MQTT |
| `WEATHER_LATITUDE` / `WEATHER_LONGITUDE` / `WEATHER_CITY` / `WEATHER_ENABLE=0` | Weather card |
| `QZDESK_MATERIAL_OPA` / `QZDESK_GLASS_OPA` | UI material opacity (0–100%, default 90%) |
| `QZDESK_REDUCE_MOTION=1` | Disable entrance and transition animations |
| `QZDESK_WIFI_IFACE` / `QZDESK_WPA_CONF` / `QZDESK_BACKLIGHT` / `QZDESK_TZ_PROFILE` | Override hardware paths on a different board or for local testing |
| `QZDESK_AUDIO_DISABLED=1` | Do not start ALSA capture/playback (on by default in the simulator) |

> [!WARNING]
> `config.toml` holds compile-time defaults and must be committed, so **never** put real cloud credentials in it. Pass every secret through the environment variables above and keep placeholders (such as `<YOUR_APP_ID>`) in the file.

## HTTP API Reference

Every capability of the web console is served by these JSON endpoints (`xiaozhi_core/src/skill_web.rs`); the device-side skills page uses the same API:

| Method | Path | Description |
| --- | --- | --- |
| `GET` | `/` | The console single page |
| `GET` | `/api/skills` | Skill list, each skill's `role`, and `summary.{primary,secondary,version}` |
| `POST` | `/api/skills` | Install/overwrite `SKILL.md` with JSON `{name, content}` |
| `POST` | `/api/skills/upload` | Upload a ZIP containing `SKILL.md` |
| `GET` | `/api/skills/<id>/content` | Read the raw `SKILL.md` |
| `PUT` | `/api/skills/<id>/selection` | Set the role: `{"role":"primary"\|"secondary"\|"none"}` |
| `DELETE` | `/api/skills/<id>` | Delete a user-installed skill |
| `POST` | `/api/chat/send` | Send a text message (synthesized locally and sent upstream), returns `202` |
| `GET` | `/api/chat/stream` | Live chat stream (SSE) |
| `GET` | `/api/chat/history` | Existing chat history |
| `GET` | `/api/weather` | Weather snapshot |
| `POST` | `/api/weather/refresh` | Refresh the weather now |
| `GET` | `/api/performance` | Performance snapshot |
| `GET` | `/api/performance/history` | Performance history for charts |
| `GET` | `/api/devices` | LAN device table |
| `POST` | `/api/devices/discover` | Trigger device discovery |
| `POST` | `/api/devices/command` | Control a device: `{id, action, value}` |
| `GET` / `PUT` | `/api/smarthome` | Read / update the MQTT hub configuration |
| `GET` / `POST` | `/api/timers` | Reminder list / create one (voice, device UI and this page share one copy) |
| `DELETE` | `/api/timers/<id>` | Delete a reminder |
| `GET` / `POST` | `/api/pomodoro` | Pomodoro state / action (`start`, `pause`, `resume`, `stop`, `status`) |
| `GET` | `/api/logs?limit=` | Recent core logs (in-memory ring, 200 lines by default) |

## MCP Tools

The core exposes 13 MCP tools to the cloud (`tools/list`), registered from the `[mcp]` section of `config.toml` plus built-in modules:

| Tool | Source | Description |
| --- | --- | --- |
| `get_system_status` | `system_status.sh` | CPU load, memory, disk, uptime |
| `robot_move` | `robot_move.sh` | Robot motion control (forward / backward / left / right / stop) |
| `set_timer` | built-in | Create a reminder; when it fires the notice goes into the chat record, visible on the device and the web page |
| `pomodoro` | built-in | Pomodoro (focus / break cycles, reports the time left) |
| `skill_list` / `skill_search` / `skill_read` | built-in | List / search / read local skills |
| `skill_use` | built-in | Switch skills inside the current session: say "switch to TCM mode" and the body arrives with the tool result, no session rebuild |
| `skill_status` / `skill_validate` / `skill_reload` | built-in | Skill diagnostics, validation and rescan |
| `device_list` / `device_control` | built-in | Smart-home device listing and control |

Transports, execution modes (`sync` / `background`) and timeouts for external tools are declared in `config.toml`; see [`xiaozhi_core/docs/MCP功能说明.md`](./xiaozhi_core/docs/MCP功能说明.md) (Chinese).

## Project Layout

```text
├── app/                      # LVGL UI: main loop, theme, mascot, config, Wi-Fi, core process management
├── pages/                    # Home / assistant / apps / skills / settings / weather / performance
├── include/                  # Public interfaces of each module
├── assets/                   # Mascot and icon source images
├── docs/                     # UI and design notes
├── tools/                    # Python bake scripts for mascot and icons
├── xiaozhi_core/             # Rust voice core (MIT)
│   ├── src/                  # Audio, network, controller, MCP gateway, skills, weather, smart home…
│   ├── web/                  # Web console (single HTML file)
│   ├── docs/                 # MCP / OTA / audio device / GUI adaptation notes
│   └── scripts/              # Cross-compilation scripts for other targets
├── run.sh                    # PC simulator: one-command build + run (the device goes through the SDK)
├── CMakeLists.txt
└── LICENSE                   # MIT
```

## UI & Design

The UI follows Apple's light-mode HIG. All design tokens live in `include/theme.h`, and animation curves and durations are collected there too; the full story — palette, material opacity, mascot bitmap baking, memory and rendering trade-offs, performance tuning — is in [`docs/UI与设计.md`](./docs/UI与设计.md) (Chinese). A few constraints worth knowing up front:

- 480×320 at 16-bit colour depth; `LV_MEM_SIZE` in `lv_conf.h` is 2MB;
- Do not animate scale or rotation on large objects (LVGL allocates a full ARGB layer for them, which easily fails on the embedded heap);
- The UI is laid out for a **480×320 design** and `include/scale.h` scales it to the **actual panel**
  horizontally and vertically by their own ratios (the screen is filled, no letterbox): the device
  reads the size from `/dev/fb0`, while the simulator picks a screen at launch with
  `QZDESK_PANEL=320x240 ./run.sh`. One binary therefore fits any panel ratio without rebuilding —
  **do not edit theme.h**.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| UI feels sluggish | Make sure it is a Release build (LVGL's software renderer is several times slower at `-O0`); delete and recreate an old `build/` directory |
| Text chat does nothing | A working text-to-speech configuration is required (`QZDESK_TTS_*`); the UI reports it when synthesis is unavailable |
| Web console unreachable | Check whether the port is taken (`run.sh` tries to free 8080) or set `QZDESK_SKILL_WEB_PORT` |
| Core will not start | `QZDESK_CORE_AUTOSTART`, `QZDESK_CORE_BIN`, and `xiaozhi_config.json` next to the core |
| No sound in the simulator | The simulator sets `QZDESK_AUDIO_DISABLED=1` by default (no ALSA devices) — expected |

> [!TIP]
> When a skill seems not to apply, have the assistant call `skill_status` or `skill_list` and trust the real return value rather than the UI impression.

## Contributing

- Make sure `cargo test` passes (core side) and that the UI still starts in the simulator;
- UI changes must follow the design tokens and motion rules in `include/theme.h` — do not hard-code colours or durations inside pages;
- Add new skills as standalone directories with a `SKILL.md` instead of modifying core code.

## License

This project and the bundled voice core in `xiaozhi_core/` are released under the **MIT** license; see [`LICENSE`](./LICENSE) and [`xiaozhi_core/LICENSE`](./xiaozhi_core/LICENSE) (Copyright © 2025 Hyrsoft).
