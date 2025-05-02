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
#include <random>
#include <vector>
#include <cmath>

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
					  clothingPiece = in.value("clothingPiece",""),
					  extraDesc     = in.value("description", "");

	bool allowEnchantment = (rarity != "Common");

	// 1) Build prompt
	std::ostringstream prompt;
	prompt << "You are a Dungeons & Dragons 5E gear generator.\n"
		   << "Produce ONLY a single JSON object (no extra text).\n";

	if (kind == "Weapon") {
		prompt << "I want a weapon\n";
		if (!name.empty()) {
			prompt << " called \"" << name << "\"";
		}
		prompt << " with these parameters:\n"
			   << "  Category: " << handedness << "\n"
			   << "  Type: "     << subtype << "\n"
			   << "  Rarity: "   << rarity << "\n\n";
			   if (!extraDesc.empty()) {
				   prompt << "\nAdditional Details: " << extraDesc << "\n";
			   }
			   prompt << "Your JSON schema should be:\n" << R"({
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
				})";
		prompt << "\nPopulate only the fields after those prefilled above.\n";
		if (allowEnchantment) {
			prompt << "Description: include a short history, benefits, and an enchantment in 150 words or less, "\
					  "scale the enchantments appropriately according to rarity, only add curses to items of legendary rarity or greater, "\
					  "most importantly: be original and imaginative. Do not rely on the term \"dying star\". "\
				      "You are encouraged to use 1/2 lb. measurements on light items (e.g. 1/2 lb. or 1 1/2 lb.).\n";
		} else {
			prompt << "Description: include a short history and benefits in 150 words or less (do NOT include any enchantment). "\
					  "Most importantly: be original and imaginative. Do not rely on the term \"dying star\". "\
					  "You are encouraged to use 1/2 lb. measurements on light items (e.g. 1/2 lb. or 1 1/2 lb.).\n";
		}

	} else if (kind == "Armor") {
		prompt << "I want an armor/clothing item\n";
		if (!name.empty()) {
			prompt << " called \"" << name << "\"";
		}
		prompt << " with these parameters:\n"
			   << "  Category: "             << subtype << "\n"
			   << "  Piece: "                << clothingPiece << "\n"
			   << "  Armor Class: "          << (subtype == "Clothes" ? "N/A" : subtype) << "\n"
			   << "  Attunement: "           << (subtype == "Clothes" ? "No"  : "Yes") << "\n"
			   << "  Stealth Disadvantage: " << ((subtype == "Heavy" || subtype == "Shield") ? "Yes" : "No")
			   << "\n\n";

		prompt << "Your JSON schema should be:\n" << R"({
			"Name": "...",
			"Piece": "...",                  // headgear / clothes / etc.
			"Category": "...",               // clothes/light/medium/heavy
			"ArmorClass": "...",             // N/A or number
			"Attunement": "...",             // Yes/No
			"StealthDisadvantage": "...",    // Yes/No
			"Weight": "...",                 // e.g. "1 lb." or "1 1/2 lbs."
			"Cost": "...",                   // e.g. "15 gp"
			"Properties": ["...", "..."],
			"Description": "..."             // lore + benefits
		})";

		if (!clothingPiece.empty()) {
			prompt << "  ClothingPiece: " << clothingPiece << "\n";
		}
		if (!extraDesc.empty()) {
			prompt << "\nAdditional Details: " << extraDesc << "\n";
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
			"Description": "..."
		})";
		prompt << "\nPopulate fields after those prefilled above.\n";
		if (allowEnchantment) {
			prompt << "Description: include a short history, benefits, and an enchantment in 150 words or less, "\
					  "scale the enchantments appropriately according to rarity, only add curses to items of legendary rarity or greater, "\
					  "most importantly: be original and imaginative. Do not rely on the term \"dying star\". "\
					  "You are encouraged to use 1/2 lb. measurements on light items (e.g. 1/2 lb. or 1 1/2 lb.).\n";
		} else {
			prompt << "Description: include a short history and benefits in 150 words or less (do NOT include any enchantment or curse). "\
					  "Most importantly: be original and imaginative. Do not rely on the term \"dying star\". "\
					  "You are encouraged to use 1/2 lb. measurements on light items (e.g. 1/2 lb. or 1 1/2 lb.).\n";
		}
	} else { 
		prompt << "You are a Dungeons & Dragons 5E jewelry crafter.\n"
			   << "Produce ONLY a single JSON object (no extra text).\n"
			   << "I want a piece of jewelry with these parameters:\n"
			   << "• Name: "    << name   << "\n"
			   << "• Type: "    << subtype<< "\n"
			   << "• Rarity: "  << rarity << "\n";
					if (!extraDesc.empty()) {
					prompt << "• Additional Details: " << extraDesc << "\n";
			    }
				prompt << "\nYour JSON schema should be:\n" << R"({
					"Name": "...",
					"Type": "...",
					"Rarity": "...",
					"Weight": "...",
					"Description": "..."
				}
				)";

		prompt << "\nPopulate only the fields after those prefilled above.\n";
		if (allowEnchantment) {
			prompt << "Description: include a short history, benefits, and an enchantment in 150 words or less, "\
					  "scale the enchantments appropriately according to rarity, only add curses to items of legendary rarity or greater, "\
					  "most importantly: be original and imaginative, you are encouraged to combine fantasy sources, "\
				      "do not rely on terms like \"serpent\" or \"whispering sand\". Item weight should be a minimum of 1/2 lb.\n";
		} else {
			prompt << "Description: include a short history and benefits in 150 words or less (do NOT include any enchantment or curse). "\
					  "Most importantly: be original and imaginative, you are encouraged to combine fantasy sources, "\
					  "do not rely on terms like \"serpent\" or \"whispering sand\". Item weight should be a minimum of 1/2 lb.\n";
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
			{"temperature",      1.0},
			{"maxOutputTokens",  768},
			{"topP",             0.95},
			{"topK",             40}      
		}}
	};

	// 3) Build URL
	std::string host  = "https://" + location + "-aiplatform.googleapis.com";
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
	if (resp.status_code < 200 || resp.status_code >= 300) {
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

// Helper: if that numeric value > 1, switch to " lbs."
static void adjustWeight(nlohmann::json &out) {
	if (!out.contains("Weight") || !out["Weight"].is_string()) return;

	// 1) grab & trim the original, e.g. "1 1/2 lb."
	std::string w = trim(out["Weight"].get<std::string>());

	// 2) split at the last space, to separate numeric and unit
	auto pos = w.find_last_of(' ');
	if (pos == std::string::npos) return;

	std::string numericPart = trim(w.substr(0, pos));
	// std::string oldUnit     = trim(w.substr(pos+1)); 

	// 3) decide singular vs plural
	std::string unit = (numericPart == "1") ? "lb." : "lbs.";

	// 4) write it back
	out["Weight"] = numericPart + " " + unit;
}

nlohmann::json queryShopkeeper(const nlohmann::json& in,
                               const nlohmann::json& adc,
                               const std::string& project,
                               const std::string& location) {
    using json = nlohmann::json;

    // 1) extract inputs (description is optional)
    std::string name          = in.value("name", "");
    std::string race          = in.value("race", "");
    std::string settlement    = in.value("settlementSize", "");
    std::string shopType      = in.value("shopType", "");
    std::string extraDesc     = in.value("description", "");

    // 2) build the user prompt
    std::ostringstream prompt;
    prompt << "You are a Dungeons & Dragons 5th Edition shopkeeper NPC generator.\n"
           << "Produce ONLY a single JSON object (no extra text) with this schema:\n"
           << R"({
				"Name": "...",
				"Race": "...",
				"SettlementSize": "...",
				"ShopType": "...",
				"Description": "...",
				"ItemsList": ["...", "...", "..."]
			})"
          << "\nHere are the parameters:\n"
          << "• Name: "           << name       << "\n"
          << "• Race: "           << race       << "\n"
          << "• Settlement Size: "<< settlement << "\n"
          << "• Shop Type: "      << shopType   << "\n";
    if (!extraDesc.empty()) {
        prompt << "• Additional Details: " << extraDesc << "\n";
    }
    prompt << "\nGenerate a list of 10–15 items this shopkeeper sells, appropriate to "\
              "the shop type and settlement size.\n";

    // 3) prepare the Vertex AI payload
    json payload = {
        {"contents", json::array({
            {
                {"role",  "user"},
                {"parts", json::array({ {{"text", prompt.str()}} }) }
            }
        })},
        {"generationConfig", {
            {"temperature",     1.0},
            {"maxOutputTokens", 1024},
            {"topP",            0.95},
			{"topK",             40}
        }}
    };

    // 4) call the Gemini endpoint (MAY CHANGE THIS TO GEMINI 2.5)
    std::string url = "https://aiplatform.googleapis.com"
                    + std::string("/v1/projects/") + project
                    + "/locations/"   + location
                    + "/publishers/google/models/gemini-2.0-flash-001:generateContent";

    std::string bearer = "Bearer " + getAccessToken(adc);
    auto resp = cpr::Post(
        cpr::Url{url},
        cpr::Header{
          {"Content-Type",  "application/json"},
          {"Authorization", bearer}
        },
        cpr::Body{payload.dump()}
    );
    if (resp.error) {
        throw std::runtime_error("Shopkeeper HTTP POST failed: " + resp.error.message);
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        throw std::runtime_error("Shopkeeper Vertex AI HTTP " +
                                 std::to_string(resp.status_code) +
                                 ": " + resp.text);
    }

    // 5) extract the JSON blob from the model’s response
    auto full = json::parse(resp.text);
    std::string raw =
      full["candidates"][0]["content"]["parts"][0]["text"].get<std::string>();
    auto start = raw.find('{'), end = raw.rfind('}');
    if (start == std::string::npos || end == std::string::npos || end <= start) {
      return full;
    }
    std::string jsonText = raw.substr(start, end - start + 1);
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

	// CLI mode
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

	// HTTP‐server mode
	crow::SimpleApp app;

	// Gear builder route
	CROW_ROUTE(app, "/api/gear").methods("POST"_method)
	([&](const crow::request& req){
		try {
			json in  = json::parse(req.body);
			json out = queryGemini(in, adc, project, location);
			adjustWeight(out);
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

	// Random‐gear route
	CROW_ROUTE(app, "/api/gear/random").methods("GET"_method)
	([&](){
		static std::mt19937_64 gen{ std::random_device{}() };

		std::vector<std::string> rarities{"Common","Uncommon","Rare","Very Rare","Legendary","Artifact"};
		std::uniform_int_distribution<> dR(0, (int)rarities.size()-1);

		std::vector<std::string> types{"Weapon","Armor"};
		std::uniform_int_distribution<> dT(0,1);

		json in;
		in["type"]   = types[dT(gen)];
		in["rarity"] = rarities[dR(gen)];
		in["name"]   = "";

		if (in["type"] == "Weapon") {
			std::vector<std::string> hands{"Single-Handed","Two-Handed"};
			std::uniform_int_distribution<> dH(0,1);
			std::string hand = hands[dH(gen)];
			in["handedness"] = hand;
			std::vector<std::string> subs = (hand == "Single-Handed")
			? std::vector<std::string>{
				"Club",
				"Dagger",
				"Flail",
				"Hand Crossbows",
				"Handaxe",
				"Javelin",
				"Light Hammer",
				"Mace",
				"Morningstar",
				"Rapier",
				"Scimitar",
				"Sickle",
				"Shortsword",
				"War pick"
			 }
			: std::vector<std::string>{
				"Battleaxe",
				"Glaive",
				"Greataxe",
				"Greatsword",
				"Halberd",
				"Longsword",
				"Maul",
				"Pike",
				"Quarterstave",
				"Spears",
				"Trident",
				"Warhammer"
			};
			std::uniform_int_distribution<> dS(0, (int)subs.size()-1);
			in["subtype"] = subs[dS(gen)];
		} else {
			std::vector<std::string> armorClasses{"Light","Medium","Heavy","Shield","Clothes"};
			std::uniform_int_distribution<> dA(0, (int)armorClasses.size()-1);
			std::string ac = armorClasses[dA(gen)];
			in["subtype"] = ac;
			if (ac != "Shield") {
				std::vector<std::string> cloths{"Helmet","Chestplate","Gauntlets","Boots","Cloak","Hat"};
				std::uniform_int_distribution<> dC(0, (int)cloths.size()-1);
				in["clothingPiece"] = cloths[dC(gen)];
			}
		}

		try {
			json out = queryGemini(in, adc, project, location);
			adjustWeight(out);
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

	CROW_ROUTE(app, "/api/shopkeeper").methods("POST"_method)
    ([&](const crow::request& req){
        try {
            auto in  = nlohmann::json::parse(req.body);
            auto out = queryShopkeeper(in, adc, project, location);
            crow::response res(out.dump());
            res.set_header("Content-Type","application/json");
            return res;
        } catch (const std::exception& e) {
            nlohmann::json err = {
                {"error",   "ProcessingFailed"},
                {"message", e.what()}
            };
            crow::response res(500, err.dump());
            res.set_header("Content-Type","application/json");
            return res;
        }
    });

	CROW_ROUTE(app, "/api/shopkeeper/random").methods("POST"_method)
    ([&](){
        static std::mt19937_64 gen{ std::random_device{}() };
        std::vector<std::string> races = {
            "Aarakocra","Aasimar","Air Genasi","Bugbear","Centaur","Changeling","Deep Gnome","Duergar","Dragonborn",
            "Dwarf","Earth Genasi","Eladrin","Elf","Fairy","Firbolg","Fire Genasi","Githyanki","Githzerai","Gnome",
            "Goliath","Half-Elf","Halfling","Half-Orc","Harengon","Hobgoblin","Human","Kenku","Kobold","Lizardfolk",
            "Minotaur","Orc","Satyr","Sea Elf","Shadar-kai","Shifter","Tabaxi","Tiefling","Tortle","Triton",
            "Water Genasi","Yuan-ti"
        };
        std::uniform_int_distribution<> dR(0, (int)races.size()-1);

        std::vector<std::string> settlements{"Outpost","Village","Town","City"};
        std::uniform_int_distribution<> dS(0, (int)settlements.size()-1);

        std::vector<std::string> shopTypes{
            "Alchemist","Apostle","Artificer","Apothecary","Blacksmith","Bookstore","Cobbler","Fletcher",
            "General Store","Haberdashery","Innkeeper","Leatherworker","Pawnshop","Tailor"
        };
        std::uniform_int_distribution<> dT(0, (int)shopTypes.size()-1);

        json in;
        in["name"]           = "";
        in["race"]           = races[dR(gen)];
        in["settlementSize"] = settlements[dS(gen)];
        in["shopType"]       = shopTypes[dT(gen)];
        in["description"]    = "";

        try {
            json out = queryShopkeeper(in, adc, project, location);
            crow::response res(out.dump());
            res.set_header("Content-Type","application/json");
            return res;
        } catch (const std::exception& e) {
            json err = {{"error","ProcessingFailed"},{"message",e.what()}};
            crow::response res(500, err.dump());
            res.set_header("Content-Type","application/json");
            return res;
        }
    });

	app.port(5000).multithreaded().run();
	return 0;
}