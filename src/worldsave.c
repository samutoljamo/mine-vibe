#include "worldsave.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#  include <direct.h>
#  include <windows.h>
#  define WS_MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <dirent.h>
#  define WS_MKDIR(p) mkdir((p), 0755)
#endif

/* ------------------------------------------------------------------ */
/*  Pure core: name sanitization                                       */
/* ------------------------------------------------------------------ */

/* A char is allowed verbatim in a directory name if it's alphanumeric, '_',
 * '-' or '.'. Spaces map to '_'; everything else (separators, control chars)
 * is dropped. Dots at the ends are trimmed afterwards so we never produce a
 * "." / ".." component. */
static bool ws_char_ok(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

void worldsave_sanitize_name(const char* in, char* out, size_t cap) {
    if (cap == 0) return;
    size_t n = 0;
    size_t limit = cap - 1;
    if (limit > WORLDSAVE_NAME_MAX - 1) limit = WORLDSAVE_NAME_MAX - 1;

    if (in) {
        for (const char* p = in; *p && n < limit; p++) {
            char c = *p;
            if (c == ' ') c = '_';
            if (ws_char_ok(c)) out[n++] = c;
        }
    }
    out[n] = '\0';

    /* Trim leading dots/dashes (avoid hidden files and "..": only dots are the
     * real hazard, but dashes can confuse CLIs as flags). */
    size_t start = 0;
    while (out[start] == '.' || out[start] == '-') start++;
    if (start > 0) {
        memmove(out, out + start, n - start + 1);
        n -= start;
    }
    /* Trim trailing dots. */
    while (n > 0 && out[n - 1] == '.') out[--n] = '\0';

    if (n == 0) {
        /* Empty or all-illegal input: fall back to a stable default. */
        snprintf(out, cap, "World");
    }
}

/* ------------------------------------------------------------------ */
/*  Pure core: path building                                           */
/* ------------------------------------------------------------------ */

static void ws_join(char* out, size_t cap, const char* name, const char* leaf) {
    char safe[WORLDSAVE_NAME_MAX];
    worldsave_sanitize_name(name, safe, sizeof(safe));
    if (leaf && leaf[0])
        snprintf(out, cap, "%s/%s/%s", WORLDSAVE_ROOT, safe, leaf);
    else
        snprintf(out, cap, "%s/%s", WORLDSAVE_ROOT, safe);
}

void worldsave_dir_path(const char* name, char* out, size_t cap) {
    ws_join(out, cap, name, NULL);
}
void worldsave_dat_path(const char* name, char* out, size_t cap) {
    ws_join(out, cap, name, "world.dat");
}
void worldsave_meta_path(const char* name, char* out, size_t cap) {
    ws_join(out, cap, name, "world.meta");
}

/* ------------------------------------------------------------------ */
/*  Pure core: meta format / parse                                     */
/* ------------------------------------------------------------------ */

void worldsave_meta_init(WorldMeta* m, const char* name,
                         int32_t seed, int64_t last_played) {
    worldsave_sanitize_name(name, m->name, sizeof(m->name));
    m->seed = seed;
    m->last_played = last_played;
}

size_t worldsave_meta_format(const WorldMeta* m, char* buf, size_t cap) {
    int n = snprintf(buf, cap,
                     "name=%s\nseed=%d\nlast_played=%lld\n",
                     m->name, (int)m->seed, (long long)m->last_played);
    if (n < 0 || (size_t)n >= cap) return 0;
    return (size_t)n;
}

/* Find "key=" on a line in buf and copy the value (up to newline) into out.
 * Returns true if found. */
static bool ws_find_field(const char* buf, const char* key,
                          char* out, size_t cap) {
    size_t klen = strlen(key);
    const char* p = buf;
    while (*p) {
        /* p is at the start of a line. */
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            const char* v = p + klen + 1;
            size_t i = 0;
            while (v[i] && v[i] != '\n' && v[i] != '\r' && i + 1 < cap) {
                out[i] = v[i];
                i++;
            }
            out[i] = '\0';
            return true;
        }
        /* Advance to next line. */
        const char* nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    return false;
}

bool worldsave_meta_parse(const char* buf, WorldMeta* out) {
    if (!buf || !out) return false;
    char name[WORLDSAVE_NAME_MAX];
    char seed_s[32];
    char last_s[32];
    if (!ws_find_field(buf, "name", name, sizeof(name))) return false;
    if (!ws_find_field(buf, "seed", seed_s, sizeof(seed_s))) return false;
    if (!ws_find_field(buf, "last_played", last_s, sizeof(last_s))) return false;
    if (name[0] == '\0') return false;

    char* end = NULL;
    long sv = strtol(seed_s, &end, 10);
    if (end == seed_s) return false;
    long long lv = strtoll(last_s, &end, 10);
    if (end == last_s) return false;

    snprintf(out->name, sizeof(out->name), "%s", name);
    out->seed = (int32_t)sv;
    out->last_played = (int64_t)lv;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Pure core: new-world name + seed derivation                        */
/* ------------------------------------------------------------------ */

void worldsave_new_name(uint64_t counter, char* out, size_t cap) {
    snprintf(out, cap, "World_%llu", (unsigned long long)(counter + 1));
}

int32_t worldsave_seed_from_counter(uint64_t counter) {
    /* SplitMix64-style mix so consecutive counters scatter across the range. */
    uint64_t z = counter + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return (int32_t)(z & 0x7FFFFFFF);   /* non-negative seed */
}

/* ------------------------------------------------------------------ */
/*  Pure core: world list                                              */
/* ------------------------------------------------------------------ */

void worldsave_list_init(WorldList* wl) {
    wl->count = 0;
}

bool worldsave_list_add(WorldList* wl, const WorldMeta* m) {
    if (wl->count >= WORLDSAVE_LIST_MAX) return false;
    wl->entries[wl->count++] = *m;
    return true;
}

void worldsave_list_sort_recent(WorldList* wl) {
    /* Simple insertion sort by last_played descending (lists are tiny). */
    for (int i = 1; i < wl->count; i++) {
        WorldMeta key = wl->entries[i];
        int j = i - 1;
        while (j >= 0 && wl->entries[j].last_played < key.last_played) {
            wl->entries[j + 1] = wl->entries[j];
            j--;
        }
        wl->entries[j + 1] = key;
    }
}

/* ------------------------------------------------------------------ */
/*  Filesystem layer                                                   */
/* ------------------------------------------------------------------ */

static bool ws_mkdir_ok(const char* path) {
    if (WS_MKDIR(path) == 0) return true;
    /* Already-exists is success for our purposes. */
#ifdef _WIN32
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return errno == EEXIST;
#endif
}

bool worldsave_ensure_dir(const char* name) {
    if (!ws_mkdir_ok(WORLDSAVE_ROOT)) return false;
    char dir[WORLDSAVE_PATH_MAX];
    worldsave_dir_path(name, dir, sizeof(dir));
    return ws_mkdir_ok(dir);
}

bool worldsave_write_meta(const WorldMeta* m) {
    if (!worldsave_ensure_dir(m->name)) return false;
    char path[WORLDSAVE_PATH_MAX];
    worldsave_meta_path(m->name, path, sizeof(path));

    char buf[WORLDSAVE_META_BUF];
    size_t n = worldsave_meta_format(m, buf, sizeof(buf));
    if (n == 0) return false;

    FILE* f = fopen(path, "wb");
    if (!f) return false;
    bool ok = (fwrite(buf, 1, n, f) == n);
    if (fclose(f) != 0) ok = false;
    return ok;
}

bool worldsave_read_meta(const char* name, WorldMeta* out) {
    char path[WORLDSAVE_PATH_MAX];
    worldsave_meta_path(name, path, sizeof(path));
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char buf[WORLDSAVE_META_BUF];
    size_t got = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[got] = '\0';
    return worldsave_meta_parse(buf, out);
}

int worldsave_scan(WorldList* wl) {
    worldsave_list_init(wl);
#ifdef _WIN32
    char pattern[WORLDSAVE_PATH_MAX];
    snprintf(pattern, sizeof(pattern), "%s\\*", WORLDSAVE_ROOT);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        WorldMeta m;
        if (worldsave_read_meta(fd.cFileName, &m))
            worldsave_list_add(wl, &m);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(WORLDSAVE_ROOT);
    if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;   /* skip ., .., hidden */
        WorldMeta m;
        if (worldsave_read_meta(e->d_name, &m))
            worldsave_list_add(wl, &m);
    }
    closedir(d);
#endif
    worldsave_list_sort_recent(wl);
    return wl->count;
}
