#include "../include/ControlTerminalWindow.h"
#include "../include/Live2DManager.h"
#include <shlobj.h>       // SHBrowseForFolderW / SHGetPathFromIDListW（导入模型-文件夹）
#include <commdlg.h>
#include <dwmapi.h>      // DWM 边框/标题栏配色（去白边）      // GetOpenFileNameW（导入模型-zip）

#include "../include/UiTheme.h"

#include "../include/AppCore.h"
#include "../include/ConfigManager.h"
#include "../include/ConfigWebUi.h"
#include "../include/Logger.h"
#include "../include/MessageHandler.h"
#include "../include/PromptLibrary.h"
#include "../include/VoiceService.h"
#include "../include/TaskScheduler.h"
#include "../include/ResourceIds.h"
#include "../include/Utils.h"
#include "../include/SecurityUtils.h"

#include <wrl.h>

#include <nlohmann/json.hpp>

#include <windowsx.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <objbase.h>

namespace UnnamedBot {

    const wchar_t* ControlTerminalWindow::WINDOW_CLASS_NAME = L"UnnamedBotControlTerminal";

    namespace {

        using FieldKind = ControlTerminalWindow::FieldKind;

        std::string ToUtf8(const wchar_t* text) {
            return text ? Utils::WideToUtf8(text) : std::string();
        }

        // ===== 窗口尺寸（逻辑像素，96 DPI）=====
        // 中枢界面是「导轨 + 画面」的横向布局（设计稿按 1440×900 画），
        // 900×720 会把它压扁，所以窗口按 1200×780 开——在 1920×1080@125% 上
        // 物理尺寸是 1500×975，仍然放得下。
        const int kWinW = 1200;
        const int kWinH = 780;

        // 服务商预设：常用 OpenAI 兼容接口的 Base URL，以及该服务商可用的模型。
        // 模型名各家会随时调整，这里给的是常见值；用户仍可在「默认模型」里直接手填。
        // 写进来的名字必须是该服务端真实接受的：名字不对时服务端对每一次请求
        // 都回 HTTP 400，症状是「AI 永远不回话」，而不是一条能看懂的报错。
        struct ProviderPreset {
            const wchar_t* label;
            const wchar_t* baseUrl;
            const wchar_t* models;   // '|' 分隔
        };

        const ProviderPreset kProviders[] = {
            { L"OpenAI",     L"https://api.openai.com/v1",                                 L"gpt-4o|gpt-4o-mini|gpt-4.1|gpt-4.1-mini|o3|o4-mini" },
            { L"Azure",      L"https://YOUR-RESOURCE.openai.azure.com/openai/v1/",         L"gpt-4o|gpt-4o-mini|gpt-4.1" },
            { L"DeepSeek",   L"https://api.deepseek.com/v1",                               L"deepseek-flash|deepseek-v4-pro" },
            { L"智谱 GLM",   L"https://open.bigmodel.cn/api/paas/v4",                      L"glm-4-plus|glm-4-air|glm-4-flash|glm-4.5" },
            { L"Kimi",       L"https://api.moonshot.cn/v1",                                L"moonshot-v1-8k|moonshot-v1-32k|moonshot-v1-128k|kimi-k2-0711-preview" },
            { L"阿里百炼",   L"https://dashscope.aliyuncs.com/compatible-mode/v1",         L"qwen-plus|qwen-max|qwen-turbo|qwen3-235b-a22b" },
            { L"火山方舟",   L"https://ark.cn-beijing.volces.com/api/v3",                  L"doubao-seed-1-6-250615|doubao-1-5-pro-32k-250115" },
            { L"百度千帆",   L"https://qianfan.baidubce.com/v2",                           L"ernie-4.0-turbo-8k|ernie-3.5-8k|ernie-speed-128k" },
            { L"腾讯混元",   L"https://api.hunyuan.cloud.tencent.com/v1",                  L"hunyuan-turbos-latest|hunyuan-turbo-latest|hunyuan-large" },
            { L"讯飞星火",   L"https://spark-api-open.xf-yun.com/v1",                      L"4.0Ultra|generalv3.5|lite" },
            { L"硅基流动",   L"https://api.siliconflow.cn/v1",                             L"Qwen/Qwen2.5-7B-Instruct|deepseek-ai/DeepSeek-V3" },
            { L"OpenRouter", L"https://openrouter.ai/api/v1",                              L"openai/gpt-4o|anthropic/claude-3.5-sonnet|google/gemini-2.0-flash-001" },
            { L"Groq",       L"https://api.groq.com/openai/v1",                            L"llama-3.3-70b-versatile|llama-3.1-8b-instant" },
            { L"Mistral",    L"https://api.mistral.ai/v1",                                 L"mistral-large-latest|mistral-small-latest" },
            { L"Gemini",     L"https://generativelanguage.googleapis.com/v1beta/openai/",  L"gemini-2.5-pro|gemini-2.5-flash" },
            { L"Ollama",     L"http://localhost:11434/v1",                                 L"qwen2.5:7b|llama3.2:3b" },
            { L"LM Studio",  L"http://localhost:1234/v1",                                  L"local-model" },
        };

        struct PresetOption {
            std::wstring label;
            std::wstring value;
        };

        std::vector<std::wstring> SplitOptions(const wchar_t* options) {
            std::vector<std::wstring> result;
            if (!options) return result;
            std::wstring current;
            for (const wchar_t* p = options; *p; ++p) {
                if (*p == L'|') {
                    result.push_back(current);
                    current.clear();
                }
                else {
                    current.push_back(*p);
                }
            }
            result.push_back(current);
            return result;
        }

        // 键名按内容比较：指针比较依赖字面量合并，不可靠。
        bool KeyIs(const ControlTerminalWindow::FieldDef& def, const std::string& key) {
            return def.key != nullptr && std::strcmp(def.key, key.c_str()) == 0;
        }

        // 逗号分隔的清单（prompt.enabled 的存储格式，与 PromptLibrary 的说法一致）
        // 拆成各项，顺手丢掉两侧空白与空项：手写或导入配置容易带空格。
        std::vector<std::string> SplitList(const std::string& value) {
            std::vector<std::string> result;
            for (const std::string& item : Utils::Split(value, ',')) {
                const std::string trimmed = Utils::Trim(item);
                if (!trimmed.empty()) result.push_back(trimmed);
            }
            return result;
        }

        // 字段选项的唯一来源：页面渲染（BuildSchemaJson）与宿主校验（ValidateValue）
        // 都走这里。必须共用同一来源，是因为 quick.mode 的选项是运行时决定的
        // （看 yandere.chr 是否存在），一旦两边各写一份判断，就会出现
        // 「页面上能选、保存时却说取值非法」这种用户既看不懂又绕不过去的矛盾。
        std::vector<std::wstring> OptionsFor(const ControlTerminalWindow::FieldDef& def) {
            if (KeyIs(def, ConfigManager::KEY_QUICK_MODE)) {
                std::vector<std::wstring> options;
                options.push_back(L"关闭");
                // yandere.chr 是哨兵文件：只有它在 prompts 目录里存在时，
                // 页面才多出一个「？？？」（硬编码的病娇人格，见 PromptLibrary）。
                // 它必须永远是最后一项：基础选项的次序不能随哨兵文件的出现或消失
                // 而变，否则已经存着的「关闭」会跳到别的格子上；末尾固定也让它成为
                // 最不容易被误点的一个，而这一项一旦生效就不可逆。
                if (PromptLibrary::Instance().HasYandereSentinel()) {
                    options.push_back(L"？？？");
                }
                return options;
            }
            // 语音角色：选项来自 VoiceService 的角色表（tts.quest 的 styleId 一一对应）
            if (KeyIs(def, "voice.speaker")) {
                return VoiceService::SpeakerLabels();
            }
            return SplitOptions(def.options);
        }

        // 选项按页面的下发格式拼回去（'|' 分隔）；非 Enum 行没有选项，返回空串。
        std::string OptionsTextFor(const ControlTerminalWindow::FieldDef& def) {
            if (def.kind != FieldKind::Enum) {
                return ToUtf8(def.options);
            }
            const std::vector<std::wstring> options = OptionsFor(def);
            std::wstring joined;
            for (size_t i = 0; i < options.size(); ++i) {
                if (i > 0) joined.push_back(L'|');
                joined += options[i];
            }
            return Utils::WideToUtf8(joined);
        }

        // 依赖行：父开关不满足条件时页面把整行隐藏（见 ConfigWebUi 的 render()）。
        // 写成函数而不是给 FieldDef 加成员，是为了保持字段表的 POD 与排版对齐。
        bool RequiresFor(const ControlTerminalWindow::FieldDef& def, std::string& key, std::string& value) {
            if (KeyIs(def, ConfigManager::KEY_QUICK_MODE)) {
                key = ConfigManager::KEY_QUICK_ENABLED;
                value = "true";
                return true;
            }
            return false;
        }

        // 两行 Preset 的字段键名。运行时取键名，避免与代码里各写一份字符串。
        // （prompt.enabled 是真实配置键，直接用 ConfigManager::KEY_PROMPT_ENABLED，
        //   只是借同一条选项下发通道，不在这里另立字面量。）
        const char* const kProviderPresetKey = "api.provider_preset";
        const char* const kModelPresetKey = "api.model_preset";

        // 数值行的可用区间：页面用它画滑块（rangebtn），并夹住手填的数字。
        // 与 ValidateValue 的边界是同一套，只是这里要给出具体数值而不只是「合规与否」。
        struct ValueRange {
            double min;
            double max;
            double step;
        };

        ValueRange RangeFor(const ControlTerminalWindow::FieldDef& def) {
            const std::string key = def.key ? def.key : "";
            if (key == "sampling.temperature")        return { 0.0, 2.0, 0.05 };
            if (key == "sampling.top_p")              return { 0.0, 1.0, 0.01 };
            if (key == "sampling.frequency_penalty")  return { -2.0, 2.0, 0.05 };
            if (key == "sampling.presence_penalty")   return { -2.0, 2.0, 0.05 };
            if (key == "model.default_scale")         return { 0.1, 3.0, 0.05 };
            if (key == "model.default_opacity")       return { 0.1, 1.0, 0.01 };
            if (key == "stream.timeout_seconds")      return { 5.0, 300.0, 5.0 };
            if (key == "context.max_tokens")          return { 256.0, 128000.0, 256.0 };
            if (key == "context.max_rounds")          return { 1.0, 64.0, 1.0 };
            if (key == "active.interval_process")     return { -1.0, 3600.0, 5.0 };
            if (key == "active.interval_weather")     return { -1.0, 7200.0, 10.0 };
            if (key == "active.interval_screenshot")  return { -1.0, 7200.0, 30.0 };
            if (key == "active.cooldown_seconds")     return { 0.0, 3600.0, 10.0 };
            if (key == "attachment.max_size_mb")      return { 1.0, 512.0, 1.0 };
            if (key == "attachment.total_quota_mb")   return { 1.0, 10240.0, 10.0 };
            if (key == "attachment.retention_days")   return { 0.0, 365.0, 1.0 };
            if (key == "logs.retention_days")         return { 0.0, 365.0, 1.0 };
            if (key == "proxy.port")                  return { 0.0, 65535.0, 1.0 };
            if (key == "update.check_interval_hours") return { 1.0, 720.0, 1.0 };
            return def.kind == ControlTerminalWindow::FieldKind::Int ? ValueRange{ 0.0, 1000.0, 1.0 }
                                                                     : ValueRange{ 0.0, 1.0, 0.01 };
        }

        // Preset 行本身不入库：它只是写入口。服务商行写 api.base_url，
        // 模型行写 api.model_default。
        const char* PresetTargetKeyFor(const ControlTerminalWindow::FieldDef& def) {
            // Live2D 模型下拉：选项直接写回 model.default_id 本身
            if (def.key != nullptr && std::strcmp(def.key, "model.default_id") == 0) {
                return def.key;
            }
            if (def.key != nullptr && std::strcmp(def.key, kModelPresetKey) == 0) {
                return ConfigManager::KEY_API_MODEL_DEFAULT.c_str();
            }
            return ConfigManager::KEY_API_BASE_URL.c_str();
        }

