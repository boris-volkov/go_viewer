#pragma once
#include "ogs_client.hpp"
#include <libwebsockets.h>
#include <string>
#include <queue>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

class OgsNet {
public:
    OgsNet();
    ~OgsNet();

    // Called once from main thread. Spawns the network thread which logs in with the
    // username/password, then opens the Socket.IO connection. False if the thread
    // fails to start (a bad login surfaces later as an AUTH_FAIL message).
    bool start(const std::string& username, const std::string& password);
    void stop();

    // Thread-safe commands from the main thread.
    void cmd_find_match(const MatchPrefs& prefs = MatchPrefs{});
    void cmd_cancel_match();
    void cmd_send_move(int game_id, int col, int row);
    void cmd_send_pass(int game_id);
    void cmd_reconnect_game(int game_id);  // re-send game/connect to pull fresh gamedata (resync)

    // Correspondence: connect to an arbitrary game the way automatch auto-connects,
    // but for a game the user picked from their list — sets active_game_id_ so the
    // subsequent move/clock/phase events route to it.
    void cmd_open_game(int game_id);
    // Leave a game's live stream (game/disconnect). Clears active_game_id_ if it matched.
    void cmd_disconnect_game(int game_id);
    void cmd_send_resign(int game_id);
    void cmd_accept_stones(int game_id);
    void cmd_accept_undo(int game_id, int move_number);
    void cmd_reject_undo(int game_id, int move_number);

    // Poll one inbound message (returns false when queue is empty).
    bool poll_msg(NetMsg& out);

    // Download the completed game's SGF from the OGS API and write it to path.
    // Blocks — call from a background thread, not from the SDL event loop.
    void fetch_sgf(int game_id, const std::string& path);

    // Snapshot the user's ongoing games. These arrive over the socket as
    // "active_game" events (the server pushes one per game after authenticate and
    // whenever a game changes) and are cached in corr_games_ — the REST ui/overview
    // endpoint rejects the socket JWT with 403, so there is no HTTP call here. Cheap
    // (a locked copy of the cache); safe to call from the SDL main thread.
    std::vector<CorrGameSummary> corr_games_snapshot();

    // Challenges (authenticated REST via the session cookie). Each blocks on HTTP —
    // call from a background thread, not the SDL event loop.
    //   fetch_challenges   — GET me/challenges/, push CHALLENGES_UPDATED
    //   accept_challenge   — POST me/challenges/{id}/accept/, then re-fetch
    //   decline_challenge  — DELETE me/challenges/{id}/, then re-fetch
    void fetch_challenges();
    void accept_challenge(int challenge_id);
    void decline_challenge(int challenge_id);

    //   fetch_friends  — GET me/friends/, push FRIENDS_UPDATED (challenge opponents)
    //   send_challenge — POST players/{id}/challenge/, or challenges/ when
    //                    req.opponent_id is 0 (an open challenge anyone may accept)
    void fetch_friends();
    void send_challenge(const ChallengeRequest& req);

    // Read-only after AUTH_OK — safe to read from main thread without lock.
    std::string my_username;
    int         my_player_id = 0;

private:
    // Credentials
    std::string username_, password_;

    // JWT + player info, set by do_auth() before WebSocket opens.
    std::string jwt_;
    std::string chat_auth_;   // from /api/v1/ui/config — needed for authenticate event

    // Session cookie + CSRF for authenticated REST calls (challenges list/accept/
    // decline, friends). The socket JWT is refused (403) by those endpoints, so REST
    // uses a real login session like the website does. Set by establish_session().
    std::string cookiejar_;   // path to the Netscape cookie file (empty = no session)
    std::string csrf_;        // csrftoken cookie value, for X-CSRFToken on writes

    // libwebsockets context and connection
    lws_context* ctx_ = nullptr;
    lws*         wsi_ = nullptr;

    // Network thread
    std::thread       thread_;
    std::atomic<bool> stop_flag_{ false };

    // Inbound queue (net → main)
    std::queue<NetMsg> inbound_;
    std::mutex         inbound_mu_;

    // Outbound queue (main → net): raw Socket.IO strings ready to send.
    std::queue<std::string> outbound_;
    std::mutex              outbound_mu_;

    // Socket.IO / EIO3 state (only touched on network thread)
    bool  sio_connected_   = false;
    bool  authenticated_   = false;
    int   ping_interval_ms_= 25000;
    std::string recv_buf_; // accumulates WebSocket fragments

    // Active game
    int         active_game_id_   = 0;
    std::string removed_stones_;   // dead-stone list from server's stone removal event
    std::string game_result_;      // outcome string from last gamedata (e.g. "W+Resign")

    // The user's ongoing games, keyed by game id, built from active_game events on
    // the network thread and read via corr_games_snapshot() under corr_mu_.
    std::mutex                     corr_mu_;
    std::map<int, CorrGameSummary> corr_games_;

    // Automatch UUID (kept for cancellation)
    std::string match_uuid_;

    // ── Internal (called on network thread) ──────────────────────────────

    void net_loop();

    // Returns false if login or config fetch failed.
    bool do_auth();

    // CSRF → password login → persistent cookiejar_/csrf_ for authenticated REST.
    // Best-effort: needs username_/password_; returns false if either is missing or
    // the login fails (the socket JWT still works, REST writes just won't).
    bool establish_session();

    // Auth for a REST call: the login session (cookie, + CSRF on writes) when we have
    // one, otherwise the socket JWT as a bearer. Bearer support is endpoint-specific
    // (ui/config accepts it, ui/overview 403s), so every caller logs the HTTP code.
    std::string rest_read_hdr()  const;   // extra header for GET
    std::string rest_write_hdr() const;   // extra header for POST/DELETE

    // WebSocket callbacks (called from lws_callback_fn → dispatched to these)
    void on_ws_open();
    void on_ws_data(const char* data, size_t len, bool final);
    void on_ws_close();

    // Process a complete Socket.IO message string.
    void dispatch_sio(const std::string& msg);

    // Dispatch a decoded Socket.IO event.
    void on_event(const std::string& name, const std::string& payload_json);

    // Update corr_games_ from one active_game event, then push a CORR_LIST_UPDATED
    // snapshot so an open MY GAMES list reflects it live.
    void handle_active_game(const std::string& payload_json);
    void push_corr_list();

    // Enqueue a raw Socket.IO string for sending on the next writable callback.
    void enqueue_raw(const std::string& raw);

    // Enqueue a Socket.IO event: 42["name", payload_json]
    void enqueue_event(const std::string& name, const std::string& payload_json);

    // Push a message to the inbound queue and wake the SDL main thread.
    void push_msg(NetMsg msg);

    // Write the next queued outbound message via lws_write.
    // Called inside LWS_CALLBACK_CLIENT_WRITEABLE.
    void do_write();

    // libwebsockets global callback (C linkage trampoline)
    static int lws_cb(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len);

    // Generate a random UUID-shaped string for automatch.
    static std::string make_uuid();

    // Format seconds as M:SS.
    static std::string fmt_clock(int secs);
};
