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

// Globals for token caching
static std::string cached_token;
static std::chrono::system_clock::time_point token_expiry;
static std::mutex token_mutex;

// trim whitespace
static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of (" \t\r\n");
    return b==std::string::npos ? "" : s.substr(b, e-b+1);
}

// Load key=value lines from .env in cwd
void loadDotenv(const std::string& filepath) {
    std::ifstream in(filepath);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0]=='#') continue;
        auto eq = line.find('=');
        if (eq==std::string::npos) continue;
        std::string key   = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq+1));
        if (value.size()>=2 &&
           ((value.front()=='"' && value.back()=='"') ||
            (value.front()=='\''&& value.back()=='\'')))
        {
            value = value.substr(1, value.size()-2);
        }
#ifdef _WIN32
        _putenv_s(key.c_str(), value.c_str());
#else
        setenv(key.c_str(), value.c_str(), 1);
#endif
    }
}

// Load the ADC JSON from the given path
json loadADC(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open ADC file: " + path);
    return json::parse(in);
}

// Exchange the refresh_token for a fresh access_token
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

// Return a valid access token, refreshing if within 1 minute of expiry
std::string getAccessToken(const json& adc) {
    std::lock_guard<std::mutex> lock(token_mutex);
    auto now = std::chrono::system_clock::now();
    if (cached_token.empty() || now + std::chrono::minutes(1) >= token_expiry) {
        return refreshAccessToken(adc);
    }
    return cached_token;
}

json queryGemini(const json& in,
                 const json& adc,
                 const std::string& project,
                 const std::string& location) {
    // build prompt...
    std::ostringstream prompt;
    prompt << "You are a creative assistant for D&D 5E.\n"
           << "Name: " << in.value("name","") << "\n"
           << "Type: " << in.value("type","") << "\n";
    if (in.value("type","")== "Weapon") {
        prompt << "Category: " << in.value("weaponCategory","") << "\n"
               << "Weapon Type: " << in.value("weaponType","") << "\n";
    } else if (in.value("type","")== "Armor") {
        prompt << "Armor Class: " << in.value("subtype","") << "\n";
        if (in.value("subtype","")!="Shield")
            prompt << "Piece: " << in.value("clothingPiece","") << "\n";
    }
    prompt << "Rarity: " << in.value("rarity","") << "\n\n"
           << "Include a short history, benefits, and an enchantment or curse.\n";

    json payload = {
        {"contents", json::array({ {{"parts", json::array({ {{"text", prompt.str()}} })}} })},
        {"generationConfig", {
            {"temperature",0.0},
            {"maxOutputTokens",256},
            {"topP",0.95}
        }}
    };

    std::string host  = "https://" + location + "-aiplatform.googleapis.com";
    std::string model = "gemini-2.0-flash-001";
    std::string url   = host
        + "/v1/projects/"  + project
        + "/locations/"   + location
        + "/publishers/google/models/" + model
        + ":generateContent";

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
        throw std::runtime_error("HTTP request failed: " + resp.error.message);
    }
    if (resp.status_code<200 || resp.status_code>=300) {
        throw std::runtime_error("Vertex AI HTTP " +
            std::to_string(resp.status_code)+": "+resp.text);
    }
    return json::parse(resp.text);
}

int main(int argc, char* argv[]) {
    // Load .env for project & location
    loadDotenv(".env");

    // ADC path
    std::string adcPath;
    if (auto env = std::getenv("GOOGLE_APPLICATION_CREDENTIALS"))
        adcPath = env;
    else if (auto home = std::getenv("HOME"))
        adcPath = std::string(home)+"/.config/gcloud/application_default_credentials.json";
    else {
        std::cerr<<"Error: set GOOGLE_APPLICATION_CREDENTIALS or HOME\n";
        return 1;
    }

    json adc;
    try { adc = loadADC(adcPath); }
    catch(const std::exception& e){
        std::cerr<<"ADC load error: "<<e.what()<<"\n";
        return 1;
    }

    const char* proj_env = std::getenv("GOOGLE_PROJECT_ID");
    const char* loc_env  = std::getenv("GOOGLE_PROJECT_LOCATION");
    if (!proj_env || !loc_env) {
        std::cerr<<"Error: GOOGLE_PROJECT_ID and GOOGLE_PROJECT_LOCATION must be set\n";
        return 1;
    }
    std::string project  = proj_env;
    std::string location = loc_env;

    // CLI mode
    if (argc>1 && std::string(argv[1])=="--cli") {
        std::string input{
            std::istreambuf_iterator<char>(std::cin),
            std::istreambuf_iterator<char>()
        };
        try {
            json in = json::parse(input);
            json out = queryGemini(in, adc, project, location);
            std::cout<<out.dump()<<"\n";
            return 0;
        } catch(const std::exception& e){
            std::cerr<<"Error in CLI: "<<e.what()<<"\n";
            return 2;
        }
    }

    // HTTP-server mode
    crow::SimpleApp app;
    CROW_ROUTE(app,"/api/gear").methods("POST"_method)
    ([&](const crow::request& req){
        try {
            json in  = json::parse(req.body);
            json out = queryGemini(in, adc, project, location);
            crow::response res(out.dump());
            res.set_header("Content-Type","application/json");
            return res;
        } catch(const std::exception& e){
            json err={{"error","ProcessingFailed"},{"message",e.what()}};
            crow::response res(500,err.dump());
            res.set_header("Content-Type","application/json");
            return res;
        }
    });

    app.port(5000).multithreaded().run();
    return 0;
}