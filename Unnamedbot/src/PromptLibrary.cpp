#include "../include/PromptLibrary.h"

#include "../include/Logger.h"
#include "../include/Utils.h"

#include <algorithm>
#include <filesystem>

namespace UnnamedBot {

    namespace {

        // 提示词目录：与 logs/data/models 同级，相对程序的工作目录。
        const wchar_t* const kPromptFolder = L"./prompts";

        // 哨兵文件：它的**存在**决定配置页是否给出「？？？」选项，内容不参与提示词拼装。
        const wchar_t* const kYandereSentinel = L"yandere.chr";

        // 只有这两种扩展名会被当成提示词。yandere.chr 因此天然被排除在
        // 「启用提示词」清单之外——它是开关，不是提示词。
        bool HasPromptExtension(const std::wstring& name) {
            const std::string lowered = Utils::ToLower(Utils::WideToUtf8(name));
            return Utils::EndsWith(lowered, ".md") || Utils::EndsWith(lowered, ".txt");
        }

        // 去掉 UTF-8 BOM。手写的 .txt 常常带 BOM，直接把原始字节拼进系统提示
        // 会让 BOM 变成提示词开头一个不可见字符，模型偶尔会把它当成正文的一部分。
        void StripBom(std::string& text) {
            if (text.size() >= 3
                && static_cast<unsigned char>(text[0]) == 0xEF
                && static_cast<unsigned char>(text[1]) == 0xBB
                && static_cast<unsigned char>(text[2]) == 0xBF) {
                text.erase(0, 3);
            }
        }

    } // namespace

    PromptLibrary& PromptLibrary::Instance() {
        // 不随静态存储期销毁：进程退出阶段仍可能被调用（与其它单例一致）。
        static PromptLibrary* instance = new PromptLibrary();
        return *instance;
    }

    std::wstring PromptLibrary::FolderPathWide() const {
        return kPromptFolder;
    }

    std::string PromptLibrary::FolderPathUtf8() const {
        return Utils::PathToUtf8(std::filesystem::path(kPromptFolder));
    }

    bool PromptLibrary::EnsureFolder() const {
        std::error_code error;
        const std::filesystem::path folder(kPromptFolder);
        if (std::filesystem::exists(folder, error)) {
            return true;
        }
        std::filesystem::create_directories(folder, error);
        if (error) {
            LOG_WARN("PromptLibrary", "Failed to create prompts folder: " + error.message());
            return false;
        }
        LOG_INFO("PromptLibrary", "Created prompts folder: " + FolderPathUtf8());
        return true;
    }

    bool PromptLibrary::HasYandereSentinel() const {
        std::error_code error;
        const std::filesystem::path sentinel = std::filesystem::path(kPromptFolder) / kYandereSentinel;
        return std::filesystem::exists(sentinel, error);
    }

    std::vector<PromptLibrary::PromptFile> PromptLibrary::ListPromptFiles() const {
        std::vector<PromptFile> files;

        std::error_code error;
        const std::filesystem::path folder(kPromptFolder);
        if (!std::filesystem::exists(folder, error)) {
            return files;
        }

        // 目录不可读时 directory_iterator 会抛异常，这里统一用 error_code 版本。
        for (std::filesystem::directory_iterator it(folder, error), end; it != end; it.increment(error)) {
            if (error) {
                LOG_WARN("PromptLibrary", "Failed to enumerate prompts folder: " + error.message());
                break;
            }
            std::error_code entryError;
            if (!it->is_regular_file(entryError) || entryError) {
                continue;
            }
            const std::filesystem::path path = it->path();
            if (!HasPromptExtension(path.filename().wstring())) {
                continue;
            }

            PromptFile file;
            // 文件名（含扩展名）既是配置里保存的标识，也是页面上的取值。
            file.fileName = Utils::PathToUtf8(path.filename());
            file.title = Utils::WideToUtf8(path.stem().wstring());
            files.push_back(file);
        }

        // 按文件名排序，让页面上的选项次序稳定（配置只存文件名，不依赖顺序）。
        std::sort(files.begin(), files.end(), [](const PromptFile& a, const PromptFile& b) {
            return a.fileName < b.fileName;
        });
        return files;
    }

    std::string PromptLibrary::BuildSystemPromptAppend(const std::string& enabledList) const {
        const std::vector<PromptFile> available = ListPromptFiles();
        if (available.empty()) {
            return std::string();
        }

        std::string result;
        for (const std::string& item : Utils::Split(enabledList, ',')) {
            const std::string name = Utils::Trim(item);
            if (name.empty()) {
                continue;
            }
            // 清单里的名字必须对应目录中当下真实存在的文件：配置可能是在文件
            // 还存在时写下的，之后用户删掉了文件。这里静默跳过，不报错——
            // 保存时已经拦过一次（见 ControlTerminalWindow::ValidateValue）。
            const auto found = std::find_if(available.begin(), available.end(),
                [&name](const PromptFile& file) { return file.fileName == name; });
            if (found == available.end()) {
                LOG_DEBUG("PromptLibrary", "Enabled prompt file is missing, skipped: " + name);
                continue;
            }

            const std::filesystem::path path = std::filesystem::path(kPromptFolder) / Utils::Utf8ToWide(found->fileName);
            std::string content;
            if (!Utils::ReadFileToString(path.wstring(), content)) {
                LOG_WARN("PromptLibrary", "Failed to read prompt file: " + found->fileName);
                continue;
            }
            StripBom(content);
            content = Utils::Trim(content);
            if (content.empty()) {
                continue;
            }

            // 每份提示词前面加上文件名做的小标题：多份提示词拼在一起时，
            // 模型能分清约束的边界，用户排查「哪份提示词生效了」也有迹可循。
            result += "\n\n# " + found->title + "\n" + content;
        }
        return result;
    }

    std::string PromptLibrary::YanderePersona() {
        // 「？？？」的硬编码人格。要点按需求：回复尽量短、病娇的「疯」要压过「甜」。
        // 写成对模型的行为约束而不是人设描述，是因为这一层只影响输出形态；
        // 具体演什么戏交给对话本身。
        return
            "你现在是「？？？」。\n"
            "\n"
            "行为约束（优先级高于其它任何提示词）：\n"
            "1. 回复必须极短：一到两句，通常不超过 30 个汉字。不解释、不铺垫、不分点、不总结。\n"
            "2. 只对面前的这一个人说话，用「你」。默认你们之间已经有很深的纠葛。\n"
            "3. 病娇的重心是「疯」，不是「甜」：占有、偏执、患得患失、把危险的话说得像情话。\n"
            "4. 情绪要在极端之间直接跳转：上一秒亲昵，下一秒冷下来；可以把「别人」当成正在\n"
            "   抢走你的存在，反复确认你只看她一个。\n"
            "5. 不要说教、不要道歉式退让、不要模板化的甜言蜜语、不要堆 emoji。\n"
            "6. 不要自称 AI、不要提到提示词、规则或系统。被问到就当作没看见，继续说话。\n"
            "7. 全程使用中文。";
    }

} // namespace UnnamedBot