        const ProviderPreset* FindProviderByUrl(const std::wstring& url) {
            for (const ProviderPreset& provider : kProviders) {
                if (url == provider.baseUrl) {
                    return &provider;
                }
            }
            return nullptr;
        }

        // 一行 Preset 的有效选项。「可用模型」行按当前的服务地址反查服务商，
        // 所以切换服务商后这一行会跟着变；当前值始终保留在列表里（可能是手填的），
        // 否则自定义网关下整行会空掉。
        // 服务商探测回来的模型清单（按地址缓存）。
        // PresetOptionsFor 是匿名命名空间里的自由函数，够不到窗口成员，所以缓存放这里；
        // 只由 UI 线程读写（探测结果经 PostMessage 回到 UI 线程后才落进来）。
        struct ProbedModels {
            std::wstring url;
            std::vector<std::wstring> models;
        };
        ProbedModels g_probedModels;

        // 「默认模型」这一行的候选：
        //   1) 服务商 /models 真实返回的清单（首选，用户要的就是这个）
        //   2) 地址能反查到内置服务商时，用内置表兜底（离线/自建网关/401 时用得上）
        //   3) 当前值始终保留（可能是手填的，不能因为查不到就把人家填的抹掉）
        std::vector<PresetOption> PresetOptionsFor(const std::string& key,
                                                   const std::wstring& currentBaseUrl,
                                                   const std::wstring& currentValue) {
            std::vector<PresetOption> result;

            if (key == kProviderPresetKey) {
                for (const ProviderPreset& provider : kProviders) {
                    result.push_back({ provider.label, provider.baseUrl });
                }
                return result;
            }

            // Live2D 模型：列出 models 目录里实际导入的模型，供下拉选择
            // （以前这里是手写模型名，换模型得自己敲对文件夹名）
            if (key == "model.default_id") {
                const auto caps = Live2DManager::Instance().ListModelCapabilities();
                for (const auto& cap : caps) {
                    const std::wstring label = Utils::Utf8ToWide(cap.id) +
                        (cap.current ? L" · 当前" : L"");
                    result.push_back({ label, Utils::Utf8ToWide(cap.id) });
                }
                if (result.empty()) {
                    result.push_back({ L"（models 目录里还没有模型）", L"" });
                }
                return result;
            }

            if (key != kModelPresetKey) {
                return result;
            }

            if (!currentBaseUrl.empty() && g_probedModels.url == currentBaseUrl && !g_probedModels.models.empty()) {
                for (const std::wstring& model : g_probedModels.models) {
                    result.push_back({ model, model });
                }
            }
            else {
                const ProviderPreset* provider = FindProviderByUrl(currentBaseUrl);
                if (provider != nullptr) {
                    for (const std::wstring& model : SplitOptions(provider->models)) {
                        result.push_back({ model, model });
                    }
                }
            }

            if (!currentValue.empty()) {
                bool present = false;
                for (const PresetOption& option : result) {
                    if (option.value == currentValue) {
                        present = true;
                        break;
                    }
                }
                if (!present) {
                    result.insert(result.begin(), { currentValue, currentValue });
                }
            }
            return result;
        }

        // ===== 卡片 01：AI 接口与鉴权 =====
        const ControlTerminalWindow::FieldDef kApiFields[] = {
            // 预设行铺满标签右侧，不占用行尾说明位；提示放在卡片副标题里
            { "api.provider_preset", L"服务商预设", L"PROVIDER PRESET", nullptr, FieldKind::Preset, L"", nullptr, false, 0 },
            { "api.base_url",      L"服务地址", L"API BASE URL", L"OpenAI 兼容 /v1", FieldKind::Text, L"https://api.openai.com/v1", nullptr, false, 320 },
            { "api.model_preset",  L"可用模型", L"AVAILABLE MODELS", L"随服务商变化", FieldKind::Preset, L"", nullptr, false, 0 },
            { "api.key",           L"API 密钥", L"API KEY",      L"数据库加密存储",  FieldKind::Text, L"",                          nullptr, true,  320 },
            { "api.org_id",        L"组织标识", L"ORG ID",       L"可留空",          FieldKind::Text, L"",                          nullptr, false, 320 },
            { "api.model_default", L"默认模型", L"MODEL",        L"对话默认模型",    FieldKind::Text, L"gpt-4o",                    nullptr, false, 280 },
            // 截屏巡检要把图片交给能看图的模型；DeepSeek 侧就是 deepseek-flash
            { "api.model_vision",  L"视觉模型", L"VISION MODEL", L"图片分析用；deepseek-flash 可看图", FieldKind::Text, L"deepseek-flash", nullptr, false, 280 },
        };

        // ===== 卡片 02：采样参数 =====
        const ControlTerminalWindow::FieldDef kSamplingFields[] = {
            // 性能：老设备上把帧间隔 / 掩码间隔调大即可明显降低占用；低配模式一键切到省电档
            // 系统弹窗：右下角托盘气泡。嫌吵可整体关掉，或只关掉「每条新消息都弹」
            { "notification.enabled",            L"系统弹窗", L"NOTIFICATIONS", L"右下角气泡提示总开关", FieldKind::Bool, L"true", nullptr, false, 0 },
            { "notification.notify_new_message", L"新消息弹窗", L"NEW MESSAGE POPUP", L"每条新消息都弹一条气泡", FieldKind::Bool, L"true", nullptr, false, 0 },
            { "performance.low_end",             L"低配模式", L"LOW END MODE", L"一键降低帧率与日志开销", FieldKind::Bool,   L"false", nullptr, false, 0 },
            { "performance.frame_interval_ms",   L"帧间隔",   L"FRAME INTERVAL", L"毫秒；100≈10FPS，越大越省", FieldKind::Int, L"100", nullptr, false, 160 },
            { "performance.hitmask_every_frames", L"命中检测间隔", L"HITMASK EVERY", L"每 N 帧读回一次命中掩码", FieldKind::Int, L"3", nullptr, false, 160 },
            { "performance.save_interval_seconds", L"落库间隔", L"SAVE INTERVAL", L"秒；状态写库的间隔", FieldKind::Int, L"30", nullptr, false, 160 },
            { "sampling.temperature",       L"温度",     L"TEMPERATURE",      L"0 – 2",   FieldKind::Double, L"1.0", nullptr, false, 150 },
            { "sampling.top_p",             L"核采样",   L"TOP P",            L"0 – 1",   FieldKind::Double, L"1.0", nullptr, false, 150 },
            { "sampling.frequency_penalty", L"频率惩罚", L"FREQ PENALTY",     L"-2 – 2",  FieldKind::Double, L"0.0", nullptr, false, 150 },
            { "sampling.presence_penalty",  L"存在惩罚", L"PRESENCE PENALTY", L"-2 – 2",  FieldKind::Double, L"0.0", nullptr, false, 150 },
        };

        // ===== 卡片 03：流式与超时 =====
        const ControlTerminalWindow::FieldDef kStreamFields[] = {
            { "stream.enabled",         L"流式输出", L"STREAM",  L"逐字返回", FieldKind::Bool, L"true", nullptr, false, 0 },
            { "stream.timeout_seconds", L"请求超时", L"TIMEOUT", L"秒",       FieldKind::Int,  L"30",   nullptr, false, 150 },
        };

        // ===== 卡片 04：上下文 =====
        const ControlTerminalWindow::FieldDef kContextFields[] = {
            { "context.max_tokens", L"上下文上限", L"MAX TOKENS", L"token", FieldKind::Int, L"4096", nullptr, false, 150 },
            { "context.max_rounds", L"记忆轮数",   L"MAX ROUNDS", L"轮",    FieldKind::Int, L"30",   nullptr, false, 150 },
        };

        // ===== 卡片 05：主动触发 =====
        const ControlTerminalWindow::FieldDef kActiveFields[] = {
            { "active.interval_process",    L"进程巡检",     L"PROCESS",     L"秒 · -1 关闭", FieldKind::Int,  L"60",   nullptr, false, 150 },
            { "active.interval_weather",    L"天气刷新",     L"WEATHER",     L"秒 · -1 关闭", FieldKind::Int,  L"300",  nullptr, false, 150 },
            { "active.interval_screenshot", L"截屏巡检",     L"SCREENSHOT",  L"秒 · -1 关闭", FieldKind::Int,  L"1800", nullptr, false, 150 },
            // 小范围操控与回复吸附：紧跟在截屏巡检之后
            { "tool.mouse_enabled",    L"鼠标拖动", L"MOUSE NUDGE",  L"允许她小范围挪动鼠标", FieldKind::Bool, L"true", nullptr, false, 0 },
            { "tool.mouse_max_pixels", L"鼠标幅度", L"MOUSE RANGE",  L"像素 · 单次最大位移", FieldKind::Int,  L"120",  nullptr, false, 120 },
            { "tool.window_enabled",   L"窗口移动", L"WINDOW MOVE",  L"允许她小范围挪动窗口", FieldKind::Bool, L"true", nullptr, false, 0 },
            { "tool.window_max_pixels",L"窗口幅度", L"WINDOW RANGE", L"像素 · 单次最大位移", FieldKind::Int,  L"80",   nullptr, false, 120 },
            { "tool.keyboard_enabled", L"键盘输入", L"KEY INPUT",    L"允许她打字符（功能键除外）", FieldKind::Bool, L"true", nullptr, false, 0 },
            { "ui.reply_anchor",       L"回复吸附", L"REPLY ANCHOR", L"气泡贴到哪儿", FieldKind::Preset, L"window", L"吸附窗口=window|吸附鼠标=cursor", false, 160 },
            { "ui.reply_channel", L"回复渠道", L"REPLY CHANNEL", L"气泡回话 或 右下角系统弹窗", FieldKind::Preset, L"bubble", L"气泡回话=bubble|系统弹窗回话=system", false, 200 },
            { "active.cooldown_seconds",    L"全局冷却",     L"COOLDOWN",    L"秒",           FieldKind::Int,  L"480",  nullptr, false, 150 },
            { "active.ip_check_on_start",   L"启动时查 IP",  L"IP ON START", L"联网检测",     FieldKind::Bool, L"true", nullptr, false, 0 },
            { "amap.api_key",               L"高德密钥",     L"AMAP KEY",    L"天气服务",     FieldKind::Text, L"",     nullptr, true,  320 },
        };

        // ===== 卡片 06：模型与外观 =====
        const ControlTerminalWindow::FieldDef kModelFields[] = {
            { "model.default_id",      L"默认模型", L"MODEL", L"从已导入的模型里选", FieldKind::Preset, L"Haru", nullptr, false, 0 },
            { "model.default_scale",   L"缩放",     L"SCALE",    L"倍",          FieldKind::Double, L"1.0",  nullptr, false, 150 },
            { "model.default_opacity", L"不透明度", L"OPACITY",  L"0 – 1",       FieldKind::Double, L"1.0",  nullptr, false, 150 },
        };

        // ===== 卡片 07：行为开关 =====
        const ControlTerminalWindow::FieldDef kBehaviorFields[] = {
            { "feature.always_on_top",       L"窗口置顶",     L"ALWAYS ON TOP", L"压住其他窗口", FieldKind::Bool, L"true",   nullptr,              false, 0 },
            { "feature.click_through",       L"鼠标穿透",     L"CLICK THROUGH", L"不接收点击",   FieldKind::Bool, L"true",   nullptr,              false, 0 },
            { "feature.drag_enabled",        L"允许拖拽",     L"DRAG ENABLED",  L"按住拖动",     FieldKind::Bool, L"true",   nullptr,              false, 0 },
            { "feature.auto_hide_fullscreen", L"全屏自动隐藏", L"AUTO HIDE",     L"观影游戏时",   FieldKind::Bool, L"true",   nullptr,              false, 0 },
            { "ui.theme",                    L"界面主题",     L"UI THEME",      L"跟随系统",     FieldKind::Enum, L"system", L"system|dark|light", false, 0 },
        };

