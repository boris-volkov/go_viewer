#include "catalog.hpp"
#include "go_viewer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

// ---------------------------------------------------------------------------
// Path helpers

std::string Catalog::join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '/' || last == '\\')
        return dir + name;
    return dir + PATH_SEP_STR + name;
}

bool Catalog::has_sgf_ext(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".sgf";
}

// ---------------------------------------------------------------------------
// SGF name extraction

// Read the first chunk of an SGF file and extract PB[] and PW[].
// Reads up to 4KB which is enough to cover even heavily-commented headers,
// and handles both multi-line and single-line SGF formats.
static bool sgf_player_names(const std::string& path,
                              std::string& black_out,
                              std::string& white_out) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return false;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (n == 0) return false;
    buf[n] = '\0';

    // Find value for a tag like "PB[" or "pb[", ignoring case for the tag.
    // Stops at the first unescaped ']'.
    auto extract = [&](const char* tag_upper, const char* tag_lower,
                       std::string& dst) {
        if (!dst.empty()) return;
        const char* p = buf;
        while (*p) {
            // Try both cases
            const char* found = nullptr;
            const char* a = strstr(p, tag_upper);
            const char* b = strstr(p, tag_lower);
            if (a && b) found = (a < b) ? a : b;
            else if (a)  found = a;
            else if (b)  found = b;
            if (!found) break;
            p = found + strlen(tag_upper);  // skip past "PB["
            const char* e = strchr(p, ']');
            if (!e) break;
            // Trim surrounding whitespace
            while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            while (e > p && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) e--;
            if (p < e) { dst.assign(p, e); return; }
            p = e + 1;
        }
    };

    black_out.clear();
    white_out.clear();
    extract("PB[", "pb[", black_out);
    extract("PW[", "pw[", white_out);
    return !black_out.empty() || !white_out.empty();
}

// Build a display label from player names, falling back to the filename stem.
static std::string make_display_name(const std::string& filename,
                                     const std::string& black_name,
                                     const std::string& white_name) {
    if (!black_name.empty() && !white_name.empty())
        return black_name + " vs " + white_name;
    if (!black_name.empty()) return black_name + " vs ?";
    if (!white_name.empty()) return "? vs " + white_name;
    // Strip extension
    std::string stem = filename;
    auto dot = stem.rfind('.');
    if (dot != std::string::npos) stem = stem.substr(0, dot);
    return stem;
}

// ---------------------------------------------------------------------------
// Directory listing

