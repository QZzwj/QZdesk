# QZdesk Echo Demo

按功能拆分的 Echo 桌面移植，界面为 Apple / iOS 浅色风格的 480×320 横屏嵌入式 UI：

```text
QZdesk-Demo/
├── app/
│   ├── main.c              # LVGL、fbdev、触摸和主循环
│   ├── theme.c             # 设计令牌、字体、卡片/滑块/开关样式与动效
│   ├── ai_face.c           # 吉祥物表情（5 种状态切换 + 呼吸/漂浮动效）
│   ├── mascot_assets.c     # 由表情图烘焙出的 LVGL 位图资源（自动生成）
│   ├── config.c            # Echo 服务配置、设备 IP、电量读取、背光/音量/时区
│   ├── skills.c            # 核心本地 Skill 服务的极简 HTTP 客户端
│   ├── qzdesk_core.c       # QZdesk 核心 UDP GUI 协议适配
│   └── qzdesk_core_process.c # 核心进程的拉起与退出
├── pages/
│   ├── desktop.c           # 主页（状态栏 + AI 大卡片 + 设置/应用卡片）
│   ├── assistant.c         # AI 对话页（聊天框 / 全屏表情 双模式）
│   ├── apps.c              # 应用菜单（独立小应用入口列表）
│   ├── skills_page.c       # 技能页（读取核心 Skill 服务，切换主/备/关闭）
│   ├── applets.c           # 四个独立小应用：系统状态 / 定时提醒 / 番茄钟 / 设备控制
│   └── settings.c          # 设置 / WLAN / 通用设置 / 关于
├── assets/mascot/          # 吉祥物表情原图（idle/happy/thinking/confused/speaking）
├── tools/                  # mk_mascot_assets.py：表情图 -> LVGL 资源
├── include/                # 各模块公开接口
├── xiaozhi_core/           # 内置 QZdesk Rust 核心（MIT）
└── CMakeLists.txt
```

## 界面说明

| 页面 | 内容 |
| --- | --- |
| 主页 | 顶部状态栏（时间 / 设备地址 / WiFi / 电量）+ 左侧 AI 助手大卡片 + 右侧天气、设置、应用卡片；两列一直铺到底部留白线 |
| AI 对话页 | 右上角一键切换「聊天框 ⇄ 全屏表情」，切换使用淡入 + 上滑动画 |
| 聊天框模式 | 左右气泡（用户为纯色 accent 胶囊）、打字气泡、胶囊输入栏（毛玻璃 + 麦克风 + 发送） |
| 全屏表情模式 | 左侧 164px 吉祥物 + 右侧状态文字与长按说话按钮，表情随核心状态在 5 种状态间切换 |
| 应用 | 2×3 紧凑圆角卡片：技能 / 系统状态 / 定时提醒 / 番茄钟 / 设备控制，每个都有独立界面 |
| 技能 | 列表显示每个 Skill 的主技能 / 备用 / 关闭状态与说明；点条目弹出切换面板，切换后核心会自动重连让新指令生效；顶部有刷新按钮，底部提示浏览器控制台地址 |
| 系统状态 | 运行时间 / 可用内存 / 电量实时统计（sysinfo），固件、IP、LVGL 内存信息卡 |
| 定时提醒 | 提醒列表 + 滚轮添加弹窗（小时 / 5 分钟步进 / 每日重复开关）；到点弹出黄绿横幅 8 秒，一次性提醒触发后自动移除（会话内有效，不持久化） |
| 番茄钟 | 25 分钟专注 / 5 分钟休息环形倒计时（蓝 / 黄绿区分阶段），开始暂停、重置、完成计数，阶段自动切换 |
| 设备控制 | 声音、背光滑块（写入真实硬件）、按键提示音开关、重新连接核心（真实调用核心状态请求） |
| 设置 | 分组卡片列表：WLAN / 通用设置 / 关于 |
| WLAN | 开关（真实启停 wpa_supplicant）+ 实时扫描列表（信号强度、锁、已连接对勾），点击加密网络弹出密码输入 |
| 通用设置 | 声音、背光滑块（左图标 + 右百分比，实时写入硬件）、时间滚轮选择器、时区列表（写入 TZ） |
| 关于 | 表情卡片 + 设备型号 / 固件版本 / 服务地址 / 当前 IP |

