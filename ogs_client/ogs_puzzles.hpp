#pragma once
#include <string>
#include <vector>
#include <memory>

// ── OGS puzzle REST API (read-only, public — no auth required) ────────────────
//
// Endpoints (verified live 2026-07):
//   GET /api/v1/puzzles/collections/?page=N&page_size=N   → paginated collections
//   GET /api/v1/puzzles/{id}                              → full puzzle + solution tree
//   GET /api/v1/puzzles/{id}/collection_summary           → ordered ids in same collection
//
// All fetchers are blocking (libcurl) — call from a worker thread, not the UI loop.

// A board annotation the author placed at this node — letter labels ("A", "X",
// "O"…) or shape marks reduced to a display character.
struct PuzzleMark {
    int  x = -1, y = -1;
    char ch = '?';
};

// One node of the authored solution tree. Root is the initial position (x=y=-1);
// each child is a move. correct/wrong flags mark judged endpoints; text carries
// the author's comment for that node (often "why this fails" on wrong branches).
struct PuzzleMoveNode {
    int  x = -1, y = -1;
    bool correct = false;
    bool wrong   = false;
    std::string text;
    std::vector<PuzzleMark>     marks;   // annotations to show while at this node
    std::vector<PuzzleMoveNode> branches;
};

struct OgsPuzzle {
    int         id     = 0;
    std::string name;
    std::string description;
    std::string type;            // "life_and_death", "tsumego", …
    int         width  = 19, height = 19;
    int         rank   = 0;      // author-assigned difficulty (0 = unrated)
    // Concatenated 2-char SGF coordinates ("dadbdc…"), same encoding as AB[]/AW[]
    std::string initial_black;
    std::string initial_white;
    bool        black_to_play = true;
    bool        opponent_auto = true;   // opponent replies play automatically from the tree
    int         collection_id = 0;
    std::string collection_name;
    PuzzleMoveNode tree;                // root: x=y=-1, branches = first player moves
};

struct OgsPuzzleCollection {
    int         id = 0;
    std::string name;
    std::string owner;
    int         puzzle_count = 0;
    int         min_rank = 0, max_rank = 0;
    float       rating = 0.f;
    int         rating_count = 0;
    int         starting_puzzle_id = 0;
};

// Fetch one page of public collections. total_out receives the total collection
// count (for "page X of Y"). Returns false on network/parse failure.
bool ogs_fetch_puzzle_collections(int page, int page_size,
                                  std::vector<OgsPuzzleCollection>& out,
                                  int& total_out);

// Fetch a single puzzle with its solution tree.
bool ogs_fetch_puzzle(int puzzle_id, OgsPuzzle& out);

// Ordered {id, name} of every puzzle in the same collection as puzzle_id —
// drives next/previous navigation while solving.
bool ogs_fetch_collection_puzzles(int puzzle_id,
                                  std::vector<std::pair<int, std::string>>& out);
