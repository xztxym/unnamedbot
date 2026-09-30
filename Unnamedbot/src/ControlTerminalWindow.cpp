#include "../include/ControlTerminalWindow.h"

#include "../include/ConfigManager.h"
#include "../include/ConfigWebUi.h"
#include "../include/Logger.h"
#include "../include/PromptLibrary.h"
#include "../include/ResourceIds.h"
#include "../include/Utils.h"

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
#include <objbase.h>

namespace UnnamedBot {

    const wchar_t* ControlTerminalWindow::WINDOW_CLASS_NAME = L"UnnamedBotControlTerminal";

    namespace {

        using FieldKind = ControlTerminalWindow::FieldKind;

        std::string ToUtf8(const wchar_t* text) {
            return text ? Utils::WideToUtf8(text) : std::string();
        }

        // ===== 窗口尺寸（逻辑像素，96 DPI）=====
        const int kWinW = 900;
        const int kWinH = 720;

        // 服务商预设：常用 OpenAI 兼容接口的 Base URL，以及该服务商可用的模型。
        // 模型名各家会随时调整，这里给的是常见值；用户仍可在「默认模型」里直接手填。
        struct ProviderPreset {
            const wchar_t* label;
            const wchar_t* baseUrl;
            const wchar_t* models;   // '|' 分隔
        };

