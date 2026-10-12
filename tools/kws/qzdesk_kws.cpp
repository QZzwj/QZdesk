// QZdesk 本地唤醒词进程（离线 KWS）。
//
// 为什么是独立进程（而不是塞进核心）：
//   * 核心是静态 musl 二进制，**不能 dlopen**；而 Rockchip 的这套音频库是 uclibc 动态库
//     （libaec_bf_process*.so → librknnmrt.so / librkaudio_common.so / libstdc++）。
//   * 推理跑在 NPU 上，放在独立进程里也不会拖慢核心的 ALSA 硬实时采集。
//   上游 README 也是这个建议："本地唤醒与 AFE …… 更适合由 BSP、独立音频前端进程提供"。
//
// 它做的事：收 PCM 帧 → 喂给 Rockchip 的 VQE（BF 链路 + EN_Wakeup）→ 命中唤醒词就
// 往核心的 UDP 口发一条 {"type":"listen","state":"start"}，核心随即进入 Listening，
// 后面就是它原本的语音对话链路（这条报文本来就是留给外部调用者的）。
//
// 三种用法：
//   qzdesk-kws                常驻：从 Unix socket 收核心旁路的 PCM
//   qzdesk-kws --wav a.wav    离线自测：把一个 wav 喂进去，打印命中（板子上也能跑）
//   qzdesk-kws --selftest     只验证库/模型能不能加载、能不能跑（不读音频）
//
// ── 引擎（库）用 dlopen 加载，而不是链接 ──────────────────────────────────
// 三个理由：
//   1. 能把 mode1 / mode2 两套模型都在镜像里带上，用环境变量切换，不用重编；
//   2. 打包时 media 的资源会覆盖 lib/ 下的同名 .so（实测确认），dlopen 指定完整路径
//      就与"谁覆盖谁"无关了；
//   3. 库缺失时给出明确的日志，而不是启动就因动态链接失败而挂掉。
// 参数构造器（rkaudio_*_param_init）全是 header 里的 inline static，不需要 dlsym。
//
// ── 唤醒词是什么，能不能换 ────────────────────────────────────────────────
// 唤醒词是**模型里定死的**，SDK 只带了两套（都是 Rockchip 官方 xiaohexiaohe 示例）：
//   mode2（默认）：唤醒词 "小和小和"，外加 19 条命令词（打电话给爸爸…）
//   mode1：8 条一次性命令词（我要拍照 / 打开关闭热点 / 录音 / 屏幕），没有独立唤醒词
// 想换成别的词（例如"你好小智"）必须**用 Rockchip 的工具重训模型**，换掉库与 .rknn
// 即可，本进程不用改。
//
// ── AEC（边放音边唤醒）────────────────────────────────────────────────────
// 库支持回声消除，但**必须有参考信号**（设备正在放的声音）。本板目前没有这条线：
//   * RV1106 acodec 的采集是 2 声道，第 1 声道是不是"播放回采"未经确认（要在板子上测）；
//   * 内核没编 snd-aloop，所以也做不了"软件回采"（要用就得改 defconfig 重编内核）。
// 这一侧的通路已经做好：只要给得出参考通道，设 QZDESK_KWS_REF_CHAN=1 就会把
// 交错帧原样喂给算法并把 AEC 主开关打开（延迟用 QZDESK_KWS_AEC_DELAY 调）。
//
// 环境变量：
//   QZDESK_KWS_MODE        mode1 | mode2（默认 mode2）
//   QZDESK_KWS_LIB         直接指定库路径（优先级高于 MODE）
//   QZDESK_KWS_MODEL       唤醒模型路径（默认按 MODE 选）
//   QZDESK_KWS_SOCKET      PCM 输入 socket     默认 /var/run/qzdesk-kws.sock
//   QZDESK_KWS_CORE_PORT   核心 UDP 口         默认 5678
//   QZDESK_KWS_RATE        采样率              默认 24000（跟随核心采集）
//   QZDESK_KWS_SRC_CHAN    麦克风声声道数      默认 1
//   QZDESK_KWS_REF_CHAN    参考声道数          默认 0（>0 才开 AEC）
//   QZDESK_KWS_INPUT_CHANNELS  核心旁路来的声道数 默认 2
//   QZDESK_KWS_FRAME       每帧每声道采样数    默认 256
//   QZDESK_KWS_AEC_DELAY / _AEC_LOOKAHEAD  AEC 的延迟补偿（要上板实测）
//   QZDESK_KWS_COOLDOWN_MS 命中后的静默期      默认 2000
//   QZDESK_KWS_FAKE_WAKE=1 不跑推理，每 3 秒假装命中一次（只验"唤醒→核心"通路）

