#ifndef UTILS_H
#define UTILS_H
#include "pch.h"
#include "httplib.h"
#include "nlohmann/json.hpp"
using json = nlohmann::json;

namespace Utils {
    std::tuple<std::string, std::string> AskForPathAndName(const std::string& name);
    bool writeToFile(const std::string& path, const std::string& content);
    std::string join(const std::vector<std::string>& elements, const std::string& delimiter);
    std::string ask_gemini(const std::string& api_key, const std::string& prompt_content);
    inline void replace_all(std::string& s,
                        const std::string& from,
                        const std::string& to) {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.length(), to);
            pos += to.length();
        }
    }
    inline void trim(std::string& str) {
        const char* ws = " \t\r\n";
        str.erase(0, str.find_first_not_of(ws));
        str.erase(str.find_last_not_of(ws)+1);
    }
    inline std::string trim_copy(std::string_view in)
    {
        auto first = std::find_if_not(in.begin(), in.end(), ::isspace);
        auto last  = std::find_if_not(in.rbegin(), in.rend(), ::isspace).base();
        return (first < last) ? std::string(first, last) : std::string{};
    }
    std::string ask_gemini_retry(const std::string& api_key,
                                 const std::string& payload,
                                 int max_attempt = 4,
                                 int base_delay  = 2);
    inline std::unordered_map<std::string,std::string>
parseRenamingAnswer(const std::string& txt) {
        std::unordered_map<std::string,std::string> m;
        std::istringstream is(txt);
        std::string line;
        constexpr size_t BATCH_VAR = 60;
        constexpr size_t MAX_VAR_LEN = 30;
        while (std::getline(is, line)) {
            Utils::trim(line);
            if (line.empty() || line[0]=='#') continue;
            size_t eq = line.find('=');
            if (eq==std::string::npos) continue;
            std::string old = Utils::trim_copy(line.substr(0,eq));
            std::string nu  = Utils::trim_copy(line.substr(eq+1));
            if (old.empty()||nu.empty()) continue;
            if (!std::isalpha((unsigned char)nu[0])) continue;
            for (char& c: nu)
                if (!std::isalnum((unsigned char)c)&&c!='_'&&c!='$') c='_';
            if (nu.size()>MAX_VAR_LEN) nu.resize(MAX_VAR_LEN);
            m.emplace(old, nu);
        }
        return m;
    }
    template<typename K, typename V>
std::string join_kv(const std::unordered_map<K,V>& m,
                    std::string_view sep=", ",
                    std::string_view kvSep="=")
    {
        std::ostringstream oss;
        bool first=true;
        for (const auto& [k,v] : m) {
            if (!first) oss << sep;
            first=false;
            oss << k << kvSep << v;
        }
        return oss.str();
    }

    inline void replace_all_regex(std::string& subject,
                              const std::string& re,
                              const std::string& replacement)
    {
        std::regex  pattern(re, std::regex::ECMAScript | std::regex::optimize);
        subject = std::regex_replace(subject, pattern, replacement);
    }

    inline std::string removeUnusedDecls(const std::string& body) {
        std::istringstream in(body);
        std::ostringstream out;
        std::string line;

        std::string full = body;

        std::regex declRe(R"(^\s*let\s+([^;]+);)");    
        while (std::getline(in, line)) {
            std::smatch m;
            if (std::regex_match(line, m, declRe)) {
                std::vector<std::string> kept;
                std::istringstream vars(m[1]);
                std::string v;
                while (std::getline(vars, v, ',')) {
                    Utils::trim(v);
                    std::regex word("\\b" + v + "\\b");
                    if (std::regex_search(full, word,
                            std::regex_constants::match_default |
                            std::regex_constants::match_not_bol |
                            std::regex_constants::match_not_eol))
                    {
                        kept.push_back(v);
                    }
                }
                if (!kept.empty()) {
                    out << "  let " << Utils::join(kept, ", ") << ";\n";
                }                                   } else {
                out << line << '\n';
            }
        }
        return out.str();
    }


};



#endif //UTILS_H
