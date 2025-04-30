#include "crow.h"
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <openssl/pem.h>
#include <openssl/evp.h>
#include <openssl/bio.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <cstdlib>
#include <chrono>
#include <mutex>
#include <iterator>

using json  = nlohmann::json;
using Clock = std::chrono::system_clock;

// Globals for cached token & expiry
static std::string             cached_token;
static Clock::time_point       token_expiry;
static std::mutex              token_mutex;

// Trim whitespace
static std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    auto e = s.find_last_not_of (" \t\r\n");
    return (b == std::string::npos)
         ? ""
         : s.substr(b, e-b+1);
}

// Load .env into environment
static void loadDotenv(const std::string& filepath) {
    std::ifstream in(filepath);
    if (!in) return;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0]=='#') continue;
        auto eq = line.find('=');
        if (eq==std::string::npos) continue;
        std::string key   = trim(line.substr(0,eq));
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

// Load JSON from file
static json loadJSON(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Cannot open JSON: "+path);
    return json::parse(in);
}

// Base64‐URL encode (no padding)
static std::string base64UrlEncode(const std::string& in) {
    int len = (int)in.size();
    int out_len = 4*((len+2)/3);
    std::string b64(out_len, '\0');
    EVP_EncodeBlock((unsigned char*)&b64[0],
                    (const unsigned char*)in.data(), len);
    std::string url;
    for(char c: b64){
        if      (c=='+') url.push_back('-');
        else if (c=='/') url.push_back('_');
        else if (c=='=') break;
        else             url.push_back(c);
    }
    return url;
}

// RSA‐SHA256 sign using PEM private key
static std::string rsaSha256Sign(const std::string& data,
                                 const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), (int)pem.size());
    EVP_PKEY* pkey = PEM_read_bio_PrivateKey(bio,nullptr,nullptr,nullptr);
    if (!pkey) { BIO_free(bio); throw std::runtime_error("Invalid key"); }
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestSignInit(ctx,nullptr,EVP_sha256(),nullptr,pkey);
    EVP_DigestSignUpdate(ctx,data.data(),data.size());
    size_t slen=0;
    EVP_DigestSignFinal(ctx,nullptr,&slen);
    std::string sig(slen,'\0');
    EVP_DigestSignFinal(ctx,(unsigned char*)&sig[0],&slen);
    sig.resize(slen);
    EVP_PKEY_free(pkey);
    BIO_free(bio);
    EVP_MD_CTX_free(ctx);
    return base64UrlEncode(sig);
}

// Build JWT from service account JSON
static std::string makeJwt(const std::string& client_email,
                           const std::string& private_key) {
    using namespace std::chrono;
    auto now = duration_cast<seconds>(Clock::now().time_since_epoch()).count();
    auto exp = now + 3600;
    const std::string hdr = R"({"alg":"RS256","typ":"JWT"})";
    std::ostringstream pl;
    pl << '{'
       << R"("iss":")"   << client_email << R"(",)"
       << R"("scope":"https://www.googleapis.com/auth/cloud-platform",)"
       << R"("aud":"https://oauth2.googleapis.com/token",)"
       << R"("iat":)"    << now << ','
       << R"("exp":)"    << exp
       << '}';
    std::string part = base64UrlEncode(hdr) + "." + base64UrlEncode(pl.str());
    return part + "." + rsaSha256Sign(part, private_key);
}

// Exchange JWT for access_token
static std::string refreshTokenWithJwt(const std::string& jwt,
                                       int& expires_in) {
    auto r = cpr::Post(
      cpr::Url{"https://oauth2.googleapis.com/token"},
      cpr::Payload{
        {"grant_type","urn:ietf:params:oauth:grant-type:jwt-bearer"},
        {"assertion", jwt}
      }
    );
    if (r.error) throw std::runtime_error("Token POST failed: "+r.error.message);
    if (r.status_code!=200)
        throw std::runtime_error("HTTP "+std::to_string(r.status_code)+": "+r.text);
    auto j = json::parse(r.text);
    expires_in = j.at("expires_in").get<int>();
    return j.at("access_token").get<std::string>();
}

// Get & cache OAuth2 token (refresh 1m early)
static std::string getAccessToken(const json& adc) {
    std::lock_guard<std::mutex> lk(token_mutex);
    auto now = Clock::now();
    if (cached_token.empty() ||
        now + std::chrono::minutes(1) >= token_expiry)
    {
        std::string jwt = makeJwt(
            adc.at("client_email" ).get<std::string>(),
            adc.at("private_key"  ).get<std::string>()
        );
        int exp_s = 0;
        cached_token = refreshTokenWithJwt(jwt, exp_s);
        token_expiry  = now + std::chrono::seconds(exp_s);
    }
    return cached_token;
}