#include <arpa/inet.h>
#include <dlfcn.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "rkaudio_preprocess.h"

namespace {

const char *kDefaultSocket = "/var/run/qzdesk-kws.sock";
/* 两套模型各自对应的库与模型（由 Makefile 装到 lib/qzdesk/，名字带 mode，互不覆盖） */
const char *kLibMode1 = "/oem/usr/lib/qzdesk/libaec_bf_process_mode1.so";
const char *kLibMode2 = "/oem/usr/lib/qzdesk/libaec_bf_process_mode2.so";
const char *kModelMode1 = "/oem/usr/share/vqefiles/rkaudio_model_mode1_word1.rknn";
const char *kModelMode2 = "/oem/usr/share/vqefiles/rkaudio_model_mode2_word1.rknn";

/* 词表来自 Rockchip 官方示例 media/rockit/.../test_mpi_ai.cpp:1141（同一套模型） */
const char *kWordsMode1[] = {
    "No wake", "我要拍照", "关闭热点", "打开热点", "关闭录音", "打开录音", "关闭屏幕", "打开屏幕",
};
const char *kWordsMode2[] = {"No wake", "小和小和"};

const char *word_name(bool mode1, int status)
{
    const char **table = mode1 ? kWordsMode1 : kWordsMode2;
    size_t count = mode1 ? sizeof(kWordsMode1) / sizeof(kWordsMode1[0])
                         : sizeof(kWordsMode2) / sizeof(kWordsMode2[0]);
    if (status >= 0 && (size_t)status < count) return table[status];
    return "unknown";
}

int env_int(const char *name, int fallback)
{
    const char *value = getenv(name);
    if (!value || !*value) return fallback;
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value) return fallback;
    return (int)parsed;
}

const char *env_str(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return (value && *value) ? value : fallback;
}

long long now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ 唤醒 → 核心 */

class Waker {
public:
    Waker(int port, int cooldown_ms) : cooldown_ms_(cooldown_ms), last_fire_ms_(0)
    {
        fd_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return;
        memset(&core_, 0, sizeof(core_));
        core_.sin_family = AF_INET;
        core_.sin_port = htons((uint16_t)port);
        core_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ok_ = true;
    }

    ~Waker()
    {
        if (fd_ >= 0) close(fd_);
    }

    bool ok() const { return ok_; }

    /* 命中唤醒词：告诉核心"用户可以说话了"。冷却期内不重复发。 */
    void fire(int status, bool mode1)
    {
        long long now = now_ms();
        if (now - last_fire_ms_ < cooldown_ms_) return;
        last_fire_ms_ = now;

        const char *message = "{\"type\":\"listen\",\"state\":\"start\"}";
        ssize_t sent = sendto(fd_, message, strlen(message), 0, (struct sockaddr *)&core_, sizeof(core_));
        fprintf(stderr, "qzdesk-kws: 唤醒命中 [%s] -> %s\n", word_name(mode1, status),
                sent > 0 ? "已通知核心" : "通知核心失败");
        fflush(stderr);
    }

private:
    int fd_ = -1;
    bool ok_ = false;
    int cooldown_ms_;
    long long last_fire_ms_;
    struct sockaddr_in core_;
};

/* ------------------------------------------------------------------ KWS 本体 */

