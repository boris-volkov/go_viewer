#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One entry in the persistent game database.
struct GameIndexEntry {
    std::string rel_path;   // path relative to base_dir, native separators
    std::string black;      // PB[] player name
    std::string white;      // PW[] player name
    std::string date;       // DT[] date string
    std::string result;     // RE[] result string
};

// Flat-file persistent index of all SGF files under a directory tree.
// Index file: <base_dir>/.go_viewer_index
// Format: header line "go_viewer_index_v1\t<count>", then one
//         tab-separated line per game: rel_path\tblack\twhite\tdate\tresult
//
// Loading is non-blocking: load_async() spawns a background thread that
// either reads the cached index file or rebuilds it from scratch.
// Call loaded() to check completion; find() and search() are safe to call
// at any time (they return empty results while loading is in progress).
class GameIndex {
public:
    ~GameIndex();

    // Start loading (or rebuilding) the index in a background thread.
    // Safe to call multiple times — only starts the thread once.
    void load_async(const std::string& base_dir);

    // Insert or update a single entry in the in-memory index and rewrite the
    // index file.  No-op if the index hasn't finished loading yet (the next
    // full rebuild will pick up the new file anyway).
    void insert_entry(const GameIndexEntry& e);

    // True once the background load has finished.
    bool loaded()    const { return loaded_.load(); }
    // True while the background thread is still running.
    bool is_loading() const { return loading_.load(); }

    // Look up an entry by its relative path (case-sensitive, native seps).
    // Returns nullptr if not found or not yet loaded.
    const GameIndexEntry* find(const std::string& rel_path) const;

    // Case-insensitive substring search across all fields.
    // Returns pointers valid until the next load_async() call.
    std::vector<const GameIndexEntry*> search(const std::string& query) const;

    // Return all entries (only valid after loaded() is true).
    // Pointers are stable for the lifetime of this GameIndex.
    std::vector<const GameIndexEntry*> get_all() const;

    // Extract a 4-digit year string (e.g. "2024") from a DT[] date value.
    // Returns empty string if no recognisable year is found.
    static std::string extract_year(const std::string& date);

    int count() const;

private:
    mutable std::mutex          mutex_;
    std::vector<GameIndexEntry> entries_;   // protected by mutex_; kept sorted by rel_path
    std::atomic<bool>           loaded_{false};
    std::atomic<bool>           loading_{false};
    std::thread                 thread_;
    std::string                 base_dir_;  // set by load_async before thread starts

    // Thread body — called with a copy of base_dir
    void do_load(std::string base_dir);

    static std::string index_file_path(const std::string& base_dir);
    static bool read_index(const std::string& path,
                           std::vector<GameIndexEntry>& out, int& stored_count);
    static bool write_index(const std::string& path,
                            const std::vector<GameIndexEntry>& entries);
    static bool scan_sgf_header(const std::string& full_path,
                                std::string& black, std::string& white,
                                std::string& date,  std::string& result);
    static bool list_sgf_recursive(const std::string& dir, const std::string& base,
                                   std::vector<std::string>& rel_paths);
    static int  count_sgf_files(const std::string& base_dir);
};