        const ProviderPreset kProviders[] = {
            { L"OpenAI",     L"https://api.openai.com/v1",                                 L"gpt-4o|gpt-4o-mini|gpt-4.1|gpt-4.1-mini|o3|o4-mini" },
            { L"Azure",      L"https://YOUR-RESOURCE.openai.azure.com/openai/v1/",         L"gpt-4o|gpt-4o-mini|gpt-4.1" },
            { L"DeepSeek",   L"https://api.deepseek.com/v1",                               L"deepseek-chat|deepseek-reasoner" },
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

        // Preset 行本身不入库：它只是写入口。服务商行写 api.base_url，
        // 模型行写 api.model_default。
        const char* PresetTargetKeyFor(const ControlTerminalWindow::FieldDef& def) {
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

            if (key != kModelPresetKey) {
                return result;
            }

            const ProviderPreset* provider = FindProviderByUrl(currentBaseUrl);
            if (provider != nullptr) {
                for (const std::wstring& model : SplitOptions(provider->models)) {
                    result.push_back({ model, model });
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
        };

        // ===== 卡片 02：采样参数 =====
        const ControlTerminalWindow::FieldDef kSamplingFields[] = {
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
            { "active.cooldown_seconds",    L"全局冷却",     L"COOLDOWN",    L"秒",           FieldKind::Int,  L"480",  nullptr, false, 150 },
            { "active.ip_check_on_start",   L"启动时查 IP",  L"IP ON START", L"联网检测",     FieldKind::Bool, L"true", nullptr, false, 0 },
            { "amap.api_key",               L"高德密钥",     L"AMAP KEY",    L"天气服务",     FieldKind::Text, L"",     nullptr, true,  320 },
        };

        // ===== 卡片 06：模型与外观 =====
        const ControlTerminalWindow::FieldDef kModelFields[] = {
            { "model.default_id",      L"默认模型", L"MODEL ID", L"Live2D 名称", FieldKind::Text,   L"Haru", nullptr, false, 240 },
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
        };

        const int kSectionCount = static_cast<int>(sizeof(kSections) / sizeof(kSections[0]));

        // 字段类型 → 页面控件类型
        const char* KindName(FieldKind kind) {
            switch (kind) {
            case FieldKind::Int:    return "int";
            case FieldKind::Double: return "double";
            case FieldKind::Bool:   return "bool";
            case FieldKind::Enum:   return "enum";
            case FieldKind::Preset: return "preset";
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

    void ControlTerminalWindow::Show(bool show) {
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
        if (!hwnd_) return;
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
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
        const int width = Scale(kWinW);
        const int height = Scale(kWinH);

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
            WS_POPUP | WS_CLIPCHILDREN,
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
            // WebView2 占满客户区，页面内的顶栏拖动由页面通知宿主（见 HandleWebMessage）。
            // 页面尚未就绪时留一条原生拖动路径，避免窗口完全无法移动。
            if (!webviewReady_) {
                POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
                ScreenToClient(hwnd, &pt);
                if (pt.y >= 0 && pt.y < Scale(104)) return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_CLOSE:
            Hide();
            return 0;

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

    void ControlTerminalWindow::CreateWebView() {
        if (!hwnd_ || environment_ || controller_) return;

        // 用户数据目录放在 LOCALAPPDATA，不污染程序目录，也不受工作目录影响。
        std::wstring userDataDir;
        {
            wchar_t buffer[MAX_PATH] = {};
            const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
            if (length > 0 && length < MAX_PATH) userDataDir.assign(buffer, length);
        }
        if (userDataDir.empty()) {
            LOG_WARN("ControlTerminal", "LOCALAPPDATA 不可用，WebView2 用户目录回退到当前目录。");
            userDataDir = L".";
        }
        userDataDir += L"\\Unnamedbot\\WebView2";

        const HRESULT hrEnv = CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            userDataDir.c_str(),
            nullptr,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this](HRESULT result, ICoreWebView2Environment* createdEnvironment) -> HRESULT {
                    if (FAILED(result) || createdEnvironment == nullptr) {
                        LOG_ERROR("ControlTerminal", "WebView2 环境创建失败，配置界面无法显示。");
                        return S_OK;
                    }
                    environment_ = createdEnvironment;

                    const HRESULT hrController = environment_->CreateCoreWebView2Controller(
                        hwnd_,
                        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this](HRESULT result2, ICoreWebView2Controller* createdController) -> HRESULT {
                                if (FAILED(result2) || createdController == nullptr) {
                                    LOG_ERROR("ControlTerminal", "WebView2 控制器创建失败。");
                                    return S_OK;
                                }
                                controller_ = createdController;

                                // 窗口底色与应用一致，避免初始化瞬间闪白。
                                ComPtr<ICoreWebView2Controller2> controller2;
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
                        LOG_ERROR("ControlTerminal", "WebView2 控制器创建调用失败。");
                    }
                    return S_OK;
                }).Get());

        if (FAILED(hrEnv)) {
            LOG_ERROR("ControlTerminal", "WebView2 环境创建调用失败（可能未安装 WebView2 Runtime）。");
        }
    }

    void ControlTerminalWindow::ConfigureWebViewSettings() {
        if (!webview_) return;

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
                        LOG_ERROR("ControlTerminal", "配置界面导航失败。");
                        return S_OK;
                    }
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
            LOG_ERROR("ControlTerminal", "配置界面装载失败。");
        }
    }

    void ControlTerminalWindow::ResizeWebView() {
        if (!controller_ || !hwnd_) return;
        RECT bounds = {};
        GetClientRect(hwnd_, &bounds);
        controller_->put_Bounds(bounds);
    }

    void ControlTerminalWindow::DestroyWebView() {
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

    std::string ControlTerminalWindow::BuildStateJson(bool resetScroll) const {
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
            return;
        }

        if (type == "drag") {
            // 页面顶栏按下时通知宿主，用系统移动循环拖动无标题栏窗口。
            ReleaseCapture();
            SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
            return;
        }

        if (type == "close") {
            Hide();
            return;
        }

        if (type == "save") {
            ApplyPostedValues(message);
            Save();
            return;
        }

        if (type == "reload") {
            Reload();
            return;
        }

        if (type == "restore") {
            RestoreDefaults();
            return;
        }

        if (type == "presetsChanged") {
            ApplyPostedValues(message);
            // 服务地址变了，「可用模型」整行要重新算；两行都回，保证选中态一致。
            for (const char* key : { kProviderPresetKey, kModelPresetKey }) {
                const nlohmann::json reply = {
                    { "type", "presetOptions" },
                    { "key", key },
                    { "options", nlohmann::json::parse(PresetOptionsJson(key,
                        CurrentValueText(ConfigManager::KEY_API_BASE_URL.c_str()),
                        CurrentValueText(ConfigManager::KEY_API_MODEL_DEFAULT.c_str()))) },
                };
                SendToPage(reply.dump());
            }
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
            draft_[key] = it->get<std::string>();
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
                if (def.kind == FieldKind::Preset) continue;
                const std::string raw = ConfigManager::Instance().GetString(def.key, Utils::WideToUtf8(def.dflt));
                // Multi（逗号分隔清单）与 Enum 一样只是字符串，直接进草稿。
                // 唯一的例外：quick.mode 存着「？？？」而哨兵文件已被删掉时，
                // 页面上根本没有这个取值（OptionsFor 不会再给出它），
                // 回退成「关闭」，免得带出一个选不中的幽灵选项。
                const bool yandereGone = KeyIs(def, ConfigManager::KEY_QUICK_MODE)
                    && Utils::Utf8ToWide(raw) == L"？？？"
                    && !PromptLibrary::Instance().HasYandereSentinel();
                const std::string value = yandereGone ? std::string("关闭") : raw;
                draft_[def.key] = value;
                loaded_[def.key] = value;
            }
        }
        restoreArmedAt_ = 0;
    }

    std::wstring ControlTerminalWindow::CurrentValueText(const char* key) const {
        const auto it = draft_.find(key);
        if (it != draft_.end()) return Utils::Utf8ToWide(it->second);
        return std::wstring();
    }

    bool ControlTerminalWindow::ValidateValue(const FieldDef& def, const std::wstring& value, std::wstring& error) const {
        // Preset 不对应配置键，写入前已被跳过，无需校验
        if (def.kind == FieldKind::Preset) {
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
                if (def.kind == FieldKind::Preset) continue;
                const std::wstring value = CurrentValueText(def.key);
                std::wstring error;
                if (!ValidateValue(def, value, error)) {
                    invalidKeys_.push_back(def.key);
                    invalidErrors_[def.key] = Utils::WideToUtf8(error);
                    continue;
                }

                const std::string narrow = Utils::Trim(Utils::WideToUtf8(value));
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
                if (def.kind == FieldKind::Preset) continue;
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