/* dlopen 出来的几个函数指针（其余参数构造器都是 header 里的 inline static） */
struct RkAudioApi {
    void *handle = NULL;
    void *(*preprocess_init)(int, int, int, int, RKAUDIOParam *) = NULL;
    int (*preprocess_short)(void *, short *, short *, int, int *) = NULL;
    void (*preprocess_destory)(void *) = NULL;
    int (*rknn_path_set)(char *, char *, char *) = NULL;
    int (*preprocess_get_cmd_id)(void *, float *, int *) = NULL;

    bool load(const char *path)
    {
        handle = dlopen(path, RTLD_NOW);
        if (!handle) {
            fprintf(stderr, "qzdesk-kws: dlopen %s 失败: %s\n", path, dlerror());
            return false;
        }
        preprocess_init = (void *(*)(int, int, int, int, RKAUDIOParam *))dlsym(handle, "rkaudio_preprocess_init");
        preprocess_short = (int (*)(void *, short *, short *, int, int *))dlsym(handle, "rkaudio_preprocess_short");
        preprocess_destory = (void (*)(void *))dlsym(handle, "rkaudio_preprocess_destory");
        rknn_path_set = (int (*)(char *, char *, char *))dlsym(handle, "rkaudio_rknn_path_set");
        preprocess_get_cmd_id = (int (*)(void *, float *, int *))dlsym(handle, "rkaudio_preprocess_get_cmd_id");
        if (!preprocess_init || !preprocess_short || !preprocess_destory) {
            fprintf(stderr, "qzdesk-kws: %s 里找不到 preprocess 接口（不是这套库？）\n", path);
            return false;
        }
        return true;
    }

    ~RkAudioApi()
    {
        if (handle) dlclose(handle);
    }
};

class KeywordSpotter {
public:
    KeywordSpotter(int rate, int src_chan, int ref_chan, int frame, const char *lib_path,
                   const char *model, bool mode1, int aec_delay, int aec_lookahead)
        : rate_(rate), src_chan_(src_chan), ref_chan_(ref_chan), frame_(frame), mode1_(mode1)
    {
        if (!api_.load(lib_path)) return;

        /* 参数结构必须与 state 同寿命：init 只是把里面的指针交给内部状态，
         * 提前 rkaudio_param_deinit 会让后面的调用踩空（实测直接 SIGSEGV）。
         * 做成成员还有一个好处：地址稳定，库若持有 &param 也不会悬空。 */
        memset(&param_, 0, sizeof(param_));
        /* 主开关：走 BF（预处理）链路；唤醒开关在 bf_param 的 model_bf_en 里 */
        param_.model_en = RKAUDIO_EN_BF;
        if (ref_chan > 0) {
            /* 有参考声道才开 AEC（库按 src/ref 声道数从同一帧里取数据） */
            param_.model_en |= RKAUDIO_EN_AEC;
        }
        param_.bf_param = rkaudio_preprocess_param_init();
        if (param_.bf_param) {
            /* header 里注释掉的示例行写得很清楚：EN_Wakeup 就是唤醒模式 */
            static_cast<SKVPreprocessParam *>(param_.bf_param)->model_bf_en = EN_Wakeup;
        }
        param_.aec_param = rkaudio_aec_param_init();   /* 不能给 NULL：会踩空 */
        if (param_.aec_param) {
            SKVAECParameter *aec = static_cast<SKVAECParameter *>(param_.aec_param);
            aec->model_aec_en = ref_chan > 0 ? EN_DELAY : 0;
            /* 播放到回采之间的延迟，单位是采样点；要上板实测（官方 json 里是 0，
             * 旧 json 里用过 -3588）。给错只会让 AEC 效果差，不会崩。 */
            aec->delay_len = aec_delay;
            aec->look_ahead = aec_lookahead;
            aec->drop_ref_channel = 0;
        }
        param_.rx_param = NULL;
        param_.read_size = frame;

        /* 模型路径必须在 init **之前**设：库里就是个全局变量（导出符号
         * global_kws_rknn_path 即它），init 内部才按路径加载模型。放到 init 之后会报
         *   invalid rknn_executor_init_from_model_path params, model_path: (null)
         * —— qemu 实测踩到过。 */
        if (model && *model && api_.rknn_path_set) {
            if (api_.rknn_path_set(NULL, const_cast<char *>(model), NULL) != 0) {
                fprintf(stderr, "qzdesk-kws: 指定唤醒模型失败（%s），改用库内默认\n", model);
            } else {
                fprintf(stderr, "qzdesk-kws: 唤醒模型 %s\n", model);
            }
        }
        int ref = ref_chan;
        state_ = api_.preprocess_init(rate, 16, src_chan, ref, &param_);
        if (!state_) {
            fprintf(stderr, "qzdesk-kws: rkaudio_preprocess_init 失败（rate=%d src=%d ref=%d frame=%d）\n",
                    rate, src_chan, ref, frame);
            return;
        }
        /* 一帧的采样数 = frame × 声道数（demo 里 read_size 也是这么算的） */
        frame_samples_ = (size_t)frame_ * (size_t)(src_chan + (ref_chan > 0 ? ref_chan : 0));
        out_.assign((size_t)frame_ * 8, 0);
        ready_ = true;
    }

