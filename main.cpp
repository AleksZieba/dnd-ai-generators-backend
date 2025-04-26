#include "crow_all.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <cstdlib>

using json = nlohmann::json;

int main() {
    crow::SimpleApp app;

    CROW_ROUTE(app, "/api/gear").methods("POST"_method)
    ([&](const crow::request& req){
        // Parse incoming JSON
        auto j = json::parse(req.body);
        std::string name = j.value("name", "");
        std::string type = j.value("type", "");
        std::string subtype = j.value("subtype", "");
        std::string rarity = j.value("rarity", "");
        std::string clothingPiece = j.value("clothingPiece", "");
        std::string handedness = j.value("handedness", "");

        // Build a prompt for Google Gemini
        std::string prompt = "Generate a detailed description for a DnD " + type;
        prompt += (type == "Weapon") ? (" (" + handedness + ", " + subtype + ")")
                                      : (" (" + subtype + ")");
        if (!clothingPiece.empty()) {
            prompt += " piece: " + clothingPiece;
        }
        prompt += ", rarity: " + rarity + ".";

        // Retrieve API key from environment
        const char* apiKey = std::getenv("GOOGLE_API_KEY");
        if (!apiKey) {
            crow::response res;
            res.code = 500;
            res.set_header("Content-Type", "application/json");
            res.body = R"({"error":"Missing GOOGLE_API_KEY"})";
            return res;
        }

        // Prepare Gemini request payload
        json gemini_req = {
            {"prompt", {{"text", prompt}}}
        };

        // Call Google Gemini (Vertex AI) Text-Bison model
        auto gemini_resp = cpr::Post(
            cpr::Url{"https://generativelanguage.googleapis.com/v1beta2/models/text-bison-001:generateText?key=" + std::string(apiKey)},
            cpr::Header{{"Content-Type", "application/json"}},
            cpr::Body{gemini_req.dump()}
        );

        if (gemini_resp.status_code != 200) {
            crow::response res;
            res.code = gemini_resp.status_code;
            res.set_header("Content-Type", "application/json");
            res.body = gemini_resp.text;
            return res;
        }

        // Parse Gemini response
        auto gemini_json = json::parse(gemini_resp.text);
        std::string description = gemini_json["candidates"][0]["output"].get<std::string>();

        // Return JSON to the frontend
        json response = {
            {"description", description}
        };
        crow::response res;
        res.set_header("Content-Type", "application/json");
        res.body = response.dump();
        return res;
    });

    app.port(5000).multithreaded().run();
    return 0;
}
