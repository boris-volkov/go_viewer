#include "ogs_net.hpp"
#include "json.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <libwebsockets.h>
#include <curl/curl.h>

static FILE* g_log_file = nullptr;

static void net_log(const char* msg) {
    // Open a timestamped log file on first call
    if (!g_log_file) {
        time_t t = time(nullptr);
        struct tm* tm = localtime(&t);
        char name[64];
        strftime(name, sizeof(name), "ogs_%Y%m%d_%H%M%S.log", tm);
        g_log_file = fopen(name, "w");
        if (g_log_file) {
            char header[128];
            strftime(header, sizeof(header), "=== OGS session %Y-%m-%d %H:%M:%S ===", tm);
            fprintf(g_log_file, "%s\n", header);
            fflush(g_log_file);
        }
    }
    // Timestamp each line
    time_t t = time(nullptr);
    struct tm* tm = localtime(&t);
    char ts[24];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm);
    if (g_log_file) {
        fprintf(g_log_file, "[%s] %s\n", ts, msg);
        fflush(g_log_file);
    }
    fprintf(stderr, "[%s] %s\n", ts, msg);
}

static void lws_log_to_file(int level, const char* line) {
    if (!line) return;
    // Strip trailing newline for clean formatting
    char buf[512];
    int len = (int)strlen(line);
    if (len > 0 && line[len-1] == '\n') len--;
    if (len >= (int)sizeof(buf)) len = (int)sizeof(buf) - 1;
    memcpy(buf, line, len);
    buf[len] = '\0';
    net_log(buf);
}

#include <SDL2/SDL.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <chrono>
#include <random>
#include <cstdlib>
#include <cassert>
#include <sstream>

using json = nlohmann::json;

// SDL custom event type — registered once in main(), read here.
extern Uint32 g_net_event_type;

// ── CURL helpers ─────────────────────────────────────────────────────────────

// Resolve the CA bundle path relative to the running executable.
static std::string find_ca_bundle() {
#ifdef _WIN32
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    // Replace filename with ca-bundle.crt
    char* last_sep = strrchr(exe, '\\');
    if (!last_sep) last_sep = strrchr(exe, '/');
    if (last_sep) {
        strcpy(last_sep + 1, "ca-bundle.crt");
        return exe;
    }
#endif
    return "ca-bundle.crt";
}

static size_t curl_write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = reinterpret_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

// GET (body empty) or POST. cookiejar: Netscape file for read/write; "" = no jar.
// extra_hdr: optional extra header line e.g. "X-CSRFToken: abc"; "" = none.
static bool curl_request(const std::string& url,
                         const std::string& body,
                         const std::string& cookiejar,
                         const std::string& extra_hdr,
                         std::string& response_out,
                         long& http_code_out,
                         const std::string& method = "")
{
    CURL* c = curl_easy_init();
    if (!c) return false;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    // Django enforces strict Referer checking on HTTPS for unsafe methods, and also
    // validates Origin when it is present. A browser sends both; curl sends neither,
    // so POST/DELETE were rejected with
    //   403 {"detail":"CSRF Failed: Referer checking failed - no Referer."}
    // even with a valid session cookie and X-CSRFToken. Every request in this client
    // targets online-go.com, so setting them unconditionally is safe.
    headers = curl_slist_append(headers, "Referer: https://online-go.com/");
    headers = curl_slist_append(headers, "Origin: https://online-go.com");
    if (!extra_hdr.empty())
        headers = curl_slist_append(headers, extra_hdr.c_str());

    static std::string ca_bundle = find_ca_bundle();

    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &response_out);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_CAINFO, ca_bundle.c_str());

    if (!cookiejar.empty()) {
        curl_easy_setopt(c, CURLOPT_COOKIEJAR,  cookiejar.c_str());
        curl_easy_setopt(c, CURLOPT_COOKIEFILE, cookiejar.c_str());
    } else {
        curl_easy_setopt(c, CURLOPT_COOKIEFILE, "");
    }

    if (method == "POST") {
        // Explicit POST — works with an empty body (e.g. challenge accept).
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
        if (!body.empty())
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    } else if (!method.empty()) {
        // DELETE / PUT / etc.
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
        if (!body.empty()) {
            curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
            curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
        }
    } else if (!body.empty()) {
        // Default: a non-empty body implies POST (unchanged prior behaviour).
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }

    CURLcode res = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code_out);
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return (res == CURLE_OK);
}

// Parse a Netscape-format cookie jar file and return the value of `name`.
static std::string read_cookie(const std::string& jar_path, const std::string& name) {
    FILE* f = fopen(jar_path.c_str(), "r");
    if (!f) return "";
    char line[4096];
    std::string result;
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        // Fields: domain \t subdomain \t path \t secure \t expiry \t name \t value
        char* fields[7] = {};
        char* tok = strtok(line, "\t");
        int i = 0;
        while (tok && i < 7) { fields[i++] = tok; tok = strtok(nullptr, "\t"); }
        if (i == 7 && fields[5] && name == fields[5]) {
            result = fields[6] ? fields[6] : "";
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
            break;
        }
    }
    fclose(f);
    return result;
}

void OgsNet::fetch_sgf(int game_id, const std::string& path) {
    // curl_request always sends Accept: application/json which the /sgf/ endpoint
    // rejects with 406. Do a bare GET with only the Authorization header.
    std::string url = "https://online-go.com/api/v1/games/"
                    + std::to_string(game_id) + "/sgf/";
    std::string data;
    long code = 0;

    CURL* c = curl_easy_init();
    if (!c) return;
    static std::string ca = find_ca_bundle();
    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, ("Authorization: Bearer " + jwt_).c_str());
    curl_easy_setopt(c, CURLOPT_URL,           url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER,    hdrs);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA,     &data);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT,       15L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER,1L);
    curl_easy_setopt(c, CURLOPT_CAINFO,        ca.c_str());
    curl_easy_setopt(c, CURLOPT_COOKIEFILE,    "");
    CURLcode res = curl_easy_perform(c);
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);

    if (res != CURLE_OK || code != 200) {
        net_log(("fetch_sgf: HTTP " + std::to_string(code) + " for game " +
                 std::to_string(game_id)).c_str());
        return;
    }
    // Cancelled/annulled games export with RE[Void] and no real moves — OGS's own
    // marker for "this game doesn't count." Nothing worth reviewing, no result to
    // record — don't let it into the personal archive at all.
    if (data.find("RE[Void]") != std::string::npos) {
        net_log(("fetch_sgf: game " + std::to_string(game_id) +
                 " was cancelled (RE[Void]) — not saving").c_str());
        return;
    }
    FILE* fp = fopen(path.c_str(), "wb");
    if (!fp) { net_log(("fetch_sgf: cannot write " + path).c_str()); return; }
    fwrite(data.data(), 1, data.size(), fp);
    fclose(fp);
    net_log(("fetch_sgf: saved " + path).c_str());
}