    ~KeywordSpotter()
    {
        if (state_) api_.preprocess_destory(state_);
        rkaudio_param_deinit(&param_);
    }

    bool ready() const { return ready_; }
    size_t frame_samples() const { return frame_samples_; }
    int rate() const { return rate_; }
    int src_chan() const { return src_chan_; }
    int ref_chan() const { return ref_chan_; }
    int input_channels_total() const { return src_chan_ + (ref_chan_ > 0 ? ref_chan_ : 0); }

    /* 喂一帧（交错 PCM，长度必须是 frame_samples()）。返回唤醒状态：0 = 没命中。 */
    int process(const short *samples)
    {
        if (!state_) return 0;
        int wakeup_status = 0;
        /* in_size 传采样数（同族 API rkaudio_sed_process 也是采样数：demo 里传的 in_size/2） */
        int result = api_.preprocess_short(state_, const_cast<short *>(samples), out_.data(),
                                           (int)frame_samples_, &wakeup_status);
        if (result != 0) {
            fprintf(stderr, "qzdesk-kws: preprocess 返回 %d\n", result);
            return 0;
        }
        if (wakeup_status > 0 && api_.preprocess_get_cmd_id) {
            float score = 0.0f;
            int cmd_id = 0;
            if (api_.preprocess_get_cmd_id(state_, &score, &cmd_id) == 0) {
                fprintf(stderr, "qzdesk-kws: cmd=%d score=%.2f\n", cmd_id, score);
            }
        }
        return wakeup_status;
    }

    bool mode1() const { return mode1_; }

private:
    RkAudioApi api_;
    RKAUDIOParam param_;
    void *state_ = NULL;
    int rate_ = 24000;
    int src_chan_ = 1;
    int ref_chan_ = 0;
    int frame_ = 256;
    size_t frame_samples_ = 0;
    bool ready_ = false;
    bool mode1_ = false;
    std::vector<short> out_;
};

/* ------------------------------------------------------------------ 重采样 */

/* 输入率 → 算法率（24k → 16k）的线性插值重采样，交错多声道各声道独立。
 *
 * 为什么必须有这一步：qemu 实测该库直接报
 *   "sample_rate should be 16000, nfft size should be 256"
 * ——它只吃 16k。而核心的采集是 24k（跟云端 hello 的采样率一致，不能改），
 * 所以只能在这一侧转换。KWS 只看特征，线性插值够用，且不用引第三方库。 */
class LinearResampler {
public:
    LinearResampler(int in_rate, int out_rate, int channels)
        : step_((double)in_rate / (double)out_rate), channels_(channels)
    {
    }

    bool needed() const { return step_ > 1.001 || step_ < 0.999; }

