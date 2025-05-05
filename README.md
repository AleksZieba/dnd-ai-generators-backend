# DND AI Generators Backend

A C++ web server that powers the AI-driven gear and shopkeeper generators for [dndgamegen.com](https://dndgamegen.com). It exposes both a CLI interface and a REST API to produce JSON-formatted output using Google Vertex AI (Gemini 2.0-Flash) and OpenAI GPT 4.1-mini, but can be easily configured to use any Gemini or GPT model.

---

## Table of Contents

- [Features](#features)  
- [Tech Stack](#tech-stack)  
- [Getting Started](#getting-started)  
  - [Prerequisites](#prerequisites)  
  - [Installation & Build](#installation--build)  
- [Configuration](#configuration)  
- [Usage](#usage)  
  - [CLI Mode](#cli-mode)  
  - [HTTP Server Mode](#http-server-mode)  
- [API Reference](#api-reference)  
- [License](#license)  

---

## Features

- **AI-Powered Gear Generation** — Generate weapons, armor, or jewelry with rich lore and stats.  
- **AI-Powered Shopkeeper NPCs** — Spin up fully-featured shopkeeper profiles with inventories.  
- **Dual Mode** — Run in CLI mode for piping JSON or as a multithreaded HTTP server.  
- **Caching & Token Refresh** — JWT-based Google Cloud OAuth2 with automatic refresh and caching.  

---

## Tech Stack

- **C++17**
- **[Crow](https://github.com/CrowCpp/Crow)** (header-only web framework)
- **[cpr](https://github.com/libcpr/cpr)** for HTTP client
- **[nlohmann/json](https://github.com/nlohmann/json)** for JSON parsing
- **OpenSSL** for RSA-SHA256 signing and base64 URL encoding
- **CMake** & **FetchContent** for dependency management

---

## Getting Started

### Prerequisites

- **CMake** ≥ 3.14  
- **A C++17-capable compiler** (e.g. GCC 9+, Clang 10+, MSVC 2019+)  
- **Git** (to fetch dependencies via CMake)  
- **OpenSSL** development headers  

### Installation & Build

1. **Clone the repository**  
```bash
git clone https://github.com/your-username/dnd-api-backend.git
cd dnd-api-backend
``` 

2. **Configure & build** 
```bash
mkdir build && cd build
cmake ..                 # Fetches crow, cpr, nlohmann/json
cmake --build . --config Release
``` 
the resulting executable will be `backend`. 

--- 

## Configuration 

Create a '.env' file in the project root, and in the build directory (or set these variables in your environment):
```bash
GOOGLE_PROJECT_ID=<YOUR-GCP-PROJECT>
GOOGLE_PROJECT_LOCATION=<GOOGLE-VERTEX-AI-ENDPOINT-WHICH-SUPPORTS-GEMINI-2.0-FLASH>
GOOGLE_APPLICATION_CREDENTIALS=<./path/to/your-service-account.json>
OPENAI_API_KEY=<YOUR_OPENAI_KEY>
```

- **GOOGLE_APPLICATION_CREDENTIALS** should point to your GCP service account JSON Token. 
- **OPENAI_API_KEY** should point to your OpenAI API key. 

--- 

## Usage 

### CLI Mode
**This mode was create for you to test that your Google project ID, project location, and application credentials are set up properly.**
Pipe a JSON request into the binary to get a single JSON object back:
```bash
cat request.json | ./backend --cli
``` 
Example payload in `request.json`:
```json
{
  "name": "Goblin Slayer",
  "type": "Weapon",
  "handedness": "Single-Handed",
  "subtype": "Shortsword",
  "rarity": "Rare"
}
```

### HTTP Server Mode 
Start the multithreaded server on port 5000:
```bash
./build/backend     # From root directory 
./backend           # From build directory 
``` 
By default, it binds to 0.0.0.0:5000 and will use available threads as needed. 

---
 
## API Reference 

All endpoints respond with application/json.

| Endpoint                         | Description                               | Query Parameters                                                            |
| -------------------------------- | ----------------------------------------- | --------------------------------------------------------------------------- |
| `GET /api/gear`                  | Generate gear with optional parameters    | `name`, `type`, `handedness`, `subtype`, `rarity`, `clothingPiece`, `description` |
| `GET /api/gear/random`           | Generate a completely random gear item    | *(no parameters)*                                                            |
| `GET /api/shopkeeper`            | Generate a shopkeeper NPC with parameters | `name`, `race`, `settlementSize`, `shopType`, `description`                   |
| `GET /api/shopkeeper/random`     | Generate a completely random shopkeeper NPC | *(no parameters)*                                                         |

--- 

## License 

This project is licensed under the GNU GPL v3 viewable [here](https://github.com/AleksZieba/dnd-ai-generators-backend/blob/main/LICENSE.md). 