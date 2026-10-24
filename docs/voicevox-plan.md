# VOICEVOX 语音方案：实现设计 + 软件内配置设计

> 决策：AI 对话语音采用 **VOICEVOX ENGINE（本地 HTTP，CPU 推理）** 作为第一个后端。
> 目标：离线可用的二次元日语人声；音色/语气可配置；后续可由 AI 按人设调参；不阻塞 UI、不影响渲染帧率。

---

## 1. 范围与目标

**做**
- 助手回复 → VOICEVOX 合成 → 播放，同时驱动 Live2D 口型、字幕气泡保持原样
- 可插拔后端（先只实现 `voicevox`，结构上给 pip/kokoro/edge 留位）
- 设置页新增「语音合成」卡片；配置全部落 SQLite `config` 表
- 情绪/语气可被 LLM 打标，映射到「角色风格 + 语速/音高/抑扬」；预设存表，AI 可改
- 引擎随程序自启、健康检查、崩溃重启；未就绪时静默降级

**不做（本期）**
- 声音克隆 / 自定义声库训练
- 多角色同时说话、合唱
- 语音输入（STT）——后续单独做

---

## 2. 总体架构

```
AIClient / MessageHandler
        │  回复文本 + 可选情绪标签
        ▼
┌──────────────────────────────────────────────┐
│ VoiceService（门面，单例）                     │
│  · Queue（最新优先，可打断）                   │
│  · SentenceSplitter（按 。！？… 切句）          │
│  · Cache（hash → wav，LRU）                    │
│  · VoicePreset（情绪 → 参数）                  │
└───────┬───────────────────────┬──────────────┘
        │ ITtsBackend            │ AudioClip
        ▼                        ▼
┌──────────────────┐    ┌─────────────────────┐
│ VoicevoxBackend  │    │ AudioPlayer         │
│  · 进程管理       │    │  · WASAPI 共享模式   │
│  · HTTP (cpr)    │    │  · 环形缓冲          │
│  · speaker 缓存   │    │  · RMS 包络 → 口型   │
└────────┬─────────┘    └──────────┬──────────┘
         │ HTTP 127.0.0.1:50021     │ SetMouthOpen(0..1)
         ▼                          ▼
   voicevox_engine            Live2DManager::SetMouthOpen
   （子进程，Job 绑定）          （写 ParamMouthOpenY）
```

线程模型：

| 线程 | 职责 | 注意事项 |
|---|---|---|
| UI 线程 | 只投递任务，绝不阻塞 | 不做 HTTP、不做文件 IO |
| VoiceWorker（1 个） | 切句 → 查缓存 → 合成 → 入播放队列 | 串行调用引擎（引擎本身串行） |
| AudioThread（1 个） | WASAPI 事件驱动，喂缓冲、算 RMS | 上抛 level 给渲染线程（原子 float） |
| 引擎进程 | 独立 exe | 放入 Job Object，主程序退出即杀 |

---

## 3. 模块与接口

### 3.1 `include/VoiceService.h`

```cpp
struct VoiceConfig {
    bool enabled = false;
    std::string backend = "voicevox";
    std::string enginePath;      // runtime/voicevox/run.exe
    int  enginePort = 50021;
    bool autostart = true;       // 随程序启动引擎
    int  speakerId = 3;          // ずんだもん ノーマル（首次运行按可用性纠正）
    std::string styleName = "ノーマル";
    double speed = 1.0, pitch = 0.0, intonation = 1.0, volume = 1.0;
    int  prePauseMs = 150, postPauseMs = 100;
    int  sampleRate = 24000; bool stereo = false;
    std::string speakMode = "auto";   // auto | tagged | off
    int  maxChars = 120;
    bool splitSentences = true;
    bool cacheEnabled = true; int cacheLimitMb = 200;
    bool lipSync = true; double lipSyncGain = 1.0; int lipSyncMs = 60;
    bool interruptOnNew = true;
    int  timeoutMs = 8000;
};

struct VoiceEmotion { std::string name; std::string style; double speed, pitch, intonation; };

class VoiceService {
public:
    static VoiceService& Instance();
    bool Initialize();                       // 读配置、起引擎（可选）、预热
    void Shutdown();
    void Reload();                           // 配置变更后热更新

    // 主入口：assistant 回复文本 + 情绪标签（可空）
    void Speak(const std::string& text, const std::string& emotion, const std::string& persona);
    void Stop();                             // 打断：淡出 + 清队列 + 引擎 cancel
    void TestSpeak(const std::string& text);  // 试听（设置页）

    bool IsReady() const;                    // 引擎可用
    std::string StatusText() const;          // "就绪 / 启动中 / 未安装 / 端口占用"
    std::vector<std::pair<int,std::string>> ListSpeakers() const;   // styleId → "角色 / 风格"
    float Level() const;                     // 当前音量包络（给口型）
};
```