// One "active_game" event: the server's push of a game the user is in. These
// arrive right after authenticate (one per ongoing game) and again whenever a game
// changes (e.g. a move flips whose turn it is), so corr_games_ stays current for as
// long as the socket is up.
void OgsNet::handle_active_game(const std::string& payload_json) {
    // Log the first payload in full, once — the exact field layout (whose-turn key,
    // players, size, speed) can then be verified against a real game.
    static bool logged_shape = false;
    if (!logged_shape) {
        logged_shape = true;
        net_log(("active_game shape: " + payload_json.substr(0, 1500)).c_str());
    }

    try {
        auto d = json::parse(payload_json);
        int gid = d.value("id", d.value("game_id", 0));
        if (gid == 0) { push_corr_list(); return; }

        auto grab = [](const json& o, int& id, std::string& name) {
            if (!o.is_object()) return;
            if (o.contains("id") && o["id"].is_number_integer()) id = o["id"].get<int>();
            name = o.value("username", name);
        };
        int black_id = 0, white_id = 0;
        std::string black_name = "Black", white_name = "White";
        grab(d.value("black", json::object()), black_id, black_name);
        grab(d.value("white", json::object()), white_id, white_name);

        // Only the user's own games (active_game is also how observed games arrive).
        if (black_id != my_player_id && white_id != my_player_id) { push_corr_list(); return; }

        // Whose turn: player_to_move, or clock.current_player as a fallback.
        int ptm = d.value("player_to_move", 0);
        if (ptm == 0 && d.contains("clock") && d["clock"].is_object())
            ptm = d["clock"].value("current_player", 0);

        CorrGameSummary s;
        s.id         = gid;
        s.board_size = d.value("width", d.value("size", 19));
        s.opp        = (black_id == my_player_id) ? white_name : black_name;
        s.my_move    = (ptm != 0 && ptm == my_player_id);

        // Correspondence vs live: active_game has no speed field, but time_per_move
        // (seconds allotted per move) separates them cleanly — correspondence is
        // hours-to-days per move, live is seconds-to-minutes. Drop anything with a
        // small positive per-move time as live; 0/absent means no clock (an unlimited
        // correspondence game), which we keep. 3600s (1h) is a safe cutoff — no live
        // speed tier (blitz/rapid/live) approaches an hour per move.
        int  tpm     = d.value("time_per_move", 0);
        bool is_live = (tpm > 0 && tpm < 3600);

        std::string phase = d.value("phase", "");
        {
            std::lock_guard<std::mutex> lk(corr_mu_);
            if (phase == "finished" || is_live) corr_games_.erase(gid);
            else                                corr_games_[gid] = s;
        }
    } catch (const std::exception& e) {
        net_log(("active_game parse error: " + std::string(e.what())).c_str());
    }
    push_corr_list();
}

// Push the current cache to the main thread as a CORR_LIST_UPDATED snapshot.
void OgsNet::push_corr_list() {
    NetMsg m;
    m.type = NetMsgType::CORR_LIST_UPDATED;
    {
        std::lock_guard<std::mutex> lk(corr_mu_);
        m.corr_games.reserve(corr_games_.size());
        for (const auto& kv : corr_games_) m.corr_games.push_back(kv.second);
    }
    push_msg(std::move(m));
}

// Locked copy of the cache — for the main thread's on-demand refresh (the events
// may all have arrived before the user opened MY GAMES).
std::vector<CorrGameSummary> OgsNet::corr_games_snapshot() {
    std::vector<CorrGameSummary> out;
    std::lock_guard<std::mutex> lk(corr_mu_);
    out.reserve(corr_games_.size());
    for (const auto& kv : corr_games_) out.push_back(kv.second);
    return out;
}

// ── Challenges (authenticated REST via the session cookie) ────────────────────

void OgsNet::fetch_challenges() {
    NetMsg m;
    m.type = NetMsgType::CHALLENGES_UPDATED;
    std::string resp;
    long code = 0;
    bool ok = curl_request("https://online-go.com/api/v1/me/challenges/", "",
                           cookiejar_, rest_read_hdr(), resp, code);
    if (!ok || code != 200) {
        net_log(("fetch_challenges: HTTP " + std::to_string(code)).c_str());
        net_log(("fetch_challenges raw: " + resp.substr(0, 300)).c_str());
        m.text = "CHALLENGES HTTP " + std::to_string(code);
        push_msg(std::move(m));
        return;
    }
    try {
        auto d = json::parse(resp);
        // The list may be a bare array or a paginated { count, results:[...] }.
        const json& arr = (d.is_object() && d.contains("results")) ? d["results"] : d;
        if (arr.is_array() && !arr.empty())
            net_log(("challenge shape: " + arr[0].dump().substr(0, 900)).c_str());

        if (arr.is_array()) for (const auto& c : arr) {
            ChallengeSummary cs;
            cs.id = c.value("id", 0);

            // Challenger object (id + username). If it's me, this is one of my own
            // outgoing challenges — not something for me to accept, so skip it.
            int challenger_id = 0;
            if (c.contains("challenger") && c["challenger"].is_object()) {
                challenger_id = c["challenger"].value("id", 0);
                cs.challenger = c["challenger"].value("username", "?");
            } else {
                cs.challenger = c.value("challenger_username", c.value("username", "?"));
            }
            if (challenger_id != 0 && challenger_id == my_player_id) continue;

            // Game params live under a nested "game" object on me/challenges/.
            const json& g = (c.contains("game") && c["game"].is_object()) ? c["game"] : c;
            cs.board_size = g.value("width", g.value("board_size", 19));
            cs.ranked     = g.value("ranked", false);

            std::string speed;
            if (g.contains("time_control_parameters") && g["time_control_parameters"].is_object())
                speed = g["time_control_parameters"].value("speed", "");
            if (speed.empty() && g.contains("time_control")) {
                if (g["time_control"].is_object())      speed = g["time_control"].value("speed", "");
                else if (g["time_control"].is_string()) speed = g["time_control"].get<std::string>();
            }
            if (speed.empty()) speed = g.value("speed", "");
            cs.correspondence = (speed == "correspondence" || speed.empty());
            cs.time_desc      = speed.empty() ? "?" : speed;

            m.challenges.push_back(std::move(cs));
        }
        net_log(("fetch_challenges: " + std::to_string((int)m.challenges.size()) +
                 " incoming challenge(s)").c_str());
    } catch (const std::exception& e) {
        net_log(("fetch_challenges parse error: " + std::string(e.what())).c_str());
        m.text = "CHALLENGES PARSE FAILED";
    }
    push_msg(std::move(m));
}

void OgsNet::accept_challenge(int challenge_id) {
    NetMsg m;
    m.type = NetMsgType::CHALLENGE_RESULT;
    std::string url = "https://online-go.com/api/v1/me/challenges/" +
                      std::to_string(challenge_id) + "/accept/";
    std::string resp;
    long code = 0;
    bool ok = curl_request(url, "", cookiejar_, rest_write_hdr(), resp, code, "POST");
    net_log(("accept_challenge " + std::to_string(challenge_id) + ": HTTP " +
             std::to_string(code) + " " + resp.substr(0, 200)).c_str());

    if (ok && (code == 200 || code == 201 || code == 204)) {
        m.text = "CHALLENGE ACCEPTED";
        // The response usually carries the created game's id.
        try {
            auto d = json::parse(resp);
            if (d.contains("game")) {
                if (d["game"].is_number_integer()) m.game_id = d["game"].get<int>();
                else if (d["game"].is_object())    m.game_id = d["game"].value("id", 0);
            } else if (d.contains("game_id")) {
                m.game_id = d.value("game_id", 0);
            }
        } catch (...) {}
    } else {
        m.text = "ACCEPT FAILED (" + std::to_string(code) + ")";
    }
    push_msg(std::move(m));
    fetch_challenges();   // refresh — the accepted challenge drops off the list
}

