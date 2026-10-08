#pragma once

#include <string>
#include <filesystem>
#include <vector>

namespace UnnamedBot {

    // 用户提示词库：从 ./prompts 目录读取 .md / .txt 文本，按启用的清单拼进系统提示。
    //
    // 目录里的 yandere.chr 是**哨兵文件**：只有它存在时，配置页才会出现「？？？」
    // 选项（见 ControlTerminalWindow 的动态选项）。它的内容不参与提示词拼装，
    // 病娇人格是硬编码的，见 YanderePersona()。
    class PromptLibrary {
    public:
        static PromptLibrary& Instance();

        struct PromptFile {
            std::string fileName;   // 目录内的文件名（UTF-8），也是配置里保存的标识
            std::string title;      // 界面显示名（去掉扩展名）
        };

        // 提示词目录：与 logs/data/models 同级，位于程序的工作目录下。
        std::wstring FolderPathWide() const;
        std::string FolderPathUtf8() const;

        // 提示词目录的实际位置（工作目录优先，否则 exe 目录）
        std::filesystem::path ResolveFolder() const;

        // 确保目录存在（启动时调用一次）。已存在也算成功。
        bool EnsureFolder() const;

        // yandere.chr 是否存在。决定配置页是否给出「？？？」选项。
        bool HasYandereSentinel() const;

        // 列出可用的提示词文件（仅 .md/.txt，按文件名排序）。
        std::vector<PromptFile> ListPromptFiles() const;

        // 把启用清单（逗号分隔的文件名）拼成系统提示的追加段。
        // 清单里不存在或读不出内容的文件会被跳过；空清单返回空串。
        std::string BuildSystemPromptAppend(const std::string& enabledList) const;

        // 硬编码的病娇人格提示词（「？？？」模式使用）。
        static std::string YanderePersona();

    private:
        PromptLibrary() = default;
        ~PromptLibrary() = default;
        PromptLibrary(const PromptLibrary&) = delete;
        PromptLibrary& operator=(const PromptLibrary&) = delete;
    };

} // namespace UnnamedBot
