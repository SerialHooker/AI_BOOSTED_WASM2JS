

#include "Utils.h"

#include <filesystem>

std::tuple<std::string, std::string> Utils::AskForPathAndName(const std::string& name) {
    std::cout << "enter absolute path of " + name + " : ";
    std::string path;
    std::cin >> path;
    std::cout << "enter filename of the output file : ";
    std::string filename;
    std::cin >> filename;

    return std::tuple(path, filename);
}

bool Utils::writeToFile(const std::string& filename, const std::string& content) {
    try {
        std::filesystem::path exePath = std::filesystem::current_path();
        std::filesystem::path outputDir = exePath / "output";

        std::filesystem::create_directories(outputDir);

        std::filesystem::path fullPath = outputDir / filename;

        std::ofstream file(fullPath, std::ios::binary);
        if (!file) {
            std::cerr << "Failed to open file: " << fullPath << std::endl;
            return false;
        }

        file.write(content.data(), content.size());
        if (!file.good()) {
            std::cerr << "Failed to write to file: " << fullPath << std::endl;
            return false;
        }

        return true;

    } catch (const std::exception& e) {
        std::cerr << "Exception while writing file: " << e.what() << std::endl;
        return false;
    }
}

std::string Utils::join(const std::vector<std::string> &elements, const std::string &delimiter) {
    if (elements.empty()) return "";

    std::ostringstream oss;
    auto it = elements.begin();
    oss << *it;

    for (++it; it != elements.end(); ++it) {
        oss << delimiter << *it;
    }

    return oss.str();
}

std::string Utils::ask_gemini(const std::string& api_key, const std::string& prompt_content) {
    httplib::SSLClient cli("generativelanguage.googleapis.com");
    cli.set_connection_timeout(10);         
    cli.set_read_timeout(30);               
    cli.enable_server_certificate_verification(true);

    json request_body;
    request_body["contents"][0]["parts"][0]["text"] = prompt_content;

    std::string body_str = request_body.dump();

    
    std::string path = "/v1beta/models/gemini-1.5-pro-latest:generateContent?key=" + api_key;

    std::cout << "    [AI] Sending request to Gemini API..." << std::endl;
    if (auto res = cli.Post(path.c_str(), body_str, "application/json")) {
        if (res->status == 200) {
            std::cout << "    [AI] Received successful response." << std::endl;
            try {
                json response_json = json::parse(res->body);
                if (response_json.contains("candidates") && !response_json["candidates"].empty()) {
                    const auto& first_candidate = response_json["candidates"][0];
                    if (first_candidate.contains("content") && first_candidate["content"].contains("parts") && !first_candidate["content"]["parts"].empty()) {
                        return first_candidate["content"]["parts"][0]["text"];
                    }
                }
                return "[AI_ERROR: Could not find text in valid response.]";

            } catch (json::parse_error& e) {
                std::cerr << "JSON Parse Error: " << e.what() << '\n';
                return "[AI_ERROR: Failed to parse JSON response.]";
            }
        } else {
            std::cerr << "HTTP Error: " << res->status << std::endl;
            std::cerr << "Response body: " << res->body << std::endl;
            return "[AI_ERROR: HTTP Status " + std::to_string(res->status) + "]";
        }
    } else {
        auto err = res.error();
        std::cerr << "HTTP Request Error: " << httplib::to_string(err) << std::endl;
        return "[AI_ERROR: Request failed. " + httplib::to_string(err) + "]";
    }
}
std::string Utils::ask_gemini_retry(const std::string& api_key,
                                    const std::string& payload,
                                    int max_attempt,
                                    int base_delay)
{
    for (int attempt = 0; attempt < max_attempt; ++attempt) {
        std::string result = Utils::ask_gemini(api_key, payload);

        if (!result.starts_with("[AI_ERROR"))             
            return result;

        int delay = base_delay * (1 << attempt);           
        std::cerr << "[AI] retrying in " << delay << " s\n";
        std::this_thread::sleep_for(std::chrono::seconds(delay));
    }
    throw std::runtime_error("Gemini unreachable after multiple retries");
}