void OgsNet::fetch_friends() {
    NetMsg m;
    m.type = NetMsgType::FRIENDS_UPDATED;
    std::string resp;
    long code = 0;
    bool ok = curl_request("https://online-go.com/api/v1/me/friends/", "",
                           cookiejar_, rest_read_hdr(), resp, code);
    if (!ok || code != 200) {
        net_log(("fetch_friends: HTTP " + std::to_string(code)).c_str());
        m.text = "FRIENDS HTTP " + std::to_string(code);
        push_msg(std::move(m));
        return;
    }
    try {
        auto d = json::parse(resp);
        const json& arr = (d.is_object() && d.contains("results")) ? d["results"] : d;
        if (arr.is_array() && !arr.empty())
            net_log(("friend shape: " + arr[0].dump().substr(0, 400)).c_str());
        if (arr.is_array()) for (const auto& f : arr) {
            // A row may be the player object itself or wrap it under "user".
            const json& p = (f.is_object() && f.contains("user") && f["user"].is_object())
                                ? f["user"] : f;
            FriendSummary fs;
            fs.id       = p.value("id", 0);
            fs.username = p.value("username", "");
            if (fs.id != 0 && !fs.username.empty()) m.friends.push_back(std::move(fs));
        }
        net_log(("fetch_friends: " + std::to_string((int)m.friends.size()) +
                 " friend(s)").c_str());
    } catch (const std::exception& e) {
        net_log(("fetch_friends parse error: " + std::string(e.what())).c_str());
        m.text = "FRIENDS PARSE FAILED";
    }
    push_msg(std::move(m));
}

void OgsNet::send_challenge(const ChallengeRequest& req) {
    NetMsg m;
    m.type = NetMsgType::CHALLENGE_RESULT;

    json tcp = {
        {"system",            "fischer"},
        {"speed",             "correspondence"},
        {"initial_time",      req.initial_time},
        {"time_increment",    req.time_increment},
        {"max_time",          req.max_time},
        {"pause_on_weekends", true}
    };
    json game = {
        {"name",                    "Friendly Match"},
        {"rules",                   "japanese"},
        {"ranked",                  req.ranked},
        {"width",                   req.board_size},
        {"height",                  req.board_size},
        {"handicap",                0},
        {"komi_auto",               "automatic"},
        {"disable_analysis",        false},
        {"pause_on_weekends",       true},
        {"time_control",            "fischer"},
        {"time_control_parameters", tcp},
        {"private",                 false}
    };
    json payload = {
        {"initialized",      false},
        {"min_ranking",      -1000},
        {"max_ranking",      1000},
        {"challenger_color", req.color},
        {"game",             game},
        {"aga_ranked",       false}
    };

    // A named opponent goes to that player's challenge endpoint; opponent_id 0 posts
    // an open challenge to the general list, which anyone may accept.
    std::string url = req.opponent_id > 0
        ? "https://online-go.com/api/v1/players/" + std::to_string(req.opponent_id) + "/challenge/"
        : "https://online-go.com/api/v1/challenges/";

    std::string resp;
    long code = 0;
    bool ok = curl_request(url, payload.dump(), cookiejar_, rest_write_hdr(),
                           resp, code, "POST");
    net_log(("send_challenge -> " + url + " HTTP " + std::to_string(code) + " " +
             resp.substr(0, 300)).c_str());

    m.text = (ok && (code == 200 || code == 201))
                 ? "CHALLENGE SENT"
                 : "CHALLENGE FAILED (" + std::to_string(code) + ")";
    push_msg(std::move(m));
}

void OgsNet::decline_challenge(int challenge_id) {
    NetMsg m;
    m.type = NetMsgType::CHALLENGE_RESULT;
    std::string url = "https://online-go.com/api/v1/me/challenges/" +
                      std::to_string(challenge_id) + "/";
    std::string resp;
    long code = 0;
    bool ok = curl_request(url, "", cookiejar_, rest_write_hdr(), resp, code, "DELETE");
    net_log(("decline_challenge " + std::to_string(challenge_id) + ": HTTP " +
             std::to_string(code)).c_str());
    m.text = (ok && (code == 200 || code == 202 || code == 204))
                 ? "CHALLENGE DECLINED"
                 : "DECLINE FAILED (" + std::to_string(code) + ")";
    push_msg(std::move(m));
    fetch_challenges();
}

// ── OgsNet lifecycle ─────────────────────────────────────────────────────────

OgsNet::OgsNet() {}

OgsNet::~OgsNet() {
    stop();
}

bool OgsNet::start(const std::string& username, const std::string& password) {
    // Reap a previous attempt's thread first. After a failed login the net thread
    // returns but thread_ stays joinable, and assigning a fresh std::thread over a
    // joinable one calls std::terminate() — which is exactly the crash seen when a
    // login is rejected (wrong password, rate-limited, ...) and the user retries.
    if (thread_.joinable()) {
        stop_flag_ = true;
        if (ctx_) lws_cancel_service(ctx_);
        thread_.join();
    }
    username_ = username;
    password_ = password;
    stop_flag_ = false;
    thread_ = std::thread([this] { net_loop(); });
    return true;
}

void OgsNet::stop() {
    stop_flag_ = true;
    if (ctx_) lws_cancel_service(ctx_);
    if (thread_.joinable()) thread_.join();
}

// ── Thread-safe command API ───────────────────────────────────────────────────

void OgsNet::cmd_find_match(const MatchPrefs& prefs) {
    match_uuid_ = make_uuid();

    static const char* size_strs[3]  = {"9x9", "13x13", "19x19"};
    // Real OGS speed values (goban's Speed type): "blitz" | "rapid" | "live" | "correspondence".
    // We expose the 3 non-correspondence tiers as explicit fast/medium/slow buckets — this
    // order must match MatchPrefs::speeds and renderer.cpp's speed_labels exactly (previously
    // it didn't: the UI showed "RAPID" in the slot that actually sent "live", and vice versa).
    static const char* speed_strs[3] = {"blitz", "rapid", "live"};

    json opts = json::array();
    for (int si = 0; si < 3; si++) {
        if (!prefs.sizes[si]) continue;
        for (int sp = 0; sp < 3; sp++) {
            if (!prefs.speeds[sp]) continue;
            for (const char* sys : {"byoyomi", "fischer"})
                opts.push_back({{"size", size_strs[si]}, {"speed", speed_strs[sp]}, {"system", sys}});
        }
    }
    if (opts.empty()) {
        // Fallback: 9x9 live if nothing selected
        for (const char* sys : {"byoyomi", "fischer"})
            opts.push_back({{"size","9x9"}, {"speed","live"}, {"system", sys}});
    }

    json payload = {
        {"uuid",             match_uuid_},
        {"lower_rank_diff",  0},
        {"upper_rank_diff",  9},
        {"rules",            {{"condition","required"},{"value","japanese"}}},
        {"handicap",         {{"condition","preferred"},{"value","enabled"}}},
        {"size_speed_options", opts}
    };
    std::string dump = payload.dump();
    net_log(("automatch/find_match: " + dump).c_str());
    enqueue_event("automatch/find_match", dump);
}