    /* 喂若干个输入帧（交错，长度是 channels 的整数倍），产出连续的交错输出。 */
    void feed(const std::vector<short> &input, std::vector<short> &output)
    {
        const size_t frames = input.size() / (size_t)channels_;
        for (size_t n = 0; n < frames; n++) {
            const short *frame = input.data() + n * (size_t)channels_;
            if (previous_.empty()) {
                previous_.assign(frame, frame + channels_);
                consumed_ = 0;
                continue;
            }
            consumed_++;
            while (next_pos_ <= (double)consumed_) {
                double frac = next_pos_ - (double)(consumed_ - 1);
                for (int ch = 0; ch < channels_; ch++) {
                    double a = previous_[ch];
                    double b = frame[ch];
                    output.push_back((short)(a + (b - a) * frac));
                }
                next_pos_ += step_;
            }
            memcpy(previous_.data(), frame, (size_t)channels_ * sizeof(short));
        }
    }

private:
    double step_;
    int channels_;
    std::vector<short> previous_;
    long long consumed_ = -1;
    double next_pos_ = 0.0;
};

/* ------------------------------------------------------------------ 输入源 */

/* 读一个 16bit PCM 的 WAV（只支持最常见的 RIFF/fmt/data 顺序）。 */
bool load_wav(const char *path, std::vector<short> &pcm, int &rate, int &channels)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "qzdesk-kws: 打不开 %s: %s\n", path, strerror(errno));
        return false;
    }
    unsigned char header[12];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "RIFF", 4) != 0) {
        fprintf(stderr, "qzdesk-kws: %s 不是 WAV\n", path);
        fclose(file);
        return false;
    }
    rate = 24000;
    channels = 1;
    int bits = 16;
    pcm.clear();
    bool got_fmt = false;
    while (true) {
        unsigned char chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) break;
        unsigned int size = (unsigned int)chunk[4] | ((unsigned int)chunk[5] << 8) |
                            ((unsigned int)chunk[6] << 16) | ((unsigned int)chunk[7] << 24);
        if (memcmp(chunk, "fmt ", 4) == 0) {
            std::vector<unsigned char> fmt(size);
            if (fread(fmt.data(), 1, size, file) != size) break;
            if (size >= 16) {
                channels = fmt[2] | (fmt[3] << 8);
                rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((unsigned int)fmt[7] << 24);
                bits = fmt[14] | (fmt[15] << 8);
            }
            got_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            std::vector<unsigned char> raw(size);
            size_t read = fread(raw.data(), 1, size, file);
            pcm.resize(read / 2);
            memcpy(pcm.data(), raw.data(), (read / 2) * 2);
            break;
        } else {
            fseek(file, (long)size + (long)(size & 1), SEEK_CUR);
        }
    }
    fclose(file);
    if (!got_fmt || pcm.empty()) {
        fprintf(stderr, "qzdesk-kws: %s 解析不出 PCM\n", path);
        return false;
    }
    if (bits != 16) {
        fprintf(stderr, "qzdesk-kws: 只支持 16bit PCM，这个文件是 %d bit\n", bits);
        return false;
    }
    return true;
}

int run_wav(const char *path, KeywordSpotter &kws, Waker &waker)
{
    std::vector<short> pcm;
    int rate = 24000, channels = 1;
    if (!load_wav(path, pcm, rate, channels)) return 1;
    fprintf(stderr, "qzdesk-kws: %s  %d Hz  %d ch  %zu 采样（%.1f 秒）\n", path, rate, channels,
            pcm.size(), (double)pcm.size() / (double)(rate * channels));

    const size_t frame_samples = kws.frame_samples();
    if ((size_t)channels != (size_t)kws.input_channels_total()) {
        fprintf(stderr, "qzdesk-kws: 提醒：文件是 %d 声道，算法要 %d 声道"
                        "（QZDESK_KWS_SRC_CHAN/REF_CHAN 调一下）\n",
                channels, kws.input_channels_total());
    }
    /* 文件采样率与算法要求不一致时先转换（SDK 的样例都是 16k，一般用不上） */
    LinearResampler resampler(rate, kws.rate(), channels);
    if (resampler.needed()) {
        std::vector<short> converted;
        resampler.feed(pcm, converted);
        pcm.swap(converted);
        fprintf(stderr, "qzdesk-kws: 重采样 %d → %d Hz（%zu 采样）\n", rate, kws.rate(), pcm.size());
    }
    int hits = 0;
    size_t frames = 0;
    for (size_t offset = 0; offset + frame_samples <= pcm.size(); offset += frame_samples) {
        frames++;
        int status = kws.process(pcm.data() + offset);
        if (status > 0) {
            hits++;
            fprintf(stderr, "qzdesk-kws: [%.2f 秒] 命中 %s\n",
                    (double)offset / (double)(rate * channels), word_name(kws.mode1(), status));
            waker.fire(status, kws.mode1());
        }
    }
    fprintf(stderr, "qzdesk-kws: 处理完 %zu 帧，命中 %d 次\n", frames, hits);
    return hits > 0 ? 0 : 3;   /* 退出码 3 = 跑通了但没命中，方便脚本判断 */
}

