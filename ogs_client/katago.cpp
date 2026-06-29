#include "katago.hpp"
#include "json.hpp"
#include <SDL2/SDL.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <string>
#include <vector>

using json = nlohmann::json;
extern Uint32 g_net_event_type;

// ── Logging ───────────────────────────────────────────────────────────────────
static FILE* g_kata_log = nullptr;
static void kata_log(const char* msg) {
    if (!g_kata_log) g_kata_log = fopen("kata.log", "w");
    time_t t = time(nullptr);
    struct tm* tm_info = localtime(&t);
    char ts[24];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm_info);
    if (g_kata_log) { fprintf(g_kata_log, "[%s] %s\n", ts, msg); fflush(g_kata_log); }
    fprintf(stderr, "[kata] [%s] %s\n", ts, msg);
}
static void kata_logf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    kata_log(buf);
}

// ── Coordinate helpers ────────────────────────────────────────────────────────

// Our (row, col) → KataGo GTP string.
// Our row=0 is the TOP of the board; KataGo GTP row 1 is the BOTTOM.
// Column letters skip 'I': A=0, B=1, ..., H=7, J=8, K=9, ...
static std::string to_gtp(int row, int col, int board_size) {
    char col_c = (col < 8) ? char('A' + col) : char('A' + col + 1);
    return std::string(1, col_c) + std::to_string(board_size - row);
}

// KataGo GTP string → our (row, col). Returns false for "pass" or invalid input.
static bool from_gtp(const std::string& gtp, int board_size, int& row_out, int& col_out) {
    if (gtp.size() < 2) return false;
    char letter = (char)toupper((unsigned char)gtp[0]);
    if (letter == 'I') return false;
    int col = (letter >= 'J') ? (letter - 'A' - 1) : (letter - 'A');
    if (col < 0 || col >= board_size) return false;
    int gtp_row = std::stoi(gtp.substr(1));
    int row = board_size - gtp_row;
    if (row < 0 || row >= board_size) return false;
    row_out = row;
    col_out = col;
    return true;
}

// KataGo ownership array indexing: ownership[y * boardXSize + x]
// where x=0 is left and y=0 is the BOTTOM of the board.
// Maps to our (row, col) where row=0 is TOP:
//   our (r, c) = kata (x=c, y=board_size-1-r)
//   kata index  = (board_size-1-r) * board_size + c
static inline int kata_idx(int r, int c, int bs) {
    return (bs - 1 - r) * bs + c;
}

// ── Process management ────────────────────────────────────────────────────────

