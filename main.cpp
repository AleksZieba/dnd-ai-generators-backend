#include "crow.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iterator>
#include <string>
#include <cstdlib>
#include <chrono>
#include <mutex>

using json = nlohmann::json;

// Globals for cached token & expiry
static std::string cached_token;
static std::chrono::system_clock::time_point token_expiry;
static std::mutex token_mutex;

// Trim whitespace
static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of(" \t\r\n");
    return (b == std::string::npos) ? "" : s.substr(b, e - b + 1);
}

// Load KEY=VALUE from .env in cwd
void loadDotenv(const std::string& filepath) {
    std::ifstream in(filepath);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key   = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 &&
           ((value.front() == '"' && value.back() == '"') ||
            (value.front() == '\''&& value.back() == '\''))) {
            value = value.substr(1, value.size() - 2);
        }
#ifdef _WIN32
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 1);
#endif
    }
}

// Load ADC JSON
json loadADC(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open ADC file: " + path);
    return json::parse(in);
}

// Exchange refresh_token for new access_token
std::string refreshAccessToken(const json& adc) {
    auto resp = cpr::Post(
        cpr::Url{"https://oauth2.googleapis.com/token"},
        cpr::Payload{
            {"client_id",     adc.at("client_id").get<std::string>()},
            {"client_secret", adc.at("client_secret").get<std::string>()},
            {"refresh_token", adc.at("refresh_token").get<std::string>()},
            {"grant_type",    "refresh_token"}
        }
    );
    if (resp.error) {
        throw std::runtime_error("Token request failed: " + resp.error.message);
    }
    if (resp.status_code != 200) {
        throw std::runtime_error("Token endpoint returned HTTP " +
                                 std::to_string(resp.status_code));
    }
    auto j = json::parse(resp.text);
    cached_token = j.at("access_token").get<std::string>();
    int expires_in = j.at("expires_in").get<int>();
    token_expiry = std::chrono::system_clock::now() + std::chrono::seconds(expires_in);
    return cached_token;
}

// Return a valid access token, refreshing if expiring within 1 minute
std::string getAccessToken(const json& adc) {
    std::lock_guard<std::mutex> lock(token_mutex);
    auto now = std::chrono::system_clock::now();
    if (cached_token.empty() || now + std::chrono::minutes(1) >= token_expiry) {
        return refreshAccessToken(adc);
    }
    return cached_token;
}