### 3.2 `VoicevoxBackend`（`include/VoicevoxBackend.h` / `src/VoicevoxBackend.cpp`）

- `bool StartEngine()`：找 `enginePath`（默认 `runtime\voicevox\run.exe`）→ `CreateProcessW`（`--host 127.0.0.1 --port <p>`）→ 放入全局 Job Object（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`）→ 轮询 `GET /version` 直到就绪（超时 20s，退避 200ms）
- `bool EnsureReady()`：未就绪则尝试启动；已启动但无响应 → 记录失败次数，3 次后放弃并置 `unavailable`
- `AudioClip Synthesize(text, styleId, params)`：
  1. `POST /audio_query?text=<urlencoded>&speaker=<styleId>` → AudioQuery JSON
  2. 覆盖字段：`speedScale/pitchScale/intonationScale/volumeScale/prePhonemeLength/postPhonemeLength/outputSamplingRate/outputStereo`
  3. `POST /synthesis?speaker=<styleId>`，body = 改好的 AudioQuery → 返回 wav bytes
- `std::vector<Speaker>` LoadSpeakers()`：`GET /speakers`，缓存；提供 `ResolveStyleId(speakerId, styleName)`
- `Prewarm(styleId)`：`POST /initialize_speaker?speaker=<id>`（首句延迟从 ~800ms 降到 ~200ms）
- `Cancel()`：`POST /cancellable_synthesis`（若用该接口）或直接丢弃响应
- 失败分类：`not_installed` / `start_failed` / `port_busy` / `timeout` / `http_error` / `bad_response`，各自不同日志与提示

> 端点/参数以引擎自带的 `http://127.0.0.1:50021/docs` 为准（版本间可能有字段增删）。**styleId 不要硬编码**——一律经 `/speakers` 解析，引擎升级后仍可用。

### 3.3 `AudioPlayer`（`include/AudioPlayer.h` / `src/AudioPlayer.cpp`）

- WASAPI 共享模式（`IAudioClient` + `IAudioRenderClient`），48k/16bit 立体声输出，内部重采样 VOICEVOX 的 24k 单声道
- 环形缓冲（`std::vector<int16_t>` + 原子读写指针），`Push(pcm, rate, ch)` / `Play()` / `Stop(fadeMs)` / `float Level()`
- `Level()`：每 10–20ms 对最近 20ms 窗口算 RMS，做 attack/release 平滑（attack 15ms、release 120ms），存 `std::atomic<float>`
- 备选：`miniaudio`（MIT，单头文件）——若不想手写 WASAPI，可直接用它，API 更短

### 3.4 `Live2DManager::SetMouthOpen(float v)`（新增）

```cpp
void Live2DManager::SetMouthOpen(float v);   // 0..1，线程安全（原子成员，渲染线程消费）
```
- 在 `Update()` 里写 `CubismModel::SetParameterValueById(CubismFramework::GetIdManager()->GetId("ParamMouthOpenY"), v)`；`ParamMouthForm` 可固定 0
- 数值曲线：`v = pow(level * gain, 0.7)`（低音量也能看出张合），再 clamp
- 说完话 200ms 内自动归零（`level` 衰减到 0 自然归零，另加超时兜底）

### 3.5 挂接点

| 位置 | 改动 |
|---|---|
| `MessageHandler`（assistant 文本最终确定时） | 解析 `<voice …/>` 标签 → `VoiceService::Instance().Speak(text, emotion, persona)` |
| 流式回复（`stream.enabled=true`） | 每攒够一个句末标点就投递一句（`splitSentences`），首句延迟最低 |
| `AppCore::Initialize` | `VoiceService::Initialize()`；`Shutdown` 里 `VoiceService::Shutdown()` |
| `AppCore` 渲染循环 | `if (voice enabled) Live2DManager::SetMouthOpen(VoiceService::Level() * gain)` |
| `SpeechBubble` | 不变（继续显示完整文本，语音只负责"说"） |
| 关闭中枢 / 退出对话页 / 新回复 | `VoiceService::Stop()`（打断） |