### 吉祥物资源

助手形象是一套烘焙好的位图表情（小狗），原图放在 `assets/mascot/`，按状态命名：

| 文件 | 使用状态 | 说明 |
| --- | --- | --- |
| `idle.png` | 待机 | 微笑抬头 |
| `happy.png` | 开心 | 抱心 |
| `thinking.png` | 思考 | 托腮（叠加矢量思考气泡） |
| `confused.png` | 困惑 | 垂眉 |
| `speaking.png` | 说话 | 张嘴举手 |
| `love.png` | 抱心 | 喜欢 / 被夸（设置页「关于」用它） |

`tools/mk_mascot_assets.py` 负责把原图烘焙成 `app/mascot_assets.c`：已经是透明底的
原图直接使用；不透明的原图会先做 4× 超采样 + 泛洪抠底（带羽化和背景溢色补偿，
避免白毛被一起抠掉、彩色卡片上出现白边），再按内容外框居中、以预乘 alpha 降采样到
`192×192`，输出 LVGL `RGB565A8`（颜色平面 + alpha 平面，约 110KB/张，6 张合计 ~660KB）。

因为各张原图取景不同（有的带全身、有的只到胸口），脚本里有 `FRAMING` 表逐张微调
缩放，保证五个表情的脸在画布里大小一致 —— 表情切换时才不会"忽大忽小"。换新图后
跑一次脚本，用 `--preview` 输出灰/蓝/白三种底色的预览图核对效果。

替换形象或新增表情：

```sh
# 用同名文件替换 assets/mascot/*.png（建议正方形、主体居中、纯色或已抠好的背景），然后：
python3 QZdesk-Demo/tools/mk_mascot_assets.py          # 加 --preview 可输出效果预览
```

换形象时**必须核对**三件事：

1. **眼睛的位置与颜色**。眨眼不是换帧，而是在位图上盖两条深色"眼皮"胶囊，位置与颜色
   取自当前形象 —— 都在 `app/ai_face.c` 顶部的 `QZ_FACE_EYE_SPAN_PCT / _W_PCT / _H_PCT /
   _ROW_PCT / _COLOR`（画布百分比，与烘焙尺寸无关）。新形象眼距不同就会盖到脸颊上，
   照新形象重量一遍即可。
2. **逐张构图**。脚本按内容外框归一化后再套 `FRAMING[state]` 的 `(zoom, dy)`；各张原图
   取景不同时必须微调，否则切表情会"忽大忽小"（用 `--preview` 的三种底色核对）。
3. **状态数**。新增一个表情要同时改四处：`mk_mascot_assets.py` 的 `STATES`、
   `ai_face.h` 的 `qz_face_state_t`、`ai_face.c` 的 `state_image()` 与 `qz_face_state_text()`。

`ai_face.c` 通过 `lv_image` + `LV_IMAGE_ALIGN_CENTER` 显示，按 `size / QZ_MASCOT_SIZE`
做等比缩放，所以 60px（关于卡片）、88px（主页大卡片）、164px（全屏表情）共用同一份资源；
图像缩放走内联采样（`lv_image_set_antialias`），不会申请图层缓冲。切换表情是淡入 + 上移，
另外有呼吸、漂浮、摇摆等位移动效维持"活着"的感觉（都不申请图层）。

体积：`RGB565A8` 每张 192² 约 110KB，五张约 550KB（`app/mascot_assets.c` 那 4MB 是
十六进制展开的文本，编进固件的是 110KB/张）。`assets/mascot/blink.png` 是早期的闭眼帧
（烘焙为 `qz_mascot_blink`），但现在眨眼走矢量眼皮、没有代码引用它 —— 换形象时不必做；
想省 110KB 也可以删掉，同时从脚本的 `STATES` 里去掉。