#ifdef _WIN32
bool Catalog::list_recursive(const std::string& dir, const std::string& base,
                             std::vector<std::string>& out) {
    std::string search = join_path(dir, "*");
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(search.c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        std::string full = join_path(dir, data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            list_recursive(full, base, out);
        } else if (has_sgf_ext(data.cFileName)) {
            // Store path relative to base
            std::string rel = full;
            if (full.size() > base.size() && full.substr(0, base.size()) == base) {
                rel = full.substr(base.size());
                if (!rel.empty() && (rel[0] == '\\' || rel[0] == '/'))
                    rel = rel.substr(1);
            }
            out.push_back(rel);
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
    return true;
}
#else
bool Catalog::list_recursive(const std::string& dir, const std::string& base,
                             std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        std::string full = join_path(dir, ent->d_name);
        bool is_dir = false;
#ifdef DT_DIR
        if (ent->d_type == DT_DIR) is_dir = true;
        else if (ent->d_type == DT_UNKNOWN)
#endif
        {
            struct stat st{};
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;
        }
        if (is_dir) {
            list_recursive(full, base, out);
        } else if (has_sgf_ext(ent->d_name)) {
            std::string rel = full;
            if (full.size() > base.size() && full.substr(0, base.size()) == base) {
                rel = full.substr(base.size());
                if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
            }
            out.push_back(rel);
        }
    }
    closedir(d);
    return true;
}
#endif

bool Catalog::list_sgf_files(const std::string& dir, std::vector<std::string>& out) {
    out.clear();
    return list_recursive(dir, dir, out);
}

// ---------------------------------------------------------------------------
// Entry loading for the current catalog directory

bool Catalog::load_entries() {
    entries.clear();
    std::string dir_path = base_dir;
    if (!current_subdir.empty())
        dir_path = join_path(base_dir, current_subdir);

    std::vector<CatalogEntry> dirs_vec, files_vec;

#ifdef _WIN32
    std::string search = join_path(dir_path, "*");
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(search.c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            dirs_vec.push_back({data.cFileName, "", true, 1});
        } else if (has_sgf_ext(data.cFileName)) {
            files_vec.push_back({data.cFileName, "", false, 0});
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
#else
    DIR* d = opendir(dir_path.c_str());
    if (!d) return false;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        std::string full = join_path(dir_path, ent->d_name);
        bool is_dir = false;
#ifdef DT_DIR
        if (ent->d_type == DT_DIR) is_dir = true;
        else if (ent->d_type == DT_UNKNOWN)
#endif
        {
            struct stat st{};
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) is_dir = true;
        }
        if (is_dir) dirs_vec.push_back({ent->d_name, "", true, 1});
        else if (has_sgf_ext(ent->d_name)) files_vec.push_back({ent->d_name, "", false, 0});
    }
    closedir(d);
#endif

    auto cmp = [](const CatalogEntry& a, const CatalogEntry& b) {
#ifdef _WIN32
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
#else
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
#endif
    };
    std::sort(dirs_vec.begin(),  dirs_vec.end(),  cmp);
    std::sort(files_vec.begin(), files_vec.end(), cmp);

    // Parent entry first when inside a subdirectory.
    // At root level, offer the virtual [BY YEAR] browser.
    if (!current_subdir.empty()) {
        entries.push_back({"..", "..", true, 2});
    } else {
        CatalogEntry by_year;
        by_year.name         = "[BY YEAR]";
        by_year.display_name = "[BY YEAR]";
        by_year.name_loaded  = true;
        by_year.type         = 4;
        entries.push_back(by_year);
    }
    for (auto& e : dirs_vec) {
        e.display_name = e.name;
        e.name_loaded  = true;
        entries.push_back(e);
    }
    for (auto& e : files_vec) {
        // Build relative path from base_dir to look up in the game index
        std::string rel = current_subdir.empty() ? e.name : join_path(current_subdir, e.name);
        const GameIndexEntry* ge = game_index.find(rel);
        if (ge) {
            e.display_name = make_display_name(e.name, ge->black, ge->white);
            e.player_black = ge->black;
            e.player_white = ge->white;
            e.name_loaded  = true;
        } else {
            // Fall back to lazy parsing via ensure_names_loaded()
            e.display_name = make_display_name(e.name, "", "");
            e.name_loaded  = false;
        }
        entries.push_back(e);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Virtual year browser

void Catalog::load_year_list() {
    entries.clear();
    entries.push_back({"..", "..", true, 2});
    year_needs_refresh = false;

    if (!game_index.loaded()) {
        // Index still building — add a placeholder and request a refresh later
        CatalogEntry wait;
        wait.name         = "";
        wait.display_name = "Building index...";
        wait.name_loaded  = true;
        wait.type         = 0;
        entries.push_back(wait);
        year_needs_refresh = true;
        return;
    }

    // Count games per year
    std::map<std::string, int> year_count;
    for (const GameIndexEntry* ge : game_index.get_all()) {
        std::string yr = GameIndex::extract_year(ge->date);
        if (yr.empty()) yr = "Unknown";
        year_count[yr]++;
    }

    // Sort: newest year first, "Unknown" at the end
    std::vector<std::pair<std::string, int>> years(year_count.begin(), year_count.end());
    std::sort(years.begin(), years.end(),
              [](const std::pair<std::string,int>& a, const std::pair<std::string,int>& b) {
                  if (a.first == "Unknown") return false;
                  if (b.first == "Unknown") return true;
                  return a.first > b.first;  // descending (newest first)
              });

    for (const auto& kv : years) {
        CatalogEntry e;
        e.name = kv.first;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s  (%d game%s)", kv.first.c_str(),
                 kv.second, kv.second == 1 ? "" : "s");
        e.display_name = buf;
        e.name_loaded  = true;
        e.type         = 3;
        entries.push_back(e);
    }
}

void Catalog::load_year_games(const std::string& year) {
    entries.clear();
    entries.push_back({"..", "..", true, 2});
    if (!game_index.loaded()) return;

    // Collect all games for this year
    std::vector<const GameIndexEntry*> matches;
    for (const GameIndexEntry* ge : game_index.get_all()) {
        std::string yr = GameIndex::extract_year(ge->date);
        if (yr.empty()) yr = "Unknown";
        if (yr == year) matches.push_back(ge);
    }

    // Sort by date then by black player name
    std::sort(matches.begin(), matches.end(),
              [](const GameIndexEntry* a, const GameIndexEntry* b) {
                  if (a->date != b->date) return a->date < b->date;
                  return a->black < b->black;
              });

    for (const GameIndexEntry* ge : matches) {
        CatalogEntry e;
        size_t sep = ge->rel_path.find_last_of("/\\");
        e.name = (sep == std::string::npos) ? ge->rel_path : ge->rel_path.substr(sep + 1);
        e.display_name = make_display_name(e.name, ge->black, ge->white);
        e.player_black = ge->black;
        e.player_white = ge->white;
        e.name_loaded  = true;
        e.type         = 0;
        e.full_path    = join_path(base_dir, ge->rel_path);
        entries.push_back(e);
    }
}

void Catalog::tick() {
    // If the year list was shown before the index finished loading, refresh it now.
    if (virtual_year_mode && virtual_year.empty() && year_needs_refresh
        && game_index.loaded()) {
        load_year_list();
        index  = 0;
        scroll = 0;
    }
}

// ---------------------------------------------------------------------------
// Search

void Catalog::apply_search() {
    entries.clear();
    index  = 0;
    scroll = 0;
    if (search_query.empty()) return;

    auto results = game_index.search(search_query);
    entries.reserve(results.size());
    for (const GameIndexEntry* ge : results) {
        CatalogEntry e;
        size_t sep = ge->rel_path.find_last_of("/\\");
        e.name = (sep == std::string::npos) ? ge->rel_path : ge->rel_path.substr(sep + 1);
        e.display_name = make_display_name(e.name, ge->black, ge->white);
        e.player_black = ge->black;
        e.player_white = ge->white;
        e.name_loaded  = true;
        e.type         = 0;
        e.full_path    = join_path(base_dir, ge->rel_path);
        entries.push_back(std::move(e));
    }
}

void Catalog::search_append(char c) {
    search_query += c;
    search_mode   = true;
    apply_search();
}

void Catalog::search_backspace() {
    if (search_query.empty()) return;
    search_query.pop_back();
    if (search_query.empty()) {
        search_clear();
    } else {
        apply_search();
    }
}

void Catalog::search_clear() {
    search_query.clear();
    search_mode       = false;
    virtual_year_mode = false;
    virtual_year.clear();
    year_needs_refresh = false;
    load_entries();
    index  = 0;
    scroll = 0;
}

// ---------------------------------------------------------------------------
// Public interface

void Catalog::open(const std::string& games_dir) {
    if (active) return;
    base_dir       = games_dir;
    current_subdir = "";
    index          = 0;
    scroll         = 0;
    selection_made = false;
    selected_path  = "";
    search_query.clear();
    search_mode        = false;
    virtual_year_mode  = false;
    virtual_year.clear();
    year_needs_refresh = false;
    // Kick off background index load — returns immediately; catalog opens
    // right away while the index is built in parallel.
    game_index.load_async(games_dir);
    if (!load_entries()) {
        entries.clear();
        return;
    }
    active = true;
}

void Catalog::close() {
    active = false;
}

std::string Catalog::selected_entry_path() const {
    if (index < 0 || index >= (int)entries.size()) return {};
    const CatalogEntry& e = entries[index];
    if (e.type != 0) return {};  // directory or ".." — no path
    if (!e.full_path.empty()) return e.full_path;  // search-results entry
    std::string dir = base_dir;
    if (!current_subdir.empty())
        dir = join_path(base_dir, current_subdir);
    return join_path(dir, e.name);
}

void Catalog::dir_up() {
    auto pos = current_subdir.find_last_of("/\\");
    if (pos == std::string::npos)
        current_subdir = "";
    else
        current_subdir = current_subdir.substr(0, pos);
}

void Catalog::ensure_names_loaded(int from, int count) {
    std::string dir_path = base_dir;
    if (!current_subdir.empty())
        dir_path = join_path(base_dir, current_subdir);

    int total = (int)entries.size();
    int end   = std::min(from + count, total);
    for (int i = std::max(0, from); i < end; i++) {
        CatalogEntry& e = entries[i];
        if (e.name_loaded || e.type != 0) continue;
        std::string bname, wname;
        sgf_player_names(join_path(dir_path, e.name), bname, wname);
        e.display_name = make_display_name(e.name, bname, wname);
        e.player_black = bname;
        e.player_white = wname;
        e.name_loaded  = true;
    }
}

void Catalog::select() {
    if (!active || index < 0 || index >= (int)entries.size()) return;
    const CatalogEntry& e = entries[index];

    // Search mode — all entries are SGF files with full_path set
    if (search_mode) {
        selected_path    = e.full_path;
        sequential_dir   = "";
        sequential_index = 0;
        selection_made   = true;
        active           = false;
        return;
    }

    // ".." navigation
    if (e.type == 2) {
        if (virtual_year_mode) {
            if (!virtual_year.empty()) {
                // Drill back to year list
                virtual_year.clear();
                load_year_list();
            } else {
                // Exit year browser back to normal dir browse
                virtual_year_mode = false;
                load_entries();
            }
            index = 0; scroll = 0;
            return;
        }
        dir_up();
        load_entries();
        index = 0; scroll = 0;
        return;
    }

    // [BY YEAR] meta-entry — enter the virtual year browser
    if (e.type == 4) {
        virtual_year_mode = true;
        virtual_year.clear();
        load_year_list();
        index = 0; scroll = 0;
        return;
    }

    // Virtual year directory — enter the game list for that year
    if (e.type == 3) {
        virtual_year = e.name;
        load_year_games(e.name);
        index = 0; scroll = 0;
        return;
    }

    // Real sub-directory
    if (e.type == 1) {
        if (current_subdir.empty())
            current_subdir = e.name;
        else
            current_subdir = join_path(current_subdir, e.name);
        load_entries();
        index = 0; scroll = 0;
        return;
    }

    // SGF file — build full path (full_path already set for year-view entries)
    if (!e.full_path.empty()) {
        selected_path = e.full_path;
    } else {
        std::string file_dir = base_dir;
        if (!current_subdir.empty())
            file_dir = join_path(base_dir, current_subdir);
        selected_path = join_path(file_dir, e.name);
    }

    // Sequential mode only in real subdirectory browsing
    sequential_dir   = (virtual_year_mode || current_subdir.empty())
                       ? ""
                       : join_path(base_dir, current_subdir);
    sequential_index = 0;

    selection_made = true;
    active         = false;
}