---

## 4. AI 情绪标签协议

系统提示里追加约定（简短，避免污染人设）：

```
回复末尾可加一行控制标签（不显示给用户）：<voice emotion="happy|sad|angry|shy|calm|surprised|whisper" intensity="0-1"/>
```

- `MessageHandler` 在展示前用正则剥掉该行，把 `emotion/intensity` 传给 `VoiceService`
- 没有标签时：用 `voice.emotion_default`（默认 `calm`）
- `intensity` 只做插值：`speed = base + (styleSpeed - base) * intensity`，避免"情绪一开就夸张"

**默认映射表（写进 `voice_preset` 初始数据，AI 可改）**

| emotion | ずんだもん style | speed | pitch | intonation |
|---|---|---|---|---|
| calm / neutral | ノーマル | 1.00 | 0.00 | 1.00 |
| happy | あまあま | 1.06 | +0.03 | 1.20 |
| sad | ささやき | 0.92 | −0.05 | 0.85 |
| angry | ツンツン | 1.08 | +0.05 | 1.30 |
| shy | ヒソヒソ | 0.96 | +0.02 | 0.95 |
| surprised | ノーマル | 1.12 | +0.08 | 1.35 |
| whisper | ささやき | 0.94 | −0.02 | 0.80 |

> 别的角色（四国めたん/春日部つむぎ…）风格名不同，`voice_preset` 里按 `speaker_id + style` 存，`/speakers` 找不到对应风格时回退 `styles[0]`。

---

## 5. 软件内配置方案

### 5.1 配置项（`config` 表，键名 `voice.*`）

| 键 | 类型 | 默认 | 范围/选项 | 说明 |
|---|---|---|---|---|
| `voice.enabled` | Bool | false | | 语音总开关 |
| `voice.backend` | Enum | voicevox | voicevox | 预留 pip/kokoro/edge |
| `voice.engine_path` | Text | `runtime\voicevox\run.exe` | | 引擎路径（留空=自动查找） |
| `voice.engine_port` | Int | 50021 | 1024–65535 | 端口（被占时自增重试） |
| `voice.autostart_engine` | Bool | true | | 随程序启动引擎 |
| `voice.speaker` | Enum(动态) | ずんだもん | 来自 `/speakers` | 角色 |
| `voice.style` | Enum(动态) | ノーマル | 随角色变化 | 风格（=说话方式） |
| `voice.speed` | Double | 1.00 | 0.50–2.00 | 语速 |
| `voice.pitch` | Double | 0.00 | −0.15–0.15 | 音高 |
| `voice.intonation` | Double | 1.00 | 0.00–2.00 | 抑扬（情感强弱） |
| `voice.volume` | Double | 1.00 | 0.00–2.00 | 音量 |
| `voice.pre_pause_ms` | Int | 150 | 0–1000 | 句首静音 |
| `voice.post_pause_ms` | Int | 100 | 0–2000 | 句尾留白 |
| `voice.sample_rate` | Int | 24000 | 8000–48000 | 输出采样率 |
| `voice.stereo` | Bool | false | | 立体声（占位，一般关） |
| `voice.speak_mode` | Enum | auto | auto/tagged/off | 每条都念 / 只在有标签时念 / 不念 |
| `voice.max_chars` | Int | 120 | 20–500 | 单次最多念多少字（超出截断并提示） |
| `voice.split_sentences` | Bool | true | | 按句切分，边合成边播 |
| `voice.interrupt_on_new` | Bool | true | | 新回复打断旧语音 |
| `voice.cache_enabled` | Bool | true | | 合成缓存 |
| `voice.cache_limit_mb` | Int | 200 | 0–2000 | 缓存上限 |
| `voice.lip_sync` | Bool | true | | 口型联动 |
| `voice.lip_sync_gain` | Double | 1.00 | 0.20–2.50 | 口型幅度 |
| `voice.lip_sync_smooth_ms` | Int | 60 | 0–300 | 口型平滑 |
| `voice.timeout_ms` | Int | 8000 | 1000–60000 | 单句合成超时 |
| `voice.emotion_default` | Enum | calm | 见映射表 | 无标签时的情绪 |
| `voice.test_text` | Text | おはよう、今日も一日がんばろうね | | 试听文本 |