> 表情集本身怎么设计（每个表情的画法、动效参数、交互优先级与状态映射、新增状态的接法）
> 见 [`吉祥物表情设计.md`](./吉祥物表情设计.md)。

### 材质与层次

界面按 Apple HIG 的浅色系统做，全部落在 `app/theme.c`。层次来自**明度**而不是阴影或颜色：
两块灰加一块白就把画布、板块、玻璃三个层次全部分开。

| 层 | 做法 |
| --- | --- |
| 画布 | 纯色 systemGray5 `#E5E5EA`，没有渐变、没有光晕、没有烘焙底图 |
| 板块 `qz_style_plate()` / `qz_card_button()` | 白 `#FFFFFF`（**透明度可调**，见下），无描边、**无阴影**；周围那圈灰就是它"浮起来"的全部依据 |
| 玻璃 `qz_style_glass()` | 白色材质 + 1px 高光描边（光打在材质边缘）+ 中性软阴影。**只给浮在内容之上的 chrome 用**（工具条胶囊、输入栏、提醒横幅、弹窗），板块不做玻璃叠玻璃 |
| 行 | 行本身透明，靠板块上的发丝线 `#C6C6C8` 分隔；按下时整行铺中性灰 `#E9E9EF`（透明度与材质联动） |
| 内容从玻璃下穿过 | 聊天列表铺满整屏，只用内边距躲开工具条与输入栏，滚动时气泡会从两者**底下**经过；工具条在 `qz_assistant_create()` 里提到最前层。没有这一步，玻璃后面永远只有一片空灰，再透也读不出"流体感" |
| 触摸高光 `qz_add_touch_glint()` | 按下时在触点画一圈柔光（两层同心圆近似径向渐变，因为纯软件渲染没有径向渐变），跟手移动，松开淡出；对象建在 `lv_layer_top()` 里，不受 flex 布局影响也不会被裁切 |
| 弹性回弹 | 按压缩短用强 ease-out（120ms），松开用 `lv_anim_path_overshoot`（220ms）回弹；两者都从屏幕上的当前值起步，快速点按不会先弹回原尺寸；只作用于小控件（缩放在 LVGL 里要申请 ARGB 图层） |

### 材质通透度（可自己调）

白色的透明度是一个**全局可调值**，改完立刻生效，不用重新编译：

```bash
# 界面里调：设置 → 通用设置 → 材质通透度（0–100%，实时）
# 或者启动前用环境变量定初值：
QZDESK_MATERIAL_OPA=85 ./run.sh     # 整机材质
QZDESK_GLASS_OPA=70 ./run.sh        # 只调工具条/输入栏，让它比板块更薄
```

- `100%` = 实心白（参考实现那套扁平观感），`0%` = 完全透明；**默认 90%**。
- 实现：`qz_style_plate()` / `qz_style_glass()` / `qz_card_button()` 把表面打上
  `LV_OBJ_FLAG_USER_1/2` 标记，`qz_material_set_percent()` 只重绘**当前屏**的标记对象，
  其它页面在 `qz_screen_load()` 里补齐 —— 所以调滑块不会重建界面，也不会有残影。
- 这一版 LVGL 没有背景模糊（backdrop blur），所以"流体感"由三样东西共同给出：材质真的半透明、
  高光描边把边缘点亮、内容从材质底下滚过。要真正的磨砂玻璃（对身后像素做模糊再上色）需要像参考
  实现那样自己做一遍 framebuffer 回读 + 盒式模糊，那是下一步的事。

### 设计令牌

配色照搬参考实现 `spi-status-display`（800×480 状态屏）的那一套 Apple HIG 浅色系统，
数值一模一样，`include/theme.h` 是唯一出口，页面不硬编码颜色：

