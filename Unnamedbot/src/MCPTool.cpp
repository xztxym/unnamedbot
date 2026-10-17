#include "../include/MCPTool.h"
#include "../include/Live2DManager.h"
#include "../include/TrayIcon.h"
#include "../include/Logger.h"
#include "../include/ConfigManager.h"
#include "../include/Utils.h"
#include <Windows.h>
#include <Psapi.h>
#include <tlhelp32.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace UnnamedBot {

    // ============ MCPToolManager 实现 ============

    MCPToolManager& MCPToolManager::Instance() {
        // 不随静态存储期销毁：进程退出阶段仍可能被调用（见 AppCore::Instance）。
        static MCPToolManager* instance = new MCPToolManager();
        return *instance;
    }

    void MCPToolManager::RegisterTool(std::shared_ptr<MCPToolBase> tool) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!tool) return;
        std::string name = tool->GetName();
        tools_[name] = tool;
        LOG_DEBUG("MCPToolManager", "Registered tool: " + name);
    }

    // ============================================================
    // Live2D：让 AI 自主驱动自己
    // ------------------------------------------------------------
    // 动作组、表情、参数都从模型文件里现读，不写死任何名字 —— 用户把任意 L2D
    // 模型丢进 models\ 目录，AI 就能用 live2d_models 看到它有什么，再用对应工具去用。
    // 换模型时会卸载其余模型，避免内存越吃越多。
    // ============================================================

    namespace {
        std::string DescribeCapabilities(const std::vector<Live2DManager::ModelCapability>& caps) {
            if (caps.empty()) {
                return "models 目录里没有找到可用的 Live2D 模型。";
            }
            std::ostringstream oss;
            oss << "已导入 " << caps.size() << " 个模型：";
            for (const auto& c : caps) {
                oss << "\n- " << c.id;
                if (c.current) oss << "（当前）";
                if (c.motions.empty()) {
                    oss << " · 无动作";
                } else {
                    oss << " · 动作: ";
                    for (size_t i = 0; i < c.motions.size(); ++i) oss << (i ? ", " : "") << c.motions[i];
                }
                if (!c.expressions.empty()) {
                    oss << " · 表情: ";
                    for (size_t i = 0; i < c.expressions.size(); ++i) oss << (i ? ", " : "") << c.expressions[i];
                }
                if (!c.parameters.empty()) {
                    oss << " · 参数 " << c.parameters.size() << " 个（live2d_set_parameter 可调，例如 ";
                    for (size_t i = 0; i < c.parameters.size() && i < 4; ++i) {
                        oss << (i ? ", " : "") << c.parameters[i];
                        if (i < c.parameterNames.size() && !c.parameterNames[i].empty() &&
                            c.parameterNames[i] != c.parameters[i]) {
                            oss << "（" << c.parameterNames[i] << "）";
                        }
                    }
                    oss << " …）";
                }
            }
            return oss.str();
        }

        nlohmann::json CapsToJson(const std::vector<Live2DManager::ModelCapability>& caps) {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& c : caps) {
                arr.push_back({
                    { "id", c.id }, { "current", c.current }, { "loaded", c.loaded },
                    { "motions", c.motions }, { "expressions", c.expressions },
                    { "parameters", c.parameters },
                    { "parameter_names", c.parameterNames },
                });
            }
            return arr;
        }
    } // namespace

    std::string Live2DListModelsTool::GetName() const { return "live2d_models"; }
    std::string Live2DListModelsTool::GetDescription() const {
        return "列出已导入的 Live2D 模型，以及每个模型自带的动作组、表情与参数名。改变自己的样子之前先用它看清楚有什么。";
    }
    nlohmann::json Live2DListModelsTool::GetParametersSchema() const { return nlohmann::json::object(); }
    MCPToolResult Live2DListModelsTool::Execute(const nlohmann::json&) {
        MCPToolResult result;
        const auto caps = Live2DManager::Instance().ListModelCapabilities();
        result.success = true;
        result.content = DescribeCapabilities(caps);
        result.data = { { "models", CapsToJson(caps) } };
        return result;
    }

    std::string Live2DSwitchModelTool::GetName() const { return "live2d_switch_model"; }
    std::string Live2DSwitchModelTool::GetDescription() const {
        return "把自己换成另一个已导入的 Live2D 模型（id 用 live2d_models 查）。换模型会卸载其他模型以省内存。";
    }
    nlohmann::json Live2DSwitchModelTool::GetParametersSchema() const {
        return { { "model_id", { { "type", "string" }, { "description", "模型 id，即 models 目录下的文件夹名" } } } };
    }
    MCPToolResult Live2DSwitchModelTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        const std::string modelId = parameters.value("model_id", std::string());
        if (modelId.empty()) {
            result.success = false;
            result.error = "缺少 model_id";
            return result;
        }
        if (Live2DManager::Instance().IsModelLoaded(modelId)) {
            // 已经在内存里的模型，切换本身很便宜，直接做
            if (!Live2DManager::Instance().SwitchModelById(modelId)) {
                result.success = false;
                result.error = "切换失败：" + modelId;
                return result;
            }
            result.success = true;
            result.content = "已经换成 " + modelId + "。\n" +
                DescribeCapabilities(Live2DManager::Instance().ListModelCapabilities());
            return result;
        }

        // 没加载过：首次加载要编译 Live2D 着色器（几十秒），不能在这里等，
        // 更不能占着渲染线程 —— 交给渲染线程在下一帧去做。
        Live2DManager::Instance().RequestModelSwitch(modelId);
        result.success = true;
        result.content = "正在把模型换成 " + modelId + "（新模型要准备一会儿，马上就好）。";
        return result;
    }

    std::string Live2DPlayMotionTool::GetName() const { return "live2d_play_motion"; }
    std::string Live2DPlayMotionTool::GetDescription() const {
        return "播放当前模型的一个动作组（名字取自 live2d_models 列出的 motions）。";
    }
    nlohmann::json Live2DPlayMotionTool::GetParametersSchema() const {
        return {
            { "group", { { "type", "string" }, { "description", "动作组名" } } },
            { "loop", { { "type", "boolean" }, { "description", "是否循环，默认 false" } } },
        };
    }
    MCPToolResult Live2DPlayMotionTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        const std::string group = parameters.value("group", std::string());
        const bool loop = parameters.value("loop", false);
        if (group.empty()) {
            result.success = false;
            result.error = "缺少 group";
            return result;
        }
        if (!Live2DManager::Instance().PlayMotion(group, loop)) {
            result.success = false;
            result.error = "这个模型没有名为 " + group + " 的动作组（先用 live2d_models 看看有哪些）";
            return result;
        }
        result.success = true;
        result.content = "正在播放动作：" + group;
        return result;
    }

    std::string Live2DSetExpressionTool::GetName() const { return "live2d_set_expression"; }
    std::string Live2DSetExpressionTool::GetDescription() const {
        return "换一个表情（名字取自 live2d_models 列出的 expressions）。";
    }
    nlohmann::json Live2DSetExpressionTool::GetParametersSchema() const {
        return { { "name", { { "type", "string" }, { "description", "表情名" } } } };
    }
    MCPToolResult Live2DSetExpressionTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        const std::string name = parameters.value("name", std::string());
        if (name.empty()) {
            result.success = false;
            result.error = "缺少 name";
            return result;
        }
        if (!Live2DManager::Instance().PlayExpression(name)) {
            result.success = false;
            result.error = "这个模型没有名为 " + name + " 的表情（先用 live2d_models 看看有哪些）";
            return result;
        }
        result.success = true;
        result.content = "表情已切换：" + name;
        return result;
    }

    std::string Live2DSetTransformTool::GetName() const { return "live2d_set_transform"; }
    std::string Live2DSetTransformTool::GetDescription() const {
        return "调整自己的位置、大小与透明度（想凑近、想缩到角落、想淡出时用）。位置是画布坐标，0 居中，范围约 -1~1。";
    }
    nlohmann::json Live2DSetTransformTool::GetParametersSchema() const {
        return {
            { "x", { { "type", "number" }, { "description", "水平位置，0 居中，约 -1~1" } } },
            { "y", { { "type", "number" }, { "description", "垂直位置，0 居中，约 -1~1" } } },
            { "scale", { { "type", "number" }, { "description", "缩放，1 为原始大小" } } },
            { "opacity", { { "type", "number" }, { "description", "不透明度 0~1" } } },
        };
    }
    MCPToolResult Live2DSetTransformTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        auto& l2d = Live2DManager::Instance();
        if (parameters.contains("x") || parameters.contains("y")) {
            float x = 0.0f;
            float y = 0.0f;
            l2d.GetModelPosition(x, y);
            if (parameters.contains("x")) x = parameters["x"].get<float>();
            if (parameters.contains("y")) y = parameters["y"].get<float>();
            l2d.SetModelPosition(x, y);
        }
        if (parameters.contains("scale")) {
            l2d.SetModelScale(parameters["scale"].get<float>());
        }
        if (parameters.contains("opacity")) {
            l2d.SetModelOpacity(parameters["opacity"].get<float>());
        }
        result.success = true;
        result.content = "位置与形态已调整。";
        return result;
    }

    std::string Live2DSetParameterTool::GetName() const { return "live2d_set_parameter"; }
    std::string Live2DSetParameterTool::GetDescription() const {
        return "直接设置当前模型的某个参数值（最通用的一招，能做动作表情覆盖不到的事：歪头、眼神、嘴型等）。参数名用 live2d_models 列出的。";
    }
    nlohmann::json Live2DSetParameterTool::GetParametersSchema() const {
        return {
            { "id", { { "type", "string" }, { "description", "参数 id，例如 ParamAngleX" } } },
            { "value", { { "type", "number" }, { "description", "参数值，多数参数范围 -30~30 或 0~1" } } },
        };
    }
    MCPToolResult Live2DSetParameterTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        const std::string id = parameters.value("id", std::string());
        if (id.empty() || !parameters.contains("value")) {
            result.success = false;
            result.error = "需要 id 与 value";
            return result;
        }
        auto model = Live2DManager::Instance().GetCurrentModel();
        if (!model) {
            result.success = false;
            result.error = "当前没有模型";
            return result;
        }
        const float value = parameters["value"].get<float>();
        model->SetParameterValue(id, value);
        result.success = true;
        result.content = "参数 " + id + " 已设为 " + std::to_string(value);
        return result;
    }

    std::string Live2DResetTool::GetName() const { return "live2d_reset"; }
    std::string Live2DResetTool::GetDescription() const {
        return "停止所有动作并把参数恢复默认（想变回平常的样子时用）。";
    }
    nlohmann::json Live2DResetTool::GetParametersSchema() const { return nlohmann::json::object(); }
    MCPToolResult Live2DResetTool::Execute(const nlohmann::json&) {
        MCPToolResult result;
        Live2DManager::Instance().StopAllMotions();
        auto model = Live2DManager::Instance().GetCurrentModel();
        if (model) {
            model->ResetParameters();
        }
        result.success = true;
        result.content = "已经回到平常的样子。";
        return result;
    }

    // ============================================================
    // 小范围操控：挪鼠标 / 挪窗口 / 打字符
    // ------------------------------------------------------------
    // 都是「小动作」：位移被夹在设置给的像素内（默认鼠标 ±120px、窗口 ±80px），
    // 键盘只接受可打印字符与少量普通键，功能键（F1~F24、Win、Alt、PrintScreen 等）
    // 一律拒绝 —— 不给她关窗口、切桌面、触发系统快捷键的能力。
    // ============================================================

    namespace {
        bool ClampOffset(int& dx, int& dy, int maxPx) {
            const int limit = (std::max)(10, (std::min)(maxPx, 400));
            dx = (std::max)(-limit, (std::min)(dx, limit));
            dy = (std::max)(-limit, (std::min)(dy, limit));
            return dx != 0 || dy != 0;
        }

        struct KeyStroke {
            WORD vk = 0;
            bool shift = false;
            bool printable = false;
            wchar_t wch = 0;
        };

        // 只认普通键；功能键/系统键一律返回 false
        bool ResolveKey(const std::string& raw, KeyStroke& out) {
            const std::string k = Utils::ToLower(Utils::Trim(raw));
            static const std::map<std::string, WORD> special = {
                { "enter", VK_RETURN }, { "tab", VK_TAB }, { "space", VK_SPACE },
                { "esc", VK_ESCAPE }, { "escape", VK_ESCAPE },
                { "backspace", VK_BACK }, { "delete", VK_DELETE }, { "del", VK_DELETE },
                { "left", VK_LEFT }, { "right", VK_RIGHT }, { "up", VK_UP }, { "down", VK_DOWN },
                { "home", VK_HOME }, { "end", VK_END },
                { "pageup", VK_PRIOR }, { "pagedown", VK_NEXT },
                { "ctrl", VK_CONTROL }, { "control", VK_CONTROL }, { "shift", VK_SHIFT },
            };
            auto it = special.find(k);
            if (it != special.end()) {
                out.vk = it->second;
                return true;
            }
            // 明确拒绝：F1~F24、Win、Alt、PrintScreen 及其它系统键
            if (k.size() >= 2 && k[0] == 'f' && std::isdigit(static_cast<unsigned char>(k[1]))) {
                return false;
            }
            static const char* forbidden[] = {
                "alt", "win", "windows", "printscreen", "prtsc", "insert", "ins", "menu",
                "capslock", "numlock", "scrolllock", "pause", "break", "apps", "lwin", "rwin",
            };
            for (const char* f : forbidden) {
                if (k == f) {
                    return false;
                }
            }
            if (k.size() == 1) {
                const wchar_t wc = static_cast<wchar_t>(static_cast<unsigned char>(k[0]));
                const SHORT vk = VkKeyScanW(wc);
                if (vk == -1) {
                    return false;
                }
                out.vk = static_cast<WORD>(vk & 0xFF);
                out.shift = (vk & 0x100) != 0;
                out.printable = true;
                out.wch = wc;
                return true;
            }
            return false;
        }

        void SendKeyDown(WORD vk, bool shift) {
            if (shift) {
                INPUT s[2] = {};
                s[0].type = INPUT_KEYBOARD; s[0].ki.wVk = VK_SHIFT;
                s[1].type = INPUT_KEYBOARD; s[1].ki.wVk = vk;
                SendInput(2, s, sizeof(INPUT));
            }
            else {
                INPUT i = {};
                i.type = INPUT_KEYBOARD;
                i.ki.wVk = vk;
                SendInput(1, &i, sizeof(INPUT));
            }
        }

        void SendKeyUp(WORD vk, bool shift) {
            if (shift) {
                INPUT s[2] = {};
                s[0].type = INPUT_KEYBOARD; s[0].ki.wVk = vk; s[0].ki.dwFlags = KEYEVENTF_KEYUP;
                s[1].type = INPUT_KEYBOARD; s[1].ki.wVk = VK_SHIFT; s[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, s, sizeof(INPUT));
            }
            else {
                INPUT i = {};
                i.type = INPUT_KEYBOARD;
                i.ki.wVk = vk;
                i.ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(1, &i, sizeof(INPUT));
            }
        }
    } // namespace

    std::string NudgeMouseTool::GetName() const { return "nudge_mouse"; }
    std::string NudgeMouseTool::GetDescription() const {
        return "小范围挪动鼠标（位移会被限制在设置的像素内，默认 ±120）。可以让指针靠过去、或者按住拖一小段。";
    }
    nlohmann::json NudgeMouseTool::GetParametersSchema() const {
        return {
            { "dx", { { "type", "integer" }, { "description", "水平位移，正数向右" } } },
            { "dy", { { "type", "integer" }, { "description", "垂直位移，正数向下" } } },
            { "drag", { { "type", "boolean" }, { "description", "是否按住左键拖动，默认 false" } } },
        };
    }
    MCPToolResult NudgeMouseTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        if (!ConfigManager::Instance().GetBool(ConfigManager::KEY_TOOL_MOUSE_ENABLED, true)) {
            result.success = false;
            result.error = "鼠标操控已被用户关闭。";
            return result;
        }
        int dx = parameters.value("dx", 0);
        int dy = parameters.value("dy", 0);
        const int maxPx = static_cast<int>(ConfigManager::Instance().GetInt(ConfigManager::KEY_TOOL_MOUSE_MAX_PX, 120));
        if (!ClampOffset(dx, dy, maxPx)) {
            result.success = false;
            result.error = "位移为零。";
            return result;
        }
        POINT p{};
        if (!GetCursorPos(&p)) {
            result.success = false;
            result.error = "读不到鼠标位置。";
            return result;
        }
        const bool drag = parameters.value("drag", false);
        if (drag) {
            INPUT down[3] = {};
            down[0].type = INPUT_MOUSE; down[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
            down[1].type = INPUT_MOUSE; down[1].mi.dwFlags = MOUSEEVENTF_MOVE;
            down[1].mi.dx = (dx * 65535) / GetSystemMetrics(SM_CXSCREEN);
            down[1].mi.dy = (dy * 65535) / GetSystemMetrics(SM_CYSCREEN);
            down[2].type = INPUT_MOUSE; down[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(3, down, sizeof(INPUT));
        }
        else {
            SetCursorPos(p.x + dx, p.y + dy);
        }
        result.success = true;
        result.content = std::string(drag ? "按住左键拖了" : "把鼠标挪了") + " (" + std::to_string(dx) + ", " + std::to_string(dy) + ") 像素。";
        return result;
    }

    std::string MoveWindowTool::GetName() const { return "move_window"; }
    std::string MoveWindowTool::GetDescription() const {
        return "把某个窗口小范围挪一下（位移限制在设置的像素内，默认 ±80）。不传 title 就挪当前最前面的窗口。";
    }
    nlohmann::json MoveWindowTool::GetParametersSchema() const {
        return {
            { "title", { { "type", "string" }, { "description", "窗口标题的一部分，可省略" } } },
            { "dx", { { "type", "integer" }, { "description", "水平位移" } } },
            { "dy", { { "type", "integer" }, { "description", "垂直位移" } } },
        };
    }
    MCPToolResult MoveWindowTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        if (!ConfigManager::Instance().GetBool(ConfigManager::KEY_TOOL_WINDOW_ENABLED, true)) {
            result.success = false;
            result.error = "窗口移动已被用户关闭。";
            return result;
        }
        int dx = parameters.value("dx", 0);
        int dy = parameters.value("dy", 0);
        const int maxPx = static_cast<int>(ConfigManager::Instance().GetInt(ConfigManager::KEY_TOOL_WINDOW_MAX_PX, 80));
        if (!ClampOffset(dx, dy, maxPx)) {
            result.success = false;
            result.error = "位移为零。";
            return result;
        }

        HWND target = nullptr;
        const std::string title = Utils::Trim(parameters.value("title", std::string()));
        if (!title.empty()) {
            const std::wstring lower = Utils::Utf8ToWide(Utils::ToLower(title));
            struct FindCtx {
                std::wstring needle;
                HWND found = nullptr;
            } ctx{ lower, nullptr };

            EnumWindows([](HWND h, LPARAM l) -> BOOL {
                auto* c = reinterpret_cast<FindCtx*>(l);
                if (!IsWindowVisible(h)) {
                    return TRUE;
                }
                wchar_t buf[512] = {};
                GetWindowTextW(h, buf, 512);
                std::wstring tl(buf);
                for (auto& ch : tl) {
                    ch = static_cast<wchar_t>(towlower(ch));
                }
                if (tl.find(c->needle) != std::wstring::npos) {
                    c->found = h;
                    return FALSE;
                }
                return TRUE;
                }, reinterpret_cast<LPARAM>(&ctx));
            target = ctx.found;
        }
        if (!target) {
            target = GetForegroundWindow();
        }
        if (!target) {
            result.success = false;
            result.error = "没找到可移动的窗口。";
            return result;
        }

        RECT rc{};
        if (!GetWindowRect(target, &rc)) {
            result.success = false;
            result.error = "读不到窗口位置。";
            return result;
        }
        SetWindowPos(target, nullptr, rc.left + dx, rc.top + dy, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        wchar_t nameBuf[256] = {};
        GetWindowTextW(target, nameBuf, 256);
        result.success = true;
        result.content = "把「" + Utils::WideToUtf8(nameBuf) + "」挪了 (" +
            std::to_string(dx) + ", " + std::to_string(dy) + ") 像素。";
        return result;
    }

    std::string SendKeysTool::GetName() const { return "send_keys"; }
    std::string SendKeysTool::GetDescription() const {
        return "敲键盘：可以按一组普通键（如 ctrl+s、enter），也可以直接打一段文字。功能键（F1~F24、Win、Alt）被禁止。";
    }
    nlohmann::json SendKeysTool::GetParametersSchema() const {
        return {
            { "keys", { { "type", "array" }, { "items", { { "type", "string" } } },
                        { "description", "按键名数组，如 [\"ctrl\",\"s\"]；最多 12 个" } } },
            { "text", { { "type", "string" }, { "description", "要打的文字（会逐字输入，最多 200 字）" } } },
        };
    }
    MCPToolResult SendKeysTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        if (!ConfigManager::Instance().GetBool(ConfigManager::KEY_TOOL_KEYBOARD_ENABLED, true)) {
            result.success = false;
            result.error = "键盘输入已被用户关闭。";
            return result;
        }

        std::vector<KeyStroke> strokes;
        if (parameters.contains("text") && parameters["text"].is_string()) {
            const std::wstring text = Utils::Utf8ToWide(parameters["text"].get<std::string>());
            for (size_t i = 0; i < text.size() && i < 200; ++i) {
                KeyStroke ks;
                ks.printable = false;
                ks.wch = text[i];
                ks.vk = 0;
                strokes.push_back(ks);
            }
        }
        else if (parameters.contains("keys") && parameters["keys"].is_array()) {
            for (const auto& k : parameters["keys"]) {
                if (!k.is_string() || strokes.size() >= 12) {
                    break;
                }
                KeyStroke ks;
                if (!ResolveKey(k.get<std::string>(), ks)) {
                    result.success = false;
                    result.error = "这个键不允许：" + k.get<std::string>() + "（功能键与系统键被禁用）";
                    return result;
                }
                strokes.push_back(ks);
            }
        }
        if (strokes.empty()) {
            result.success = false;
            result.error = "没有要发送的按键或文字。";
            return result;
        }

        for (const auto& ks : strokes) {
            if (!ks.printable) {
                // 文字模式：按字符走 Unicode 输入，中文也能打
                INPUT in[2] = {};
                in[0].type = INPUT_KEYBOARD; in[0].ki.wScan = ks.wch; in[0].ki.dwFlags = KEYEVENTF_UNICODE;
                in[1].type = INPUT_KEYBOARD; in[1].ki.wScan = ks.wch; in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
                SendInput(2, in, sizeof(INPUT));
            }
            else {
                SendKeyDown(ks.vk, ks.shift);
                SendKeyUp(ks.vk, ks.shift);
            }
        }
        result.success = true;
        result.content = "已发送 " + std::to_string(strokes.size()) + " 个按键/字符。";
        return result;
    }

    void MCPToolManager::RegisterDefaultTools() {
        RegisterTool(std::make_shared<GetCurrentTimeTool>());
        RegisterTool(std::make_shared<GetFrontendProcessesTool>());
        RegisterTool(std::make_shared<GetWeatherTool>());
        RegisterTool(std::make_shared<GetScreenCaptureTool>());
        RegisterTool(std::make_shared<OpenUrlTool>());
        RegisterTool(std::make_shared<SendNotificationTool>());
        // Live2D 自主控制：换模型、放动作、换表情、调形态与参数
        RegisterTool(std::make_shared<Live2DListModelsTool>());
        RegisterTool(std::make_shared<Live2DSwitchModelTool>());
        RegisterTool(std::make_shared<Live2DPlayMotionTool>());
        RegisterTool(std::make_shared<Live2DSetExpressionTool>());
        RegisterTool(std::make_shared<Live2DSetTransformTool>());
        RegisterTool(std::make_shared<Live2DSetParameterTool>());
        RegisterTool(std::make_shared<Live2DResetTool>());
        // 小范围操控：挪鼠标、挪窗口、打字符（功能键除外）
        RegisterTool(std::make_shared<NudgeMouseTool>());
        RegisterTool(std::make_shared<MoveWindowTool>());
        RegisterTool(std::make_shared<SendKeysTool>());
        LOG_INFO("MCPToolManager", "Registered default tools.");
    }

    std::vector<ToolDefinition> MCPToolManager::GetToolDefinitions() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<ToolDefinition> definitions;

        for (const auto& pair : tools_) {
            const auto& tool = pair.second;
            ToolDefinition def;
            def.type = "function";
            def.function.name = tool->GetName();
            def.function.description = tool->GetDescription();

            // GetParametersSchema() 给的是「属性表」（参数名 -> 参数描述），
            // 而 ToolDefinition::function.parameters 按约定要的是完整 JSON Schema。
            // 少了这层包裹，参数为空的工具会送出 "parameters": {}，服务端一律拒成
            // HTTP 400：Invalid schema for function 'get_current_time':
            // schema must be a JSON Schema of 'type: "object"', got 'type: null'。
            // 结果是「只要请求带上工具就必然失败」——AI 一直不回话的元凶之一。
            const nlohmann::json properties = tool->GetParametersSchema();
            def.function.parameters = {
                { "type", "object" },
                { "properties", properties.is_object() ? properties : nlohmann::json::object() },
            };
            definitions.push_back(def);
        }

        return definitions;
    }

    MCPToolResult MCPToolManager::ExecuteTool(const std::string& toolName, const nlohmann::json& parameters) {
        // 使用默认回调（如果有）
        return ExecuteTool(toolName, parameters, defaultConfirmCallback_);
    }

    MCPToolResult MCPToolManager::ExecuteTool(
        const std::string& toolName,
        const nlohmann::json& parameters,
        ConfirmationCallback confirmCallback
    ) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = tools_.find(toolName);
        if (it == tools_.end()) {
            MCPToolResult result;
            result.success = false;
            result.error = "Tool not found: " + toolName;
            LOG_ERROR("MCPToolManager", result.error);
            return result;
        }

        auto& tool = it->second;

        // 检查是否需要确认
        if (tool->RequiresConfirmation()) {
            if (!confirmCallback) {
                // 没有确认回调，直接返回需要确认状态
                MCPToolResult result;
                result.success = false;
                result.requiresConfirmation = true;
                result.confirmationMessage = "Tool " + toolName + " requires user confirmation.";
                return result;
            }

            // 显示确认对话框
            std::string confirmMsg = "Do you want to execute tool: " + toolName + "?\n\n";
            confirmMsg += "Parameters: " + parameters.dump(2);

            if (!confirmCallback(confirmMsg)) {
                MCPToolResult result;
                result.success = false;
                result.error = "User cancelled tool execution.";
                LOG_INFO("MCPToolManager", "Tool execution cancelled by user: " + toolName);
                return result;
            }
        }

        LOG_DEBUG("MCPToolManager", "Executing tool: " + toolName + " with params: " + parameters.dump());
        return tool->Execute(parameters);
    }

    std::vector<std::string> MCPToolManager::GetToolNames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        for (const auto& pair : tools_) {
            names.push_back(pair.first);
        }
        return names;
    }

    bool MCPToolManager::HasTool(const std::string& toolName) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return tools_.find(toolName) != tools_.end();
    }

    void MCPToolManager::SetConfirmationCallback(ConfirmationCallback callback) {
        std::lock_guard<std::mutex> lock(mutex_);
        defaultConfirmCallback_ = callback;
    }

    // ============ GetCurrentTimeTool 实现 ============

    MCPToolResult GetCurrentTimeTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            auto now = std::chrono::system_clock::now();
            auto time_t_now = std::chrono::system_clock::to_time_t(now);
            struct tm tm_now;
            localtime_s(&tm_now, &time_t_now);

            std::ostringstream oss;
            oss << std::put_time(&tm_now, "%Y-%m-%d %H:%M:%S");

            // 获取时区信息
            TIME_ZONE_INFORMATION tzInfo;
            GetTimeZoneInformation(&tzInfo);
            int biasMinutes = tzInfo.Bias;
            char tzSign = biasMinutes <= 0 ? '+' : '-';
            int tzHours = abs(biasMinutes) / 60;
            int tzMins = abs(biasMinutes) % 60;

            oss << " (UTC" << tzSign << std::setw(2) << std::setfill('0') << tzHours
                << ":" << std::setw(2) << std::setfill('0') << tzMins << ")";

            result.content = oss.str();
            result.data["datetime"] = oss.str();
            result.data["timestamp"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                now.time_since_epoch()
            ).count();

            LOG_DEBUG("GetCurrentTimeTool", "Current time: " + result.content);
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to get time: " + std::string(e.what());
            LOG_ERROR("GetCurrentTimeTool", result.error);
        }

        return result;
    }

    // ============ GetFrontendProcessesTool 实现 ============

    MCPToolResult GetFrontendProcessesTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            auto processes = MCPUtils::GetFrontendProcesses();
            nlohmann::json processArray = nlohmann::json::array();

            for (const auto& proc : processes) {
                nlohmann::json procJson;
                procJson["name"] = proc.at("name");
                procJson["title"] = proc.at("title");
                procJson["path"] = proc.at("path");
                procJson["pid"] = std::stoi(proc.at("pid"));
                processArray.push_back(procJson);
            }

            result.data["processes"] = processArray;
            result.data["count"] = processArray.size();

            // 生成文本描述
            std::ostringstream oss;
            oss << "Found " << processArray.size() << " frontend applications:\n";
            for (const auto& proc : processes) {
                oss << "  - " << proc.at("title") << " (" << proc.at("name") << ")\n";
            }
            result.content = oss.str();

            LOG_DEBUG("GetFrontendProcessesTool", "Retrieved " + std::to_string(processes.size()) + " processes.");
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to get processes: " + std::string(e.what());
            LOG_ERROR("GetFrontendProcessesTool", result.error);
        }

        return result;
    }

    // ============ GetWeatherTool 实现 ============

    MCPToolResult GetWeatherTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            // 获取城市参数
            std::string city;
            if (parameters.contains("city") && parameters["city"].is_string()) {
                city = parameters["city"];
            }
            else {
                // 默认使用IP获取城市
                std::string ip = MCPUtils::GetPublicIP();
                city = MCPUtils::GetCityByIP(ip);
                if (city.empty()) {
                    city = "北京"; // 默认
                }
            }

            // 获取高德API Key
            std::string apiKey = ConfigManager::Instance().GetString(ConfigManager::KEY_AMAP_API_KEY);
            if (apiKey.empty()) {
                result.success = false;
                result.error = "AMap API Key not configured. Please set it in settings.";
                LOG_ERROR("GetWeatherTool", result.error);
                return result;
            }

            std::string weatherData = MCPUtils::GetWeatherByCity(city, apiKey);
            if (weatherData.empty()) {
                result.success = false;
                result.error = "Failed to get weather data for city: " + city;
                LOG_ERROR("GetWeatherTool", result.error);
                return result;
            }

            auto jsonData = nlohmann::json::parse(weatherData);
            if (jsonData.contains("status") && jsonData["status"] == "1") {
                result.data = jsonData;
                result.content = "Weather for " + city + ":\n";

                if (jsonData.contains("lives") && jsonData["lives"].is_array() && !jsonData["lives"].empty()) {
                    auto live = jsonData["lives"][0];
                    result.content += "  Weather: " + live["weather"].get<std::string>() + "\n";
                    result.content += "  Temperature: " + live["temperature"].get<std::string>() + "°C\n";
                    result.content += "  Wind: " + live["winddirection"].get<std::string>() + " " + live["windpower"].get<std::string>() + "\n";
                    result.content += "  Humidity: " + live["humidity"].get<std::string>() + "%\n";
                    result.content += "  Report Time: " + live["reporttime"].get<std::string>();
                }
            }
            else {
                result.success = false;
                result.error = "API returned error: " + jsonData.dump();
                LOG_ERROR("GetWeatherTool", result.error);
            }
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to get weather: " + std::string(e.what());
            LOG_ERROR("GetWeatherTool", result.error);
        }

        return result;
    }

    // ============ GetScreenCaptureTool 实现 ============

    MCPToolResult GetScreenCaptureTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            std::vector<unsigned char> imageData;
            int width = 0, height = 0;

            if (!MCPUtils::CaptureScreen(imageData, width, height)) {
                result.success = false;
                result.error = "Failed to capture screen.";
                LOG_ERROR("GetScreenCaptureTool", result.error);
                return result;
            }

            // 将图像数据转换为Base64
            std::string base64Data;
            // 这里简化处理，实际应该使用Base64编码
            // 暂时返回占位符
            result.data["width"] = width;
            result.data["height"] = height;
            result.data["size"] = imageData.size();
            result.data["base64"] = "BASE64_DATA_PLACEHOLDER"; // 实际需要Base64编码
            result.data["format"] = "png";

            result.content = "Screen captured successfully. Size: " + std::to_string(width) + "x" + std::to_string(height);
            LOG_DEBUG("GetScreenCaptureTool", result.content);
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to capture screen: " + std::string(e.what());
            LOG_ERROR("GetScreenCaptureTool", result.error);
        }

        return result;
    }

    // ============ OpenUrlTool 实现 ============

    MCPToolResult OpenUrlTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            if (!parameters.contains("url") || !parameters["url"].is_string()) {
                result.success = false;
                result.error = "Missing 'url' parameter.";
                LOG_ERROR("OpenUrlTool", result.error);
                return result;
            }

            std::string url = parameters["url"];

            // 打开URL
            SHELLEXECUTEINFO sei = {};
            sei.cbSize = sizeof(SHELLEXECUTEINFO);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpFile = url.c_str();
            sei.nShow = SW_SHOWNORMAL;

            if (ShellExecuteEx(&sei)) {
                if (sei.hProcess) {
                    CloseHandle(sei.hProcess);
                }
                result.content = "Opened URL: " + url;
                result.data["url"] = url;
                LOG_DEBUG("OpenUrlTool", result.content);
            }
            else {
                result.success = false;
                result.error = "Failed to open URL: " + url + " (Error: " + std::to_string(GetLastError()) + ")";
                LOG_ERROR("OpenUrlTool", result.error);
            }
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to open URL: " + std::string(e.what());
            LOG_ERROR("OpenUrlTool", result.error);
        }

        return result;
    }

    // ============ SendNotificationTool 实现 ============

    MCPToolResult SendNotificationTool::Execute(const nlohmann::json& parameters) {
        MCPToolResult result;
        result.success = true;

        try {
            if (!parameters.contains("text") || !parameters["text"].is_string()) {
                result.success = false;
                result.error = "Missing 'text' parameter.";
                LOG_ERROR("SendNotificationTool", result.error);
                return result;
            }

            std::string text = parameters["text"];
            std::string title = parameters.contains("title") && parameters["title"].is_string()
                ? parameters["title"].get<std::string>()
                : "UnnamedBot Notification";

            // 使用Windows通知（系统托盘气球提示）
            // 这里简化，使用MessageBox作为替代
            // 实际应使用托盘通知
            if (!ConfigManager::Instance().GetBool(ConfigManager::KEY_NOTIFY_ENABLED, true)) {
                // 用户关掉了系统弹窗：明确告诉她，免得她以为发送成功而反复重试
                result.success = true;
                result.content = "系统弹窗已被用户关闭，这条通知没有弹出来。";
                return result;
            }

            // 走右下角托盘气泡：非阻塞，也不再是以前那个 Modal MessageBox。
            // 编码必须用 Utils::Utf8ToWide —— 原来的 std::wstring(text.begin(), text.end())
            // 会把 UTF-8 按字节塞进 wchar_t，中文全是乱码。
            TrayIcon::Instance().ShowNotification(
                Utils::Utf8ToWide(title),
                Utils::Utf8ToWide(text),
                NIIF_INFO,
                5000);


            result.content = "Notification sent: " + text;
            result.data["title"] = title;
            result.data["text"] = text;
            LOG_DEBUG("SendNotificationTool", result.content);
        }
        catch (const std::exception& e) {
            result.success = false;
            result.error = "Failed to send notification: " + std::string(e.what());
            LOG_ERROR("SendNotificationTool", result.error);
        }

        return result;
    }

    // ============ MCPUtils 实现 ============

    namespace MCPUtils {

        std::string GetWeatherByCity(const std::string& city, const std::string& apiKey) {
            try {
                // 首先根据城市名获取城市编码
                std::string geoUrl = "https://restapi.amap.com/v3/geocode/geo";
                std::string geoParams = "?address=" + city + "&key=" + apiKey;

                auto geoResponse = cpr::Get(
                    cpr::Url{ geoUrl + geoParams },
                    cpr::Timeout{ std::chrono::seconds(10) }
                );

                if (geoResponse.status_code != 200) {
                    LOG_ERROR("MCPUtils", "Failed to get city code: " + geoResponse.text);
                    return "";
                }

                auto geoJson = nlohmann::json::parse(geoResponse.text);
                if (geoJson.contains("status") && geoJson["status"] == "1" &&
                    geoJson.contains("geocodes") && geoJson["geocodes"].is_array() &&
                    !geoJson["geocodes"].empty()) {

                    std::string cityCode = geoJson["geocodes"][0]["adcode"];

                    // 获取天气
                    std::string weatherUrl = "https://restapi.amap.com/v3/weather/weatherInfo";
                    std::string weatherParams = "?city=" + cityCode + "&key=" + apiKey + "&extensions=base";

                    auto weatherResponse = cpr::Get(
                        cpr::Url{ weatherUrl + weatherParams },
                        cpr::Timeout{ std::chrono::seconds(10) }
                    );

                    if (weatherResponse.status_code == 200) {
                        return weatherResponse.text;
                    }
                    else {
                        LOG_ERROR("MCPUtils", "Failed to get weather: " + weatherResponse.text);
                    }
                }
                else {
                    LOG_ERROR("MCPUtils", "Failed to get city code: " + geoResponse.text);
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("MCPUtils", "GetWeatherByCity exception: " + std::string(e.what()));
            }

            return "";
        }

        std::string GetPublicIP() {
            try {
                auto response = cpr::Get(
                    cpr::Url{ "http://ip-api.com/json/" },
                    cpr::Timeout{ std::chrono::seconds(5) }
                );

                if (response.status_code == 200) {
                    auto json = nlohmann::json::parse(response.text);
                    if (json.contains("query")) {
                        return json["query"];
                    }
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("MCPUtils", "GetPublicIP exception: " + std::string(e.what()));
            }
            return "";
        }

        std::string GetCityByIP(const std::string& ip) {
            try {
                std::string url = "http://ip-api.com/json/" + ip;
                auto response = cpr::Get(
                    cpr::Url{ url },
                    cpr::Timeout{ std::chrono::seconds(5) }
                );

                if (response.status_code == 200) {
                    auto json = nlohmann::json::parse(response.text);
                    if (json.contains("city")) {
                        return json["city"];
                    }
                }
            }
            catch (const std::exception& e) {
                LOG_ERROR("MCPUtils", "GetCityByIP exception: " + std::string(e.what()));
            }
            return "";
        }

        double CalculateDistance(double lat1, double lon1, double lat2, double lon2) {
            // 使用Haversine公式计算距离（公里）
            const double R = 6371.0; // 地球半径（公里）
            double dLat = (lat2 - lat1) * 3.141592653589793 / 180.0;
            double dLon = (lon2 - lon1) * 3.141592653589793 / 180.0;
            double a = sin(dLat / 2) * sin(dLat / 2) +
                cos(lat1 * 3.141592653589793 / 180.0) * cos(lat2 * 3.141592653589793 / 180.0) *
                sin(dLon / 2) * sin(dLon / 2);
            double c = 2 * atan2(sqrt(a), sqrt(1 - a));
            return R * c;
        }

        bool CaptureScreen(std::vector<unsigned char>& imageData, int& width, int& height) {
            // 使用DXGI捕获屏幕
            HRESULT hr = S_OK;

            // 创建DXGI设备
            IDXGIFactory1* factory = nullptr;
            hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory);
            if (FAILED(hr)) {
                LOG_ERROR("MCPUtils", "Failed to create DXGI factory.");
                return false;
            }

            // 获取第一个适配器
            IDXGIAdapter1* adapter = nullptr;
            hr = factory->EnumAdapters1(0, &adapter);
            if (FAILED(hr)) {
                factory->Release();
                LOG_ERROR("MCPUtils", "Failed to enumerate adapters.");
                return false;
            }

            // 创建D3D11设备
            ID3D11Device* device = nullptr;
            ID3D11DeviceContext* context = nullptr;
            D3D_FEATURE_LEVEL level;
            hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &level, &context);
            if (FAILED(hr)) {
                adapter->Release();
                factory->Release();
                LOG_ERROR("MCPUtils", "Failed to create D3D11 device.");
                return false;
            }

            // 获取输出（DuplicateOutput 定义在 IDXGIOutput1 上）
            IDXGIOutput* outputBase = nullptr;
            hr = adapter->EnumOutputs(0, &outputBase);
            if (FAILED(hr)) {
                device->Release();
                context->Release();
                adapter->Release();
                factory->Release();
                LOG_ERROR("MCPUtils", "Failed to enumerate outputs.");
                return false;
            }

            IDXGIOutput1* output = nullptr;
            hr = outputBase->QueryInterface(IID_PPV_ARGS(&output));
            outputBase->Release();
            if (FAILED(hr)) {
                device->Release();
                context->Release();
                adapter->Release();
                factory->Release();
                LOG_ERROR("MCPUtils", "Failed to query IDXGIOutput1.");
                return false;
            }

            // 获取桌面大小
            DXGI_OUTPUT_DESC desc;
            output->GetDesc(&desc);
            width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

            // 创建输出复制
            IDXGIOutputDuplication* duplication = nullptr;
            hr = output->DuplicateOutput(device, &duplication);
            if (FAILED(hr)) {
                output->Release();
                device->Release();
                context->Release();
                adapter->Release();
                factory->Release();
                LOG_ERROR("MCPUtils", "Failed to duplicate output.");
                return false;
            }

            // 获取桌面帧
            IDXGIResource* resource = nullptr;
            DXGI_OUTDUPL_FRAME_INFO frameInfo;
            hr = duplication->AcquireNextFrame(1000, &frameInfo, &resource);
            if (SUCCEEDED(hr) && resource) {
                // 获取纹理
                ID3D11Texture2D* texture = nullptr;
                hr = resource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&texture);
                if (SUCCEEDED(hr) && texture) {
                    // 获取纹理描述
                    D3D11_TEXTURE2D_DESC texDesc;
                    texture->GetDesc(&texDesc);

                    // 创建CPU可访问的纹理
                    D3D11_TEXTURE2D_DESC stagingDesc = texDesc;
                    stagingDesc.Usage = D3D11_USAGE_STAGING;
                    stagingDesc.BindFlags = 0;
                    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    stagingDesc.MiscFlags = 0;

                    ID3D11Texture2D* stagingTexture = nullptr;
                    hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
                    if (SUCCEEDED(hr)) {
                        // 复制到CPU可访问纹理
                        context->CopyResource(stagingTexture, texture);

                        // 映射数据
                        D3D11_MAPPED_SUBRESOURCE mapped;
                        hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mapped);
                        if (SUCCEEDED(hr)) {
                            // 计算图像数据大小（简化为PNG格式，但这里只保存原始数据）
                            size_t dataSize = mapped.RowPitch * texDesc.Height;
                            imageData.resize(dataSize);
                            memcpy(imageData.data(), mapped.pData, dataSize);
                            context->Unmap(stagingTexture, 0);
                        }
                        stagingTexture->Release();
                    }
                    texture->Release();
                }
                resource->Release();
                duplication->ReleaseFrame();
            }

            // 释放资源
            duplication->Release();
            output->Release();
            device->Release();
            context->Release();
            adapter->Release();
            factory->Release();

            return !imageData.empty();
        }

        std::vector<std::map<std::string, std::string>> GetFrontendProcesses() {
            std::vector<std::map<std::string, std::string>> processes;

            // 获取所有窗口
            EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
                auto* processesPtr = reinterpret_cast<std::vector<std::map<std::string, std::string>>*>(lParam);

                // 检查窗口是否可见且不是子窗口
                if (!IsWindowVisible(hwnd)) return TRUE;
                if (GetParent(hwnd) != nullptr) return TRUE;
                if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;

                // 获取窗口标题
                wchar_t windowTitle[256];
                GetWindowTextW(hwnd, windowTitle, 256);
                std::wstring titleW = windowTitle;
                if (titleW.empty()) return TRUE;

                // 获取进程ID
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid == 0) return TRUE;

                // 获取进程句柄
                HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
                if (!hProcess) return TRUE;

                // 获取进程可执行文件名
                wchar_t exePath[MAX_PATH];
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size)) {
                    std::wstring exePathW = exePath;
                    std::string exeName = Utils::PathToUtf8(std::filesystem::path(exePathW).filename());

                    std::map<std::string, std::string> proc;
                    proc["title"] = Utils::WideToUtf8(titleW);
                    proc["name"] = exeName;
                    proc["path"] = Utils::PathToUtf8(std::filesystem::path(exePathW));
                    proc["pid"] = std::to_string(pid);
                    processesPtr->push_back(proc);
                }

                CloseHandle(hProcess);
                return TRUE;
                }, reinterpret_cast<LPARAM>(&processes));

            return processes;
        }

        // 辅助：wstring转string
        std::string wstring_to_utf8(const std::wstring& wstr) {
            if (wstr.empty()) return "";
            int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string str(size_needed - 1, 0);
            WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], size_needed, nullptr, nullptr);
            return str;
        }

    } // namespace MCPUtils

} // namespace UnnamedBot