#include "ogs_puzzles.hpp"
#include "json.hpp"

#include <cstring>
#include <cstdio>
#include <curl/curl.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using json = nlohmann::json;

// ── HTTP GET → string (mirrors ogs_net.cpp's curl setup) ─────────────────────

static std::string puzzles_ca_bundle() {
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    char* last_sep = strrchr(exe, '\\');
    if (!last_sep) last_sep = strrchr(exe, '/');
    if (last_sep) {
        strcpy(last_sep + 1, "ca-bundle.crt");
        return exe;
    }
#endif
    return "ca-bundle.crt";
}

static size_t puzzles_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = reinterpret_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

static bool http_get_json(const std::string& url, json& out) {
    std::string data;
    long code = 0;

    CURL* c = curl_easy_init();
    if (!c) return false;
    static std::string ca = puzzles_ca_bundle();
    curl_easy_setopt(c, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION,  puzzles_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,      &data);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,        15L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO,         ca.c_str());
    CURLcode res = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);

    if (res != CURLE_OK || code != 200) return false;
    try {
        out = json::parse(data);
    } catch (...) {
        return false;
    }
    return true;
}

// ── Parsers ───────────────────────────────────────────────────────────────────

static void parse_move_tree(const json& j, PuzzleMoveNode& node) {
    node.x       = j.value("x", -1);
    node.y       = j.value("y", -1);
    node.correct = j.value("correct_answer", false);
    node.wrong   = j.value("wrong_answer",   false);
    node.text    = j.value("text", std::string());
    if (j.contains("branches") && j["branches"].is_array()) {
        for (const auto& b : j["branches"]) {
            node.branches.emplace_back();
            parse_move_tree(b, node.branches.back());
        }
    }
}

bool ogs_fetch_puzzle(int puzzle_id, OgsPuzzle& out) {
    json j;
    if (!http_get_json("https://online-go.com/api/v1/puzzles/"
                       + std::to_string(puzzle_id), j))
        return false;
    if (!j.contains("puzzle") || !j["puzzle"].is_object()) return false;

    try {
        out = OgsPuzzle{};
        out.id   = j.value("id", puzzle_id);
        out.rank = j.value("rank", 0);
        const auto& p = j["puzzle"];
        out.name        = p.value("name", std::string());
        out.description = p.value("puzzle_description", std::string());
        out.type        = p.value("puzzle_type", std::string());
        out.width       = p.value("width", 19);
        out.height      = p.value("height", 19);
        if (p.contains("initial_state") && p["initial_state"].is_object()) {
            out.initial_black = p["initial_state"].value("black", std::string());
            out.initial_white = p["initial_state"].value("white", std::string());
        }
        out.black_to_play = p.value("initial_player", std::string("black")) == "black";
        out.opponent_auto = p.value("puzzle_opponent_move_mode", std::string("automatic"))
                            == "automatic";
        if (j.contains("collection") && j["collection"].is_object()) {
            out.collection_id   = j["collection"].value("id", 0);
            out.collection_name = j["collection"].value("name", std::string());
        }
        if (p.contains("move_tree") && p["move_tree"].is_object())
            parse_move_tree(p["move_tree"], out.tree);
    } catch (...) {
        return false;
    }
    return true;
}

bool ogs_fetch_puzzle_collections(int page, int page_size,
                                  std::vector<OgsPuzzleCollection>& out,
                                  int& total_out) {
    json j;
    if (!http_get_json("https://online-go.com/api/v1/puzzles/collections/?page="
                       + std::to_string(page) + "&page_size=" + std::to_string(page_size), j))
        return false;
    if (!j.contains("results") || !j["results"].is_array()) return false;

    try {
        total_out = j.value("count", 0);
        out.clear();
        for (const auto& r : j["results"]) {
            OgsPuzzleCollection c;
            c.id           = r.value("id", 0);
            c.name         = r.value("name", std::string());
            if (r.contains("owner") && r["owner"].is_object())
                c.owner = r["owner"].value("username", std::string());
            c.puzzle_count = r.value("puzzle_count", 0);
            c.min_rank     = r.value("min_rank", 0);
            c.max_rank     = r.value("max_rank", 0);
            c.rating       = r.value("rating", 0.f);
            c.rating_count = r.value("rating_count", 0);
            if (r.contains("starting_puzzle") && r["starting_puzzle"].is_object())
                c.starting_puzzle_id = r["starting_puzzle"].value("id", 0);
            out.push_back(std::move(c));
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool ogs_fetch_collection_puzzles(int puzzle_id,
                                  std::vector<std::pair<int, std::string>>& out) {
    json j;
    if (!http_get_json("https://online-go.com/api/v1/puzzles/"
                       + std::to_string(puzzle_id) + "/collection_summary", j))
        return false;
    if (!j.is_array()) return false;

    try {
        out.clear();
        for (const auto& e : j)
            out.push_back({e.value("id", 0), e.value("name", std::string())});
    } catch (...) {
        return false;
    }
    return true;
}