void OgsNet::cmd_cancel_match() {
    if (match_uuid_.empty()) { net_log("cmd_cancel_match: no uuid (already cleared?)"); return; }
    net_log(("automatch/cancel: uuid=" + match_uuid_).c_str());
    json payload = {{"uuid", match_uuid_}};
    enqueue_event("automatch/cancel", payload.dump());
    match_uuid_.clear();
}

// OGS move encoding: two-character letter string, e.g. col=0,row=1 → "ab".
// Pass (col=-1,row=-1) → "..". Matches the Python API's _num2char convention.
static std::string encode_move(int col, int row) {
    static const char* letters = "abcdefghijklmnopqrstuvwxyz";
    char s[3] = { col >= 0 ? letters[col] : '.', row >= 0 ? letters[row] : '.', 0 };
    return std::string(s, 2);
}

void OgsNet::cmd_send_move(int game_id, int col, int row) {
    json payload = {{"game_id", game_id}, {"player_id", my_player_id},
                    {"move", encode_move(col, row)}};
    enqueue_event("game/move", payload.dump());
}

void OgsNet::cmd_send_pass(int game_id) {
    json payload = {{"game_id", game_id}, {"player_id", my_player_id}, {"move", ".."}};
    enqueue_event("game/move", payload.dump());
}

// Re-send game/connect so OGS re-emits gamedata — the main thread calls this to
// force an authoritative board rebuild when the live move stream has desynced.
// Same payload as the automatch auto-connect.
void OgsNet::cmd_reconnect_game(int game_id) {
    json conn = {{"game_id", game_id}, {"player_id", my_player_id}, {"chat", false}};
    enqueue_event("game/connect", conn.dump());
}

// Open a game the user picked from their correspondence list. Same connect payload
// as the automatch auto-connect, but here we also claim active_game_id_ (automatch
// does that in its automatch/start handler) so move/clock/phase events for this game
// are recognised. Clear any stale removed-stones from a previously-open game.
void OgsNet::cmd_open_game(int game_id) {
    active_game_id_ = game_id;
    removed_stones_.clear();
    json conn = {{"game_id", game_id}, {"player_id", my_player_id}, {"chat", false}};
    net_log(("cmd_open_game: connecting to " + std::to_string(game_id)).c_str());
    enqueue_event("game/connect", conn.dump());
}

// Stop receiving a game's live event stream. Sent when the user backs out of a
// correspondence board so events for games they're no longer viewing don't pile up.
void OgsNet::cmd_disconnect_game(int game_id) {
    json payload = {{"game_id", game_id}, {"player_id", my_player_id}};
    net_log(("cmd_disconnect_game: " + std::to_string(game_id)).c_str());
    enqueue_event("game/disconnect", payload.dump());
    if (active_game_id_ == game_id) active_game_id_ = 0;
}

void OgsNet::cmd_send_resign(int game_id) {
    json payload = {{"game_id", game_id}, {"player_id", my_player_id}};
    enqueue_event("game/resign", payload.dump());
}

void OgsNet::cmd_accept_stones(int game_id) {
    // Payload/event name per the real client (goban's OGSConnectivity.acceptRemovedStones):
    // no player_id, no all_removed — just game_id/stones/strict_seki_mode on "game/removed_stones/accept".
    json payload = {
        {"game_id",          game_id},
        {"stones",           removed_stones_},
        {"strict_seki_mode", false}
    };
    std::string dump = payload.dump();
    net_log(("cmd_accept_stones: " + dump).c_str());
    enqueue_event("game/removed_stones/accept", dump);
}

void OgsNet::cmd_accept_undo(int game_id, int move_number) {
    json payload = {{"game_id", game_id}, {"move_number", move_number}};
    std::string dump = payload.dump();
    net_log(("cmd_accept_undo: " + dump).c_str());
    enqueue_event("game/undo/accept", dump);
}

void OgsNet::cmd_reject_undo(int game_id, int move_number) {
    json payload = {{"game_id", game_id}, {"move_number", move_number}};
    std::string dump = payload.dump();
    net_log(("cmd_reject_undo: " + dump).c_str());
    enqueue_event("game/undo/cancel", dump);
}

bool OgsNet::poll_msg(NetMsg& out) {
    std::lock_guard<std::mutex> lock(inbound_mu_);
    if (inbound_.empty()) return false;
    out = std::move(inbound_.front());
    inbound_.pop();
    return true;
}

// ── Auth (runs on network thread before WebSocket opens) ─────────────────────

// CSRF → password login → persistent cookiejar_/csrf_. Best-effort.
bool OgsNet::establish_session() {
    if (username_.empty() || password_.empty()) {
        net_log("establish_session: no username/password — REST writes disabled");
        return false;
    }
    const char* tmp = getenv("TEMP");
    if (!tmp) tmp = getenv("TMP");
    if (!tmp) tmp = "/tmp";
    cookiejar_ = std::string(tmp) + "/ogs_cookies.txt";
    remove(cookiejar_.c_str());

    std::string resp;
    long code = 0;

    // Step 1: GET /api/v1/ to receive the csrftoken cookie.
    if (!curl_request("https://online-go.com/api/v1/", "", cookiejar_, "", resp, code)) {
        net_log("establish_session: CSRF GET failed");
        cookiejar_.clear();
        return false;
    }
    csrf_ = read_cookie(cookiejar_, "csrftoken");
    if (csrf_.empty()) {
        net_log("establish_session: no csrftoken cookie");
        cookiejar_.clear();
        return false;
    }

    // Step 2: POST credentials with CSRF header. OGS has moved its login route over
    // time — /api/v1/login/ now 404s — so try the known ones and keep the first that
    // works. A wrong URL just 404s harmlessly.
    json login_body = {{"username", username_}, {"password", password_}};
    static const char* kLoginUrls[] = {
        "https://online-go.com/api/v0/login",
        "https://online-go.com/api/v1/login/",
    };
    bool logged_in = false;
    for (const char* lu : kLoginUrls) {
        resp.clear();
        code = 0;
        if (curl_request(lu, login_body.dump(), cookiejar_,
                         "X-CSRFToken: " + csrf_, resp, code) && code == 200) {
            net_log((std::string("establish_session: login OK via ") + lu).c_str());
            logged_in = true;
            break;
        }
        net_log((std::string("establish_session: login HTTP ") + std::to_string(code) +
                 " via " + lu + "  " + resp.substr(0, 200)).c_str());
    }
    if (!logged_in) {
        cookiejar_.clear();
        csrf_.clear();
        return false;
    }
    // The csrftoken cookie can rotate after login — re-read for subsequent writes.
    std::string c2 = read_cookie(cookiejar_, "csrftoken");
    if (!c2.empty()) csrf_ = c2;
    net_log("establish_session: REST session established");
    return true;
}