int run_live(KeywordSpotter &kws, Waker &waker)
{
    const char *path = env_str("QZDESK_KWS_SOCKET", kDefaultSocket);
    /* 核心旁路来的声道数：核心采集是交错多声道，这里按需取 */
    const int in_channels = env_int("QZDESK_KWS_INPUT_CHANNELS", 2);
    int fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        fprintf(stderr, "qzdesk-kws: 建 socket 失败: %s\n", strerror(errno));
        return 1;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    unlink(path);   /* 上次异常退出留下的节点 */
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        fprintf(stderr, "qzdesk-kws: bind %s 失败: %s\n", path, strerror(errno));
        close(fd);
        return 1;
    }
    chmod(path, 0666);   /* 核心可能以别的身份跑，别让权限挡住 */
    fprintf(stderr, "qzdesk-kws: 就绪，等核心的 PCM（%s，输入 %d 声道）\n", path, in_channels);
    fflush(stderr);

    const bool fake_wake = getenv("QZDESK_KWS_FAKE_WAKE") != NULL;
    if (fake_wake) {
        fprintf(stderr, "qzdesk-kws: QZDESK_KWS_FAKE_WAKE=1 —— 不跑推理，每 3 秒假装命中一次\n");
    }

    /* 核心按 ALSA period 投帧，大小不固定，所以这里累积到"每帧采样数"再处理 */
    const size_t frame_samples = kws.frame_samples();
    /* 算法要几声道就给几声道：核心旁路是 2 声道交错，而 mode2 的 BF 正好要 2 麦，
     * 所以正常情况下原样透传；只有对不上时才退化成"只取第 0 声道"。 */
    const bool keep_interleaved = (kws.input_channels_total() == in_channels);
    const int input_rate = env_int("QZDESK_KWS_INPUT_RATE", 24000);
    LinearResampler resampler(input_rate, kws.rate(), in_channels);
    if (kws.input_channels_total() != in_channels) {
        fprintf(stderr, "qzdesk-kws: 提醒：算法要 %d 声道、核心给 %d 声道，只取第 0 声道"
                        "（用 QZDESK_KWS_SRC_CHAN 调）\n", kws.input_channels_total(), in_channels);
    }
    if (resampler.needed()) {
        fprintf(stderr, "qzdesk-kws: 重采样 %d → %d Hz\n", input_rate, kws.rate());
    }
    std::vector<short> datagram(8192);
    std::vector<short> block;
    std::vector<short> converted;
    std::vector<short> mono;
    mono.reserve(frame_samples * 4);
    long long last_fake_ms = 0;
    bool logged = false;

    while (true) {
        ssize_t got = recv(fd, datagram.data(), datagram.size() * sizeof(short), 0);
        if (got > 0) {
            size_t samples = (size_t)got / sizeof(short);
            samples -= samples % (size_t)in_channels;
            block.assign(datagram.begin(), datagram.begin() + (long)samples);
            converted.clear();
            if (resampler.needed()) {
                resampler.feed(block, converted);
            } else {
                converted.swap(block);
            }
            if (keep_interleaved) {
                mono.insert(mono.end(), converted.begin(), converted.end());
            } else {
                for (size_t i = 0; i < converted.size(); i += (size_t)in_channels) {
                    mono.push_back(converted[i]);   /* 只要第 0 声道 */
                }
            }
            if (!logged) {
                fprintf(stderr, "qzdesk-kws: 收到 %zd 字节/包（输入 %d 声道%s → 每帧 %zu 采样）\n",
                        got, in_channels, keep_interleaved ? "，原样透传" : "，取第 0 声道",
                        frame_samples);
                fflush(stderr);
                logged = true;
            }
            size_t offset = 0;
            while (mono.size() - offset >= frame_samples) {
                int status = kws.process(mono.data() + offset);
                if (status > 0) waker.fire(status, kws.mode1());
                offset += frame_samples;
            }
            if (offset > 0) mono.erase(mono.begin(), mono.begin() + (long)offset);
        } else if (got < 0 && errno != EINTR && errno != EAGAIN) {
            fprintf(stderr, "qzdesk-kws: recv 出错: %s\n", strerror(errno));
            usleep(100 * 1000);
        }

        if (fake_wake && now_ms() - last_fake_ms >= 3000) {
            last_fake_ms = now_ms();
            waker.fire(1, kws.mode1());
        }
    }
    return 0;
}

}  // namespace