### 5.2 设置页：新增第 11 张卡片

```cpp
// ===== 卡片 11：语音合成 =====
const ControlTerminalWindow::FieldDef kVoiceFields[] = {
  { "voice.enabled",      L"语音合成",   L"VOICE",        L"回复用 VOICEVOX 念出来", FieldKind::Bool, L"false", nullptr, false, 0 },
  { "voice.speaker",      L"角色",       L"SPEAKER",      L"引擎可用角色",           FieldKind::Enum, L"ずんだもん", nullptr, false, 0 },  // OptionsFor 动态
  { "voice.style",        L"风格",       L"STYLE",        L"随角色变化",             FieldKind::Enum, L"ノーマル",  nullptr, false, 0 },  // OptionsFor 动态
  { "voice.speed",        L"语速",       L"SPEED",        L"0.5 – 2.0",             FieldKind::Double, L"1.0", nullptr, false, 150 },
  { "voice.pitch",        L"音高",       L"PITCH",        L"−0.15 – 0.15",          FieldKind::Double, L"0.0", nullptr, false, 150 },
  { "voice.intonation",   L"抑扬",       L"INTONATION",   L"0 – 2，情绪强弱",        FieldKind::Double, L"1.0", nullptr, false, 150 },
  { "voice.volume",       L"音量",       L"VOLUME",       L"0 – 2",                 FieldKind::Double, L"1.0", nullptr, false, 150 },
  { "voice.speak_mode",   L"朗读时机",   L"SPEAK MODE",   L"每条 / 仅带标签 / 关闭", FieldKind::Enum, L"auto", L"auto|tagged|off", false, 160 },
  { "voice.emotion_default", L"默认情绪", L"EMOTION",     L"无标签时使用",           FieldKind::Enum, L"calm", nullptr, false, 160 },  // OptionsFor 动态
  { "voice.max_chars",    L"单次字数",   L"MAX CHARS",    L"超出截断",               FieldKind::Int, L"120", nullptr, false, 150 },
  { "voice.lip_sync",     L"口型联动",   L"LIP SYNC",     L"按音量驱动嘴部",         FieldKind::Bool, L"true", nullptr, false, 0 },
  { "voice.lip_sync_gain",L"口型幅度",   L"LIP GAIN",     L"0.2 – 2.5",             FieldKind::Double, L"1.0", nullptr, false, 150 },
  { "voice.cache_enabled",L"合成缓存",   L"CACHE",        L"常用台词秒出",           FieldKind::Bool, L"true", nullptr, false, 0 },
  { "voice.test_text",    L"试听文本",   L"TEST TEXT",    L"点右侧按钮试听",          FieldKind::Text,  L"おはよう", nullptr, false, 320 },
};
{ L"11", L"语音合成", L"VOICE", L"用 VOICEVOX 把回复念出来；引擎未启动时可在这里一键拉起。", kVoiceFields, 14 },
```

### 5.3 需要扩展的设置页能力

1. **`FieldKind::Action`（新）**：渲染成按钮，点击 `post({type:"fieldAction", key:"<key>"})`。用于：
   - `voice.test` → 试听
   - `voice.engine` → 启动/重启引擎
   - `voice.refresh` → 重新拉取角色列表
   - `voice.preview_preset` → 播放当前情绪预设示例
2. **`OptionsFor` 动态选项**：`voice.speaker` 取 `VoiceService::ListSpeakers()` 的 `speakerId|角色名`；`voice.style` 取当前角色 styles；`voice.emotion_default` 取映射表键；引擎不可用时返回占位项并保留当前值（现有"缺值自动补一项"的逻辑可复用）。
3. **状态行**：卡片副标题追加引擎状态（`就绪 / 启动中 / 未安装 / 端口占用`），由 `BuildStateJson()` 的 `status.voice` 下发，页面在卡片右上角画一个和 `API 未测试` 同款的小圆点。
4. **字段可见性（可选）**：`voice.enabled=false` 时把其余 voice 字段灰掉（现有页面已有 `requires` 机制，可复用）。

### 5.4 新增数据表