// With a login session, auth rides on the cookie (no extra header for reads, CSRF
// for writes). Without one, fall back to the socket JWT as a bearer token.
std::string OgsNet::rest_read_hdr() const {
    if (!cookiejar_.empty()) return "";
    return "Authorization: Bearer " + jwt_;
}

std::string OgsNet::rest_write_hdr() const {
    if (!cookiejar_.empty()) return "X-CSRFToken: " + csrf_;
    return "Authorization: Bearer " + jwt_;
}

bool OgsNet::do_auth() {
    // One path: log in with username/password (establish_session), then read the
    // socket JWT + player info out of ui/config. That single login yields both of the
    // things the app needs — the JWT authenticates the WebSocket, and the session
    // cookie it also leaves behind authorises the REST calls (challenges, friends).
    // The user never provides or sees a token.
    if (!establish_session()) return false;

    std::string resp;
    long code = 0;
    if (!curl_request("https://online-go.com/api/v1/ui/config/",
                      "", cookiejar_, "", resp, code) || code != 200)
        return false;
    try {
        auto cfg = json::parse(resp);
        jwt_          = cfg.at("user_jwt").get<std::string>();
        my_player_id  = cfg.at("user").at("id").get<int>();
        my_username   = cfg.at("user").at("username").get<std::string>();
        chat_auth_    = cfg.value("chat_auth", "");
    } catch (...) {
        return false;
    }
    return true;
}

// ── Socket.IO send helpers ────────────────────────────────────────────────────

void OgsNet::enqueue_raw(const std::string& raw) {
    {
        std::lock_guard<std::mutex> lock(outbound_mu_);
        outbound_.push(raw);
    }
    // lws_cancel_service is the only thread-safe way to wake the service loop
    // from outside the LWS thread. lws_callback_on_writable is NOT safe here.
    if (ctx_) lws_cancel_service(ctx_);
}

void OgsNet::enqueue_event(const std::string& name, const std::string& payload_json) {
    // 42["event_name", payload]
    std::string msg = "42[\"" + name + "\"," + payload_json + "]";
    enqueue_raw(msg);
}

void OgsNet::push_msg(NetMsg msg) {
    {
        std::lock_guard<std::mutex> lock(inbound_mu_);
        inbound_.push(std::move(msg));
    }
    // Wake the SDL main thread
    SDL_Event ev;
    SDL_memset(&ev, 0, sizeof(ev));
    ev.type = g_net_event_type;
    SDL_PushEvent(&ev);
}

// ── WebSocket callbacks ───────────────────────────────────────────────────────

void OgsNet::on_ws_open() {
    sio_connected_ = false;
    authenticated_ = false;
    recv_buf_.clear();
    // Socket.IO v4 / EIO4: auth is embedded in the namespace connect packet,
    // not sent as a separate "authenticate" event.  The server expects:
    //   40{"auth":{"token":"<JWT>"}}
    json connect_payload = {{"auth", {{"token", jwt_}}}};
    std::string msg = "40" + connect_payload.dump();
    enqueue_raw(msg);
}

void OgsNet::on_ws_data(const char* data, size_t len, bool final) {
    recv_buf_.append(data, len);
    if (!final) return;

    std::string msg = std::move(recv_buf_);
    recv_buf_.clear();
    dispatch_sio(msg);
}

void OgsNet::on_ws_close() {
    net_log("WebSocket closed");
    sio_connected_ = false;
    authenticated_ = false;
    wsi_ = nullptr;

    if (!stop_flag_) {
        NetMsg m;
        m.type = NetMsgType::DISCONNECTED;
        m.text = "Connection lost";
        push_msg(std::move(m));
    }
}

void OgsNet::do_write() {
    std::string msg;
    {
        std::lock_guard<std::mutex> lock(outbound_mu_);
        if (outbound_.empty()) return;
        msg = outbound_.front();
        outbound_.pop();
    }

    std::vector<unsigned char> buf(LWS_PRE + msg.size());
    memcpy(buf.data() + LWS_PRE, msg.data(), msg.size());
    int rc = lws_write(wsi_, buf.data() + LWS_PRE, msg.size(), LWS_WRITE_TEXT);
    if (rc < 0) net_log("lws_write error");

    // If more messages queued, request another writeable callback
    {
        std::lock_guard<std::mutex> lock(outbound_mu_);
        if (!outbound_.empty())
            lws_callback_on_writable(wsi_);
    }
}

// ── Socket.IO / EIO3 protocol dispatch ───────────────────────────────────────

void OgsNet::dispatch_sio(const std::string& msg) {
    if (msg.empty()) return;

    char eio = msg[0];

    // Engine.IO PING from server → respond with PONG
    if (eio == '2') {
        net_log("EIO ping → pong");
        enqueue_raw("3");
        return;
    }

    // Engine.IO OPEN — parse pingInterval; '40' + authenticate already queued from on_ws_open()
    if (eio == '0') {
        try {
            auto info = json::parse(msg.substr(1));
            ping_interval_ms_ = info.value("pingInterval", 25000);
            } catch (...) {}
        return;
    }

    // Engine.IO MESSAGE (Socket.IO packet)
    if (eio != '4' || msg.size() < 2) return;

    char sio = msg[1];

    // Log SIO type for anything we don't explicitly handle (helps catch server errors)
    if (sio != '0' && sio != '2' && sio != '4') {
        net_log(("SIO type=" + std::string(1, sio) + " msg=" + msg.substr(0, 120)).c_str());
    }

    // Socket.IO CONNECT acknowledged (SIOv4: payload contains {"sid":"..."})
    // Receiving this means auth was accepted.
    if (sio == '0') {
        sio_connected_ = true;
        if (!authenticated_) {
            authenticated_ = true;
            // Send OGS-level authenticate event (required even with EIO=4 JWT auth
            // to fully register the session — without it the server closes after ~3 min)
            if (!chat_auth_.empty()) {
                json auth = {
                    {"auth",      chat_auth_},
                    {"player_id", my_player_id},
                    {"username",  my_username},
                    {"jwt",       jwt_}
                };
                net_log(("sending authenticate, chat_auth len=" + std::to_string(chat_auth_.size())).c_str());
                enqueue_event("authenticate", auth.dump());
                enqueue_event("automatch/list", "{}");
            } else {
                net_log("WARNING: no chat_auth, skipping authenticate event");
            }
            NetMsg m;
            m.type = NetMsgType::AUTH_OK;
            push_msg(std::move(m));
        }
        return;
    }

    // Socket.IO NAMESPACE ERROR (SIOv4: auth rejected → "44{"message":"..."}")
    if (sio == '4') {
        net_log(("SIO namespace error (auth rejected?): " + msg).c_str());
        return;
    }

    // Socket.IO EVENT
    if (sio == '2') {
        // Remainder is a JSON array: ["event_name", payload]
        std::string body = msg.substr(2);
        try {
            auto arr = json::parse(body);
            if (!arr.is_array() || arr.size() < 1) return;
            std::string name = arr[0].get<std::string>();
            std::string payload = (arr.size() > 1) ? arr[1].dump() : "{}";
            // Log more for automatch events so we can see the full payload format
            size_t preview = (name.find("automatch") != std::string::npos) ? 2000
                           : (name.find("clock")     != std::string::npos) ? 600
                           : 80;
            net_log(("event: " + name + " " + payload.substr(0, preview)).c_str());
            on_event(name, payload);
        } catch (const std::exception& e) {
            net_log(("event parse error: " + std::string(e.what()) +
                     "  msg=" + body.substr(0, 80)).c_str());
        }
        return;
    }
}