int main(int argc, char **argv)
{
    /* 默认值就是 qemu 实测出来的"库能接受的组合"：16k / 2 麦 / 256 点。
     * （库会打印 "sample_rate should be 16000" 与 "only used for 2 microphones"） */
    const int rate = env_int("QZDESK_KWS_RATE", 16000);
    const int src_chan = env_int("QZDESK_KWS_SRC_CHAN", 2);
    const int ref_chan = env_int("QZDESK_KWS_REF_CHAN", 0);
    const int frame = env_int("QZDESK_KWS_FRAME", 256);
    const int cooldown = env_int("QZDESK_KWS_COOLDOWN_MS", 2000);
    const int core_port = env_int("QZDESK_KWS_CORE_PORT", 5678);
    const int aec_delay = env_int("QZDESK_KWS_AEC_DELAY", 0);
    const int aec_lookahead = env_int("QZDESK_KWS_AEC_LOOKAHEAD", 0);

    const char *mode = env_str("QZDESK_KWS_MODE", "mode2");
    bool mode1 = strcmp(mode, "mode1") == 0;
    const char *lib_path = env_str("QZDESK_KWS_LIB", mode1 ? kLibMode1 : kLibMode2);
    const char *model = env_str("QZDESK_KWS_MODEL", mode1 ? kModelMode1 : kModelMode2);

    const char *wav = NULL;
    bool selftest = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            wav = argv[++i];
        } else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode1 = strcmp(argv[++i], "mode1") == 0;
        } else if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
            lib_path = argv[++i];
        } else if (strcmp(argv[i], "--selftest") == 0) {
            selftest = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("用法: %s [--wav 文件.wav | --selftest] [--mode mode1|mode2] [--lib 库路径]\n", argv[0]);
            return 0;
        }
    }

    fprintf(stderr, "qzdesk-kws: mode=%s rate=%d src=%d ref=%d frame=%d cooldown=%dms\n",
            mode1 ? "mode1" : "mode2", rate, src_chan, ref_chan, frame, cooldown);
    fprintf(stderr, "qzdesk-kws: 库=%s\n", lib_path);
    if (ref_chan > 0) {
        fprintf(stderr, "qzdesk-kws: AEC 已请求（ref=%d delay=%d look_ahead=%d）\n",
                ref_chan, aec_delay, aec_lookahead);
    }

    Waker waker(core_port, cooldown);
    if (!waker.ok()) {
        fprintf(stderr, "qzdesk-kws: 建核心 UDP 通道失败: %s\n", strerror(errno));
        return 1;
    }

    KeywordSpotter kws(rate, src_chan, ref_chan, frame, lib_path, model, mode1, aec_delay, aec_lookahead);
    if (selftest) {
        return kws.ready() ? 0 : 2;
    }
    if (!kws.ready()) {
        /* 初始化失败不要紧：核心的按键说话路径不受影响，这里直接退出让日志说话 */
        return 2;
    }

    if (wav) return run_wav(wav, kws, waker);
    return run_live(kws, waker);
}
