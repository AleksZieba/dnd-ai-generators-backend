#include "crow.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <cstdlib>       // for std::getenv / setenv
#include <fstream>       // for std::ifstream
#include <iostream>
#include <sstream>
#include <iterator>
#include <string>

#ifdef _WIN32
  #define SET_ENV(name, value) _putenv_s((name), (value))
#else
  #define SET_ENV(name, value) setenv((name), (value), 1)
#endif

// trim whitespace from both ends
static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of (" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e-b+1);
}

// Load .env file (from cwd) into process environment
void loadDotenv(const std::string& filepath) {
    std::ifstream in(filepath);
    if (!in) return;  // skip if not found

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key   = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));

        // strip surrounding quotes
        if (value.size() >= 2 &&
           ((value.front() == '"' && value.back() == '"') ||
            (value.front() == '\''&& value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }

        if (!key.empty()) {
            SET_ENV(key.c_str(), value.c_str());
        }
    }
}

using json = nlohmann::json;

// Build prompt, call Gemini, return full JSON response
json queryGemini(const json& in,
                 const std::string& api_key,
                 const std::string& project,
                 const std::string& location) {
    std::ostringstream prompt;
    prompt << "You are a creative assistant for Dungeons & Dragons 5th Edition.\n"
           << "Generate a vivid, lore-rich description of a magic item with these details:\n"
           << "• Name: " << in.value("name", "") << "\n"
           << "• Type: " << in.value("type", "") << "\n";

    if (in.value("type", "") == "Weapon") {
        prompt << "• Category: " << in.value("weaponCategory", "") << "\n"
               << "• Weapon Type: " << in.value("weaponType", "") << "\n";
    } else if (in.value("type", "") == "Armor") {
        prompt << "• Armor Class: " << in.value("subtype", "") << "\n";
        if (in.value("subtype", "") != "Shield") {
            prompt << "• Piece: " << in.value("clothingPiece", "") << "\n";
        }
    }

    prompt << "• Rarity: " << in.value("rarity", "") << "\n\n"
           << "Include in your response:\n"
           << "  – A short piece of in-world history or legend\n"
           << "  – Its mechanical benefits\n"
           << "  – A suggestion for one possible enchantment or curse\n";

    json payload = {
        {"instances", {{{"content", prompt.str()}}}},
        {"parameters", {
            {"temperature",      0.0},
            {"maxOutputTokens", 256},
            {"topP",           0.95}
        }}
    };

    // Note: still using API key in URL for now
    std::string host = "https://" + location + "-aiplatform.googleapis.com";
    std::string url  = host
        + "/v1/projects/"  + project
        + "/locations/"   + location
        + "/publishers/google/models/gemini-2.0-flash-001:predict"
        + "?key=" + api_key;

    auto resp = cpr::Post(
        cpr::Url{url},
        cpr::Header{{"Content-Type", "application/json"}},
        cpr::Body{payload.dump()}
    );
    if (resp.error) {
        throw std::runtime_error("HTTP request failed: " + resp.error.message);
    }
    return json::parse(resp.text);
}

int main(int argc, char* argv[]) {
    // Load .env from cwd (wherever you run the binary)
    loadDotenv(".env");

    // Read environment
    const char* api_env  = std::getenv("GOOGLE_API_KEY");
    const char* proj_env = std::getenv("GOOGLE_PROJECT_ID");
    const char* loc_env  = std::getenv("GOOGLE_PROJECT_LOCATION");
    if (!api_env || !proj_env || !loc_env) {
        std::cerr << "Error: GOOGLE_API_KEY, GOOGLE_PROJECT_ID and "
                  << "GOOGLE_PROJECT_LOCATION must be set\n";
        return 1;
    }
    std::string api_key  = api_env;
    std::string project  = proj_env;
    std::string location = loc_env;

    // CLI mode
    if (argc > 1 && std::string(argv[1]) == "--cli") {
        std::string input{
            std::istreambuf_iterator<char>(std::cin),
            std::istreambuf_iterator<char>()
        };
        try {
            json in  = json::parse(input);
            json out = queryGemini(in, api_key, project, location);
            std::cout << out.dump() << "\n";
            return 0;
        } catch (const std::exception& e) {
            std::cerr << "Error in CLI mode: " << e.what() << "\n";
            return 2;
        }
    }

    // HTTP-server mode
    crow::SimpleApp app;

    CROW_ROUTE(app, "/api/gear").methods("POST"_method)
    ([&](const crow::request& req){
        try {
            json in  = json::parse(req.body);
            json out = queryGemini(in, api_key, project, location);
            crow::response res(out.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            json err = {
              {"error",   "ProcessingFailed"},
              {"message", e.what()}
            };
            crow::response res(500, err.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        }
    });

    app.port(5000).multithreaded().run();
    return 0;
}