// ── OGS event handlers ────────────────────────────────────────────────────────

struct ClockInfo { int secs = -1; int periods = -1; int period_secs = -1; bool in_byo = false; };

static ClockInfo extract_clock(const json& time_obj) {
    if (time_obj.is_null()) return {};
    ClockInfo c;
    // In byo-yomi OGS sends thinking_time = the full period duration (e.g. 30), not the
    // remaining time. period_time_left is the actual seconds left in the current period.
    // Prefer it so the countdown reflects reality rather than always starting from 30.
    bool has_ptl = time_obj.contains("period_time_left") && !time_obj["period_time_left"].is_null();
    if (has_ptl)
        c.secs = (int)time_obj["period_time_left"].get<double>();
    else if (time_obj.contains("thinking_time"))
        c.secs = (int)time_obj["thinking_time"].get<double>();
    else if (time_obj.contains("time_left"))
        c.secs = (int)time_obj["time_left"].get<double>();
    if (time_obj.contains("periods"))
        c.periods = time_obj["periods"].get<int>();
    if (time_obj.contains("period_time"))
        c.period_secs = (int)time_obj["period_time"].get<double>();
    // "In byo-yomi" must be decided here, at parse time: once reduced to plain
    // numbers, a period countdown is indistinguishable from low main time.
    c.in_byo = c.periods > 0 &&
               (has_ptl ||
                (time_obj.contains("thinking_time") &&
                 (int)time_obj["thinking_time"].get<double>() <= 0));
    return c;
}