| 令牌 | 色值 | 角色 |
| --- | --- | --- |
| `QZ_BG` | `#E5E5EA` | systemGray5：画布 |
| `QZ_CARD` / `QZ_BAR` | `#FFFFFF` | 分组板块、工具条 |
| `QZ_FILL` / `QZ_FILL_PRESSED` | `#F2F2F7` / `#E9E9EF` | 轨道、凹陷、禁用 / 手指下的中性填充 |
| `QZ_SEPARATOR` | `#C6C6C8` | 行间发丝线 |
| `QZ_TEXT` / `_SECONDARY` / `_TERTIARY` | `#000000` / `#6E6E73` / `#8E8E93` | 主 / 次 / 装饰文字 |
| `QZ_ACCENT` / `_DARK` / `_TEXT` / `_TINT` | `#007AFF` / `#0062CC` / `#0062CC` / `#E4EFFF` | systemBlue：唯一彩色。填充、圆环、大图标用 `#007AFF`；正文大小的蓝色文字降到 `#0062CC` 保证 4.5:1；`#E4EFFF` 是约 10% 的选中/聚焦底 |
| `QZ_GREEN` / `QZ_ORANGE` / `QZ_RED` | `#248A3D` / `#C93400` / `#D70015` | 语义色，取**深色变体**（浅色变体在白底上只有 2:1）。各只有一个含义：绿=良好（已连接、休息中），橙=未就绪，红=失败 |

三条纪律，越界即视为缺陷：

1. **一个 accent。** 除它以外没有第二个品牌色；相邻图标之间的区分靠明度（实心 accent ↔
   `#E4EFFF` 淡底），不靠换色相。
2. **没有渐变。** 主页大卡片、图标块、主按钮、气泡全部是纯色填充；此前用过的渐变全部删掉
   （留着 `bg_grad_dir` 而没有渐变终色会一路渐变到黑，这类残留也要清）。
3. **accent 只出现在可操作或已选中的东西上。** 层级用字体大小与灰阶表达，而不是给大块面积上色
   —— 所以主页大卡片是白板块（黑标题 + 灰副标题 + 一个 accent 折角符），不是一整块蓝。

子页面顶部是悬浮玻璃胶囊工具条，内容从其下方滚过；默认 LVGL 主题的主色也在 `main.c`
里设成了 systemBlue。

### 动效规范

曲线与时长集中在 `include/theme.h`，按「进入/退出用强 ease-out、屏内移动用强
ease-in-out、UI 不用 ease-in、UI 时长不超过 300ms」的规则取值：

| 令牌 | 值 | 用途 |
| --- | --- | --- |
| `QZ_DUR_PRESS` | 120ms | 按压反馈 |
| `QZ_DUR_QUICK` | 140ms | 对等视图切换（聊天/表情，一天几十次，<150ms 且不做位移） |
| `QZ_DUR_SMALL` | 180ms | chip / 触摸高光淡出 |
| `QZ_DUR_PANEL` | 220ms | 卡片入场、按压回弹 |
| `QZ_DUR_MODAL` | 250ms | 弹窗 + 遮罩（同一时钟，读作一体） |
| `QZ_DUR_SCREEN` | 260ms | 页面切换 |
| `QZ_DUR_TOAST` | 240ms | 提示进出 |

- `qz_anim_ease_out()` / `qz_anim_ease_in_out()` 提供 `cubic-bezier(0.23, 1, 0.32, 1)`
  与 `cubic-bezier(0.77, 0, 0.175, 1)`（LVGL 内置曲线偏弱，不用）。
- 按压：小控件（图标按钮、开关、卡片 ≤ 260×160）在按下瞬间缩放 96–97%，同时改底色；
  整行宽卡片只改底色 —— LVGL 给缩放对象分配 ARGB 图层，宽行代价过大。