bool KatagoProc::start(const std::string& exe, const std::string& model, const std::string& cfg) {
    if (running_.load()) return true;

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength        = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE h_stdin_r = NULL, h_stdout_w = NULL;
    if (!CreatePipe(&h_stdin_r, &h_write_, &sa, 0))        return false;
    if (!CreatePipe(&h_read_,   &h_stdout_w, &sa, 0)) {
        CloseHandle(h_stdin_r); CloseHandle(h_write_); h_write_ = NULL;
        return false;
    }
    // Make our (parent) ends non-inheritable
    SetHandleInformation(h_write_, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(h_read_,  HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "\"" + exe + "\" analysis"
                    + " -model \"" + model + "\""
                    + " -config \"" + cfg + "\"";
    kata_logf("starting: %s", cmd.c_str());
    std::vector<char> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back('\0');

    // Run KataGo from its own directory so logDir and other relative paths resolve correctly.
    std::string kata_dir = exe;
    auto sep = kata_dir.find_last_of("/\\");
    if (sep != std::string::npos) kata_dir.resize(sep);
    const char* kata_cwd = kata_dir.empty() ? nullptr : kata_dir.c_str();
    kata_logf("working directory: %s", kata_cwd ? kata_cwd : "(inherited)");

    STARTUPINFOA si{};
    si.cb         = sizeof(si);
    si.dwFlags    = STARTF_USESTDHANDLES;
    si.hStdInput  = h_stdin_r;
    si.hStdOutput = h_stdout_w;
    si.hStdError  = h_stdout_w;   // merge stderr so we see errors too

    BOOL ok = CreateProcessA(nullptr, cmd_buf.data(),
                             nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, kata_cwd,
                             &si, &proc_);
    CloseHandle(h_stdin_r);
    CloseHandle(h_stdout_w);
    if (!ok) {
        kata_logf("CreateProcess FAILED (error %lu)", GetLastError());
        CloseHandle(h_write_); CloseHandle(h_read_); h_write_ = h_read_ = NULL;
        return false;
    }
#endif

    kata_log("process created — reader thread starting");
    running_.store(true);
    reader_ = std::thread([this]() { reader_loop(); });

    // Send a minimal 1-visit ping so we can measure how long init takes.
    // The ping response will be logged as "moves response id=0 ...".
    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_query_id_    = 0;
        pending_response_id_ = 0;
        pending_query_bs_    = 1;
    }
    json ping = {
        {"id",          "0"},
        {"moves",       json::array()},
        {"initialStones", json::array()},
        {"rules",       "chinese"},
        {"komi",        7.5},
        {"boardXSize",  2},
        {"boardYSize",  2},
        {"maxVisits",   1},
        {"analyzeTurns", {0}}
    };
    kata_log("startup ping sent — waiting for first response to confirm KataGo is ready");
    write_line(ping.dump());
    return true;
}

void KatagoProc::stop() {
    if (!running_.load() && !reader_.joinable()) return;
    running_.store(false);
#ifdef _WIN32
    if (h_write_) { CloseHandle(h_write_); h_write_ = NULL; }  // EOF → katago exits
    if (reader_.joinable()) reader_.join();
    if (h_read_)  { CloseHandle(h_read_);  h_read_  = NULL; }
    if (proc_.hProcess) { TerminateProcess(proc_.hProcess, 0);
                          CloseHandle(proc_.hProcess); proc_.hProcess = NULL; }
    if (proc_.hThread)  { CloseHandle(proc_.hThread);  proc_.hThread  = NULL; }
#endif
}

void KatagoProc::write_line(const std::string& s) {
#ifdef _WIN32
    if (!h_write_) return;
    std::string line = s + "\n";
    DWORD written = 0;
    WriteFile(h_write_, line.c_str(), DWORD(line.size()), &written, nullptr);
#endif
}

// ── Query ─────────────────────────────────────────────────────────────────────

void KatagoProc::query_ownership(const char  board[][MAX_BOARD_SIZE], int board_size,
                                  const bool  dead[][MAX_BOARD_SIZE],  float komi) {
    if (!running_.load()) { kata_log("query_ownership: process not running — skipped"); return; }

    // Build initialStones: all living stones on the board
    json stones = json::array();
    for (int r = 0; r < board_size; r++) {
        for (int f = 0; f < board_size; f++) {
            if (board[r][f] == 0) continue;
            if (dead && dead[r][f])  continue;   // dead stone — exclude
            stones.push_back({ (board[r][f] == 1) ? "B" : "W", to_gtp(r, f, board_size) });
        }
    }

    int qid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        result_ready_        = false;
        moves_ready_         = false;
        qid                  = ++pending_query_id_;
        pending_response_id_ = qid;
        pending_query_bs_    = board_size;
    }

    json q = {
        {"id",             std::to_string(qid)},
        {"moves",          json::array()},
        {"initialStones",  stones},
        {"rules",          "chinese"},
        {"komi",           komi},
        {"boardXSize",     board_size},
        {"boardYSize",     board_size},
        {"includeOwnership", true},
        {"maxVisits",      1},
        {"analyzeTurns",   {0}}
    };
    kata_logf("ownership query id=%d bs=%d stones=%d", qid, board_size, (int)stones.size());
    write_line(q.dump());
}

// ── Move-suggestion query ─────────────────────────────────────────────────────

