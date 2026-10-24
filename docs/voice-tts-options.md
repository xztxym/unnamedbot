# AI 对话语音（TTS）方案调研

> 目标：给桌宠的 AI 对话加上**二次元音声**，音色/语气可自定义，并且后续能由 AI 按「模型 + 人设」自动调参。
> 项目现状：C++ Win32 + D3D11 + Live2D Cubism 5 + WebView2 + SQLite，已链接 `cpr`（HTTP 客户端，底层 libcurl）；**目前没有任何音频播放层**，`Live2DManager` 也还没有口型接口 —— 这两块需要新建。

---

## 一、候选方案

### 方案 A：本地 GPT-SoVITS（推荐主力）

- **是什么**：RVC-Boss 的 few-shot TTS，1 分钟素材即可克隆音色；官方带 `api_v2.py` HTTP 服务，支持参考音频（音色）、参考文本、语速、切分、流式。
- **二次元能力**：社区有大量动漫角色音色模型；日语/中文都行，日语发音依赖参考音频质量。
- **自定义**：★★★★★ 音色=换参考音频/换模型；情绪=换参考音频（同一角色不同语气各录一段），可细到"每句话挑参考片段"。
- **AI 可调性**：★★★★☆ 接口就是 JSON，参数（ref_audio/text、speed、top_k、temperature…）天然适合由 LLM 生成；但情绪是"隐式"的（靠参考音频），需要建一张"情绪→参考音频"的映射表。
- **成本**：本地 GPU（本机 RTX 3070 Ti Laptop 够用）；显存约 4–6 GB；首次响应通常 0.3–1.5 s/句（估算，视句子长度与是否流式）。
- **授权**：代码 MIT。**音色模型的授权要单独看**——用别人克隆的真人声优音色有风险，商业分发尤其要注意。
- **接入**：`VoiceService` → `POST http://127.0.0.1:9880/tts` → wav 流 → 播放。

### 方案 B：本地 Style-Bert-VITS2（二次元特化）

- **是什么**：BERT-VITS2 的"风格"分支，专门做**角色语音**：JP-Extra 系列模型（如 litagin 的 base-JP-Extra）就是为日语动漫角色训练；支持**スタイル（style）向量**、情感强度、语速。
- **二次元能力**：★★★★★ 日语动漫腔最正，社区模型/预训练权重多；有论文专门做过「VITS vs Style-Bert-VITS2 的表现力日语角色 TTS」对比。
- **自定义**：★★★★☆ 音色=换模型；情绪=style 名（如 Neutral / Happy / Angry / Sad…）；比 A 的"参考音频"更**显式、可枚举**，适合让 AI 选。
- **AI 可调性**：★★★★★ style 是离散标签 + 强度浮点，LLM 直接输出 `{style:"Angry", intensity:0.7}` 即可；参数少、调起来稳。
- **成本**：同 A（GPU 推理，ONNX 版本可 CPU）；模型比 GPT-SoVITS 小、启动快。
- **授权**：**代码 AGPL-3.0**（对闭源分发敏感）；模型权重各有条款（base 模型通常允许生成，但**禁止用生成音频冒充真人/违法用途**）——落地前逐项核对。
- **接入**：本地 FastAPI server（`/voice`、`/style`）→ wav。

### 方案 C：云端音色库（Fish Audio / ElevenLabs v3）

- **是什么**：
  - **Fish Audio（S1/OpenAudio）**：中文/日语强，支持 `[laugh]`、`[sigh]`、`[whisper]` 这类**行内情感标签**，也有细粒度情绪控制文档；有官方 API。
  - **ElevenLabs v3**：`[whispers]`、`[excited]` 等 audio tags；instant voice clone 几分钟素材即可；音色库最丰富。
- **二次元能力**：★★★★☆ 取决于你选的音色；日语角色音很多，但"动漫腔"需要挑。
- **自定义**：★★★★☆ 换 voice_id 即可；标签化情绪表达力强。
- **AI 可调性**：★★★★★ **最适合 AI 自动调**——LLM 直接在台词里插 `[sigh]`/`[excited]`，或输出 stability/similarity/style 参数；无需本地环境。
- **成本**：按字符/时长计费（对话场景每月用量要估）；**离线不可用**；隐私上要把对话内容发给第三方。
- **接入**：`cpr` POST（返回 mp3/pcm），支持流式分块 → 边说边播。