```sql
-- 情绪预设：AI 调参的落点（也可手改）
create table if not exists voice_preset (
  id integer primary key autoincrement,
  persona   text not null default 'default',   -- 人设名
  emotion   text not null,                     -- calm/happy/sad/angry/shy/surprised/whisper
  speaker   text,                              -- 可覆盖全局角色
  style     text not null,                     -- 风格名
  speed     real not null default 1.0,
  pitch     real not null default 0.0,
  intonation real not null default 1.0,
  extra     text,                              -- JSON：预留 pause/volume 等
  updated_at integer not null,
  unique(persona, emotion)
);

-- 缓存索引（文件本体在 data/voice_cache/）
create table if not exists voice_cache (
  key       text primary key,   -- sha1(text|speaker|style|params)
  path      text not null,
  bytes     integer not null,
  last_used integer not null
);
```

首次启动（或 `voice.enabled` 第一次打开）时用默认映射表灌入 `persona='default'` 的 7 条记录。

### 5.5 目录与打包

```
<安装目录>/
  Unnamedbot.exe
  runtime/
    voicevox/           # 可选组件，不进 git
      run.exe
      *.dll
      resources/…
  data/
    voice_cache/        # 合成缓存（可随时删）
```

- `.gitignore` 追加：`runtime/`、`*.wav`（缓存）、`*.onnx`、`*.pth`
- 安装包二选一：
  - **A（推荐）**：VOICEVOX 作为可选组件（约 1 GB），勾选才装；
  - **B**：首次开启语音时自动下载 + 解压到 `runtime/voicevox/`（带进度条与校验）
- 文档/关于页需写明 credit：`VOICEVOX:ずんだもん` 等（各角色条款要求署名；商用与再分发规则以 VOICEVOX 官方为准）

---

## 6. 关键流程

**普通回复**
1. assistant 文本就绪 → 剥 `<voice>` 标签 → `Speak(text, emotion, persona)`
2. `VoicePreset` 查表得 `style/speed/pitch/intonation`（无则用默认映射）
3. 切句：`。！？…!?` + 长度上限（默认 40 字/句）
4. 逐句：缓存命中→播；未命中→合成→缓存→播
5. 播放时每 20ms 更新 `Level()`，渲染线程读它写 `ParamMouthOpenY`

**打断**
- 新回复 / 用户点击气泡 / 关闭对话页 / 退出程序 → `Stop()`：播放器 60ms 淡出 → 清队列 → 丢当前 HTTP 请求 → 口型归零

**引擎不可用**
- `Speak()` 直接返回 false，记一次 warn（同因不刷屏），气泡不需要特殊提示；设置页状态灯显示原因；用户点「启动引擎」可重试

---

## 7. 分阶段任务与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P0**（1–2 天） | `AudioPlayer` + `SetMouthOpen` + `VoiceService`(单后端、整句合成) + 卡片/配置 | 开关打开后回复能念出来，嘴会动，能停 |
| **P1**（2–3 天） | 引擎自启/健康检查/重启、speaker 动态列表、按句流式、缓存、打断 | 首句 < 0.8s；连续对话不卡顿；杀掉引擎能自恢复 |
| **P2**（2 天） | `<voice>` 标签 + `voice_preset` 表 + 情绪映射 + 试听按钮 | 同一句话 7 种情绪听感明显不同 |
| **P3**（按需） | AI 调参入口（改 preset 热生效）、多后端适配（pip/kokoro/edge）、安装包组件 | 切换后端只需改 `voice.backend` |

---

## 8. 风险与待定

1. **磁盘**：引擎 ~1 GB，安装包会变大；用可选组件/首启下载缓解。
2. **首次延迟**：引擎冷启动 2–4 s；靠"随程序启动 + `initialize_speaker` 预热"压到背景里。
3. **端口冲突**：50021 被占时按 `engine_port` 自增尝试，并把实际端口回写配置。
4. **版本漂移**：`/audio_query` 字段可能随引擎升级变化；只设已知字段、未知字段原样透传；styleId 一律运行时解析。
5. **多语言**：VOICEVOX 只支持日语（部分角色有英语/中文风格，视版本而定）——中文对话目前会读成日式音，后续可加 `edge`/`kokoro` 后端按语言路由。
6. **授权**：引擎与各角色条款不同（署名、商用限制），发布前逐项核对并在关于页列出 credit。
