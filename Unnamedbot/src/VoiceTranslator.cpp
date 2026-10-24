#include "../include/VoiceTranslator.h"
#include "../include/AIClient.h"
#include "../include/Logger.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>

namespace UnnamedBot {

    namespace {

        bool IsCjkLead(unsigned char c) {
            // UTF-8 三字节：0xE0-0xEF 覆盖中日韩、全角标点
            return c >= 0xE0 && c <= 0xEF;
        }

        std::string Trim(const std::string& s) {
            size_t b = 0, e = s.size();
            while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
            while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
            return s.substr(b, e - b);
        }

        std::string StripDecorations(std::string s) {
            s = Trim(s);
            if (s.size() > 1 && s.front() == '"' && s.back() == '"') {
                s = s.substr(1, s.size() - 2);
            }
            const char* prefixes[] = { "Translation:", "translation:", "English:" };
            for (const char* p : prefixes) {
                const size_t n = std::strlen(p);
                if (s.size() > n && s.compare(0, n, p) == 0) {
                    s = Trim(s.substr(n));
                    break;
                }
            }
            return s;
        }

        // MyMemory 偶尔返回 &quot; &#39; 这类实体，念出来会变成乱码
        std::string UnescapeHtml(std::string s) {
            struct Pair { const char* from; const char* to; };
            const Pair pairs[] = {
                { "&quot;", "\"" }, { "&#39;", "'" }, { "&apos;", "'" },
                { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" }, { "&nbsp;", " " },
            };
            for (const auto& p : pairs) {
                size_t pos = 0;
                while ((pos = s.find(p.from, pos)) != std::string::npos) {
                    s.replace(pos, std::strlen(p.from), p.to);
                    pos += std::strlen(p.to);
                }
            }
            return s;
        }

        // 无 key 时的兜底翻译：MyMemory 公共接口（匿名、有每日额度）
        bool TranslateMyMemory(const std::string& text, std::string& out, std::string& error) {
            const cpr::Response resp = cpr::Get(
                cpr::Url{ "https://api.mymemory.translated.net/get" },
                cpr::Parameters{ { "q", text }, { "langpair", "zh-CN|en" } },
                cpr::Timeout{ 6000 });
            if (resp.status_code != 200) {
                error = "mymemory http " + std::to_string(resp.status_code);
                return false;
            }
            nlohmann::json json;
            try {
                json = nlohmann::json::parse(resp.text);
            } catch (const std::exception& e) {
                error = std::string("mymemory bad json: ") + e.what();
                return false;
            }
            if (json.value("quotaFinished", false)) {
                error = "mymemory quota finished";
                return false;
            }
            std::string translated;
            if (json.contains("responseData") && json["responseData"].contains("translatedText")) {
                translated = json["responseData"]["translatedText"].get<std::string>();
            }
            translated = UnescapeHtml(StripDecorations(translated));
            if (translated.empty()) {
                error = "mymemory empty result";
                return false;
            }
            out = translated;
            return true;
        }

        // 短句缓存：同一句话重复出现（提示语、口头禅）直接命中，省掉往返
        std::mutex g_cacheMutex;
        std::map<std::string, std::string> g_cache;
        std::deque<std::string> g_cacheOrder;
        constexpr size_t kCacheLimit = 256;

        bool CacheGet(const std::string& key, std::string& out) {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            const auto it = g_cache.find(key);
            if (it == g_cache.end()) return false;
            out = it->second;
            return true;
        }

        void CachePut(const std::string& key, const std::string& value) {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (g_cache.count(key)) return;
            g_cache[key] = value;
            g_cacheOrder.push_back(key);
            while (g_cacheOrder.size() > kCacheLimit) {
                g_cache.erase(g_cacheOrder.front());
                g_cacheOrder.pop_front();
            }
        }

    } // namespace

    VoiceTranslator& VoiceTranslator::Instance() {
        static VoiceTranslator instance;
        return instance;
    }

    void VoiceTranslator::SetClient(AIClient* client) {
        std::lock_guard<std::mutex> lock(mutex_);
        client_ = client;
    }

    bool VoiceTranslator::NeedsTranslation(const std::string& text) {
        for (size_t i = 0; i < text.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (IsCjkLead(c)) return true;
        }
        return false;
    }

    bool VoiceTranslator::Translate(const std::string& text, std::string& out, std::string& error) {
        const std::string source = Trim(text);
        if (source.empty()) {
            error = "empty text";
            return false;
        }
        if (!NeedsTranslation(source)) {
            out = source;
            return true;
        }
        if (CacheGet(source, out)) {
            return true;
        }

        // 1) 优先用用户自己的对话模型：术语一致、译文更自然
        AIClient* client = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            client = client_;
        }
        if (client) {
            std::vector<Message> messages;
            Message system;
            system.role = MessageRole::System;
            system.content =
                "You are a translation engine for text-to-speech. "
                "Translate the user's text into natural spoken English. "
                "Rules: output ONLY the translation, no quotes, no notes, no markdown, no explanation. "
                "Keep the original meaning and roughly the same length. "
                "If the text is already English, output it unchanged.";
            Message user;
            user.role = MessageRole::User;
            user.content = source;
            messages.push_back(system);
            messages.push_back(user);

            AIResponse response = client->SendChatMessage(messages);
            if (!client->LastRequestFailed()) {
                const std::string translated = StripDecorations(response.content);
                if (!translated.empty()) {
                    out = translated;
                    CachePut(source, out);
                    return true;
                }
            } else {
                error = client->LastRequestError();
            }
        } else {
            error = "ai client not ready";
        }

        // 2) 兜底：不需要 key 的公共翻译
        std::string fallbackError;
        if (TranslateMyMemory(source, out, fallbackError)) {
            LOG_INFO("Voice", "Translated by public fallback (no API key).");
            CachePut(source, out);
            return true;
        }
        error = "ai: " + error + "; fallback: " + fallbackError;
        return false;
    }

} // namespace UnnamedBot