### 方案 D：VOICEVOX（正版日系角色音，零训练）

- **是什么**：日语 TTS 引擎 + 一堆**授权明确**的日系角色音（ずんだもん、四国めたん 等），本地 HTTP 引擎（`/audio_query` → `/synthesis`）。
- **二次元能力**：★★★★☆ 标准动漫配音腔，但**音色固定**、不能克隆。
- **自定义**：★★☆☆☆ 只能调 speed/pitch/intonation/volume/停顿，角色只能从库里选。
- **AI 可调性**：★★★★☆ 参数是显式 JSON（`AudioQuery`），LLM 好调；情绪靠 intonation/pitch/speed 组合近似。
- **成本**：免费、CPU 可跑、体积小；**角色有各自使用条款**（商用/再分发要看清，多数要求署名、禁止不当使用）。
- **定位**：**兜底方案**——没 GPU、或不想碰模型授权时，这是最省事且合法的日系音。

### 方案 E：通用云 TTS（Azure / OpenAI / Gemini）

- **Azure AI Speech**：HD voices + SSML（`<mstts:express-as style="cheerful" styledegree="1.5">`），稳定、可商用、可控停顿/语速/音高。
- **OpenAI `gpt-4o-mini-tts`**：用自然语言 `instructions` 控制"怎么念"（语气/情绪/节奏），**对 AI 调参最友好**。
- **二次元能力**：★★☆☆☆ 偏"播音/自然"，动漫味需要靠 style/instructions 硬掰。
- **定位**：要合规、要稳定 SLA、要低延迟时的企业向选择；纯二次元不如 A/B/C。

---

## 二、对比表

| | A GPT-SoVITS | B Style-Bert-VITS2 | C Fish/ElevenLabs | D VOICEVOX | E Azure/OpenAI |
|---|---|---|---|---|---|
| 二次元味 | ★★★★★ | ★★★★★ | ★★★★☆ | ★★★★☆ | ★★☆☆☆ |
| 音色自定义 | 克隆/换模型 | 换模型 | 换 voice_id | 固定角色 | 固定音色 |
| 情绪控制 | 参考音频（隐式） | style 标签（显式） | 行内标签（显式） | 参数近似 | SSML/instructions |
| AI 自动调参 | 中（要建映射表） | **强**（枚举+强度） | **强**（标签） | 中 | 强 |
| 离线 | ✅ | ✅ | ❌ | ✅ | ❌ |
| 硬件 | GPU 4–6G | GPU/CPU(ONNX) | 无 | CPU | 无 |
| 延迟 | 0.3–1.5s/句 | 0.2–1.0s/句 | 0.3–1.2s | 0.1–0.4s | 0.3–1.0s |
| 授权风险 | 中（音色模型） | 中（AGPL+模型条款） | 低（商用条款） | 低（角色条款） | 低 |
| 接入成本 | 中 | 中 | 低 | **低** | 低 |

> 延迟/显存为估算值，需在本机压测确认。

---

## 三、推荐组合

**主：A（GPT-SoVITS）** —— 二次元音色自由度最高，本机 GPU 够，离线可用。
**兜底：D（VOICEVOX）** —— 无 GPU / 授权敏感 / 模型没下载时自动降级，保证"一定有声音"。
**可选增强：C（Fish Audio）** —— 想追求"笑声/叹气/耳语"这类细腻表现，或不想在本机跑模型时切过去。

架构上用**统一后端适配层**，配置里切 backend 即可，不把任何一家写死。

---

## 四、工程落地设计（针对本项目）

```
LLM 回复文本
   ↓ ① 台词预处理（LLM 或规则）：拆句 + 标情绪
   ↓ ② VoiceService::Synthesize(text, preset) → AudioClip（wav/pcm）
   ↓ ③ AudioPlayer（miniaudio）：环形缓冲播放，支持流式分块
   ↓ ④ 播放线程每 20ms 算 RMS 包络 → Live2DManager::SetMouthOpen(0..1) → ParamMouthOpenY
   ↓ ⑤ SpeechBubble 同步显示字幕；新回复/用户打断 → 淡出并取消
```

**新增模块**

