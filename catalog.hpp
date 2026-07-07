#pragma once
#include "game_index.hpp"
#include <string>
#include <vector>

// CatalogEntry::type values:
//   0 = SGF file
//   1 = real sub-directory
//   2 = parent ".."
//   3 = virtual year directory  (name = year string e.g. "2024")
//   4 = [BY YEAR] meta-entry
//   5 = virtual player directory (name = player name)
//   6 = [BY DIRECTORY] meta-entry
//   7 = [BY PLAYER] meta-entry (directory root → virtual player browser)
struct CatalogEntry {
    std::string name;              // raw filename / directory name / year/player string
    std::string display_name;      // rendered label (fallback for non-player entries)
    bool        name_loaded = false;
    int         type = 0;
    std::string full_path;         // absolute path — set for search results and index-view files
    std::string player_black;      // PB name — if set, rendered as three yellow/white columns
    std::string player_white;      // PW name
    std::string player_black_rank; // BR rank string, e.g. "5k" — shown next to the name
    std::string player_white_rank; // WR rank string
    std::string date;              // DT date string (e.g. "2024-01-15") — shown as 4th column
    uint64_t    mtime = 0;         // file last-write time (OS ticks) — tiebreaker within same date
};

class Catalog {
public:
    bool active           = false;
    bool selection_made   = false;
    std::vector<CatalogEntry> entries;
    int  index            = 0;
    int  scroll           = 0;
    std::string base_dir;
    std::string current_subdir;
    std::string selected_path;

    // Sequential playback state
    std::string sequential_dir;
    int         sequential_index = 0;

    // Persistent game index (loaded once per session)
    GameIndex game_index;

    // Search state
    std::string search_query;
    bool        search_mode = false;

    // Virtual year browser state
    bool        virtual_year_mode  = false;
    std::string virtual_year;       // empty = showing year list; non-empty = games for that year
    bool        year_needs_refresh = false; // reload year list once index finishes

    // Virtual player browser state (default root view)
    bool        virtual_player_mode  = false;
    std::string virtual_player;       // empty = showing player list; non-empty = games for that player
    bool        player_needs_refresh = false; // reload player list once index finishes

    // Open the file browser rooted at games_dir.
    void open(const std::string& games_dir);
    // Close without selecting.
    void close();
    // Confirm the current index selection.
    void select();

    // Called every draw frame to handle deferred state transitions
    // (e.g. refresh player/year list once the background index finishes loading).
    void tick();

    // Search: append a character, remove last character, or clear entirely.
    void search_append(char c);
    void search_backspace();
    void search_clear();

    // Enumerate all .sgf files (recursively) under dir into out.
    static bool list_sgf_files(const std::string& dir, std::vector<std::string>& out);

    // Build a full path from two components (handles trailing separators).
    static std::string join_path(const std::string& dir, const std::string& name);

    // Unicode-safe file I/O: Windows' plain fopen()/FindFirstFileA() etc. go through
    // the process ANSI codepage, which can't represent most non-Latin filenames (e.g.
    // Chinese Fox Go Server exports) — silently mangling the path so the file can never
    // actually be opened. These route through the wide (UTF-16) Win32 APIs instead, with
    // paths kept as UTF-8 std::string everywhere else in the app. No-ops through to the
    // plain libc calls on non-Windows, where paths are already raw UTF-8 bytes.
    static std::wstring utf8_to_wide(const std::string& s);
    static std::string  wide_to_utf8(const std::wstring& w);
    static FILE*        fopen_utf8(const std::string& path, const char* mode);

    // Return the full filesystem path of the currently selected entry,
    // or empty string if the selection is a directory or parent link.
    std::string selected_entry_path() const;

    // Parse SGF player names for entries in [from, from+count) that haven't
    // been loaded yet.
    void ensure_names_loaded(int from, int count);

    bool load_entries();       // (re)populates entries from current_subdir on disk

private:
    void dir_up();
    void load_year_list();     // populates entries with virtual year dirs
    void load_year_games(const std::string& year); // populates entries with games for a year
    void load_player_list();   // populates entries with unique player names from index
    void load_player_games(const std::string& player); // populates entries with games for a player
    void apply_search();       // populates entries from search_query via game_index
    static bool list_recursive(const std::string& dir, const std::string& base,
                               std::vector<std::string>& out);
    static bool has_sgf_ext(const std::string& name);
};
