#pragma once
#include <string>
#include <vector>
#include <utility>

// ── Inbound: network thread → main thread ──────────────────────────────────

enum class NetMsgType {
    AUTH_OK,         // authenticated; player info set in OgsNet
    AUTH_FAIL,       // login or socket auth failed; check msg.text for reason
    MATCH_FOUND,     // automatch start; game_id valid; we auto-send game/connect
    GAME_CONNECTED,  // gamedata received; board state, names, clocks populated
    OPPONENT_MOVE,   // opponent played; col/row valid (-1/-1 = pass)
    CLOCK_UPDATE,    // black_secs / white_secs updated
    STONE_REMOVAL,   // game entered stone removal phase; text holds server's dead-stone payload
    GAME_OVER,       // phase changed to "finished"; text holds the result string
    UNDO_REQUESTED,  // opponent requested undo; undo_move_number holds the move to undo
    RESUME_PLAY,     // stone removal cancelled; game returned to play phase
    DISCONNECTED,    // connection lost unexpectedly
    ACCEPT_STATUS,   // stone removal: one or both players accepted; check my_accepted/opp_accepted
};

struct NetMsg {
    NetMsgType type = NetMsgType::DISCONNECTED;

    // MATCH_FOUND / GAME_CONNECTED / OPPONENT_MOVE / CLOCK_UPDATE / GAME_OVER
    int game_id = 0;

    // GAME_CONNECTED
    int board_size      = 19;
    int my_color        = 1;    // 1 = black, 0 = white
    int my_player_id    = 0;
    int opp_player_id   = 0;
    std::string black_name, white_name;
    std::string black_rank, white_rank;
    // Handicap info
    int   handicap      = 0;    // number of handicap stones
    bool  free_handicap = false; // true → first `handicap` move events are all black placements
    int  initial_player = 1;    // who plays the first real move: 1=black, 0=white
    // Pre-placed stones from initial_state (non-free handicap only); always black
    std::vector<std::pair<int,int>> initial_handicap_stones;
    // moves already played (reconnect / non-fresh game); [col,row] 0-indexed
    std::vector<std::pair<int,int>> initial_moves;

    // OPPONENT_MOVE
    int col         = -1;   // -1 = pass
    int row         = -1;
    int move_number = 0;

    // CLOCK_UPDATE / GAME_CONNECTED
    int black_secs        = -1;  // thinking_time (main time or current byo-yomi period)
    int white_secs        = -1;
    int black_periods     = -1;  // byo-yomi periods remaining (-1 = not byo-yomi)
    int white_periods     = -1;
    int black_period_secs = -1;  // seconds per byo-yomi period
    int white_period_secs = -1;

    // UNDO_REQUESTED: move number the opponent wants to undo
    int undo_move_number = 0;

    // GAME_OVER / AUTH_FAIL / DISCONNECTED
    std::string text;

    // STONE_REMOVAL: raw JSON ownership array (2D, [row][col]: 1=black, -1=white, 0=neutral)
    std::string ownership_json;

    // ACCEPT_STATUS: -1=unknown, 0=not accepted, 1=accepted
    int my_accepted  = -1;
    int opp_accepted = -1;
};

// ── Match preferences (used to build automatch payload) ───────────────────

struct MatchPrefs {
    // Board sizes — index 0=9x9, 1=13x13, 2=19x19
    bool sizes[3]  = {true, false, false};
    // Speeds    — index 0=blitz, 1=live, 2=rapid  (OGS mode only)
    bool speeds[3] = {false, true, false};
    // Local play vs KataGo human SL model
    bool katago_mode = false;
    int  katago_str  = 2;   // index into strength table (0=20k … 6=5d)
};

// ── Outbound: main thread → network thread ─────────────────────────────────

enum class CmdType {
    FIND_MATCH,
    CANCEL_MATCH,
    SEND_MOVE,    // col + row set
    SEND_PASS,
    SEND_RESIGN,
};

struct NetCmd {
    CmdType type    = CmdType::FIND_MATCH;
    int     game_id = 0;
    int     col     = 0;
    int     row     = 0;
};