- 页面切换前进 `MOVE_LEFT`、返回 `MOVE_RIGHT`（进出同路径）；
  弹窗进场「底板下沉 + 遮罩变暗」，退场原路返回后再删除。
- 聊天/表情是对等视图，切换只做 140ms 交叉淡入、**不做位移**：滑动会暗示并不存在的层级，
  而这个动作一天会出现几十次，按频率门槛只配最轻的动效。
- 所有动画都从**当前值**接着做（改值前先 `lv_anim_delete` 掉同属性的旧动画），不回到固定
  起点，避免被打断时跳变。
- `QZDESK_REDUCE_MOTION=1` 时：去掉位移与循环呼吸，保留淡入淡出与颜色反馈
  （页面切换降级为交叉淡入），按压缩放也一并关闭。

字体使用仓库根目录的 `NanoTikBazHei-Bold.ttf`，由 `qz_font_init()` 载入并按需生成
10–26px 多档字号；键盘等纯符号控件改用 Montserrat（`qz_symbol_font()`），因为内置字体
不含 `LV_SYMBOL_*` 字形。界面图标为 `LV_SYMBOL_*` 与少量矢量图元（麦克风、锁、信号条、
info、chevron），只有助手形象使用位图资源。

### 内存与渲染注意事项

- `third_party/lv_conf.h` 的 `LV_MEM_SIZE` 为 2MB：480×320 画布的控件与样式本身远小于
  1MB，但页面滑动切换会为舞台上的两块屏各申请一个全屏缓冲，需要预留这部分空间。
- 大尺寸对象不要使用 `transform_scale` / `transform_rotation` 动画，也不要在按下态
  留缩放假样式：LVGL 会为该对象申请一整块 ARGB 图层（480 宽的全宽卡片约 300KB，
  720 宽时约 1MB），在嵌入式堆上会分配失败并让界面卡死。`theme.c` 的按压反馈只改颜色，
  模式切换动画只做透明度与位移。
- 16 位色深下，吉祥物位图等自带渐变的资源会出现轻微色带，属正常量化现象；界面本身已没有
  渐变，画布与板块都是平色，不会有量化边界。

### 性能

真机上如果滑动发涩，先确认构建类型——LVGL 的软件渲染是逐像素循环，`-O0` 和 `-O2/-O3`
差距是数倍：

```sh
# CMakeLists.txt 现在默认 Release（-O3 -DNDEBUG），只有在显式传
# -DCMAKE_BUILD_TYPE= 时才会退回无优化。旧版本用 run.sh 建的 build/ 目录
# 里 CMAKE_BUILD_TYPE 是空的，请删掉重建：
rm -rf build && ./run.sh
```

其余与流畅度相关的取舍都写在代码注释里，要点：

| 项 | 做法 | 原因 |
| --- | --- | --- |
| 画布 | 纯色填充（systemGray5） | 整屏渐变 + 光晕每帧都要逐像素混合；平色是一次填充，也顺带省掉了一张 300KB 的烘焙位图 |
| 阴影 | 卡片 8px、气泡不画阴影 | 阴影的模糊面积随扩散半径平方增长，是列表里最贵的部分 |
| 阴影缓存 | `LV_DRAW_SW_SHADOW_CACHE_SIZE 32` | 界面里有大量同尺寸的卡片/行，缓存后同一尺寸只算一次（代价 1KB RAM） |
| 刷新周期 | `LV_DEF_REFR_PERIOD 30`（33 FPS） | 动画目标降到 33 FPS，CPU 占用接近减半，小屏观感差异很小 |
| 气泡 | 不透明填充 + 1px 高光边 | 半透明的话滚动时每个气泡都要与背景混合，而"玻璃感"由高光边承担 |

要看真机上的实时数据，把 `third_party/lv_conf.h` 里这三行打开后重编译：

```c
#define LV_USE_SYSMON   1
#define LV_USE_PERF_MONITOR 1
#define LV_USE_PERF_MONITOR_LOG_MODE 1   /* 0 = 屏幕角落浮层，1 = 打印日志 */
```