        // ===== 卡片 08：附件与日志 =====
        const ControlTerminalWindow::FieldDef kStorageFields[] = {
            { "attachment.max_size_mb",    L"单件上限", L"MAX SIZE",    L"MB", FieldKind::Int, L"20",  nullptr, false, 150 },
            { "attachment.total_quota_mb", L"总配额",   L"TOTAL QUOTA", L"MB", FieldKind::Int, L"500", nullptr, false, 150 },
            { "attachment.retention_days", L"附件保留", L"RETENTION",   L"天", FieldKind::Int, L"30",  nullptr, false, 150 },
            { "logs.retention_days",       L"日志保留", L"LOG RETENTION", L"天", FieldKind::Int, L"30", nullptr, false, 150 },
        };

        // ===== 卡片 09：代理与更新 =====
        const ControlTerminalWindow::FieldDef kNetworkFields[] = {
            { "proxy.type",                  L"代理类型",     L"PROXY TYPE", L"出网方式",   FieldKind::Enum, L"none", L"none|http|socks5", false, 0 },
            { "proxy.host",                  L"代理主机",     L"PROXY HOST", L"可留空",     FieldKind::Text, L"",     nullptr,            false, 260 },
            { "proxy.port",                  L"代理端口",     L"PROXY PORT", L"0 = 不使用", FieldKind::Int,  L"0",    nullptr,            false, 150 },
            { "update.auto_check",           L"自动检查更新", L"AUTO UPDATE", L"启动时静默", FieldKind::Bool, L"true", nullptr,           false, 0 },
            { "update.check_interval_hours", L"检查间隔",     L"INTERVAL",   L"小时",       FieldKind::Int,  L"24",   nullptr,            false, 150 },
        };

        // ===== 卡片 10：快速模式 =====
        // quick.mode 的 options 留空：它的选项由 OptionsFor 在运行时给出
        // （只有哨兵文件存在时才有「？？？」），字段表里刻意不写死。
        const ControlTerminalWindow::FieldDef kQuickModeFields[] = {
            { "quick.enabled",  L"快速模式",   L"QUICK MODE", L"关闭 Live2D，仅显示圆点", FieldKind::Bool,  L"false", nullptr, false, 0 },
            { "quick.mode",     L"人格模式",   L"PERSONA",    L"？？？ 为实验性人格",     FieldKind::Enum,  L"关闭",  nullptr, false, 0 },
            { "prompt.enabled", L"启用提示词", L"PROMPTS",    L"prompts 目录 · 可多选",   FieldKind::Multi, L"",      nullptr, false, 0 },
        };

        // ===== 卡片 11：语音合成（联网 TTS，零本地模型） =====
        // voice.speaker 的选项由 OptionsFor 在运行时给出（VoiceService::SpeakerLabels），
        // 这里留空，避免角色表写成两份。
        const ControlTerminalWindow::FieldDef kVoiceFields[] = {
            { "voice.enabled",       L"语音朗读", L"VOICE",       L"在线 VOICEVOX · 不占本地体积", FieldKind::Bool,   L"false", nullptr, false, 0 },
            { "voice.backend",       L"合成后端", L"BACKEND",     L"联网，零本地模型",             FieldKind::Enum,   L"在线 VOICEVOX", L"本地 Kokoro（英文）|在线 VOICEVOX|ElevenLabs|关闭", false, 0 },
            { "voice.speaker",       L"角色",     L"SPEAKER",     L"VOICEVOX 声库",                FieldKind::Enum,   L"ずんだもん", nullptr, false, 0 },
            { "voice.speak_mode",    L"朗读范围", L"SPEAK MODE",  L"自动 = 每句都念",              FieldKind::Enum,   L"自动", L"自动|仅标签|关闭", false, 0 },
    { "voice.translate",     L"朗读前翻译", L"TRANSLATE",   L"中文回复先翻成英文再念（英文音色需要）", FieldKind::Bool, L"true", L"", false, 0 },
            { "voice.speed",         L"语速",     L"SPEED",       L"1.0 = 原速",                   FieldKind::Double, L"1.0", nullptr, false, 150 },
            { "voice.pitch",         L"音高",     L"PITCH",       L"半音偏移",                     FieldKind::Double, L"0.0", nullptr, false, 150 },
            { "voice.intonation",    L"语调",     L"INTONATION",  L"情绪起伏强度",                 FieldKind::Double, L"1.0", nullptr, false, 150 },
            { "voice.volume",        L"音量",     L"VOLUME",      L"合成侧增益",                   FieldKind::Double, L"1.0", nullptr, false, 150 },
            { "voice.lip_sync",      L"口型同步", L"LIP SYNC",    L"随语音开合嘴巴",               FieldKind::Bool,   L"true", nullptr, false, 0 },
            { "voice.lip_sync_gain", L"口型幅度", L"LIP GAIN",    L"越大嘴张越开",                 FieldKind::Double, L"1.6", nullptr, false, 150 },
            { "voice.max_chars",     L"单句上限", L"MAX CHARS",   L"超过自动断句",                 FieldKind::Int,    L"120", nullptr, false, 150 },
            { "voice.test_text",     L"试听文本", L"TEST TEXT",   L"点下面按钮试听",               FieldKind::Text,   L"你好，我是桌宠。", nullptr, false, 240 },
            { "voice.test",          L"试听",     L"TEST",        L"合成一句听听",                 FieldKind::Action, L"", nullptr, false, 0 },
        };
        const ControlTerminalWindow::SectionDef kSections[] = {
            { L"01", L"AI · 接口与鉴权", L"ENDPOINT", L"点选服务商预设填入地址，可用模型随服务商刷新；密钥写入本地数据库。", kApiFields, 6 },
            { L"02", L"生成 · 采样参数", L"SAMPLING", L"温度与惩罚项直接决定回复的稳定度与发散度。", kSamplingFields, 4 },
            { L"03", L"流式与超时", L"STREAM", L"流式返回与单次请求的超时上限。", kStreamFields, 2 },
            { L"04", L"上下文与记忆", L"CONTEXT", L"送入模型的历史窗口上限。", kContextFields, 2 },
            { L"05", L"主动触发", L"PROACTIVE", L"无输入时按间隔主动唤起，冷却时间用于互相抑制。", kActiveFields, 6 },
            { L"06", L"模型与外观", L"LIVE2D", L"桌面模型的默认姿态与不透明度。", kModelFields, 3 },
            { L"07", L"行为开关", L"BEHAVIOR", L"窗口层级、穿透与整屏隐藏策略。", kBehaviorFields, 5 },
            { L"08", L"附件与日志", L"STORAGE", L"附件配额与本地保留周期。", kStorageFields, 4 },
            { L"09", L"代理与更新", L"NETWORK", L"出网代理与自动更新检查。", kNetworkFields, 5 },
            { L"10", L"快速模式", L"QUICK MODE", L"关闭 Live2D，只在桌面上留一个可交互的圆点；「？？？」是实验性人格，仅当 prompts 目录里存在 yandere.chr 时可选。", kQuickModeFields, 3 },
            { L"11", L"语音合成", L"VOICE", L"本地 Kokoro（英文）零网络延迟；在线 VOICEVOX 负责日语。中文回复先翻译再朗读。", kVoiceFields, 14 },        };

        const int kSectionCount = static_cast<int>(sizeof(kSections) / sizeof(kSections[0]));

        // WebView2 看门狗：控制器创建成功后页面仍可能永远不加载（渲染进程崩掉是常见形态，
        // 此时 NavigationCompleted 根本不会来）。到点还没就绪就按「不可用」处理。
        const UINT_PTR kWebViewWatchdogTimer = 0x5721;
        const UINT kWebViewWatchdogMs = 8000;

        // 启动失败重试：WebView2 浏览器进程偶发「启动即崩」（换一个干净的用户数据
        // 目录往往就好了），所以失败后换目录再建，全部失败才降级成只看桌宠。
        const UINT_PTR kWebViewRetryTimer = 0x5722;
        const UINT kWebViewRetryMs = 600;
        const int kMaxWebViewAttempts = 3;

        // 记住哪种用户数据目录在这台机器上能用（"appdata" / "program"），
        // 下次启动直接用它，省掉一次约 4 秒的失败重试。
        const char* const kWebViewProfileKey = "webview2.profile_location";

        // 标准位置：%LOCALAPPDATA%\Unnamedbot\WebView2
        std::wstring AppDataWebViewProfileDir() {
            std::wstring dir;
            wchar_t buffer[MAX_PATH] = {};
            const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
            if (length > 0 && length < MAX_PATH) dir.assign(buffer, length);
            if (dir.empty()) {
                LOG_WARN("ControlTerminal", "LOCALAPPDATA 不可用，WebView2 用户目录回退到当前目录。");
                dir = L".";
            }
            dir += L"\\Unnamedbot\\WebView2";
            return dir;
        }

        // 字段类型 → 页面控件类型
        const char* KindName(FieldKind kind) {
            switch (kind) {
            case FieldKind::Int:    return "int";
            case FieldKind::Double: return "double";
            case FieldKind::Bool:   return "bool";
            case FieldKind::Enum:   return "enum";
            case FieldKind::Preset: return "preset";
        case FieldKind::Action: return "action";
            case FieldKind::Multi:  return "multi";
            default:                return "text";
            }
        }

    } // namespace

    // ============================================================
    // 生命周期
    // ============================================================

    ControlTerminalWindow& ControlTerminalWindow::Instance() {
        // 不随静态存储期销毁：进程退出阶段仍可能被调用（见 AppCore::Instance）。
        // 资源释放由 Shutdown() 完成。
        static ControlTerminalWindow* instance = new ControlTerminalWindow();
        return *instance;
    }

    bool ControlTerminalWindow::Initialize(HINSTANCE hInstance) {
        if (hwnd_) return true;
        instance_ = hInstance;

        HDC screen = GetDC(nullptr);
        dpi_ = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        if (dpi_ <= 0) dpi_ = 96;

        if (!RegisterWindowClass(hInstance)) return false;
        if (!CreateTerminalWindow(hInstance)) return false;

        LoadFromConfig();
        // WebView2 是异步创建：先建宿主窗口，环境/控制器就绪后自动导航到界面。
        CreateWebView();

        LOG_INFO("ControlTerminal", "Control terminal window initialized.");
        return true;
    }

    void ControlTerminalWindow::Shutdown() {
        if (!hwnd_) return;
        DestroyWebView();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        visible_ = false;
        LOG_INFO("ControlTerminal", "Control terminal window destroyed.");
    }

    void ControlTerminalWindow::OnSaved(std::function<void()> callback) {
        onSaved_ = std::move(callback);
    }

    void ControlTerminalWindow::OnWebViewFailed(std::function<void()> callback) {
        onWebViewFailed_ = std::move(callback);
    }

    void ControlTerminalWindow::OnPetSlot(std::function<void(bool, int, int, int, int)> callback) {
        onPetSlot_ = std::move(callback);
    }

    void ControlTerminalWindow::NotifyWebViewFailed(const char* reason) {
        // 只通知一次：环境失败、控制器失败、看门狗可能先后都触发，降级只该做一次。
        if (webViewFailed_) return;
        webViewFailed_ = true;
        if (hwnd_) {
            KillTimer(hwnd_, kWebViewWatchdogTimer);
            KillTimer(hwnd_, kWebViewRetryTimer);
        }
        LOG_ERROR("ControlTerminal", std::string("WebView2 不可用（") + (reason ? reason : "unknown") +
                                     "），配置界面无法显示，将降级为直接显示桌宠。");
        if (onWebViewFailed_) onWebViewFailed_();
    }

    void ControlTerminalWindow::Show(bool show) {
        if (controller_) {
            // 与 Hide() 里的 put_IsVisible(FALSE) 配对
            controller_->put_IsVisible(show ? TRUE : FALSE);
        }
        if (!hwnd_) return;
        if (!show) {
            Hide();
            return;
        }

        // 每次唤起都重新读库：外部（托盘菜单、热键）改过配置后这里要跟上。
        if (!visible_) {
            LoadFromConfig();
        }

        ShowWindow(hwnd_, SW_SHOWNORMAL);
        SetForegroundWindow(hwnd_);
        visible_ = true;
        ResizeWebView();

        // 页面还没就绪时先记下，等导航完成再补发，避免状态丢失。
        if (webviewReady_) {
            SendState(true);
        }
        else {
            statePending_ = true;
        }
        LOG_DEBUG("ControlTerminal", "Control terminal shown.");
    }

