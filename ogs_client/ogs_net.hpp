#pragma once
#include "ogs_client.hpp"
#include <libwebsockets.h>
#include <string>
#include <queue>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

class OgsNet {
public:
    OgsNet();
    ~OgsNet();

    // Called once from main thread. Spawns the network thread which authenticates
    // then opens the Socket.IO connection. Returns false if thread fails to start.
    bool start(const std::string& username, const std::string& password,
               const std::string& jwt_override = "");
    void stop();

    // Thread-safe commands from the main thread.
    void cmd_find_match(const MatchPrefs& prefs = MatchPrefs{});
    void cmd_cancel_match();
    void cmd_send_move(int game_id, int col, int row);
    void cmd_send_pass(int game_id);
    void cmd_send_resign(int game_id);
    void cmd_accept_stones(int game_id);
    void cmd_accept_undo(int game_id, int move_number);
    void cmd_reject_undo(int game_id, int move_number);

    // Poll one inbound message (returns false when queue is empty).
    bool poll_msg(NetMsg& out);

    // Download the completed game's SGF from the OGS API and write it to path.
    // Blocks — call from a background thread, not from the SDL event loop.
    void fetch_sgf(int game_id, const std::string& path);

    // Read-only after AUTH_OK — safe to read from main thread without lock.
    std::string my_username;
    int         my_player_id = 0;

private:
    // Credentials
    std::string username_, password_;

    // JWT + player info, set by do_auth() before WebSocket opens.
    std::string jwt_;
    std::string chat_auth_;   // from /api/v1/ui/config — needed for authenticate event

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

    // Automatch UUID (kept for cancellation)
    std::string match_uuid_;

    // ── Internal (called on network thread) ──────────────────────────────

    void net_loop();

    // Returns false if login or config fetch failed.
    bool do_auth();

    // WebSocket callbacks (called from lws_callback_fn → dispatched to these)
    void on_ws_open();
    void on_ws_data(const char* data, size_t len, bool final);
    void on_ws_close();

    // Process a complete Socket.IO message string.
    void dispatch_sio(const std::string& msg);

    // Dispatch a decoded Socket.IO event.
    void on_event(const std::string& name, const std::string& payload_json);

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
