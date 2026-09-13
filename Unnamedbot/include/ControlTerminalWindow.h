#pragma once

#include <Windows.h>
#include <string>
#include <vector>
#include <map>
#include <functional>

namespace UnnamedBot {

    // 控制终端：编辑 ./data/app.db 中配置项的 GUI 窗口。
    // 视觉规范取自 prototype (4).html —— 深蓝黑底、冷冰蓝描边、等宽字体与切角按钮。
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

        // ---------- 配置项描述（终端展示的全部字段） ----------

        // 字段类型决定控件形态：文本框 / 整数 / 浮点 / 开关 / 单选项
        enum class FieldKind { Text, Int, Double, Bool, Enum };

        struct FieldDef {
            const char* key;            // ConfigManager 键名
            const wchar_t* label;       // 主标签
            const wchar_t* labelEn;     // 标签下方的等宽小字
            const wchar_t* note;        // 行尾右对齐说明
            FieldKind kind;
            const wchar_t* dflt;        // 键缺失时的默认值
            const wchar_t* options;     // Enum 专用，形如 L"none|http|socks5"
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

        // 一行的绘制/命中几何，随滚动偏移重新计算
        struct RowLayout {
            const FieldDef* def = nullptr;
            RECT label{};
            RECT control{};
            RECT note{};
            RECT segments[4]{};
            int segmentCount = 0;
            HWND edit = nullptr;
        };

        struct CardLayout {
            const SectionDef* sec = nullptr;
            RECT rc{};
            RECT title{};
            RECT sub{};
            int rowCount = 0;
        };

        struct ButtonLayout {
            int id = 0;
            RECT rc{};
            const wchar_t* text = nullptr;
            bool primary = false;
        };

        struct FontSet {
            HFONT title = nullptr;
            HFONT heading = nullptr;
            HFONT label = nullptr;
            HFONT labelEn = nullptr;
            HFONT body = nullptr;
            HFONT mono = nullptr;
            HFONT monoSm = nullptr;
            HFONT monoXs = nullptr;
            HFONT button = nullptr;
        };

        static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

        bool RegisterWindowClass(HINSTANCE hInstance);
        bool CreateTerminalWindow(HINSTANCE hInstance);

        void CreateFonts();
        void DestroyFonts();

        // 逻辑像素（96 DPI）转物理像素
        int Scale(int value) const;
        int ClientWidth() const;
        int ClientHeight() const;
        int ViewportTop() const;
        int ViewportHeight() const;

        void RebuildLayout();
        void CreateFieldControls();
        void DestroyFieldControls();
        void SyncEditVisibility();

        void EnsureBackdrop();
        void ReleaseBackdrop();

        void Paint(HDC hdc);
        void PaintHeader(HDC hdc);
        void PaintCards(HDC hdc);
        void PaintFooter(HDC hdc);

        void LoadFromConfig();
        void RecalcDirty();
        std::wstring CurrentValue(const FieldDef& def) const;
        bool ValidateValue(const FieldDef& def, const std::wstring& value, std::wstring& error) const;
        void Save();
        void Reload();
        void RestoreDefaults();
        void SetStatus(const std::wstring& text, int kind);
        void ScrollBy(int pixels);
        std::wstring NowString() const;
        bool IsInvalid(const FieldDef& def) const;

        int ButtonIndexAt(POINT pt) const;
        int BoolRowAt(POINT pt) const;
        int EnumRowAt(POINT pt, int& optionIndex) const;
        bool IsHeaderDragArea(POINT pt) const;

        // 窗口句柄
        HWND hwnd_ = nullptr;
        HINSTANCE instance_ = nullptr;
        bool visible_ = false;

        // DPI 与字体
        int dpi_ = 96;
        FontSet fonts_;
        HBRUSH inputBrush_ = nullptr;

        // 背景缓存（渐变 + 辉光 + 网格只生成一次）
        HDC backdropDc_ = nullptr;
        HBITMAP backdropBitmap_ = nullptr;
        HBITMAP backdropOld_ = nullptr;
        int backdropW_ = 0;
        int backdropH_ = 0;

        // 布局结果
        std::vector<RowLayout> rows_;
        std::vector<CardLayout> cards_;
        std::vector<ButtonLayout> buttons_;
        std::map<std::string, HWND> edits_;
        RECT closeButton_{};

        // 滚动与内容尺寸
        int scrollY_ = 0;
        int contentHeight_ = 0;

        // 配置草稿
        std::map<std::string, std::string> draft_;
        std::map<std::string, std::string> loaded_;
        std::vector<std::string> invalidKeys_;
        bool dirty_ = false;

        // 状态行：0=普通 1=成功 2=失败
        std::wstring statusText_;
        int statusKind_ = 0;

        // 「恢复默认」二次确认的时间戳
        ULONGLONG restoreArmedAt_ = 0;

        // 鼠标悬停
        int hoveredButton_ = -1;

        std::function<void()> onSaved_;

        static const wchar_t* WINDOW_CLASS_NAME;

        // 按钮 ID
        static const int ID_BTN_SAVE = 1;
        static const int ID_BTN_RELOAD = 2;
        static const int ID_BTN_RESTORE = 3;
        static const int ID_BTN_CLOSE = 4;

        // 输入框控件 ID 起始值
        static const int ID_FIRST_EDIT = 2000;
    };

} // namespace UnnamedBot
