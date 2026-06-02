#include "game_index.hpp"
#include "go_viewer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

// ---------------------------------------------------------------------------
// Local helpers

static std::string gi_join_path(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    char last = dir.back();
    if (last == '/' || last == '\\') return dir + name;
    return dir + PATH_SEP_STR + name;
}

static bool gi_has_sgf_ext(const std::string& name) {
    if (name.size() < 4) return false;
    std::string ext = name.substr(name.size() - 4);
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".sgf";
}

// ---------------------------------------------------------------------------
// Destructor — join background thread if still running

GameIndex::~GameIndex() {
    if (thread_.joinable())
        thread_.join();
}

// ---------------------------------------------------------------------------
// SGF header scan — reads first 4 KB and extracts PB/PW/DT/RE

bool GameIndex::scan_sgf_header(const std::string& full_path,
                                std::string& black, std::string& white,
                                std::string& date,  std::string& result) {
    FILE* fp = fopen(full_path.c_str(), "rb");
    if (!fp) return false;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (!n) return false;
    buf[n] = '\0';

    auto extract = [&](const char* tag_up, const char* tag_lo, std::string& dst) {
        if (!dst.empty()) return;
        const char* p = buf;
        while (*p) {
            const char* a = strstr(p, tag_up);
            const char* b = strstr(p, tag_lo);
            const char* found = nullptr;
            if (a && b) found = (a < b) ? a : b;
            else        found = a ? a : b;
            if (!found) break;
            p = found + strlen(tag_up);
            const char* e = strchr(p, ']');
            if (!e) break;
            while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
            while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) e--;
            if (p < e) { dst.assign(p, e); return; }
            p = e + 1;
        }
    };

    extract("PB[", "pb[", black);
    extract("PW[", "pw[", white);
    extract("DT[", "dt[", date);
    extract("RE[", "re[", result);
    return !black.empty() || !white.empty() || !date.empty() || !result.empty();
}

// ---------------------------------------------------------------------------
// Recursive SGF file listing (mirrors Catalog::list_recursive)

#ifdef _WIN32
bool GameIndex::list_sgf_recursive(const std::string& dir, const std::string& base,
                                   std::vector<std::string>& rel_paths) {
    std::string search = gi_join_path(dir, "*");
    WIN32_FIND_DATAA data;
    HANDLE h = FindFirstFileA(search.c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (strcmp(data.cFileName, ".") == 0 || strcmp(data.cFileName, "..") == 0) continue;
        std::string full = gi_join_path(dir, data.cFileName);
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            list_sgf_recursive(full, base, rel_paths);
        } else if (gi_has_sgf_ext(data.cFileName)) {
            std::string rel = full;
            if (full.size() > base.size() && full.substr(0, base.size()) == base) {
                rel = full.substr(base.size());
                if (!rel.empty() && (rel[0] == '\\' || rel[0] == '/'))
                    rel = rel.substr(1);
            }
            rel_paths.push_back(rel);
        }
    } while (FindNextFileA(h, &data));
    FindClose(h);
    return true;
}
#else
bool GameIndex::list_sgf_recursive(const std::string& dir, const std::string& base,
                                   std::vector<std::string>& rel_paths) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        std::string full = gi_join_path(dir, ent->d_name);
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
            list_sgf_recursive(full, base, rel_paths);
        } else if (gi_has_sgf_ext(ent->d_name)) {
            std::string rel = full;
            if (full.size() > base.size() && full.substr(0, base.size()) == base) {
                rel = full.substr(base.size());
                if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
            }
            rel_paths.push_back(rel);
        }
    }
    closedir(d);
    return true;
}
#endif

int GameIndex::count_sgf_files(const std::string& base_dir) {
    std::vector<std::string> files;
    list_sgf_recursive(base_dir, base_dir, files);
    return (int)files.size();
}

// ---------------------------------------------------------------------------
// Index file I/O

std::string GameIndex::index_file_path(const std::string& base_dir) {
    return gi_join_path(base_dir, ".go_viewer_index");
}

bool GameIndex::read_index(const std::string& path,
                           std::vector<GameIndexEntry>& out, int& stored_count) {
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return false;

    char line[2048];
    static const char* HEADER = "go_viewer_index_v1\t";
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return false; }
    if (strncmp(line, HEADER, strlen(HEADER)) != 0) { fclose(fp); return false; }
    stored_count = atoi(line + strlen(HEADER));

    out.clear();
    while (fgets(line, sizeof(line), fp)) {
        int len = (int)strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;

        GameIndexEntry e;
        const char* parts[5] = {};
        char* p = line;
        int n = 0;
        while (n < 5) {
            parts[n++] = p;
            char* t = strchr(p, '\t');
            if (!t) break;
            *t = '\0';
            p = t + 1;
        }
        if (!parts[0] || !*parts[0]) continue;
        e.rel_path = parts[0];
        if (n > 1 && parts[1]) e.black  = parts[1];
        if (n > 2 && parts[2]) e.white  = parts[2];
        if (n > 3 && parts[3]) e.date   = parts[3];
        if (n > 4 && parts[4]) e.result = parts[4];
        out.push_back(std::move(e));
    }
    fclose(fp);
    return true;
}

bool GameIndex::write_index(const std::string& path,
                            const std::vector<GameIndexEntry>& entries) {
    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) return false;

    fprintf(fp, "go_viewer_index_v1\t%d\n", (int)entries.size());
    for (const auto& e : entries) {
        auto clean = [](const std::string& s) -> std::string {
            std::string r = s;
            for (char& c : r) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            return r;
        };
        fprintf(fp, "%s\t%s\t%s\t%s\t%s\n",
                clean(e.rel_path).c_str(),
                clean(e.black).c_str(),
                clean(e.white).c_str(),
                clean(e.date).c_str(),
                clean(e.result).c_str());
    }
    fclose(fp);
    return true;
}