日志里每 300ms 输出一次 `refr Xms (render Yms | flush Zms)`：`render` 是 LVGL 绘制耗时
（界面复杂度决定），`flush` 是显示驱动写入耗时（fbdev 缓冲/带宽决定）。滑动时看这两个值
就能判断该优化哪一边。

### 技能页与核心的对接

设备端技能页读的是核心自带的那个 Web 控制台同一套接口（`xiaozhi_core/src/skill_web.rs`），
由 `app/skills.c` 里一个手写的极简 HTTP 客户端访问（只有两个请求，不值得引入 curl）：

| 设备端动作 | 请求 |
| --- | --- |
| 打开页面 / 点刷新 | `GET /api/skills` → 列表、`role`、`summary.primary/secondary` |
| 面板里选主技能 / 备用 / 关闭 | `PUT /api/skills/<id>/selection`，body `{"role":"primary"\|"secondary"\|"none"}` |

端口取 `QZDESK_SKILL_WEB_PORT`（其次 `XIAOZHI_SKILL_WEB_PORT`，默认 8080），连通超时 2 秒，
失败时页面显示「本地服务未连接 / 请确认 QZdesk 核心正在运行」。**新增、编辑、删除技能仍在
浏览器里做**——480×320 上放不下 Markdown 编辑器，页面底部那行就是控制台地址。

### 硬件接入点

界面里的开关和滑块都落到真机接口，没有示例数据：

| 功能 | 实现 | 位置 |
| --- | --- | --- |
| WLAN 扫描 / 连接 | `wpa_cli scan_results / status / add_network…select_network`，开启开关时按 SDK 流程 `ifconfig <iface> up` → `wpa_supplicant -B -c <conf> -i <iface>`，连上后 `udhcpc -i <iface>` 取地址 | `app/wifi.c` |
| 背光 | `/sys/class/backlight/backlight/brightness`（按同级 `max_brightness` 换算百分比；写 0 会保持最低一档，避免全黑） | `app/config.c` |
| 音量 | `amixer -c 0 cset name='DAC LINEOUT Volume' <0..30>` | `app/config.c` |
| 时区 | POSIX TZ 字符串（如 `CST-8`）：进程内 `setenv/tzset` 立即生效，并写入 `/etc/profile` 的 `export TZ=` 行 | `app/config.c` |
| 存在检测 | `/dev/video*` 取 YUV 帧（`GREY` / `NV12` / `YUYV` 任选其一），下采样成 32×24 亮度网格做帧差；有人靠近就把背光拉到设置值并向核心报一次（核心让助手打招呼，5 分钟冷却）。没有摄像头时可用 `QZDESK_PRESENCE_FAKE=1` 造事件 | `app/face_camera.c` |

`wpa_supplicant.conf` 缺 `ctrl_interface / ap_scan / update_config` 时会自动补上（只读
rootfs 时跳过），否则 `wpa_cli` 无法连接。无线网卡、配置文件和背光节点都可用环境变量
覆盖，便于换板或本地验证：`QZDESK_WIFI_IFACE`、`QZDESK_WPA_CONF`、
`QZDESK_BACKLIGHT`、`QZDESK_TZ_PROFILE`。没有无线网卡（例如模拟器）时列表显示
"设备未提供无线网卡"，滑块保持可拖动但不会写入硬件。

## 构建

界面在 PC 上用 LVGL SDL 模拟器跑（`480x320` 窗口，鼠标模拟触摸）。一键脚本会自动配置、编译 QZdesk 和 Rust 核心，并启动界面：

```sh
./run.sh                      # 480×320（默认）
QZDESK_PANEL=320x240 ./run.sh # 同比例的小屏
```

真机镜像（界面与核心一起打进 `update.img`）由 Rockchip SDK 的 `./build_qzdesk.sh` 出，见仓库根 README。

