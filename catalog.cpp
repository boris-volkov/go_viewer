#include "catalog.hpp"
#include "go_viewer.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

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
            dirs_vec.push_back({data.cFileName, 1});
        } else if (has_sgf_ext(data.cFileName)) {
            files_vec.push_back({data.cFileName, 0});
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
        if (is_dir) dirs_vec.push_back({ent->d_name, 1});
        else if (has_sgf_ext(ent->d_name)) files_vec.push_back({ent->d_name, 0});
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

    // Parent entry first when inside a subdirectory
    if (!current_subdir.empty())
        entries.push_back({"..", 2});
    for (auto& e : dirs_vec)  entries.push_back(e);
    for (auto& e : files_vec) entries.push_back(e);
    return true;
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

void Catalog::select() {
    if (!active || index < 0 || index >= (int)entries.size()) return;
    const CatalogEntry& e = entries[index];

    if (e.type == 2) { // ".." parent
        dir_up();
        load_entries();
        index = 0; scroll = 0;
        return;
    }
    if (e.type == 1) { // sub-directory
        if (current_subdir.empty())
            current_subdir = e.name;
        else
            current_subdir = join_path(current_subdir, e.name);
        load_entries();
        index = 0; scroll = 0;
        return;
    }
    // SGF file selected
    std::string file_dir = base_dir;
    if (!current_subdir.empty())
        file_dir = join_path(base_dir, current_subdir);

    selected_path = join_path(file_dir, e.name);

    // Set sequential mode when selecting from a subdirectory
    if (!current_subdir.empty()) {
        sequential_dir   = file_dir;
        sequential_index = 0;
    } else {
        sequential_dir   = "";
        sequential_index = 0;
    }

    selection_made = true;
    active         = false;
}