- `include/VoiceService.h` / `src/VoiceService.cpp`
  - `bool Initialize(const VoiceConfig&)`、`void Shutdown()`
  - `void Speak(const std::string& text, const VoicePreset&)`、`void Stop()`
  - 后端适配：`ITtsBackend`（`LocalGptSoVits` / `LocalSbv2` / `Voicevox` / `CloudFish` / `CloudEleven` / `OpenAI`）
  - HTTP 统一走已有 `cpr`；流式用 `cpr::Session` + 回调写入播放环形缓冲
- `include/AudioPlayer.h` / `src/AudioPlayer.cpp`
  - 单头文件 **miniaudio**（MIT，无额外依赖）或直接 WASAPI
  - 提供 `Push(pcm, rate, channels)`、`Play()`、`Stop()`、`float GetLevel()`（RMS）
- `Live2DManager::SetMouthOpen(float)` / `SetMouthForm(float)`
  - 在 `Update()` 前把参数写进 `CubismModel`，配合 `CubismPhysics` 自然回弹

**配置项（SQLite `config` 表）**

`voice.enabled` / `voice.backend` / `voice.endpoint` / `voice.model` / `voice.voice_id` /
`voice.language`（ja/zh）/ `voice.speed` / `voice.pitch` / `voice.volume` /
`voice.emotion_default` / `voice.cache_enabled` / `voice.latency_mode`（流式/整句）

**新表 `voice_preset`**（人设 × 情绪 → 参数），供 AI 调参与手动覆盖：

```sql
create table voice_preset (
  id integer primary key autoincrement,
  persona text not null,        -- 例如 default / 角色名
  emotion text not null,        -- neutral / happy / sad / angry / shy / whisper …
  backend text,                 -- 可覆盖全局
  params text not null,         -- JSON: {speed, style, ref_audio, intensity…}
  updated_at integer
);
```

**AI 调参闭环（"由 AI 调试"的落地方式）**

1. 回复生成时让 LLM 额外输出控制块（同一轮，不额外请求）：

```json
{"say":"……","voice":{"emotion":"shy","intensity":0.6,"speed":0.95,"pause_ms":180}}
```

2. `VoiceService` 把 `emotion` 查 `voice_preset` 表 → 合并默认参数 → 调后端；
3. 调参时给 AI 一个"参数说明书"（哪个后端支持哪些字段、取值范围、常见映射），AI 只能改 `voice_preset` 与 `voice.*` 配置，改完即时生效（不重启）；
4. 可选的客观回环：合成后跑一次轻量 ASR（whisper.cpp）比对文本，识别率过低 → 自动降语速/换 style，避免"AI 调出听不清的怪声"。

**缓存与打断**

- 缓存键 `hash(text + preset + backend)` → `data/voice_cache/<hash>.wav`，命中直接播（0 延迟），LRU 上限 200 MB；
- 任何新回复 / 用户按键 / 关闭对话页 → `Stop()`：淡出 60ms、清空环形缓冲、口型归零。

---

## 五、分阶段计划

| 阶段 | 内容 | 产出 |
|---|---|---|
| P0（1–2 天） | `AudioPlayer` + `SetMouthOpen` + `VoiceService` 单后端（D VOICEVOX 或 E 云） | 说一句话、口型能动 |
| P1（3–5 天） | 接入 A/B 本地后端、流式播放、打断、缓存、配置项 | 日常对话可用 |
| P2（2–3 天） | `voice_preset` 表 + LLM 情绪标签 + AI 调参入口 + ASR 回环 | "AI 自己调音色/情绪" |
| P3（按需） | 多音色切换（按人设）、字幕气泡联动、安装包附带模型下载器 | 可分发 |

---

## 六、风险与注意

1. **音色授权**：克隆真人（声优/主播）音色在商业分发中风险高；建议用自制/授权音色库，并在安装包与文档里注明来源。
2. **模型体积**：GPT-SoVITS/SBV2 权重 GB 级，**不要进 git**（当前 `.gitignore` 需加 `models/voice/`、`*.pth`、`*.onnx`、`*.safetensors`），改为首启下载或安装包可选组件。
3. **AGPL**：Style-Bert-VITS2 是 AGPL-3.0，若本项目要闭源分发，优先 A（MIT）或 D。
4. **延迟**：整句合成会"等 1 秒才出声"；流式分块（首块 ~200–400ms）体验更好，但需要播放器支持边收边播。
5. **离线优先**：桌宠是常驻程序，云端 TTS 断网会哑；必须有本地/预缓存兜底。