void OgsNet::on_event(const std::string& name, const std::string& payload_json) {
    // ---------- authenticate response ----------
    if (name == "authenticate") {
        // Server echoes back player info confirming auth
        try {
            auto d = json::parse(payload_json);
            // Some OGS versions send {"id":...} here; others send nothing useful.
            // We already have my_player_id from the REST config, so just confirm.
            (void)d;
        } catch (...) {}
        authenticated_ = true;
        NetMsg m;
        m.type = NetMsgType::AUTH_OK;
        push_msg(std::move(m));
        return;
    }

    // ---------- automatch start ----------
    if (name == "automatch/start") {
        try {
            auto d = json::parse(payload_json);
            int gid = d.at("game_id").get<int>();
            active_game_id_ = gid;
            match_uuid_.clear();
            removed_stones_.clear();
            // Connect to the game immediately
            json conn = {
                {"game_id",   gid},
                {"player_id", my_player_id},
                {"chat",      false}
            };
            enqueue_event("game/connect", conn.dump());

            NetMsg m;
            m.type    = NetMsgType::MATCH_FOUND;
            m.game_id = gid;
            push_msg(std::move(m));
        } catch (const std::exception& e) {
            fprintf(stderr, "[ogs_net] automatch/start parse: %s\n", e.what());
        }
        return;
    }

    // ---------- active game (the user's ongoing games list) ----------
    if (name == "active_game") {
        handle_active_game(payload_json);
        return;
    }

    // ---------- gamedata ----------
    // Event name is "game/<id>/gamedata"
    if (name.size() > 9 && name.substr(name.size() - 9) == "/gamedata") {
        try {
            auto d = json::parse(payload_json);
            int gid = d.value("game_id", active_game_id_);

            NetMsg m;
            m.type    = NetMsgType::GAME_CONNECTED;
            m.game_id = gid;

            // Determine my colour
            auto& players = d.at("players");
            int black_id = players.at("black").at("id").get<int>();
            int white_id = players.at("white").at("id").get<int>();
            m.my_color      = (my_player_id == black_id) ? 1 : 0;
            m.my_player_id  = my_player_id;
            m.opp_player_id = (m.my_color == 1) ? white_id : black_id;

            m.black_name = players.at("black").value("username", "Black");
            m.white_name = players.at("white").value("username", "White");
            // OGS sends rank as a number (1=29k, 30=1k, 31=1d ...) or sometimes a string
            auto rank_str = [](const json& p) -> std::string {
                if (!p.contains("rank")) return "?";
                const auto& r = p["rank"];
                if (r.is_string()) return r.get<std::string>();
                if (r.is_number()) {
                    int rn = (int)r.get<double>();
                    if (rn <= 0) return "?";
                    if (rn < 31) return std::to_string(31 - rn) + "k";
                    return std::to_string(rn - 30) + "d";
                }
                return "?";
            };
            m.black_rank = rank_str(players.at("black"));
            m.white_rank = rank_str(players.at("white"));

            // Board size — OGS sends "width"/"height"; fall back to legacy "boardsize"
            if (d.contains("width") && d["width"].is_number_integer())
                m.board_size = d["width"].get<int>();
            else if (d.contains("boardsize") && d["boardsize"].is_number_integer())
                m.board_size = d["boardsize"].get<int>();
            else
                m.board_size = 19;
            net_log(("gamedata boardsize=" + std::to_string(m.board_size)).c_str());

            // Handicap handling
            m.handicap      = d.value("handicap", 0);
            m.free_handicap = d.value("free_handicap_placement", false);
            // With pre-placed handicap stones white moves first; otherwise black does
            m.initial_player = (m.handicap > 0 && !m.free_handicap) ? 0 : 1;

            // Parse pre-placed stones from initial_state (non-free handicap)
            if (!m.free_handicap && m.handicap > 0 && d.contains("initial_state")) {
                const auto& st = d["initial_state"];
                auto decode_pos = [&](const char* key) {
                    if (!st.contains(key) || !st[key].is_string()) return;
                    std::string s = st[key].get<std::string>();
                    for (size_t i = 0; i + 1 < s.size(); i += 2) {
                        int col = s[i] - 'a';
                        int row = s[i+1] - 'a';
                        m.initial_handicap_stones.push_back({col, row});
                    }
                };
                decode_pos("black");  // white handicap stones are rare but handle both
            }

            // Existing moves: array of [col, row] or integer-encoded moves
            if (d.contains("moves") && d["moves"].is_array()) {
                int boardsize = m.board_size;
                for (auto& mv : d["moves"]) {
                    if (mv.is_array() && mv.size() >= 2) {
                        int col = mv[0].get<int>();
                        int row = mv[1].get<int>();
                        m.initial_moves.push_back({col, row});
                    } else if (mv.is_number_integer()) {
                        int enc = mv.get<int>();
                        int col = enc % boardsize;
                        int row = enc / boardsize;
                        m.initial_moves.push_back({col, row});
                    }
                }
            }

            // Clocks — full extraction: this previously only took secs, so a
            // reconnect into a byo-yomi game arrived with periods=-1 (no byo state)
            // until the first standalone clock event happened to correct it.
            if (d.contains("clock") && d["clock"].is_object()) {
                auto& clk = d["clock"];
                if (clk.contains("black_time")) {
                    auto c = extract_clock(clk["black_time"]);
                    m.black_secs = c.secs; m.black_periods = c.periods;
                    m.black_period_secs = c.period_secs; m.black_in_byo = c.in_byo;
                }
                if (clk.contains("white_time")) {
                    auto c = extract_clock(clk["white_time"]);
                    m.white_secs = c.secs; m.white_periods = c.periods;
                    m.white_period_secs = c.period_secs; m.white_in_byo = c.in_byo;
                }
            }

            std::string phase = d.value("phase", "");
            net_log(("gamedata phase=" + (phase.empty() ? "(none)" : phase)).c_str());

            // Build a human-readable result string from outcome + winner
            if (d.contains("outcome") && !d["outcome"].get<std::string>().empty()) {
                std::string outcome = d["outcome"].get<std::string>();
                int winner_id = d.value("winner", 0);
                bool i_won = (winner_id == my_player_id);
                // outcome is e.g. "Resignation", "5.5", "Timeout", etc.
                game_result_ = (i_won ? "YOU WON by " : "YOU LOST by ") + outcome;
                net_log(("gamedata result=" + game_result_).c_str());
            }

            push_msg(std::move(m));

            // If the game is already finished (e.g. we reconnected after it ended)
            if (phase == "finished") {
                net_log("gamedata: phase=finished → firing GAME_OVER");
                NetMsg gover;
                gover.type    = NetMsgType::GAME_OVER;
                gover.game_id = gid;
                gover.text    = game_result_;
                push_msg(std::move(gover));
            }
        } catch (const std::exception& e) {
            fprintf(stderr, "[ogs_net] gamedata parse: %s\n", e.what());
        }
        return;
    }

    // ---------- live move ----------
    // Event name: "game/<id>/move"
    if (name.size() > 5 && name.substr(name.size() - 5) == "/move") {
        try {
            auto d = json::parse(payload_json);
            int gid = active_game_id_;
            // Try to extract game_id from event name "game/<id>/move"
            auto slash = name.find('/');
            if (slash != std::string::npos) {
                auto slash2 = name.find('/', slash + 1);
                if (slash2 != std::string::npos)
                    gid = std::stoi(name.substr(slash + 1, slash2 - slash - 1));
            }

            NetMsg m;
            m.type        = NetMsgType::OPPONENT_MOVE;
            m.game_id     = gid;
            m.move_number = d.value("move_number", 0);

            // Move can be [col,row] array or integer encoding
            int boardsize = 19;
            if (d.contains("move")) {
                auto& mv = d["move"];
                if (mv.is_array() && mv.size() >= 2) {
                    m.col = mv[0].is_null() ? -1 : mv[0].get<int>();
                    m.row = mv[1].is_null() ? -1 : mv[1].get<int>();
                } else if (mv.is_number_integer()) {
                    int enc = mv.get<int>();
                    if (enc >= boardsize * boardsize) {
                        m.col = m.row = -1;  // pass
                    } else {
                        m.col = enc % boardsize;
                        m.row = enc / boardsize;
                    }
                }
            }

            // Clocks bundled with the move — full extraction. This previously only
            // took secs while main.cpp's handler copies all clock fields whenever
            // secs >= 0, so every move event clobbered periods/period_secs back to
            // -1 and wiped the byo-yomi state until the next standalone clock event
            // (the "byo-yomi clock only sometimes red" bug).
            if (d.contains("black_time")) {
                auto c = extract_clock(d["black_time"]);
                m.black_secs = c.secs; m.black_periods = c.periods;
                m.black_period_secs = c.period_secs; m.black_in_byo = c.in_byo;
            }
            if (d.contains("white_time")) {
                auto c = extract_clock(d["white_time"]);
                m.white_secs = c.secs; m.white_periods = c.periods;
                m.white_period_secs = c.period_secs; m.white_in_byo = c.in_byo;
            }

            push_msg(std::move(m));
        } catch (const std::exception& e) {
            fprintf(stderr, "[ogs_net] move parse: %s\n", e.what());
        }
        return;
    }

    // ---------- clock update ----------
    if (name.size() > 6 && name.substr(name.size() - 6) == "/clock") {
        try {
            auto d = json::parse(payload_json);
            NetMsg m;
            m.type    = NetMsgType::CLOCK_UPDATE;
            m.game_id = active_game_id_;
            if (d.contains("black_time")) {
                auto c = extract_clock(d["black_time"]);
                m.black_secs = c.secs; m.black_periods = c.periods;
                m.black_period_secs = c.period_secs; m.black_in_byo = c.in_byo;
            }
            if (d.contains("white_time")) {
                auto c = extract_clock(d["white_time"]);
                m.white_secs = c.secs; m.white_periods = c.periods;
                m.white_period_secs = c.period_secs; m.white_in_byo = c.in_byo;
            }
            push_msg(std::move(m));
        } catch (...) {}
        return;
    }

    // ---------- stone removal ----------
    // Event name: "game/<id>/removed_stones"
    if (name.size() > 15 && name.substr(name.size() - 15) == "/removed_stones") {
        net_log(("removed_stones payload: " + payload_json.substr(0, 300)).c_str());
        try {
            auto d = json::parse(payload_json);
            // Store the removed-stone positions so we can echo them in the accept command.
            if (d.contains("all_removed") && d["all_removed"].is_string())
                removed_stones_ = d["all_removed"].get<std::string>();
            else
                removed_stones_ = "";
        } catch (...) {
            removed_stones_ = "";
        }
        net_log(("removed_stones stored: '" + removed_stones_ + "'").c_str());
        NetMsg m;
        m.type           = NetMsgType::STONE_REMOVAL;
        m.game_id        = active_game_id_;
        m.text           = removed_stones_;
        // Pass ownership array so the UI can shade territory
        try {
            auto d2 = json::parse(payload_json);
            if (d2.contains("ownership") && d2["ownership"].is_array())
                m.ownership_json = d2["ownership"].dump();
        } catch (...) {}
        push_msg(std::move(m));
        return;
    }

    // ---------- phase change ----------
    if (name.size() > 6 && name.substr(name.size() - 6) == "/phase") {
        try {
            auto d = json::parse(payload_json);
            std::string phase = d.is_string() ? d.get<std::string>() : d.value("phase", "");
            net_log(("/phase event: " + (phase.empty() ? "(empty)" : phase)).c_str());
            if (phase == "finished") {
                net_log("/phase=finished → firing GAME_OVER");
                NetMsg m;
                m.type    = NetMsgType::GAME_OVER;
                m.game_id = active_game_id_;
                m.text    = game_result_;
                push_msg(std::move(m));
            } else if (phase == "stone removal") {
                // Fire STONE_REMOVAL immediately — /removed_stones may not arrive (e.g. no dead stones)
                NetMsg m;
                m.type    = NetMsgType::STONE_REMOVAL;
                m.game_id = active_game_id_;
                m.text    = removed_stones_;  // empty until /removed_stones arrives
                push_msg(std::move(m));
            } else if (phase == "play") {
                // Opponent (or us) cancelled stone removal — return to playing
                NetMsg m;
                m.type    = NetMsgType::RESUME_PLAY;
                m.game_id = active_game_id_;
                push_msg(std::move(m));
            }
            // /removed_stones event will re-fire STONE_REMOVAL with actual dead stone data.
        } catch (...) {}
        return;
    }

    // ---------- undo requested ----------
    if (name.size() > 16 && name.substr(name.size() - 16) == "/undo_requested") {
        try {
            auto d = json::parse(payload_json);
            int move_num     = d.value("move_number",  0);
            int requested_by = d.value("requested_by", 0);
            if (requested_by != my_player_id && move_num > 0) {
                NetMsg m;
                m.type             = NetMsgType::UNDO_REQUESTED;
                m.game_id          = active_game_id_;
                m.undo_move_number = move_num;
                push_msg(std::move(m));
            }
        } catch (...) {}
        return;
    }
}