// Build prompt, call Vertex AI, parse JSON response
static json queryGemini(const json& in,
                        const json& adc,
                        const std::string& project,
                        const std::string& location)
{
    // pull inputs
    const std::string name          = in.value("name",""),
                      kind          = in.value("type",""),
                      handedness    = in.value("handedness",""),
                      subtype       = in.value("subtype",""),
                      rarity        = in.value("rarity",""),
                      clothingPiece = in.value("clothingPiece","");

    bool allowEnchantment = (rarity != "Common");

    // 1) Build prompt
    std::ostringstream prompt;
    prompt << "You are a Dungeons & Dragons 5E gear generator.\n"
           << "Produce ONLY a single JSON object (no extra text).\n";

    if (kind == "Weapon") {
        prompt << "I want a weapon with these parameters:\n"
               << "  Name: "     << name << "\n"
               << "  Category: " << handedness << "\n"
               << "  Type: "     << subtype << "\n"
               << "  Rarity: "   << rarity << "\n\n"
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

    // 2) Prepare payload
    json payload = {
      {"contents", json::array({
         {
           {"role","user"},
           {"parts", json::array({ {{"text", prompt.str()}} })}
         }
      })},
      {"generationConfig", {
         {"temperature",     0.3},
         {"maxOutputTokens",768},
         {"topP",            0.95}
      }}
    };

    // 3) Build URL
    std::string host = std::string("https://")
                     + location
                     + "-aiplatform.googleapis.com";
    std::string model = "gemini-2.0-flash-001";
    std::string url   = host
        + "/v1/projects/" + project
        + "/locations/"   + location
        + "/publishers/google/models/" + model
        + ":generateContent";

    // 4) Send POST
    auto resp = cpr::Post(
      cpr::Url{url},
      cpr::Header{
        {"Content-Type","application/json"},
        {"Authorization","Bearer "+getAccessToken(adc)}
      },
      cpr::Body{payload.dump()}
    );
    if (resp.error) {
        throw std::runtime_error("HTTP POST failed: " + resp.error.message);
    }
    if (resp.status_code<200 || resp.status_code>=300) {
        throw std::runtime_error(
          "Vertex AI HTTP " + std::to_string(resp.status_code)
          + ": " + resp.text
        );
    }

    // 5) Parse & clean
    auto full = json::parse(resp.text);
    std::string raw = full["candidates"][0]["content"]["parts"][0]["text"];
    auto start = raw.find('{');
    auto end   = raw.rfind('}');
    if (start==std::string::npos||end==std::string::npos||end<=start) {
        return full;
    }
    std::string jsonText = raw.substr(start, end-start+1);
    return json::parse(jsonText);
}

int main(int argc, char* argv[]) {
    loadDotenv(".env");

    const char* cred_env = std::getenv("GOOGLE_APPLICATION_CREDENTIALS");
    if (!cred_env) {
        std::cerr<<"Error: GOOGLE_APPLICATION_CREDENTIALS not set\n";
        return 1;
    }
    json adc;
    try { adc = loadJSON(cred_env); }
    catch(const std::exception& e) {
        std::cerr<<"Cred load failed: "<<e.what()<<"\n";
        return 1;
    }

    const char* proj = std::getenv("GOOGLE_PROJECT_ID");
    const char* loc  = std::getenv("GOOGLE_PROJECT_LOCATION");
    if (!proj||!loc) {
        std::cerr<<"Error: GOOGLE_PROJECT_ID or LOCATION missing\n";
        return 1;
    }
    std::string project  = proj;
    std::string location = loc;

    // CLI
    if (argc>1 && std::string(argv[1])=="--cli") {
        std::string inraw{
          std::istreambuf_iterator<char>(std::cin),
          std::istreambuf_iterator<char>()
        };
        try {
            json in  = json::parse(inraw);
            json out = queryGemini(in, adc, project, location);
            std::cout<<out.dump()<<"\n";
            return 0;
        } catch(const std::exception& e) {
            std::cerr<<"CLI error: "<<e.what()<<"\n";
            return 2;
        }
    }

    // HTTP server
    crow::SimpleApp app;
    CROW_ROUTE(app,"/api/gear").methods("POST"_method)
    ([&](const crow::request& req){
        try {
            json in  = json::parse(req.body);
            json out = queryGemini(in, adc, project, location);
            crow::response res(out.dump());
            res.set_header("Content-Type","application/json");
            return res;
        } catch(const std::exception& e) {
            json err = {{"error","ProcessingFailed"},{"message",e.what()}};
            crow::response res(500, err.dump());
            res.set_header("Content-Type","application/json");
            return res;
        }
    });

    app.port(5000).multithreaded().run();
    return 0;
}