void KatagoProc::query_moves(const char board[][MAX_BOARD_SIZE], int board_size,
                              bool black_to_play, float komi, int max_visits) {
    if (!running_.load()) { kata_log("query_moves: process not running — skipped"); return; }

    json stones = json::array();
    for (int r = 0; r < board_size; r++)
        for (int c = 0; c < board_size; c++) {
            if (board[r][c] == 0) continue;
            stones.push_back({ board[r][c] == 1 ? "B" : "W", to_gtp(r, c, board_size) });
        }

    int qid;
    {
        std::lock_guard<std::mutex> lk(mu_);
        result_ready_        = false;
        moves_ready_         = false;
        qid                  = ++pending_query_id_;
        pending_response_id_ = qid;
        pending_query_bs_    = board_size;
    }

    json q = {
        {"id",             std::to_string(qid)},
        {"moves",          json::array()},
        {"initialStones",  stones},
        {"initialPlayer",  black_to_play ? "B" : "W"},
        {"rules",          "chinese"},
        {"komi",           komi},
        {"boardXSize",     board_size},
        {"boardYSize",     board_size},
        {"maxVisits",      max_visits},
        {"analyzeTurns",   {0}},
        {"includePV",      true}
    };
    kata_logf("moves query id=%d bs=%d %s stones=%d visits=%d",
              qid, board_size, black_to_play ? "B" : "W", (int)stones.size(), max_visits);
    write_line(q.dump());
}

// ── Result polling ────────────────────────────────────────────────────────────

bool KatagoProc::poll_ownership(int out[][MAX_BOARD_SIZE], int& board_size_out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!result_ready_) return false;
    result_ready_ = false;
    board_size_out = result_bs_;
    memcpy(out, result_own_, sizeof(result_own_));
    return true;
}

bool KatagoProc::poll_moves(MoveSuggestion out[], int& count_out, float& score_lead_out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!moves_ready_) return false;
    moves_ready_ = false;
    count_out = moves_count_;
    score_lead_out = moves_score_lead_;
    memcpy(out, moves_result_, moves_count_ * sizeof(MoveSuggestion));
    return true;
}

// ── Reader thread ─────────────────────────────────────────────────────────────