// ---------------------------------------------------------------------------
// Background load / rebuild

// Called on the background thread.
void GameIndex::do_load(std::string base_dir) {
    std::vector<GameIndexEntry> built;

    // Fast path: try to use the cached index file.
    {
        std::vector<GameIndexEntry> tmp;
        int stored_count = 0;
        if (read_index(index_file_path(base_dir), tmp, stored_count)) {
            int actual = count_sgf_files(base_dir);
            if (actual == stored_count) {
                std::sort(tmp.begin(), tmp.end(),
                          [](const GameIndexEntry& a, const GameIndexEntry& b) {
                              return a.rel_path < b.rel_path;
                          });
                std::lock_guard<std::mutex> lock(mutex_);
                entries_ = std::move(tmp);
                loaded_.store(true);
                loading_.store(false);
                return;
            }
        }
    }

    // Slow path: rebuild from all SGF files.
    std::vector<std::string> rel_paths;
    list_sgf_recursive(base_dir, base_dir, rel_paths);

    built.reserve(rel_paths.size());
    for (const auto& rel : rel_paths) {
        GameIndexEntry e;
        e.rel_path = rel;
        scan_sgf_header(gi_join_path(base_dir, rel), e.black, e.white, e.date, e.result);
        built.push_back(std::move(e));
    }

    std::sort(built.begin(), built.end(),
              [](const GameIndexEntry& a, const GameIndexEntry& b) {
                  return a.rel_path < b.rel_path;
              });

    write_index(index_file_path(base_dir), built);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_ = std::move(built);
    }
    loaded_.store(true);
    loading_.store(false);
}

void GameIndex::load_async(const std::string& base_dir) {
    // Only start once per session
    if (loaded_.load() || loading_.load()) return;
    base_dir_ = base_dir;   // store before thread starts so insert_entry can use it
    loading_.store(true);
    // Join any previous (finished) thread before spawning a new one
    if (thread_.joinable()) thread_.join();
    thread_ = std::thread(&GameIndex::do_load, this, base_dir);
}

void GameIndex::insert_entry(const GameIndexEntry& e) {
    // If the index hasn't loaded yet, skip — the next full rebuild will find the file.
    if (!loaded_.load()) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Sorted insert / update (entries_ is kept sorted by rel_path)
        auto it = std::lower_bound(entries_.begin(), entries_.end(), e.rel_path,
            [](const GameIndexEntry& a, const std::string& key) {
                return a.rel_path < key;
            });
        if (it != entries_.end() && it->rel_path == e.rel_path)
            *it = e;   // update existing
        else
            entries_.insert(it, e);  // insert in sorted position

        // Rewrite the index file so the fast path is used on the next catalog open.
        if (!base_dir_.empty())
            write_index(index_file_path(base_dir_), entries_);
    }
}

// ---------------------------------------------------------------------------
// Query (safe to call from main thread while load is in progress)

int GameIndex::count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (int)entries_.size();
}

const GameIndexEntry* GameIndex::find(const std::string& rel_path) const {
    if (!loaded_.load()) return nullptr;
    std::lock_guard<std::mutex> lock(mutex_);
    if (entries_.empty()) return nullptr;
    auto it = std::lower_bound(
        entries_.begin(), entries_.end(), rel_path,
        [](const GameIndexEntry& e, const std::string& key) {
            return e.rel_path < key;
        });
    if (it != entries_.end() && it->rel_path == rel_path) return &*it;
    return nullptr;
}

std::vector<const GameIndexEntry*> GameIndex::get_all() const {
    std::vector<const GameIndexEntry*> result;
    if (!loaded_.load()) return result;
    std::lock_guard<std::mutex> lock(mutex_);
    result.reserve(entries_.size());
    for (const auto& e : entries_) result.push_back(&e);
    return result;
}

std::string GameIndex::extract_year(const std::string& date) {
    // Look for the first run of 4 digits that forms a plausible year (1900-2099).
    for (size_t i = 0; i + 3 < date.size(); i++) {
        if (!std::isdigit((unsigned char)date[i])) continue;
        if (!std::isdigit((unsigned char)date[i+1])) continue;
        if (!std::isdigit((unsigned char)date[i+2])) continue;
        if (!std::isdigit((unsigned char)date[i+3])) continue;
        int y = (date[i]-'0')*1000 + (date[i+1]-'0')*100
              + (date[i+2]-'0')*10  + (date[i+3]-'0');
        if (y >= 1900 && y <= 2099) return date.substr(i, 4);
    }
    return {};
}

std::vector<const GameIndexEntry*> GameIndex::search(const std::string& query) const {
    std::vector<const GameIndexEntry*> results;
    if (!loaded_.load() || query.empty()) return results;

    std::string q = query;
    for (char& c : q) c = (char)std::toupper((unsigned char)c);

    auto icontains = [&](const std::string& s) -> bool {
        if (s.empty() || s.size() < q.size()) return false;
        for (size_t i = 0; i + q.size() <= s.size(); i++) {
            bool match = true;
            for (size_t j = 0; j < q.size(); j++) {
                if ((char)std::toupper((unsigned char)s[i + j]) != q[j]) { match = false; break; }
            }
            if (match) return true;
        }
        return false;
    };

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& e : entries_) {
        if (icontains(e.black)    ||
            icontains(e.white)    ||
            icontains(e.date)     ||
            icontains(e.result)   ||
            icontains(e.rel_path)) {
            results.push_back(&e);
        }
    }
    return results;
}