面板时序在 SDK 的设备树里设置（例如 `SDK/sysdrv/source/kernel/arch/arm/boot/dts/` 下的
`*-86panel-ipc.dtsi`，其中 `hactive/vactive` 目前是 720×720）。换屏只改那一处 panel
节点：界面在启动时从 `/dev/fb0` 读实际尺寸（`include/scale.h` 的
`qz_scale_init_from_system()`），自动缩放到新面板，**一行代码都不用改、也不用重编**。

```sh
QZDESK_PANEL=320x240 ./run.sh          # 模拟器上直接看 320×240，同一份二进制
```

布局一律按 **480×320 的设计稿**写，`include/scale.h` 在 LVGL 调用边界上把这些
像素值缩到当前面板（页面里没有一处面板尺寸，所以不会出现「面板尺寸 - 设计常量」
的混合算式）。**横向纵向各按各自的比例**：x 坐标 / 宽度 / 左右内边距走宽比，
y 坐标 / 高度 / 上下内边距走高比，整屏铺满、不留白边；只有一个数管两个方向的量
（字号、圆角、线宽、图片 zoom）取较小的那个比例。480×320 下两个比例都是 1，
这层是恒等变换。

320×240 是 4:3、比设计稿的 3:2 略高，于是宽比 2/3、高比 3/4：矩形与圆形会比
"等比"时略高 12.5%，字形不受影响（字号是单值），吉祥物与图标这类方形素材仍按较小
比例缩放、不会被拉伸。字号有 9px 下限（`-DQZ_MIN_FONT_PX` 可调），免得缩到读不清。

LVGL 9.2.3 的源码与配置随仓库提供，放在 `third_party/`（`lvgl/`、`lv_conf.h`、`conf/dev_conf.h`）。
三者必须保持同级：`lv_conf.h` 里有 `#include "conf/dev_conf.h"`。

## QZdesk AI Core

`xiaozhi_core/` 是 QZdesk 的 Rust 核心源码，包含实时音频、云端 WebSocket、设备激活和 MCP 功能。构建产物与 LVGL 界面是两个进程，通过本机 UDP 通信：核心监听 `5678`，QZdesk 监听 `5679`。

核心不单独手工构建：模拟器下由 `./run.sh` 一并编出，真机则由 SDK 的 `./build_qzdesk.sh` 交叉编译后跟界面一起打进镜像。

将生成的 `xiaozhi_linux_rs` 与其运行时 `xiaozhi_config.json` 部署到设备后，先启动核心，再启动 `qzdesk_screen`。助手页面会主动请求连接状态；TTS 文本、激活码和核心状态会直接显示到聊天界面，同时驱动全屏表情的状态切换。

QZdesk 也支持自动启动核心：将 `xiaozhi_linux_rs` 放在 `qzdesk_screen` 同一目录后，启动界面即可自动发起 OTA 激活检查。首次运行时，进入 AI 助手页面，在手机端打开 `xiaozhi.me` 并输入聊天区域显示的六位激活码。若希望由系统服务单独管理核心，可设置 `QZDESK_CORE_AUTOSTART=0`；核心路径也可通过 `QZDESK_CORE_BIN=/path/to/xiaozhi_linux_rs` 覆盖。

一键脚本会在启动前关闭占用 TCP `8080` 的旧服务，然后由 QZdesk Skill Manager 接管该端口；这不会影响 OTA 激活或 GUI UDP 通信。设置 `QZDESK_REPLACE_PORT_8080=0` 可禁用自动关闭，设置 `QZDESK_SKILL_WEB_PORT` 可更换端口。

端口和本机核心地址可以在启动 QZdesk 时覆盖：`QZDESK_CORE_HOST`、`QZDESK_CORE_PORT`、`QZDESK_CORE_GUI_PORT`，默认值分别为 `127.0.0.1`、`5678`、`5679`。设置 `QZDESK_REDUCE_MOTION=1` 可关闭入场与切换动效。