void KatagoProc::reader_loop() {
    std::string buf;
    char tmp[8192];

    kata_log("reader thread running");
    while (running_.load()) {
#ifdef _WIN32
        DWORD avail = 0;
        if (!PeekNamedPipe(h_read_, nullptr, 0, nullptr, &avail, nullptr)) {
            DWORD peek_err = GetLastError();
            DWORD exit_code = 0;
            GetExitCodeProcess(proc_.hProcess, &exit_code);
            kata_logf("reader: PeekNamedPipe failed (error %lu) exit_code=%lu — process crashed",
                      peek_err, exit_code);
            break;
        }
        if (avail == 0) { Sleep(10); continue; }
        DWORD nread = 0;
        if (!ReadFile(h_read_, tmp, sizeof(tmp) - 1, &nread, nullptr) || nread == 0) {
            kata_log("reader: ReadFile returned 0 — pipe closed");
            break;
        }
        tmp[nread] = '\0';
        buf.append(tmp, nread);
#endif

        // Process all complete newline-terminated lines
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (line.empty()) continue;
            if (line[0] != '{') {
                // KataGo startup / error output (not JSON) — log it verbatim
                kata_logf("katago: %s", line.c_str());
                continue;
            }
            try {
                auto d = json::parse(line);

                // Match query id to avoid stale results
                int resp_id = 0;
                if (d.contains("id") && d["id"].is_string())
                    resp_id = std::stoi(d["id"].get<std::string>());

                bool do_push = false;

                // ── Ownership response (stone removal phase) ──────────────
                if (d.contains("ownership") && d["ownership"].is_array()) {
                    auto& ow = d["ownership"];
                    int n = int(ow.size());
                    int bs = 0;
                    for (int s = 2; s <= MAX_BOARD_SIZE; s++) if (s * s == n) { bs = s; break; }
                    if (bs > 0) {
                        std::lock_guard<std::mutex> lk(mu_);
                        if (resp_id == pending_response_id_) {
                            result_bs_ = bs;
                            memset(result_own_, 0, sizeof(result_own_));
                            for (int r = 0; r < bs; r++)
                                for (int c = 0; c < bs; c++) {
                                    float v = ow[kata_idx(r, c, bs)].get<float>();
                                    result_own_[r][c] = (v > 0.5f) ? 1 : (v < -0.5f) ? -1 : 0;
                                }
                            result_ready_ = true;
                            do_push = true;
                            kata_logf("ownership response id=%d bs=%d OK", resp_id, bs);
                        } else {
                            kata_logf("ownership response id=%d stale (want %d) — discarded",
                                      resp_id, pending_response_id_);
                        }
                    } else {
                        kata_logf("ownership response id=%d: unrecognised array size %d", resp_id, n);
                    }
                }

                // ── Move-suggestion response (history review) ─────────────
                if (d.contains("moveInfos") && d["moveInfos"].is_array()) {
                    auto& mi = d["moveInfos"];
                    int bs = 0;
                    MoveSuggestion tmp[MAX_SUGGESTIONS];
                    int count = 0;
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        if (resp_id == pending_response_id_) {
                            bs = pending_query_bs_;
                            // rootInfo.scoreLead is from Black's absolute perspective
                            // (positive = Black leading, negative = White leading).
                            float sl = FLT_MAX;
                            if (d.contains("rootInfo") && d["rootInfo"].is_object()) {
                                auto& ri = d["rootInfo"];
                                if (ri.contains("scoreLead") && ri["scoreLead"].is_number())
                                    sl = ri["scoreLead"].get<float>();
                            }
                            moves_score_lead_ = sl;
                            for (auto& info : mi) {
                                if (count >= MAX_SUGGESTIONS) break;
                                if (!info.contains("move") || !info["move"].is_string()) continue;
                                int row, col;
                                if (!from_gtp(info["move"].get<std::string>(), bs, row, col)) continue;
                                MoveSuggestion s{};
                                s.row     = row;
                                s.col     = col;
                                s.winrate    = info.value("winrate",    0.5f);
                                s.score_lead = info.value("scoreLead",  0.f);
                                s.order      = info.value("order",      count);
                                // PV: KataGo includes the move itself as pv[0]; skip it, store continuations
                                if (info.contains("pv") && info["pv"].is_array()) {
                                    auto& pv = info["pv"];
                                    int pc = 0;
                                    bool skip_first = true;
                                    for (auto& mv : pv) {
                                        if (skip_first) { skip_first = false; continue; }
                                        if (pc >= MAX_PV || !mv.is_string()) break;
                                        int pr, pf;
                                        if (from_gtp(mv.get<std::string>(), bs, pr, pf)) {
                                            s.pv_row[pc] = pr;
                                            s.pv_col[pc] = pf;
                                            pc++;
                                        }
                                    }
                                    s.pv_count = pc;
                                }
                                tmp[count++] = s;
                            }
                            memcpy(moves_result_, tmp, count * sizeof(MoveSuggestion));
                            moves_count_ = count;
                            moves_ready_ = true;
                            do_push = true;
                            kata_logf("moves response id=%d bs=%d count=%d (raw=%d)",
                                      resp_id, bs, count, (int)mi.size());
                        } else {
                            kata_logf("moves response id=%d stale (want %d) — discarded",
                                      resp_id, pending_response_id_);
                        }
                    }
                }

                // KataGo sends {"id":..., "error":"..."} for bad queries
                if (d.contains("error") && d["error"].is_string())
                    kata_logf("katago ERROR id=%d: %s", resp_id,
                              d["error"].get<std::string>().c_str());

                if (do_push) {
                    SDL_Event ev{};
                    ev.type = g_net_event_type;
                    SDL_PushEvent(&ev);
                }

            } catch (const std::exception& e) {
                kata_logf("JSON parse error: %s  line=%.80s", e.what(), line.c_str());
            } catch (...) {
                kata_logf("unknown exception parsing JSON line=%.80s", line.c_str());
            }
        }
    }
    running_.store(false);   // process died or stop() was called — no more queries
    kata_log("reader thread exited");
}
