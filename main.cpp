#include "crow_all.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <cstdlib>
#include <iostream>
#include <sstream>

using json = nlohmann::json;

int main() {
    crow::SimpleApp app;

    // Load configuration from environment
    const char* api_key_env     = std::getenv("GOOGLE_API_KEY");
    const char* project_env     = std::getenv("GOOGLE_PROJECT_ID");
    const char* location_env    = std::getenv("GOOGLE_PROJECT_LOCATION");
    if (!api_key_env || !project_env || !location_env) {
        std::cerr << "Error: GOOGLE_API_KEY, GOOGLE_PROJECT_ID or "
                     "GOOGLE_PROJECT_LOCATION not set\n";
        return 1;
    }
    std::string api_key  = api_key_env;
    std::string project  = project_env;
    std::string location = location_env;

    CROW_ROUTE(app, "/api/gear").methods("POST"_method)
    ([&](const crow::request& req){
        // 1) Parse incoming JSON
        auto in = json::parse(req.body);
        std::string name           = in.value("name", "");
        std::string type           = in.value("type", "");
        std::string rarity         = in.value("rarity", "");
        std::string weaponCategory = in.value("weaponCategory", "");
        std::string weaponType     = in.value("weaponType", "");
        std::string armorClass     = in.value("subtype", "");
        std::string clothingPiece  = in.value("clothingPiece", "");

        // 2) Build the text prompt
        std::ostringstream prompt;
        prompt << "You are a creative assistant for Dungeons & Dragons 5th Edition.\n"
               << "Generate a vivid, lore-rich description of a magic item with these details:\n"
               << "• Name: " << name << "\n"
               << "• Type: " << type << "\n";
        if (type == "Weapon") {
            prompt << "• Category: " << weaponCategory << "\n"
                   << "• Weapon Type: " << weaponType << "\n";
        } else if (type == "Armor") {
            prompt << "• Armor Class: " << armorClass << "\n";
            if (armorClass != "Shield") {
                prompt << "• Piece: " << clothingPiece << "\n";
            }
        }
        prompt << "• Rarity: " << rarity << "\n\n"
               << "Include in your response:\n"
               << "  – A short piece of in-world history or legend\n"
               << "  – Its mechanical benefits\n"
               << "  – A suggestion for one possible enchantment or curse\n";

        // 3) Prepare the Vertex AI request body
        json payload = {
            {"instances", {{{"content", prompt.str()}}}},
            {"parameters", {
                {"temperature",      0.0},
                {"maxOutputTokens", 256},
                {"topP",           0.95}
            }}
        };

        // 4) Use Gemini 2.0 Flash model
        std::string url =
            "https://" + location + "-aiplatform.googleapis.com"
            + "/v1/projects/" + project
            + "/locations/" + location
            + "/publishers/google/models/gemini-2.0-flash-001:predict"
            + "?key=" + api_key;

        // 5) Send POST to Vertex AI
        auto resp = cpr::Post(
            cpr::Url{url},
            cpr::Header{{"Content-Type", "application/json"}},
            cpr::Body{payload.dump()}
        );

        // 6) Handle HTTP errors
        if (resp.error) {
            json err = {
                {"error",   "RequestFailed"},
                {"message", resp.error.message}
            };
            crow::response c(500, err.dump());
            c.set_header("Content-Type", "application/json");
            return c;
        }

        // 7) Forward Gemini’s JSON verbatim (RFC 8259 compliant)
        json fullResponse = json::parse(resp.text);
        crow::response c(fullResponse.dump());
        c.set_header("Content-Type", "application/json");
        return c;
    });

    app.port(5000).multithreaded().run();
}