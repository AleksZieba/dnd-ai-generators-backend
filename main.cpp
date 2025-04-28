#include "crow.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <cstdlib>       // std::getenv / setenv
#include <fstream>       // std::ifstream
#include <iostream>
#include <sstream>
#include <iterator>
#include <string>
#include <iomanip>       // for std::hex, std::setw

#ifdef _WIN32
  #define SET_ENV(name, value) _putenv_s((name), (value))
#else
  #define SET_ENV(name, value) setenv((name), (value), 1)
#endif

static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of (" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

void loadDotenv(const std::string& filepath) {
    std::ifstream in(filepath);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0]=='#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key   = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq+1));
        if (value.size()>=2 &&
           ((value.front()=='"' && value.back()=='"') ||
            (value.front()=='\''&& value.back()=='\'')))
        {
            value = value.substr(1, value.size()-2);
        }
        if (!key.empty()) {
            SET_ENV(key.c_str(), value.c_str());
        }
    }
}

using json = nlohmann::json;

// Calls Gemini.generateText, checks HTTP status, then parses JSON
json queryGemini(const json& in,
                 const std::string& access_token,
                 const std::string& project,
                 const std::string& location) {
    // Build the prompt
    std::ostringstream prompt;
    prompt << "You are a creative assistant for Dungeons & Dragons 5th Edition.\n"
           << "Generate a vivid, lore-rich description of a magic item with these details:\n"
           << "• Name: " << in.value("name", "") << "\n"
           << "• Type: " << in.value("type", "") << "\n";
    if (in.value("type","") == "Weapon") {
        prompt << "• Category: "   << in.value("weaponCategory","") << "\n"
               << "• Weapon Type: "<< in.value("weaponType","")     << "\n";
    } else if (in.value("type","") == "Armor") {
        prompt << "• Armor Class: "<< in.value("subtype","")       << "\n";
        if (in.value("subtype","") != "Shield") {
            prompt << "• Piece: " << in.value("clothingPiece","")  << "\n";
        }
    }
    prompt << "• Rarity: " << in.value("rarity","") << "\n\n"
           << "Include in your response:\n"
           << "  – A short piece of in-world history or legend\n"
           << "  – Its mechanical benefits\n"
           << "  – A suggestion for one possible enchantment or curse\n";

    // Prepare the generative request
    json payload = {
      {"contents", json::array({ {{"parts", json::array({ {{"text", prompt.str()}} })}} })},
      {"generationConfig", {
         {"temperature",     0.0},
         {"maxOutputTokens",256},
         {"topP",            0.95}
      }}
    };

    // Build the generateText URL
    std::string host = "https://" + location + "-aiplatform.googleapis.com";
    std::string url  = host
        + "/v1/projects/"  + project
        + "/locations/"   + location
        + "/publishers/google/models/gemini-2.0-flash-001:generateContent";

    // Send the request
    std::string bearer = "Bearer " + access_token;
    auto resp = cpr::Post(
        cpr::Url{url},
        cpr::Header{
          {"Content-Type","application/json"},
          {"Authorization",bearer}
        },
        cpr::Body{payload.dump()}
    );

    // 1) Transport-level errors
    if (resp.error) {
        throw std::runtime_error("HTTP request failed: " + resp.error.message);
    }

    // 2) Non-2xx status codes
    if (resp.status_code < 200 || resp.status_code >= 300) {
        // Dump a bit of the body for inspection
        std::string snippet = resp.text.substr(0, 64);
        std::ostringstream dbg;
        dbg << "HTTP " << resp.status_code
            << " response body (first 64 chars): ["
            << snippet << "]";
        throw std::runtime_error(dbg.str());
    }

    // 3) Now it’s safe to parse JSON
    return json::parse(resp.text);
}

int main(int argc, char* argv[]) {
    // Load .env (from cwd)
    loadDotenv(".env");

    // Read environment
    const char* token_env = std::getenv("ACCESS_TOKEN");
    const char* proj_env  = std::getenv("GOOGLE_PROJECT_ID");
    const char* loc_env   = std::getenv("GOOGLE_PROJECT_LOCATION");
    if (!token_env || !proj_env || !loc_env) {
        std::cerr<<"Error: ACCESS_TOKEN, GOOGLE_PROJECT_ID and GOOGLE_PROJECT_LOCATION must be set\n";
        return 1;
    }
    std::string access_token = token_env;
    std::string project      = proj_env;
    std::string location     = loc_env;

    // CLI mode
    if (argc>1 && std::string(argv[1])=="--cli") {
        // 1) Read all stdin
        std::string input{
            std::istreambuf_iterator<char>(std::cin),
            std::istreambuf_iterator<char>()
        };
        // 2) Parse user JSON input
        json in = json::parse(input);  // we already validated this
        try {
            // 3) Query Gemini
            json out = queryGemini(in, access_token, project, location);
            std::cout<< out.dump() << "\n";
            return 0;
        } catch (const std::exception &e) {
            std::cerr<<"Error in CLI mode: "<<e.what()<<"\n";
            return 2;
        }
    }

    // HTTP server mode
    crow::SimpleApp app;
    CROW_ROUTE(app, "/api/gear").methods("POST"_method)
    ([&](const crow::request& req){
        try {
            json in  = json::parse(req.body);
            json out = queryGemini(in, access_token, project, location);
            crow::response res(out.dump());
            res.set_header("Content-Type","application/json");
            return res;
        } catch (const std::exception &e) {
            json err = {{"error","ProcessingFailed"},{"message",e.what()}};
            crow::response res(500, err.dump());
            res.set_header("Content-Type","application/json");
            return res;
        }
    });

    app.port(5000).multithreaded().run();
    return 0;
}