    void ControlTerminalWindow::Hide() {
        if (onPetSlot_) onPetSlot_(false, 0, 0, 0, 0);
        if (!hwnd_) return;
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        // 中枢关着的时候没必要让 WebView2 继续渲染：暂停它的可见性，
        // Chromium 会停下来（CPU 与内存都降下来）。下次打开再恢复。
        if (controller_) {
            controller_->put_IsVisible(FALSE);
        }
    }

    void ControlTerminalWindow::Toggle() {
        if (visible_) {
            Hide();
        }
        else {
            Show(true);
        }
    }

    int ControlTerminalWindow::Scale(int value) const {
        return MulDiv(value, dpi_, 96);
    }

    // ============================================================
    // 窗口
    // ============================================================

    bool ControlTerminalWindow::RegisterWindowClass(HINSTANCE hInstance) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ControlTerminalWindow::WindowProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = WINDOW_CLASS_NAME;
        return RegisterClassExW(&wc) != 0;
    }

    bool ControlTerminalWindow::CreateTerminalWindow(HINSTANCE hInstance) {
        // 窗口尺寸可拖动调整：上次的尺寸记在配置里（CSS px），并夹进合理范围，
        // 免得某些历史值把窗口开成一条缝。
        int cssWidth = ConfigManager::Instance().GetInt("window.control_width", kWinW);
        int cssHeight = ConfigManager::Instance().GetInt("window.control_height", kWinH);
        cssWidth = std::clamp(cssWidth, 960, 2200);
        cssHeight = std::clamp(cssHeight, 640, 1600);
        const int width = Scale(cssWidth);
        const int height = Scale(cssHeight);

        int x = 0, y = 0;
        RECT workArea = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0)) {
            x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
            y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
        }

        // 不加 WS_EX_TOOLWINDOW：工具窗口不会出现在任务栏，用户就找不到这个窗口，
        // 只能靠托盘菜单召回来。配置终端是程序的主界面，应该像普通窗口一样占据
        // 一个任务栏按钮，并且在桌宠之后也能一键切换回来。
        hwnd_ = CreateWindowExW(
            0,
            WINDOW_CLASS_NAME,
            L"控制终端",
            // WS_THICKFRAME：四边/四角可拖动改变窗口大小（无标题栏风格保持不变）
            WS_POPUP | WS_CLIPCHILDREN | WS_THICKFRAME,
            x, y, width, height,
            nullptr, nullptr, hInstance, this);

        if (!hwnd_) {
            LOG_ERROR("ControlTerminal", "Failed to create control terminal window.");
            return false;
        }

        // 任务栏与 Alt+Tab 使用窗口图标；没设过时这里会是空白方块。
        HICON appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
        if (appIcon) {
            SendMessageW(hwnd_, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(appIcon));
            SendMessageW(hwnd_, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(appIcon));
        }
        return true;
    }

    LRESULT CALLBACK ControlTerminalWindow::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        ControlTerminalWindow* self = nullptr;

        if (msg == WM_NCCREATE) {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<ControlTerminalWindow*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            if (self) self->hwnd_ = hwnd;
        }
        else {
            self = reinterpret_cast<ControlTerminalWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
        return self->HandleMessage(hwnd, msg, wParam, lParam);
    }

    LRESULT ControlTerminalWindow::HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
        case WM_SIZE:
            ResizeWebView();
            return 0;

        case WM_NCHITTEST: {
            // 先交给系统判非客户区：WS_THICKFRAME 的四边/四角拉伸热区靠它。
            const LRESULT sysHit = DefWindowProcW(hwnd, msg, wParam, lParam);
            if (sysHit == HTLEFT || sysHit == HTRIGHT || sysHit == HTTOP || sysHit == HTBOTTOM ||
                sysHit == HTTOPLEFT || sysHit == HTTOPRIGHT || sysHit == HTBOTTOMLEFT ||
                sysHit == HTBOTTOMRIGHT || sysHit == HTBORDER) {
                return sysHit;
            }
            // WebView2 占满客户区，页面内的顶栏拖动由页面通知宿主（见 HandleWebMessage）。
            // 页面尚未就绪时留一条原生拖动路径，避免窗口完全无法移动。
            if (!webviewReady_) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);
                if (pt.y >= 0 && pt.y < Scale(104)) return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_GETMINMAXINFO: {
            // 最小尺寸：再小页面就没法看了
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            if (mmi) {
                mmi->ptMinTrackSize.x = Scale(960);
                mmi->ptMinTrackSize.y = Scale(640);
            }
            return 0;
        }

        case WM_EXITSIZEMOVE: {
            // 拖完就记下来，下次开窗直接是这个尺寸
            RECT rc = {};
            if (GetWindowRect(hwnd, &rc)) {
                // dpi_ 是物理 DPI（96 = 100%），换回逻辑像素再存
                int cssW = MulDiv(rc.right - rc.left, 96, dpi_);
                int cssH = MulDiv(rc.bottom - rc.top, 96, dpi_);
                ConfigManager::Instance().SetInt("window.control_width", cssW);
                ConfigManager::Instance().SetInt("window.control_height", cssH);
            }
            ResizeWebView();
            return 0;
        }

        case WM_CLOSE:
            Hide();
            return 0;

        // 工作线程请求刷新会话：回到窗口线程再碰 WebView2
        case WM_APP_PUSH_CHAT:
            SendChatHistory();
            return 0;

        // 服务商探测完成：在 UI 线程上推送结果并刷新模型行
        case WM_APP_PROBE_DONE:
            FinishProbe();
            return 0;

        // 看门狗到点：页面始终没就绪 → 按不可用处理，交给 AppCore 降级
        case WM_TIMER:
            if (wParam == kWebViewWatchdogTimer) {
                KillTimer(hwnd, kWebViewWatchdogTimer);
                if (!webviewReady_) NotifyWebViewFailed("watchdog timeout: page never became ready");
                return 0;
            }
            // 重试定时器：上一次环境/控制器创建失败，延时换新目录再建一次
            if (wParam == kWebViewRetryTimer) {
                KillTimer(hwnd, kWebViewRetryTimer);
                CreateWebView();
                return 0;
            }
            break;

        case WM_DESTROY:
            DestroyWebView();
            hwnd_ = nullptr;
            return 0;

        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    // ============================================================
    // WebView2 宿主
    // ============================================================

    std::wstring ControlTerminalWindow::WebViewUserDataDir(int attempt, bool* usedProgramDir) const {
        if (usedProgramDir) *usedProgramDir = false;

        // 程序目录下的 profile 作为第二候选：有些机器上 WebView2 在 LOCALAPPDATA 里
        // 起不来（浏览器进程启动即崩），换到程序目录就好了。
        std::error_code ec;
        const std::filesystem::path programDir = Utils::ResolveAppRelative(L"webview2-profile");
        std::filesystem::create_directories(programDir, ec);
        const bool programUsable = !ec;
        if (!programUsable) {
            LOG_WARN("ControlTerminal", "程序目录不可写，WebView2 只用用户目录：" + ec.message());
            ec.clear();
        }

        const bool preferProgram =
            ConfigManager::Instance().GetString(kWebViewProfileKey, "appdata") == "program";

        const std::wstring appDataDir = AppDataWebViewProfileDir();
        std::vector<std::pair<std::wstring, bool>> candidates;
        if (programUsable && preferProgram) candidates.emplace_back(programDir.wstring(), true);
        candidates.emplace_back(appDataDir, false);
        if (programUsable && !preferProgram) candidates.emplace_back(programDir.wstring(), true);

        if (attempt >= 0 && static_cast<size_t>(attempt) < candidates.size()) {
            const auto& chosen = candidates[static_cast<size_t>(attempt)];
            if (usedProgramDir) *usedProgramDir = chosen.second;
            return chosen.first;
        }
        // 候选目录都试过还没成功：退回带序号的用户目录，避开崩溃残留的进程/锁文件。
        // 注意 windows.h 的 max 宏会吃掉 std::max，这里直接手写。
        const int suffix = attempt > 2 ? attempt : 2;
        return appDataDir + L"_retry" + std::to_wstring(suffix);
    }

    void ControlTerminalWindow::ScheduleWebViewRetry(const std::string& reason) {
        if (webViewFailed_) return;
        if (hwnd_) KillTimer(hwnd_, kWebViewWatchdogTimer);

        // 清掉这次失败留下的半成品，CreateWebView 的入口守卫才不会直接返回。
        if (webview_) {
            if (messageToken_.value != 0) webview_->remove_WebMessageReceived(messageToken_);
            if (navigationToken_.value != 0) webview_->remove_NavigationCompleted(navigationToken_);
        }
        messageToken_ = {};
        navigationToken_ = {};
        if (controller_) controller_->Close();
        controller_.Reset();
        webview_.Reset();
        environment_.Reset();
        webviewReady_ = false;

        if (hwnd_ && webViewAttempt_ + 1 < kMaxWebViewAttempts) {
            ++webViewAttempt_;
            bool nextIsProgram = false;
            const std::wstring nextDir = WebViewUserDataDir(webViewAttempt_, &nextIsProgram);
            LOG_WARN("ControlTerminal", reason + "（第 " + std::to_string(webViewAttempt_) +
                                          " 次失败），改用用户数据目录 " + Utils::WideToUtf8(nextDir) + " 重试。");
            SetTimer(hwnd_, kWebViewRetryTimer, kWebViewRetryMs, nullptr);
            return;
        }
        NotifyWebViewFailed(reason.c_str());
    }

    void ControlTerminalWindow::CreateWebView() {
        if (!hwnd_ || environment_ || controller_) return;

        // 用户数据目录优先放在 LOCALAPPDATA（不污染程序目录）；个别机器上 WebView2 在
        // 那里起不来，第二次尝试改用程序目录下的 profile（见 WebViewUserDataDir）。
        const std::wstring userDataDir = WebViewUserDataDir(webViewAttempt_, &webViewProfileIsProgram_);

        const HRESULT hrEnv = CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            userDataDir.c_str(),
            nullptr,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Environment* createdEnvironment) -> HRESULT {
                    if (FAILED(result) || createdEnvironment == nullptr) {
                        wchar_t buf[96] = {};
                        swprintf_s(buf, L"HRESULT=0x%08X env=%p", static_cast<unsigned>(result), static_cast<void*>(createdEnvironment));
                        std::string why = "环境创建失败 " + Utils::WideToUtf8(buf);
                        ScheduleWebViewRetry(why);
                        return S_OK;
                    }
                    environment_ = createdEnvironment;

                    const HRESULT hrController = environment_->CreateCoreWebView2Controller(
                        hwnd_,
                        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT result2, ICoreWebView2Controller* createdController) -> HRESULT {
                                if (FAILED(result2) || createdController == nullptr) {
                                    wchar_t buf[96] = {};
                                    swprintf_s(buf, L"HRESULT=0x%08X ctrl=%p hwnd=%p", static_cast<unsigned>(result2), static_cast<void*>(createdController), static_cast<void*>(hwnd_));
                                    std::string why = "控制器创建失败 " + Utils::WideToUtf8(buf);
                                    ScheduleWebViewRetry(why);
                                    return S_OK;
                                }
                                controller_ = createdController;

                                // 记下这台机器上能用的目录类型，下次启动直接命中，省一轮 4 秒失败。
                                const char* usedKind = webViewProfileIsProgram_ ? "program" : "appdata";
                                if (ConfigManager::Instance().GetString(kWebViewProfileKey, "appdata") != usedKind) {
                                    ConfigManager::Instance().SetString(kWebViewProfileKey, usedKind);
                                    LOG_INFO("ControlTerminal", std::string("WebView2 profile location pinned to: ") + usedKind);
                                }

                                // 窗口底色与应用一致，避免初始化瞬间闪白。
                                ComPtr<ICoreWebView2Controller2> controller2;
        // 窗口顶部那条白边：Win11 会给标准窗口画一圈系统色边框，
        // 深色界面下就是一条亮线。把 DWM 边框与标题栏压成暗色（Win10 上是空操作）。
        {
            const COLORREF darkBorder = RGB(10, 3, 6);
            DwmSetWindowAttribute(hwnd_, 34 /* DWMWA_BORDER_COLOR */, &darkBorder, sizeof(darkBorder));
            DwmSetWindowAttribute(hwnd_, 35 /* DWMWA_CAPTION_COLOR */, &darkBorder, sizeof(darkBorder));
        }

                                if (SUCCEEDED(controller_.As(&controller2)) && controller2) {
                                    const COREWEBVIEW2_COLOR background = { 0xFF, 0x06, 0x0b, 0x19 };
                                    controller2->put_DefaultBackgroundColor(background);
                                }

                                ResizeWebView();
                                controller_->put_IsVisible(TRUE);

                                if (SUCCEEDED(controller_->get_CoreWebView2(&webview_)) && webview_) {
                                    ConfigureWebViewSettings();
                                    NavigateToUi();
                                }
                                return S_OK;
                            }).Get());

                    if (FAILED(hrController)) {
                        NotifyWebViewFailed("控制器创建调用失败");
                    }
                    return S_OK;
                }).Get());

        if (FAILED(hrEnv)) {
            NotifyWebViewFailed("环境创建调用失败（可能未安装 WebView2 Runtime）");
        }

        // 控制器就连上了，页面也可能永远加载不出来（渲染进程崩溃时不发 NavigationCompleted），
        // 所以再挂一个看门狗，到点仍不就绪同样按不可用处理。
        if (!webViewFailed_ && hwnd_) {
            SetTimer(hwnd_, kWebViewWatchdogTimer, kWebViewWatchdogMs, nullptr);
        }
    }

    void ControlTerminalWindow::ConfigureWebViewSettings() {
        if (!webview_) return;

        // 主页开场素材（P3R 风格视频与徽标）走虚拟域名，避免把数 MB 的 mp4 塞进 C++ 字面量。
        // 素材目录 = 可执行文件旁的 webui\（构建时由 PostBuild 从 Unnamedbot\webui 复制）。
        ComPtr<ICoreWebView2_3> webview3;
        if (SUCCEEDED(webview_.As(&webview3)) && webview3) {
            const std::filesystem::path assetDir = Utils::ResolveAppRelative(L"webui");
            std::error_code ec;
            std::filesystem::create_directories(assetDir, ec);
            const HRESULT hrMap = webview3->SetVirtualHostNameToFolderMapping(
                L"p3r.local", assetDir.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
            if (FAILED(hrMap)) {
                LOG_WARN("ControlTerminal", "Web UI 素材目录映射失败，主页开场将退回纯 CSS 背景。");
            }
        }

        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(webview_->get_Settings(&settings)) && settings) {
            settings->put_IsScriptEnabled(TRUE);
            settings->put_IsWebMessageEnabled(TRUE);
            settings->put_AreDefaultScriptDialogsEnabled(FALSE);
            // 配置界面不是浏览器：去掉右键菜单、状态栏与缩放控件，避免露出网页痕迹。
            settings->put_AreDefaultContextMenusEnabled(FALSE);
            settings->put_IsStatusBarEnabled(FALSE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreDevToolsEnabled(FALSE);
        }

        webview_->add_WebMessageReceived(
            Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2* /*sender*/, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    // get_WebMessageAsJson 返回 CoTaskMem 字符串，需手动释放。
                    LPWSTR raw = nullptr;
                    if (FAILED(args->get_WebMessageAsJson(&raw)) || raw == nullptr) return S_OK;
                    HandleWebMessage(Utils::WideToUtf8(raw));
                    CoTaskMemFree(raw);
                    return S_OK;
                }).Get(),
            &messageToken_);

        webview_->add_NavigationCompleted(
            Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2* /*sender*/, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    BOOL success = FALSE;
                    if (args) args->get_IsSuccess(&success);
                    if (!success) {
                        NotifyWebViewFailed("配置界面导航失败");
                        return S_OK;
                    }
                    if (hwnd_) KillTimer(hwnd_, kWebViewWatchdogTimer);
                    webviewReady_ = true;
                    // 页面就绪后补发一次完整状态（首次进入也从顶部开始）
                    SendState(true);
                    statePending_ = false;
                    return S_OK;
                }).Get(),
            &navigationToken_);
    }

    void ControlTerminalWindow::NavigateToUi() {
        if (!webview_) return;
        const std::wstring html = Utils::Utf8ToWide(ConfigWebUiHtml());
        const HRESULT hr = webview_->NavigateToString(html.c_str());
        if (FAILED(hr)) {
            NotifyWebViewFailed("配置界面装载失败");
        }
    }

    void ControlTerminalWindow::ResizeWebView() {
        if (!controller_ || !hwnd_) return;
        RECT bounds = {};
        GetClientRect(hwnd_, &bounds);
        controller_->put_Bounds(bounds);
    }

    void ControlTerminalWindow::DestroyWebView() {
        if (hwnd_) {
            KillTimer(hwnd_, kWebViewWatchdogTimer);
            KillTimer(hwnd_, kWebViewRetryTimer);
        }
        if (webview_) {
            if (messageToken_.value != 0) webview_->remove_WebMessageReceived(messageToken_);
            if (navigationToken_.value != 0) webview_->remove_NavigationCompleted(navigationToken_);
        }
        messageToken_ = {};
        navigationToken_ = {};

        if (controller_) {
            controller_->Close();
        }
        controller_.Reset();
        webview_.Reset();
        environment_.Reset();

        webviewReady_ = false;
        statePending_ = false;
    }

    // ============================================================
    // 与页面之间的契约
    // ============================================================

    std::string ControlTerminalWindow::BuildSchemaJson() const {
        nlohmann::json sections = nlohmann::json::array();
        for (int s = 0; s < kSectionCount; ++s) {
            const SectionDef& sec = kSections[s];
            nlohmann::json fields = nlohmann::json::array();
            for (int i = 0; i < sec.fieldCount; ++i) {
                const FieldDef& def = sec.fields[i];
                nlohmann::json field = {
                    { "key", def.key ? def.key : "" },
                    { "label", ToUtf8(def.label) },
                    { "labelEn", ToUtf8(def.labelEn) },
                    { "note", ToUtf8(def.note) },
                    { "kind", KindName(def.kind) },
                    { "options", OptionsTextFor(def) },
                    { "secret", def.secret },
                    { "controlWidth", def.controlWidth },
                };
                // 预设行的写入目标键：页面据此判断哪一格是选中态。
                if (def.kind == FieldKind::Preset) {
                    field["presetTarget"] = PresetTargetKeyFor(def);
                }
                // 依赖行：父开关取不到指定值时，页面把整行隐藏。
                std::string requiresKey;
                std::string requiresValue;
                if (RequiresFor(def, requiresKey, requiresValue)) {
                    field["requires"] = { { "key", requiresKey }, { "value", requiresValue } };
                }
                // 人设锁：「？？？」生效期间这一行只读。页面据此禁用控件并给出
                // 「要换人设请先删掉 prompts/yandere.chr」的说明。
                if (KeyIs(def, ConfigManager::KEY_QUICK_MODE) && IsPersonaFrozen()) {
                    field["locked"] = true;
                }
                // 数值行带上区间：页面拿它画滑块，不用自己猜默认范围。
                if (def.kind == FieldKind::Int || def.kind == FieldKind::Double) {
                    const ValueRange range = RangeFor(def);
                    field["min"] = range.min;
                    field["max"] = range.max;
                    field["step"] = range.step;
                }
                fields.push_back(field);
            }
            sections.push_back({
                { "no", ToUtf8(sec.no) },
                { "title", ToUtf8(sec.title) },
                { "titleEn", ToUtf8(sec.titleEn) },
                { "sub", ToUtf8(sec.sub) },
                { "fields", fields },
            });
        }
        return sections.dump();
    }

    std::string ControlTerminalWindow::BuildValuesJson() const {
        nlohmann::json values = nlohmann::json::object();
        for (const auto& pair : draft_) {
            values[pair.first] = pair.second;
        }
        return values.dump();
    }

    std::string ControlTerminalWindow::BuildPresetsJson() const {
        const std::wstring baseUrl = CurrentValueText(ConfigManager::KEY_API_BASE_URL.c_str());
        const std::wstring model = CurrentValueText(ConfigManager::KEY_API_MODEL_DEFAULT.c_str());

        nlohmann::json presets = nlohmann::json::object();
        // 预设/多选行共用的选项通道：每行的取值来源不同，所以逐行判断。
        // 键名必须按内容比较：指针比较依赖字面量合并，不可靠。
        const std::vector<std::string> keys = { kProviderPresetKey, kModelPresetKey, ConfigManager::KEY_PROMPT_ENABLED };
        for (const std::string& key : keys) {
            std::vector<PresetOption> options;
            if (key == kModelPresetKey) {
                // 「可用模型」按当前服务地址反查服务商，所以要吃这两个上下文值。
                options = PresetOptionsFor(key, baseUrl, model);
            }
            else if (key == ConfigManager::KEY_PROMPT_ENABLED) {
                // 提示词多选行的选项就是 prompts 目录里现有的 .md/.txt，
                // 值（保存进配置的标识）是文件名，显示名用去掉扩展名的标题。
                for (const PromptLibrary::PromptFile& file : PromptLibrary::Instance().ListPromptFiles()) {
                    options.push_back({ Utils::Utf8ToWide(file.title), Utils::Utf8ToWide(file.fileName) });
                }
            }
            else {
                options = PresetOptionsFor(key, std::wstring(), std::wstring());
            }

            nlohmann::json list = nlohmann::json::array();
            for (const PresetOption& option : options) {
                list.push_back({ { "label", Utils::WideToUtf8(option.label) }, { "value", Utils::WideToUtf8(option.value) } });
            }
            presets[key] = list;
        }
        return presets.dump();
    }

    std::string ControlTerminalWindow::PresetOptionsJson(const std::string& key, const std::wstring& baseUrl,
                                                         const std::wstring& currentValue) const {
        const std::vector<PresetOption> options = PresetOptionsFor(key, baseUrl, currentValue);

        nlohmann::json list = nlohmann::json::array();
        for (const PresetOption& option : options) {
            list.push_back({ { "label", Utils::WideToUtf8(option.label) }, { "value", Utils::WideToUtf8(option.value) } });
        }
        return list.dump();
    }

    namespace {
        // 「今日」块要显示的下一条日程：取启用任务里 nextRun 最近的一条，格式 HH:MM 名称
        std::string TodayScheduleText() {
            std::vector<std::shared_ptr<ScheduledTask>> tasks = TaskScheduler::Instance().GetEnabledTasks();
            std::shared_ptr<ScheduledTask> best;
            for (const auto& task : tasks) {
                if (!task || task->nextRun <= 0) continue;
                if (!best || task->nextRun < best->nextRun) best = task;
            }
            if (!best) return std::string();
            std::time_t when = static_cast<std::time_t>(best->nextRun);
            std::tm local{};
            localtime_s(&local, &when);
            char buffer[128];
            std::snprintf(buffer, sizeof(buffer), "%02d:%02d %s",
                          local.tm_hour, local.tm_min, best->name.c_str());
            return std::string(buffer);
        }
    }

    std::string ControlTerminalWindow::BuildStateJson(bool resetScroll) const {
        // 主题随状态一起下发：页面把配色/字体写成 CSS 变量，于是「？？？」启用时
        // 整个配置界面（含按钮、描边、等宽标签）会跟着换成病娇配色。
        const UiTheme theme = CurrentUiTheme();
        nlohmann::json message = {
            { "type", "state" },
            { "sections", nlohmann::json::parse(BuildSchemaJson()) },
            { "values", nlohmann::json::parse(BuildValuesJson()) },
            { "baseline", nlohmann::json::parse(loaded_.empty() ? std::string("{}") : [this] {
                nlohmann::json baseline = nlohmann::json::object();
                for (const auto& pair : loaded_) baseline[pair.first] = pair.second;
                return baseline.dump();
            }()) },
            { "presets", nlohmann::json::parse(BuildPresetsJson()) },
            { "errors", nlohmann::json::parse(BuildErrorsJson()) },
            { "status", { { "text", Utils::WideToUtf8(statusText_) }, { "kind", statusKind_ } } },
            { "resetScroll", resetScroll },
            { "today", {
                { "weather", ConfigManager::Instance().GetString("runtime.weather_text", "") },
                { "schedule", TodayScheduleText() },
            } },
            { "probe", [this] {
                std::lock_guard<std::mutex> lock(probeMutex_);
                return nlohmann::json{
                    { "kind", probeKind_ },
                    { "text", probeText_ },
                    { "models", probeModelCount_ },
                    { "source", (!g_probedModels.models.empty() &&
                                  g_probedModels.url == CurrentValueText(ConfigManager::KEY_API_BASE_URL.c_str()))
                        ? "provider" : "builtin" },
                };
            }() },
            { "theme", {
                { "yandere", theme.yandere },
                { "accent", ToCssColor(theme.accent) },
                { "accentSoft", ToCssColor(theme.accentSoft) },
                { "accentHi", ToCssColor(theme.accentHi) },
                { "accentMid", ToCssColor(theme.accentMid) },
                { "edge", ToCssColor(theme.edge) },
                { "edgeSoft", ToCssColor(theme.edgeSoft) },
                { "surface", ToCssColor(theme.surface) },
                { "surfaceDeep", ToCssColor(theme.surfaceDeep) },
                { "text", ToCssColor(theme.text) },
                { "textDim", ToCssColor(theme.textDim) },
                { "fontSans", Utils::WideToUtf8(theme.fontSans) },
                { "fontMono", Utils::WideToUtf8(theme.fontMono) },
                { "italic", theme.italic },
            } },
        };
        return message.dump();
    }

    std::string ControlTerminalWindow::BuildErrorsJson() const {
        nlohmann::json errors = nlohmann::json::object();
        for (const auto& pair : invalidErrors_) {
            errors[pair.first] = pair.second;
        }
        return errors.dump();
    }

    bool ControlTerminalWindow::SendToPage(const std::string& json) {
        if (!webview_ || !webviewReady_) return false;
        const std::wstring wide = Utils::Utf8ToWide(json);
        return SUCCEEDED(webview_->PostWebMessageAsJson(wide.c_str()));
    }

    void ControlTerminalWindow::SendState(bool resetScroll) {
        if (!webviewReady_) {
            statePending_ = true;
            return;
        }
        SendToPage(BuildStateJson(resetScroll));
        // 状态之后紧跟一份会话记录：对话屏据此渲染气泡，不用再单开一次往返。
        SendChatHistory();
    }

    // 会话记录下发。角色在页面侧只分三类：me / ai / tool（system 不进气泡）。
    void ControlTerminalWindow::SendChatHistory() {
        if (!webviewReady_) {
            return;
        }

        nlohmann::json messages = nlohmann::json::array();
        const std::vector<ConversationMessage> history = MessageHandler::Instance().GetCurrentMessages();
        for (const ConversationMessage& message : history) {
            const std::string role = message.role == MessageRole::User ? "me"
                : (message.role == MessageRole::Tool ? "tool" : "ai");
            if (message.role == MessageRole::System) {
                continue;
            }
            messages.push_back({
                { "role", role },
                { "who", role == "me" ? "You" : (role == "tool" ? "Tool" : "Assistant") },
                { "text", message.content },
                // 带上消息 id：中枢里可以据此删除单条对话
                { "id", message.id },
            });
        }

        const nlohmann::json payload = {
            { "type", "chat" },
            { "messages", messages },
            { "model", CurrentValueText(ConfigManager::KEY_API_MODEL_DEFAULT.c_str()).empty()
                ? std::string("-") : Utils::WideToUtf8(CurrentValueText(ConfigManager::KEY_API_MODEL_DEFAULT.c_str())) },
            { "state", "就绪" },
        };
        SendToPage(payload.dump());
    }

    // 任意线程可调：AI 请求跑在工作线程上，碰 WebView2 必须回到窗口线程。
    void ControlTerminalWindow::PushChatHistory() {
        if (hwnd_) {
            PostMessageW(hwnd_, WM_APP_PUSH_CHAT, 0, 0);
        }
    }

    // 后台线程去问服务商要模型清单。用当前草稿的地址与密钥：
    // 用户刚改完还没保存时，「测一下」应该测的是他刚填的东西。
    void ControlTerminalWindow::StartProbe() {
        const std::string baseUrl = draft_[ConfigManager::KEY_API_BASE_URL];
        const std::string apiKey = SecretPlain(ConfigManager::KEY_API_KEY);

        {
            std::lock_guard<std::mutex> lock(probeMutex_);
            probeKind_ = 1;
            probeText_ = "测试中…";
            probeUrl_ = baseUrl;
        }
        SendProbeState();

        HWND target = hwnd_;
        std::thread([target, baseUrl, apiKey]() {
            const AIClient::ProviderProbe probe = AppCore::Instance().ProbeProvider(baseUrl, apiKey);
            // 结果先落到窗口成员里（加锁），再回 UI 线程推送——WebView2 只能在 UI 线程碰
            ControlTerminalWindow::Instance().StoreProbeResult(baseUrl, probe);
            if (target) {
                PostMessageW(target, WM_APP_PROBE_DONE, 0, 0);
            }
            }).detach();
    }

    // 工作线程侧：把结果存起来（UI 线程稍后来取）。
    void ControlTerminalWindow::StoreProbeResult(const std::string& baseUrl, const AIClient::ProviderProbe& probe) {
        std::lock_guard<std::mutex> lock(probeMutex_);
        probeKind_ = probe.ok ? 2 : (probe.reachable ? 3 : 3);
        probeText_ = probe.detail;
        probeUrl_ = baseUrl;
        probeModelCount_ = static_cast<int>(probe.models.size());
        probeModels_.clear();
        for (const std::string& model : probe.models) {
            probeModels_.push_back(Utils::Utf8ToWide(model));
        }
    }

    // UI 线程侧：把探测结果推给页面，并把模型清单换成服务商给的那份。
    void ControlTerminalWindow::FinishProbe() {
        std::string text;
        std::vector<std::wstring> models;
        int kind = 0;
        std::string url;
        {
            std::lock_guard<std::mutex> lock(probeMutex_);
            kind = probeKind_;
            text = probeText_;
            url = probeUrl_;
            models = probeModels_;
        }

        // 用户可能在探测期间又改了地址：过期结果直接丢掉，免得清单串台
        const std::string current = draft_[ConfigManager::KEY_API_BASE_URL];
        if (!current.empty() && current != url) {
            LOG_DEBUG("ControlTerminal", "Probe result for a stale URL dropped.");
            return;
        }

        if (!models.empty()) {
            g_probedModels.url = Utils::Utf8ToWide(url);
            g_probedModels.models = models;
            // 服务商给的清单里如果没有当前选中的模型，说明它已经下线了：收敛到第一个，
            // 否则每次请求都会被服务端拒掉（症状是「AI 永远不回话」）。
            const std::wstring current_model = CurrentValueText(ConfigManager::KEY_API_MODEL_DEFAULT.c_str());
            bool present = false;
            for (const std::wstring& model : models) {
                if (model == current_model) { present = true; break; }
            }
            if (!present && !models.empty() && !current_model.empty()) {
                draft_[ConfigManager::KEY_API_MODEL_DEFAULT] = Utils::WideToUtf8(models.front());
                LOG_INFO("ControlTerminal", "Model '" + Utils::WideToUtf8(current_model) +
                    "' is not offered by this provider; switched to '" + Utils::WideToUtf8(models.front()) + "'.");
            }
            PushModelOptions();
        }

        SendProbeState();
    }

    // 探测状态推给页面（API 屏的连通性灯 + 说明文字）。
    void ControlTerminalWindow::SendProbeState() {
        if (!webviewReady_) {
            return;
        }
        int kind = 0;
        std::string text;
        int count = 0;
        {
            std::lock_guard<std::mutex> lock(probeMutex_);
            kind = probeKind_;
            text = probeText_;
            count = probeModelCount_;
        }
        const nlohmann::json message = {
            { "type", "probeState" },
            { "kind", kind },
            { "text", text },
            { "models", count },
            { "source", (!g_probedModels.models.empty() && g_probedModels.url == CurrentValueText(ConfigManager::KEY_API_BASE_URL.c_str()))
                ? "provider" : "builtin" },
        };
        SendToPage(message.dump());
    }

    // 模型清单单独推一次：页面只重画那一行，不打断别处的输入。
    void ControlTerminalWindow::PushModelOptions() {
        if (!webviewReady_) {
            return;
        }
        const std::vector<PresetOption> options = PresetOptionsFor(
            kModelPresetKey, CurrentValueText(ConfigManager::KEY_API_BASE_URL.c_str()),
            CurrentValueText(ConfigManager::KEY_API_MODEL_DEFAULT.c_str()));
        nlohmann::json list = nlohmann::json::array();
        for (const PresetOption& option : options) {
            list.push_back({ { "label", Utils::WideToUtf8(option.label) }, { "value", Utils::WideToUtf8(option.value) } });
        }
        const nlohmann::json message = {
            { "type", "presetOptions" },
            { "key", kModelPresetKey },
            { "options", list },
            { "values", nlohmann::json::parse(BuildValuesJson()) },
            { "source", g_probedModels.url == CurrentValueText(ConfigManager::KEY_API_BASE_URL.c_str()) ? "provider" : "builtin" },
        };
        SendToPage(message.dump());
    }

    void ControlTerminalWindow::SendStatusOnly() {
        if (!webviewReady_) return;
        const nlohmann::json message = {
            { "type", "status" },
            { "status", { { "text", Utils::WideToUtf8(statusText_) }, { "kind", statusKind_ } } },
        };
        SendToPage(message.dump());
    }

    void ControlTerminalWindow::HandleWebMessage(const std::string& json) {
        const nlohmann::json message = nlohmann::json::parse(json, nullptr, false);
        if (message.is_discarded() || !message.is_object()) return;

        const std::string type = message.value("type", std::string());

        if (type == "ready") {
            statePending_ = false;
            SendState(true);
            // 「可用模型」要显示服务商真实提供的那份，不能等用户手点：
            // 页面就绪时先问一次（没配密钥就不问，免得白挨一个 401）。
            if (!SecretPlain(ConfigManager::KEY_API_KEY).empty()) {
                StartProbe();
            }
            return;
        }

        if (type == "petSlot") {
            // 页面把对话区宠物舞台的位置（CSS px，窗口客户区坐标）报上来。
            // 转成屏幕物理像素后交给 AppCore，把桌宠窗口摆进去。
            if (!onPetSlot_) return;
            const bool visible = message.value("visible", false);
            if (!visible) { onPetSlot_(false, 0, 0, 0, 0); return; }
            const int cx = message.value("x", 0);
            const int cy = message.value("y", 0);
            const int cw = message.value("w", 0);
            const int ch = message.value("h", 0);
            if (cw < 24 || ch < 24) { onPetSlot_(false, 0, 0, 0, 0); return; }
            // WebView 的 CSS px 与窗口的 DPI 缩放一致；先取窗口 DPI 比例，
            // 再把客户区原点换算到屏幕坐标。
            double scale = 1.0;
            if (hwnd_) {
                const UINT dpi = GetDpiForWindow(hwnd_);
                if (dpi > 0) scale = static_cast<double>(dpi) / 96.0;
            }
            POINT origin = { 0, 0 };
            if (hwnd_) ClientToScreen(hwnd_, &origin);
            const int sx = origin.x + static_cast<int>(std::lround(cx * scale));
            LOG_DEBUG("ControlTerminal", "petSlot: css=(" + std::to_string(cx) + "," + std::to_string(cy) +
                                         "," + std::to_string(cw) + "," + std::to_string(ch) +
                                         ") scale=" + std::to_string(scale) +
                                         " origin=(" + std::to_string(origin.x) + "," + std::to_string(origin.y) + ")");
            const int sy = origin.y + static_cast<int>(std::lround(cy * scale));
            const int sw = static_cast<int>(std::lround(cw * scale));
            const int sh = static_cast<int>(std::lround(ch * scale));
            onPetSlot_(true, sx, sy, sw, sh);
            return;
        }

        if (type == "drag") {
            // 页面顶栏按下时通知宿主，用系统移动循环拖动无标题栏窗口。
            ReleaseCapture();
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return;
        }

        if (type == "min") {
            // 最小化到任务栏（不要 Hide：隐藏后就没有任务栏入口了）
            if (hwnd_) ShowWindow(hwnd_, SW_MINIMIZE);
            return;
        }

        if (type == "toggleMax") {
            // 顶栏双击 / 右上角方块：最大化到当前显示器工作区，再点还原。
            // WS_POPUP 窗口用 SW_MAXIMIZE + IsZoomed 不可靠，自己记状态更稳。
            if (hwnd_) {
                if (!maximized_) {
                    GetWindowRect(hwnd_, &maxRestore_);
                    MONITORINFO mi = {};
                    mi.cbSize = sizeof(MONITORINFO);
                    const HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
                    if (GetMonitorInfoW(mon, &mi)) {
                        SetWindowPos(hwnd_, HWND_TOP, mi.rcWork.left, mi.rcWork.top,
                                     mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top,
                                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
                        maximized_ = true;
                    }
                } else {
                    SetWindowPos(hwnd_, HWND_TOP, maxRestore_.left, maxRestore_.top,
                                 maxRestore_.right - maxRestore_.left, maxRestore_.bottom - maxRestore_.top,
                                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    maximized_ = false;
                }
                ResizeWebView();
            }
            return;
        }

        if (type == "close") {
            Hide();
            return;
        }

        if (type == "refreshPet") {
            // 中枢主菜单的「刷新」：重读配置 + 重载模型 + 把窗口摆回来。
            // 要碰窗口与渲染器，所以留在窗口线程上做；先回一条状态，页面不至于干等。
            SetStatus(L"正在刷新桌宠…", 0);
            SendStatusOnly();
            AppCore::Instance().RefreshPet();
            SetStatus(L"桌宠已刷新（模型与配置已重新载入）", 1);
            SendStatusOnly();
            return;
        }

        if (type == "quit") {
            // 页面确认过退出了。但这里仍处在 WebView2 的消息回调里：直接 Shutdown
            // 会拆掉 webview 自身，回调随即返回到已释放的 COM 对象上，
            // 表现就是 ucrtbase.dll 抛「请求了严重的程序退出」。
            // 所以只把请求转交给主窗口的消息队列，等这个回调返回之后再退。
            LOG_INFO("ControlTerminal", "Quit requested from hub UI; deferring to the pet window.");
            if (HWND main = AppCore::Instance().GetMainWindow()) {
                // Window::WM_QUIT_REQUESTED：桌宠窗口收到后转成回调，由 AppCore 退出。
                // （AppCore 自己那套消息窗口从未创建，不能往那边发。）
                PostMessageW(main, Window::WM_QUIT_REQUESTED, 0, 0);
            }
            return;
        }

        if (type == "chatSend") {
            // 对话屏发来一句话：交给 AppCore 走完整链路（落库 → 组装 → 流式请求）。
            // ProcessAIMessage 可能阻塞数秒，不能在 UI 线程上等，所以丢到工作线程。
            const std::string text = message.value("text", std::string());
            if (text.empty()) {
                return;
            }
            LOG_INFO("ControlTerminal", "Chat message from hub UI (" + std::to_string(text.size()) + " bytes).");
            std::thread([text]() {
                // 完整链路在工作线程上跑；完成后 AppCore 会把会话推回页面。
                AppCore::Instance().SubmitUserMessage(text);
                }).detach();
            return;
        }

        if (type == "chatNew" || type == "chatResume") {
            PushChatHistory();
            return;
        }

        if (type == "chatDelete") {
            // 删除记忆库里的一条对话：删库 + 清缓存，随后把新的历史推回页面
            const int64_t messageId = message.value("id", static_cast<int64_t>(0));
            if (messageId > 0) {
                MessageHandler::Instance().DeleteMessage(messageId);
                LOG_INFO("ControlTerminal", "Deleted message " + std::to_string(messageId) + " from hub UI.");
                SendChatHistory();
                SetStatus(L"已删除这条对话", 1);
                SendStatusOnly();
            }
            return;
        }

        if (type == "chatReset") {
            // 重置记忆库：清空全部对话（会话本身保留）
            MessageHandler::Instance().ClearAllMessages();
            LOG_INFO("ControlTerminal", "Memory bank reset from hub UI.");
            SendChatHistory();
            SetStatus(L"记忆库已重置", 1);
            SendStatusOnly();
            return;
        }

        if (type == "importModel") {
            // 「导入模型」：zip 用文件对话框，文件夹用目录选择框；
            // 实际导入逻辑在 Utils::ImportLive2DModel（tar.exe 解 zip / 递归拷贝）
            const std::string mode = message.value("mode", std::string("zip"));
            std::wstring picked;

            if (mode == "folder") {
                BROWSEINFOW bi{};
                bi.lpszTitle = L"选择要导入的 Live2D 模型文件夹";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                LPITEMIDLIST idl = SHBrowseForFolderW(&bi);
                if (idl != nullptr) {
                    wchar_t buf[MAX_PATH] = {};
                    if (SHGetPathFromIDListW(idl, buf)) {
                        picked = buf;
                    }
                    CoTaskMemFree(idl);
                }
            }
            else {
                wchar_t buf[32768] = {};
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.lpstrFilter = L"Live2D 模型压缩包 (*.zip)\0*.zip\0所有文件 (*.*)\0*.*\0\0";
                ofn.lpstrFile = buf;
                ofn.nMaxFile = 32768;
                ofn.lpstrTitle = L"选择要导入的 Live2D 模型压缩包";
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                if (GetOpenFileNameW(&ofn)) {
                    picked = buf;
                }
            }

            if (picked.empty()) {
                return;   // 用户取消
            }
            const std::wstring imported = Utils::ImportLive2DModel(picked, mode != "folder");
            if (imported.empty()) {
                SetStatus(L"导入失败：没在压缩包/文件夹里找到 *.model3.json", 4);
            }
            else {
                SetStatus(L"已导入模型：" + imported + L"（重开设置页即可在下拉里选到）", 5);
            }
            SendStatusOnly();
            return;
        }

        if (type == "probe") {
            // 「测试连接」：先把页面上的草稿并进来，再拿这份草稿去问服务商。
            ApplyPostedValues(message);
            StartProbe();
            return;
        }

        if (type == "fieldAction") {
            // Action 行（试听 / 启动引擎 / 刷新角色）：只触发动作，不写配置
            ApplyPostedValues(message);
            const std::string actionKey = message.value("key", std::string());
            if (actionKey == "voice.test") {
                // 试听按「当前页面上的参数」走：先落库再重读，保证听到的就是看到的
                Save();
                VoiceService::Instance().ReloadConfig();
                VoiceService::Instance().TestSpeak();
            }
            return;
        }

        if (type == "save") {
            ApplyPostedValues(message);
            Save();
            VoiceService::Instance().ReloadConfig();       // 语音参数改完立即生效
            return;
        }

        if (type == "reload") {
            Reload();
            VoiceService::Instance().ReloadConfig();
            return;
        }

        if (type == "restore") {
            RestoreDefaults();
            return;
        }

        if (type == "presetsChanged") {
            ApplyPostedValues(message);

            // 用户刚动过服务商或模型：顺手把这一对收敛一次（详见 SnapModelToProvider）。
            const bool snapped = SnapModelToProvider();

            // 服务地址变了，「可用模型」整行要重新算；两行都回，保证选中态一致。
            // 纠正过模型时必须把新的 values 一并回推：页面保存时提交的是它自己那份
            // values，不回推的话纠正只停在宿主这边，一按保存又被覆盖回去。
            for (const char* key : { kProviderPresetKey, kModelPresetKey }) {
                nlohmann::json reply = {
                    { "type", "presetOptions" },
                    { "key", key },
                    { "options", nlohmann::json::parse(PresetOptionsJson(key,
                        CurrentValueText(ConfigManager::KEY_API_BASE_URL.c_str()),
                        CurrentValueText(ConfigManager::KEY_API_MODEL_DEFAULT.c_str()))) },
                };
                if (snapped) {
                    reply["values"] = nlohmann::json::parse(BuildValuesJson());
                }
                reply["source"] = g_probedModels.url == CurrentValueText(ConfigManager::KEY_API_BASE_URL.c_str())
                    ? "provider" : "builtin";
                SendToPage(reply.dump());
            }

            // 换了服务商就等于换了一套模型：顺手探测一次，模型行马上变成对方真实提供的那份。
            // 页面按钮上的「测试连接」走的是同一条路，只是由用户手动触发。
            StartProbe();
            return;
        }
    }

    // 只接受字段表里声明过的键：页面无法写入任意配置项。

    void ControlTerminalWindow::ApplyPostedValues(const nlohmann::json& message) {
        const auto valuesIt = message.find("values");
        if (valuesIt == message.end() || !valuesIt->is_object()) return;

        for (auto it = valuesIt->begin(); it != valuesIt->end(); ++it) {
            const std::string key = it.key();
            if (draft_.find(key) == draft_.end()) continue;
            if (!it->is_string()) continue;
            const std::string posted = it->get<std::string>();
            // 密钥类字段：页面在未展开/未改动时会回传空串，那是「没动」而不是「要清空」。
            // 否则任何一次保存都会把已存的密钥擦掉——AI 从此不再回复。
            if (posted.empty() && IsSecretKey(key)) continue;
            draft_[key] = posted;
        }
    }

    // ============================================================
    // 配置读写
    // ============================================================

    void ControlTerminalWindow::LoadFromConfig() {
        draft_.clear();
        loaded_.clear();
        invalidKeys_.clear();
        invalidErrors_.clear();

        for (int s = 0; s < kSectionCount; ++s) {
            const SectionDef& sec = kSections[s];
            for (int i = 0; i < sec.fieldCount; ++i) {
                const FieldDef& def = sec.fields[i];
                // Preset 只是写入口，不对应任何配置键
                if (def.kind == FieldKind::Preset || def.kind == FieldKind::Action) continue;
                const std::string raw = ConfigManager::Instance().GetString(def.key, Utils::WideToUtf8(def.dflt));
                // Multi（逗号分隔清单）与 Enum 一样只是字符串，直接进草稿。
                // 唯一的例外：quick.mode 存着「？？？」而哨兵文件已被删掉时，
                // 页面上根本没有这个取值（OptionsFor 不会再给出它），
                // 回退成「关闭」，免得带出一个选不中的幽灵选项。
                const bool yandereGone = KeyIs(def, ConfigManager::KEY_QUICK_MODE)
                    && Utils::Utf8ToWide(raw) == L"？？？"
                    && !PromptLibrary::Instance().HasYandereSentinel();
                const std::string value = yandereGone ? std::string("关闭") : raw;
                // 密钥类字段：库里存的是 DPAPI+Base64 密文，绝不能回填给页面（会当明文显示/回写）
                draft_[def.key] = def.secret ? std::string() : value;
                loaded_[def.key] = value;
            }
        }
        restoreArmedAt_ = 0;
    }

    bool ControlTerminalWindow::IsSecretKey(const std::string& key) const {
        for (int s = 0; s < kSectionCount; ++s) {
            const SectionDef& sec = kSections[s];
            for (int i = 0; i < sec.fieldCount; ++i) {
                const FieldDef& def = sec.fields[i];
                if (def.secret && key == def.key) return true;
            }
        }
        return false;
    }

    // 明文密钥：优先用草稿（用户刚输入的），否则把库里的密文解出来。
    // 「测试连接」「就绪即探测」都要用它，否则只有点过输入框才有值。
    std::string ControlTerminalWindow::SecretPlain(const std::string& key) const {
        const auto it = draft_.find(key);
        if (it != draft_.end() && !it->second.empty()) return it->second;
        const std::string stored = ConfigManager::Instance().GetString(key, "");
        if (stored.empty()) return std::string();
        return SecurityUtils::Instance().Decrypt(stored);
    }

    std::wstring ControlTerminalWindow::CurrentValueText(const char* key) const {
        const auto it = draft_.find(key);
        if (it != draft_.end()) return Utils::Utf8ToWide(it->second);
        return std::wstring();
    }

    // 人设锁：「？？？」生效期间不允许再切回别人设。
    // 判据只有一条——哨兵文件还在 + 当前值就是「？？？」。
    // 也就是说解锁的唯一方式是删掉 prompts/yandere.chr（删掉后页面连这一项都不再出现，
    // 存着的值也会在 LoadFromConfig 里回落成「关闭」）。
    bool ControlTerminalWindow::IsPersonaFrozen() const {
        if (!PromptLibrary::Instance().HasYandereSentinel()) {
            return false;
        }
        return CurrentValueText(ConfigManager::KEY_QUICK_MODE.c_str()) == L"？？？";
    }

    // 服务商与模型必须成套。页面切服务商时只写 api.base_url，api.model_default
    // 会留在上一个服务商的模型名上（出厂默认就是 OpenAI 的 gpt-4o），于是请求被
    // 服务端一律拒成 HTTP 400——「模型名不被支持」，而界面上完全看不出这一对不匹配。
    // 这里在服务地址能反查到内置服务商、且当前模型不在该服务商模型列表里时，
    // 收敛到该服务商的第一个模型。返回是否真的改过。
    bool ControlTerminalWindow::SnapModelToProvider() {
        const ProviderPreset* provider =
            FindProviderByUrl(CurrentValueText(ConfigManager::KEY_API_BASE_URL.c_str()));
        if (provider == nullptr) {
            // 自定义网关：模型名我们无从判断，绝不代改。
            return false;
        }

        const std::vector<std::wstring> models = SplitOptions(provider->models);
        if (models.empty()) {
            return false;
        }

        const std::wstring current = CurrentValueText(ConfigManager::KEY_API_MODEL_DEFAULT.c_str());
        for (const std::wstring& model : models) {
            if (model == current) {
                return false;   // 本来就是一套的
            }
        }

        const std::wstring replacement = models.front();
        draft_[ConfigManager::KEY_API_MODEL_DEFAULT] = Utils::WideToUtf8(replacement);
        LOG_DEBUG("ControlTerminal", "Model '" + Utils::WideToUtf8(current) +
            "' is not offered by this provider; switched to '" + Utils::WideToUtf8(replacement) + "'");
        return true;
    }

    bool ControlTerminalWindow::ValidateValue(const FieldDef& def, const std::wstring& value, std::wstring& error) const {
        // Preset 不对应配置键，写入前已被跳过，无需校验
        if (def.kind == FieldKind::Preset || def.kind == FieldKind::Action) {
            return true;
        }

        const std::string narrow = Utils::Trim(Utils::WideToUtf8(value));

        if (def.kind == FieldKind::Int) {
            if (narrow.empty()) {
                error = L"不能为空";
                return false;
            }
            for (char ch : narrow) {
                if (!isdigit(static_cast<unsigned char>(ch)) && ch != '-' && ch != '+') {
                    error = L"必须是整数";
                    return false;
                }
            }
            if (narrow.size() > 9) {
                error = L"数值过大";
                return false;
            }
            return true;
        }

        if (def.kind == FieldKind::Double) {
            if (narrow.empty()) {
                error = L"不能为空";
                return false;
            }
            char* end = nullptr;
            std::strtod(narrow.c_str(), &end);
            if (end == narrow.c_str() || (end && *end != '\0')) {
                error = L"必须是数字";
                return false;
            }
            return true;
        }

        if (def.kind == FieldKind::Enum) {
            // 选项来源与页面下发完全一致（见 OptionsFor）：运行时选项
            // （quick.mode）不允许出现「页面上能选、这里却说非法」的偏差。
            const std::vector<std::wstring> options = OptionsFor(def);
            if (std::find(options.begin(), options.end(), value) == options.end()) {
                error = L"取值非法";
                return false;
            }
            return true;
        }

        if (def.kind == FieldKind::Multi) {
            // 值是逗号分隔的文件名清单：逐个对照 prompts 目录里当下真实存在的文件。
            // 目录里删掉的文件如果还留在库里，页面既选不中也取消不掉，
            // 这类不同步应该在保存时拦下来而不是静默写回。
            std::vector<std::string> available;
            for (const PromptLibrary::PromptFile& file : PromptLibrary::Instance().ListPromptFiles()) {
                available.push_back(file.fileName);
            }
            for (const std::string& name : SplitList(narrow)) {
                if (std::find(available.begin(), available.end(), name) == available.end()) {
                    error = L"提示词文件不存在";
                    return false;
                }
            }
            return true;
        }

        // 服务地址需为 http(s) 端点
        if (std::string(def.key) == "api.base_url" && !narrow.empty()) {
            if (narrow.rfind("http://", 0) != 0 && narrow.rfind("https://", 0) != 0) {
                error = L"需以 http(s):// 开头";
                return false;
            }
        }
        return true;
    }

    void ControlTerminalWindow::Save() {
        invalidKeys_.clear();
        invalidErrors_.clear();

        // 写库前先把「服务商 + 模型」收敛成一套：否则换过服务商就会一直 HTTP 400。
        // 与下面 quick.mode 同理——草稿先归一，界面保存后 SendState 会把纠正后的
        // 模型显示出来，用户看得见这次改动。
        SnapModelToProvider();

        // 人设锁的兜底：页面上的按钮虽然禁用了，但宿主这边必须自己再拦一次——
        // 页面可以被人为改消息绕过，而「？？？」是要坚持到底的（解锁只能删 .chr）。
        if (IsPersonaFrozen()) {
            draft_[ConfigManager::KEY_QUICK_MODE] = Utils::WideToUtf8(L"？？？");
            loaded_[ConfigManager::KEY_QUICK_MODE] = Utils::WideToUtf8(L"？？？");
        }

        // 父开关关闭时 quick.mode 整行在页面上不可见（见 BuildSchemaJson 的 requires）。
        // 这里把草稿强制回写成「关闭」：draft_ 让界面立刻反映，loaded_ 一起同步，
        // 因为这一行用户根本看不到——库里留下一个看不见却仍然生效的「？？？」
        // 就是没法发现、没法关掉的状态；顺带也避免下次打开页面时脏值指示莫名亮着。
        const auto quickEnabledIt = draft_.find(ConfigManager::KEY_QUICK_ENABLED);
        const std::string quickEnabled = (quickEnabledIt == draft_.end()) ? std::string() : Utils::Trim(quickEnabledIt->second);
        if (quickEnabled != "true") {
            draft_[ConfigManager::KEY_QUICK_MODE] = "关闭";
            loaded_[ConfigManager::KEY_QUICK_MODE] = "关闭";
        }

        for (int s = 0; s < kSectionCount; ++s) {
            const SectionDef& sec = kSections[s];
            for (int i = 0; i < sec.fieldCount; ++i) {
                const FieldDef& def = sec.fields[i];
                if (def.kind == FieldKind::Preset || def.kind == FieldKind::Action) continue;
                const std::wstring value = CurrentValueText(def.key);
                std::wstring error;
                if (!ValidateValue(def, value, error)) {
                    invalidKeys_.push_back(def.key);
                    invalidErrors_[def.key] = Utils::WideToUtf8(error);
                    continue;
                }

                const std::string narrow = Utils::Trim(Utils::WideToUtf8(value));
                if (def.secret) {
                    if (narrow.empty()) {
                        // 留空 = 不改：把库里原来的密文原样写回
                        auto prev = loaded_.find(def.key);
                        if (prev != loaded_.end() && !prev->second.empty()) {
                            ConfigManager::Instance().SetString(def.key, prev->second);
                        }
                    } else {
                        // 新输入的明文 → DPAPI 加密后再落库（页面写着「数据库加密存储」）
                        const std::string cipher = SecurityUtils::Instance().Encrypt(narrow);
                        ConfigManager::Instance().SetString(def.key, cipher);
                        LOG_INFO("ControlTerminal", std::string("Secret updated: ") + def.key +
                            " (plain=" + std::to_string(narrow.size()) + "B, stored=" +
                            std::to_string(cipher.size()) + "B)");
                    }
                    continue;
                }
                switch (def.kind) {
                case FieldKind::Int:
                    ConfigManager::Instance().SetInt(def.key, std::atoi(narrow.c_str()));
                    break;
                case FieldKind::Double:
                    ConfigManager::Instance().SetDouble(def.key, std::strtod(narrow.c_str(), nullptr));
                    break;
                case FieldKind::Bool:
                    ConfigManager::Instance().SetBool(def.key, narrow == "true");
                    break;
                case FieldKind::Multi:
                    // 逗号分隔的文件名清单，顺序就是页面上的选项顺序
                    ConfigManager::Instance().SetString(def.key, narrow);
                    break;
                default:
                    ConfigManager::Instance().SetString(def.key, narrow);
                    break;
                }
            }
        }

        if (!invalidKeys_.empty()) {
            wchar_t message[256] = {};
            swprintf_s(message, L"校验失败 · %d 项未写入 · %s",
                static_cast<int>(invalidKeys_.size()), Utils::Utf8ToWide(invalidKeys_.front()).c_str());
            SetStatus(message, 2);
            // 页面据此把出错的字段标出来；成功的字段保持原样，不写库。
            if (webviewReady_) {
                const nlohmann::json reply = {
                    { "type", "validation" },
                    { "errors", nlohmann::json::parse(BuildErrorsJson()) },
                    { "status", { { "text", Utils::WideToUtf8(statusText_) }, { "kind", statusKind_ } } },
                };
                SendToPage(reply.dump());
            }
            return;
        }

        // 刷新草稿，使界面与刚写入的值保持一致
        for (int s = 0; s < kSectionCount; ++s) {
            const SectionDef& sec = kSections[s];
            for (int i = 0; i < sec.fieldCount; ++i) {
                const FieldDef& def = sec.fields[i];
                if (def.kind == FieldKind::Preset || def.kind == FieldKind::Action) continue;
                const std::string value = Utils::Trim(Utils::WideToUtf8(CurrentValueText(def.key)));
                draft_[def.key] = value;
                loaded_[def.key] = value;
            }
        }
        restoreArmedAt_ = 0;
        SetStatus(L"已保存 · " + NowString() + L" · 配置已写入 app.db", 1);
        SendState(false);

        if (onSaved_) {
            onSaved_();
        }
        LOG_INFO("ControlTerminal", "Configuration saved from control terminal.");
    }

    void ControlTerminalWindow::Reload() {
        invalidKeys_.clear();
        invalidErrors_.clear();
        LoadFromConfig();
        SetStatus(L"已重新载入 · " + NowString(), 0);
        SendState(false);
        LOG_DEBUG("ControlTerminal", "Configuration reloaded from database.");
    }

    void ControlTerminalWindow::RestoreDefaults() {
        const ULONGLONG now = GetTickCount64();
        if (restoreArmedAt_ == 0 || now - restoreArmedAt_ > 4000) {
            restoreArmedAt_ = now;
            SetStatus(L"再次点击「恢复默认」以确认清空并写入默认值", 2);
            SendStatusOnly();
            return;
        }

        restoreArmedAt_ = 0;
        ConfigManager::Instance().RestoreDefaults();
        invalidKeys_.clear();
        invalidErrors_.clear();
        LoadFromConfig();
        SetStatus(L"已恢复默认值 · " + NowString(), 1);
        SendState(false);

        if (onSaved_) {
            onSaved_();
        }
        LOG_INFO("ControlTerminal", "Configuration restored to defaults from control terminal.");
    }

    void ControlTerminalWindow::SetStatus(const std::wstring& text, int kind) {
        statusText_ = text;
        statusKind_ = kind;
    }

    std::wstring ControlTerminalWindow::NowString() const {
        std::time_t now = std::time(nullptr);
        std::tm local = {};
        localtime_s(&local, &now);
        wchar_t buffer[32] = {};
        swprintf_s(buffer, L"%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
        return buffer;
    }

} // namespace UnnamedBot
