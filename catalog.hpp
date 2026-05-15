#pragma once
#include <string>
#include <vector>

struct CatalogEntry {
    std::string name;
    int type = 0; // 0 = SGF file, 1 = sub-directory, 2 = parent ".."
};

class Catalog {
public:
    bool active           = false;
    bool selection_made   = false;
    std::vector<CatalogEntry> entries;
    int  index            = 0;
    int  scroll           = 0;
    std::string base_dir;       // root passed to open()
    std::string current_subdir; // relative path inside base_dir
    std::string selected_path;  // full path of the selected game file

    // Sequential playback state
    std::string sequential_dir; // empty => random mode
    int         sequential_index = 0;

    // Open the file browser rooted at games_dir.
    void open(const std::string& games_dir);
    // Close without selecting.
    void close();
    // Confirm the current index selection.
    // If a file is selected, sets selected_path and selection_made.
    void select();

    // Enumerate all .sgf files (recursively) under dir into out.
    // Returns false on error.
    static bool list_sgf_files(const std::string& dir, std::vector<std::string>& out);

    // Build a full path from two components (handles trailing separators).
    static std::string join_path(const std::string& dir, const std::string& name);

    // Return the full filesystem path of the currently selected entry,
    // or empty string if the selection is a directory or parent link.
    std::string selected_entry_path() const;

private:
    void dir_up();
    bool load_entries();
    static bool list_recursive(const std::string& dir, const std::string& base,
                               std::vector<std::string>& out);
    static bool has_sgf_ext(const std::string& name);
};