// Build prompt, call Vertex AI generateContent, parse JSON response
json queryGemini(const json& in,
                 const json& adc,
                 const std::string& project,
                 const std::string& location) {
    // Pull user inputs
    const std::string name           = in.value("name", "");
    const std::string kind           = in.value("type", "");                // "Weapon" or "Armor"
    const std::string handedness     = in.value("handedness", "");         // for Weapon
    const std::string subtype        = in.value("subtype", "");            // weaponType or armorClass
    const std::string rarity         = in.value("rarity", "");
    const std::string clothingPiece  = in.value("clothingPiece", "");      // for Armor

    // Determine if we should include enchantment/curse
    const bool allowEnchantment = (rarity != "Common");

    // 1) Build the standardized JSON-output prompt
    std::ostringstream prompt;
    prompt << "You are a Dungeons & Dragons 5E gear generator.\n"
           << "Produce ONLY a single JSON object (no extra text).\n";

    if (kind == "Weapon") {
        prompt << "I want a weapon with these parameters:\n"
               << "  Name: "       << name << "\n"
               << "  Category: "   << handedness << "\n"
               << "  Type: "       << subtype << "\n"
               << "  Rarity: "     << rarity << "\n\n"
               << "Your JSON schema should be:\n" << R"({
  "Name": "...",
  "Category": "...",
  "Type": "...",
  "Rarity": "...",
  "Cost": "...",
  "DamageDice": "...",
  "DamageType": "...",
  "Weight": "...",
  "Properties": ["...", "..."],
  "Description": "..."
}
)";
        prompt << "\nPopulate only the fields after those prefilled above.\n";
        if (allowEnchantment) {
            prompt << "Description: include a short history, benefits, and an enchantment in 150 words or less, scale the enchantments appropriately according to rarity, only add curses to items of legendary rarity or greater.\n";
        } else {
            prompt << "Description: include a short history and benefits in 150 words or less (do NOT include any enchantment).\n";
        }
    } else {
        // Armor / clothing path
        prompt << "I want an armor/clothing item with these parameters:\n"
               << "  Name: "       << name << "\n"
               << "  Armor Class: "<< subtype << "\n"
               << "  Rarity: "     << rarity << "\n";
        if (!clothingPiece.empty()) {
            prompt << "  ClothingPiece: " << clothingPiece << "\n";
        }
        prompt << "\nYour JSON schema should be:\n" << R"({
  "Name": "...",
  "ItemType": "...",
  "Rarity": "...",
  "Category": "...",
  "Cost": "...",
  "ArmorClass": "...",
  "Attunement": "...",
  "Weight": "...",
  "Properties": ["...", "..."],
  "Charges": "...",
  "Description": "..."
}
)";
        prompt << "\nPopulate fields after those prefilled above.\n";
        if (allowEnchantment) {
            prompt << "Description: include a short history, benefits, and an enchantment in 150 words or less, scale the enchantments appropriately according to rarity, only add curses to items of legendary rarity or greater.\n";
        } else {
            prompt << "Description: include a short history and benefits in 150 words or less (do NOT include any enchantment or curse).\n";
        }
    }

    // 2) Prepare the generative request payload
    json payload = {
        {"contents", json::array({
            {
                {"role",  "user"},
                {"parts", json::array({ {{"text", prompt.str()}} })}
            }
        })},
        {"generationConfig", {
            {"temperature",      0.3},
            {"maxOutputTokens",  768},
            {"topP",             0.95}
        }}
    };

    // 3) Build the generateContent URL
    std::string host  = "https://" + location + "-aiplatform.googleapis.com";
    std::string model = "gemini-2.0-flash-001";
    std::string url   = host
        + "/v1/projects/" + project
        + "/locations/"   + location
        + "/publishers/google/models/" + model
        + ":generateContent";

    // 4) Fetch token and send request
    std::string bearer = "Bearer " + getAccessToken(adc);
    auto resp = cpr::Post(
        cpr::Url{url},
        cpr::Header{
            {"Content-Type","application/json"},
            {"Authorization",bearer}
        },
        cpr::Body{payload.dump()}
    );
    if (resp.error) {
        throw std::runtime_error("HTTP POST failed: " + resp.error.message);
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        throw std::runtime_error(
          "Vertex AI HTTP " + std::to_string(resp.status_code) + ": " + resp.text
        );
    }

    // 5) Parse and clean the AI response
    auto full = json::parse(resp.text);
    std::string raw = full["candidates"][0]["content"]["parts"][0]["text"];
    auto start = raw.find('{');
    auto end   = raw.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end <= start) {
        return full;
    }
    std::string jsonText = raw.substr(start, end - start + 1);
    return json::parse(jsonText);
}

int main(int argc, char* argv[]) {
    // Load .env for project & location
    loadDotenv(".env");

    // Determine ADC path
    std::string adcPath;
    if (auto env = std::getenv("GOOGLE_APPLICATION_CREDENTIALS")) {
        adcPath = env;
    } else if (auto home = std::getenv("HOME")) {
        adcPath = std::string(home)
                  + "/.config/gcloud/application_default_credentials.json";
    } else {
        std::cerr << "Error: set GOOGLE_APPLICATION_CREDENTIALS or HOME\n";
        return 1;
    }

    // Load ADC
    json adc;
    try {
        adc = loadADC(adcPath);
    } catch (const std::exception& e) {
        std::cerr << "ADC load error: " << e.what() << "\n";
        return 1;
    }

    // Read project & location
    const char* proj_env = std::getenv("GOOGLE_PROJECT_ID");
    const char* loc_env  = std::getenv("GOOGLE_PROJECT_LOCATION");
    if (!proj_env || !loc_env) {
        std::cerr << "Error: GOOGLE_PROJECT_ID and GOOGLE_PROJECT_LOCATION must be set\n";
        return 1;
    }
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
            json out = queryGemini(in, adc, project, location);
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
            json out = queryGemini(in, adc, project, location);
            crow::response res(out.dump());
            res.set_header("Content-Type", "application/json");
            return res;
        } catch (const std::exception& e) {
            json err = {{"error","ProcessingFailed"},{"message",e.what()}};
            crow::response res(500,err.dump());
            res.set_header("Content-Type","application/json");
            return res;
        }
    });

    app.port(5000).multithreaded().run();
    return 0;
}