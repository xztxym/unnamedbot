#pragma once

#include <Windows.h>

// 探测结果里用到 AIClient::ProviderProbe，所以这里要拿到完整声明
#include "AIClient.h"

#include <wrl/client.h>
#include <WebView2.h>

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace UnnamedBot {

    // ComPtr 模板名提到命名空间作用域（与 Renderer.h 一致）；
    // 类内的 using 声明不能引入外部命名空间的类模板。
    using Microsoft::WRL::ComPtr;

    // 控制终端：编辑 ./data/app.db 中配置项的 GUI 窗口。
    //
    // 前端是自实现的 HTML/CSS/JS（见 ConfigWebUi.cpp），由 WebView2 承载在本窗口内，
    // 不再使用 GDI 自绘。职责划分刻意保持不变：
    //   * C++ 持有字段表、校验规则与写库逻辑，是配置的唯一事实来源；
    //   * 页面只负责把宿主下发的字段表渲染成控件，并把输入回传。
    // 因此配置项、取值约束与保存行为与原实现一致，换掉的只是呈现层。
    class ControlTerminalWindow {
    public:
        static ControlTerminalWindow& Instance();

        bool Initialize(HINSTANCE hInstance);
        void Shutdown();

        void Show(bool show = true);
        void Hide();
        void Toggle();
        bool IsVisible() const { return visible_; }
        HWND GetHandle() const { return hwnd_; }

        // 配置保存成功后的回调（AppCore 用它重新加载并应用配置）
        void OnSaved(std::function<void()> callback);

        // WebView2 不可用（运行时崩溃 / 未安装 / 页面迟迟不加载）时的回调。
        // AppCore 用它做降级：直接把桌宠窗口放出来，别让用户对着空白配置窗口发呆。
        // 只会触发一次，且在 UI 线程上。
        void OnWebViewFailed(std::function<void()> callback);
    // 对话区宠物舞台的屏幕矩形（物理像素）：宿主把桌宠窗口摆到这个矩形里。
    // visible=false 表示离开对话页，桌宠恢复桌面位置。
    void OnPetSlot(std::function<void(bool visible, int x, int y, int w, int h)> callback);

        // 是否已判定 WebView2 不可用（AppCore 用它检测「回调注册前就已失败」的同步路径）
        bool IsWebViewFailed() const { return webViewFailed_; }

        // 任意线程可调：把当前会话记录重新推给「对话」屏。
        // AI 请求跑在工作线程上，而 WebView2 只能在 UI 线程碰，所以内部只 PostMessage。
        void PushChatHistory();

        static constexpr UINT WM_APP_PUSH_CHAT = WM_APP + 41;

        // ---------- 配置项描述（终端展示的全部字段） ----------

        // 字段类型决定控件形态：文本框 / 整数 / 浮点 / 开关 / 单选项 / 预设网格 / 多选清单
        enum class FieldKind { Text, Int, Double, Bool, Enum, Preset, Multi };

        struct FieldDef {
            const char* key;            // ConfigManager 键名
            const wchar_t* label;       // 主标签
            const wchar_t* labelEn;     // 标签下方的等宽小字
            const wchar_t* note;        // 行尾右对齐说明
            FieldKind kind;
            const wchar_t* dflt;        // 键缺失时的默认值
            const wchar_t* options;     // Enum 用 L"none|http"；Preset 用 L"显示名=值|..."
            bool secret;                // 是否以密码方式显示（密钥类字段）
            int controlWidth;           // 输入类控件的逻辑宽度
        };

        struct SectionDef {
            const wchar_t* no;          // 卡片序号，等宽显示
            const wchar_t* title;
            const wchar_t* titleEn;
            const wchar_t* sub;
            const FieldDef* fields;
            int fieldCount;
        };

    private:
        ControlTerminalWindow() = default;
        ~ControlTerminalWindow() = default;
        ControlTerminalWindow(const ControlTerminalWindow&) = delete;
        ControlTerminalWindow& operator=(const ControlTerminalWindow&) = delete;

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        bool RegisterWindowClass(HINSTANCE hInstance);
        bool CreateTerminalWindow(HINSTANCE hInstance);

        // 逻辑像素（96 DPI）转物理像素
        int Scale(int value) const;

        // ---- WebView2 宿主 ----
        void CreateWebView();
        // 用户数据目录：第 0 次用上次成功过的那类目录，重试时换另一类/带序号的干净目录，
        // 绕开被上次崩溃残留的进程/文件锁住的 profile。usedProgramDir 回传是否用了程序目录。
        std::wstring WebViewUserDataDir(int attempt, bool* usedProgramDir = nullptr) const;
        void ConfigureWebViewSettings();
        void ResizeWebView();
        void DestroyWebView();
        void NavigateToUi();
        // WebView2 任一环节失败时统一走这里：还允许重试就先换目录重试，否则记日志并
        // （只一次）通知 AppCore 降级。
        void NotifyWebViewFailed(const char* reason);
        // 环境/控制器创建失败后的处理：清理半成品对象，能重试就排一个重试定时器。
        void ScheduleWebViewRetry(const std::string& reason);

        // ---- 配置读写（与原实现一致） ----
        void LoadFromConfig();
        std::wstring CurrentValueText(const char* key) const;
        // 服务地址对应内置服务商、而当前模型不在该服务商列表里时，把默认模型
        // 收敛到该服务商的第一个模型（返回是否改过）。服务商与模型不匹配时
        // 服务端会对每次请求回 HTTP 400。
        bool SnapModelToProvider();
        // 「？？？」一旦选中就锁住人设行：除非把哨兵文件 prompts/yandere.chr 删掉。
        // 判定同样看哨兵：文件在 = 锁；文件删了 = 页面上连这一项都不会再出现。
        bool IsPersonaFrozen() const;
        bool ValidateValue(const FieldDef& def, const std::wstring& value, std::wstring& error) const;
        void Save();
        void Reload();
        void RestoreDefaults();
        void SetStatus(const std::wstring& text, int kind);
        std::wstring NowString() const;

        // ---- 与页面之间的契约 ----
        std::string BuildSchemaJson() const;
        std::string BuildValuesJson() const;
        std::string BuildPresetsJson() const;
        std::string PresetOptionsJson(const std::string& key, const std::wstring& baseUrl,
                                      const std::wstring& currentValue) const;
        std::string BuildStateJson(bool resetScroll) const;
        std::string BuildErrorsJson() const;
        bool SendToPage(const std::string& json);
        void SendState(bool resetScroll);
        void SendStatusOnly();
        void HandleWebMessage(const std::string& json);
        void ApplyPostedValues(const nlohmann::json& message);

        // ---- 对话（中枢的「对话」屏）----
        // 真正下发（必须在 UI 线程上调用）。
        void SendChatHistory();

        // ---- 服务商探测（中枢「API 接入」屏）----
        // 在后台线程问服务商要模型清单，回来后推送结果。地址与密钥用当前草稿，
        // 所以「测一下」测的是用户刚填的东西，而不是已经保存的那份。
        void StartProbe();
        // 工作线程侧：暂存结果（加锁）
        void StoreProbeResult(const std::string& baseUrl, const AIClient::ProviderProbe& probe);
        // UI 线程侧：把结果推给页面，并按服务商给的清单刷新模型行
        void FinishProbe();
        void SendProbeState();
        // 推送模型清单（服务商没给出可用清单时回退到内置表）
        void PushModelOptions();

        static constexpr UINT WM_APP_PROBE_DONE = WM_APP + 42;

        // 探测结果（跨线程：工作线程写，UI 线程读）。
        // mutex 是 mutable：BuildStateJson 是 const，但也需要把状态一起下发。
        mutable std::mutex probeMutex_;
        int probeKind_ = 0;                         // 0=未测 1=进行中 2=可达 3=失败
        std::string probeText_;
        std::string probeUrl_;                      // 结果对应的地址，用来丢弃过期结果
        int probeModelCount_ = 0;                    // 服务商给出的模型数量（0 = 只能用内置表）
        std::vector<std::wstring> probeModels_;

        // 窗口句柄
        HWND hwnd_ = nullptr;
        HINSTANCE instance_ = nullptr;
        bool visible_ = false;

        // DPI
    // 最大化状态（本窗口是 WS_POPUP，IsZoomed 不可靠，自己记）
    bool maximized_ = false;
    RECT maxRestore_ = {};

        int dpi_ = 96;

        // WebView2 对象
        ComPtr<ICoreWebView2Environment> environment_;
        ComPtr<ICoreWebView2Controller> controller_;
        ComPtr<ICoreWebView2> webview_;
        EventRegistrationToken messageToken_{};
        EventRegistrationToken navigationToken_{};
        bool webviewReady_ = false;   // 页面已导航完成，可以接收状态
        bool statePending_ = false;   // 页面就绪前先攒着，就绪后补发

        // 配置草稿
        std::map<std::string, std::string> draft_;
        std::map<std::string, std::string> loaded_;
        std::vector<std::string> invalidKeys_;
        std::map<std::string, std::string> invalidErrors_;   // key -> 失败原因（UTF-8）

        // 状态行：0=普通 1=成功 2=失败
        std::wstring statusText_;
        int statusKind_ = 0;

        // 「恢复默认」二次确认的时间戳
        ULONGLONG restoreArmedAt_ = 0;

        std::function<void()> onSaved_;
        std::function<void()> onWebViewFailed_;
    std::function<void(bool, int, int, int, int)> onPetSlot_;
        bool webViewFailed_ = false;   // WebView2 已判定不可用（降级回调只发一次）
        int webViewAttempt_ = 0;       // 当前是第几次尝试（重试时用来换用户数据目录）
        bool webViewProfileIsProgram_ = false;  // 本次用的是程序目录下的 profile

        static const wchar_t* WINDOW_CLASS_NAME;
    };

} // namespace UnnamedBot