---

## 附：低性能 / 高泛用方案（零 GPU、CPU 实时、模型小）

> 目标换成：**任何一台 Windows 都能跑、不装 GPU 依赖、模型几十 MB 级、多语言覆盖广**。
> 关键前提：音频播放层 / 口型 / 缓存 / 打断这套基础设施不变，只把「后端」换成下面任何一种，后续想升级 GPT-SoVITS 只是多写一个 backend 实现。

| 档位 | 方案 | 本地算力 | 磁盘 | 联网 | 日语二次元味 | 中文 | 接入方式 |
|---|---|---|---|---|---|---|---|
| T0 | **Windows SAPI**（`ISpVoice`） | 几乎为零 | 0 | ❌ | ★☆☆☆☆ | 系统音（本机仅 Huihui） | COM，`Speak()` 直接出声 |
| T1 | **Edge TTS**（微软神经语音，免费无 key） | 几乎为零 | 0 | ✅ | ★★★☆☆（ja-JP-Nanami 等） | ★★★★☆（Xiaoxiao 等） | WebSocket（可用小 sidecar） |
| T2 | **Piper**（ONNX，预编译 `piper.exe`） | CPU 实时（RTF 0.05–0.2） | 20–60 MB/音色 | ❌ | ★★☆☆☆ | ★★★☆☆ | 子进程 + stdin/stdout，**零 C++ 依赖** |
| T2 | **Kokoro-82M**（ONNX，Apache-2.0） | CPU 实时（int8，RTF ~0.2–0.5） | ~80–330 MB | ❌ | ★★★☆☆ | ★★★★☆ | ONNX Runtime C++ 或 Python sidecar |
| T2 | **Supertonic**（~66M ONNX，31 语言） | CPU 实时 | ~100 MB | ❌ | ★★☆☆☆ | ★★★☆☆ | ONNX Runtime |
| T3 | **VOICEVOX**（正版日系角色音） | CPU 实时，内存 0.5–1 GB | 引擎 ~1 GB | ❌ | ★★★★★（角色音） | ★★☆☆☆ | 本地 HTTP `/audio_query`→`/synthesis` |

**本机实测情况**：系统里只装了 `Microsoft Huihui`（中文）和 `Microsoft Zira`（英文），**没有日语系统音**——所以 T0 想日文得先装日语语音包，T0 基本只能当"断网兜底"。

**推荐组合（低配路线）**

1. **日常主力：Edge TTS** —— 零磁盘、零算力、音质接近 Azure 神经语音，中文日语都够用；缺点是要联网、是微软的非官方公开接口，不适合当唯一依赖。
2. **离线主力：Kokoro-82M（int8）** —— 80 MB 级模型、CPU 实时、Apache-2.0、中英日都可，综合性价比最高；代价是要带一个 ONNX Runtime（~15 MB）。
3. **最省事离线：Piper** —— 官方有预编译 `piper.exe`，直接子进程喂文本、拿 wav，**项目里一行新库都不用加**；日语一般，中文可以。
4. **要正版日系角色腔：VOICEVOX** —— 还是 CPU 跑，但磁盘/内存比 T2 大一档，音色固定。

**"泛用性"体现在**：上面这些都不挑 GPU、跨平台（Piper/Kokoro/Edge TTS 都有 Win/macOS/Linux 版本），语言覆盖从 2 种（SAPI）到 30+ 种（Piper/Supertonic）；音色切换是"换模型文件/换 voice 名"，不用重新训练。

**"AI 调参"在低配方案里的映射**：
- Edge TTS → `voice`、`rate`、`pitch`、`volume`（SSML 式），LLM 直接给数值；
- Kokoro → `voice`（音色）+ `speed`，情绪靠**文本标点与语气词**（"……" 拖长、"！" 提调）近似；
- Piper → `length_scale`（语速）、`noise_scale/noise_w`（韵律随机度）、`speaker`（多说话人模型）；
- VOICEVOX → `AudioQuery`（`speedScale/pitchScale/intonationScale/pauseLengthScale`），可精细到每个停顿。