// ── libwebsockets callback ────────────────────────────────────────────────────

int OgsNet::lws_cb(struct lws* wsi, enum lws_callback_reasons reason, void* /*user*/, void* in, size_t len) {
    auto* self = reinterpret_cast<OgsNet*>(lws_context_user(lws_get_context(wsi)));
    if (!self) return 0;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        self->wsi_ = wsi;
        self->on_ws_open();
        break;

    case LWS_CALLBACK_CLIENT_RECEIVE:
        self->on_ws_data(reinterpret_cast<const char*>(in), len,
                         lws_is_final_fragment(wsi) != 0);
        break;

    case LWS_CALLBACK_CLIENT_WRITEABLE:
        self->do_write();
        break;

    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
        const char* err = in ? reinterpret_cast<const char*>(in) : "(null)";
        net_log(("WS connection error: " + std::string(err, err + (in ? strlen(err) : 0))).c_str());
        self->on_ws_close();
        break;
    }

    case LWS_CALLBACK_CLIENT_CLOSED:
        if (in && len >= 2) {
            auto* b = reinterpret_cast<const uint8_t*>(in);
            uint16_t code = (uint16_t)((b[0] << 8) | b[1]);
            std::string reason(len > 2 ? reinterpret_cast<const char*>(b + 2) : "", len > 2 ? len - 2 : 0);
            net_log(("WS close code=" + std::to_string(code) + " reason=" + reason).c_str());
        }
        self->on_ws_close();
        break;

    default:
        break;
    }
    return 0;
}

// ── Network thread main loop ──────────────────────────────────────────────────

void OgsNet::net_loop() {
    // Wrap entire loop so an unhandled exception causes a clean AUTH_FAIL
    // instead of std::terminate() crashing the process.
    try {
    // libcurl requires global init before any easy handle is used.
    curl_global_init(CURL_GLOBAL_DEFAULT);

    if (!do_auth()) {
        NetMsg m;
        m.type = NetMsgType::AUTH_FAIL;
        m.text = "Login failed — check username/password in config.ini";
        push_msg(std::move(m));
        curl_global_cleanup();
        return;
    }

    // --- Phase 2: Open WebSocket ---
    // Redirect LWS internal logs into our timestamped log file
    lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE, lws_log_to_file);

    static const struct lws_protocols protocols[] = {
        {
            "ogs-eio3",
            &OgsNet::lws_cb,
            0,       // per-session data size (we use context user data)
            65536,   // rx buffer
            0, nullptr, 0
        },
        LWS_PROTOCOL_LIST_TERM
    };

    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    info.port      = CONTEXT_PORT_NO_LISTEN;
    info.protocols = protocols;
    info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.user      = this;

    ctx_ = lws_create_context(&info);
    if (!ctx_) {
        net_log("lws_create_context failed");
        NetMsg m;
        m.type = NetMsgType::DISCONNECTED;
        m.text = "Failed to create WebSocket context";
        push_msg(std::move(m));
        curl_global_cleanup();
        return;
    }
    struct lws_client_connect_info conn;
    memset(&conn, 0, sizeof(conn));
    conn.context  = ctx_;
    conn.address  = "online-go.com";
    conn.port     = 443;
    conn.path     = "/socket.io/?EIO=4&transport=websocket";
    conn.host     = "online-go.com";
    conn.origin   = "https://online-go.com";
    conn.protocol = nullptr;  // no Sec-WebSocket-Protocol header; Socket.IO doesn't use one
    conn.ssl_connection = LCCSCF_USE_SSL;

    wsi_ = lws_client_connect_via_info(&conn);
    if (!wsi_) {
        net_log("lws_client_connect_via_info failed");
        lws_context_destroy(ctx_);
        ctx_ = nullptr;
        NetMsg m;
        m.type = NetMsgType::DISCONNECTED;
        m.text = "Failed to initiate WebSocket connection";
        push_msg(std::move(m));
        return;
    }

    // --- Phase 3: Service loop ---
    Uint32 last_ping_ms = 0;
    while (!stop_flag_) {
        int rc = lws_service(ctx_, 50);
        if (rc < 0) break;
        if (!wsi_) break;

        // If main thread enqueued a message and called lws_cancel_service to
        // wake us, request a writable callback now (safe: we're on the LWS thread).
        {
            std::lock_guard<std::mutex> lock(outbound_mu_);
            if (!outbound_.empty())
                lws_callback_on_writable(wsi_);
        }

        // Send net/ping every 10 seconds for OGS application-level keepalive.
        if (sio_connected_) {
            Uint32 now = SDL_GetTicks();
            if (now - last_ping_ms >= 10000) {
                last_ping_ms = now;
                json ping = {{"client", (long long)now}, {"drift", 0}, {"latency", 0}};
                enqueue_event("net/ping", ping.dump());
            }
        }
    }
    lws_context_destroy(ctx_);
    ctx_ = nullptr;
    wsi_ = nullptr;

    curl_global_cleanup();

    } catch (const std::exception& ex) {
        char buf[512];
        snprintf(buf, sizeof(buf), "net thread exception: %s", ex.what());
        net_log(buf);
        NetMsg m;
        m.type = NetMsgType::DISCONNECTED;
        m.text = ex.what();
        push_msg(std::move(m));
    } catch (...) {
        net_log("net thread unknown exception");
        NetMsg m;
        m.type = NetMsgType::DISCONNECTED;
        m.text = "Unknown error in network thread";
        push_msg(std::move(m));
    }
}

// ── Utilities ─────────────────────────────────────────────────────────────────

std::string OgsNet::make_uuid() {
    // RFC 4122 v4 UUID using Windows BCryptGenRandom for full 32-bit entropy
    std::random_device rd;
    unsigned int r[4] = { rd(), rd(), rd(), rd() };
    char buf[37];
    snprintf(buf, sizeof(buf),
             "%08x-%04x-4%03x-%04x-%08x%04x",
             r[0],
             (r[1] >> 16) & 0xFFFF,
             r[1] & 0x0FFF,
             ((r[2] >> 16) & 0x3FFF) | 0x8000,
             r[3],
             r[2] & 0xFFFF);
    return buf;
}

std::string OgsNet::fmt_clock(int secs) {
    if (secs < 0) return "--:--";
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d", secs / 60, secs % 60);
    return buf;
}
