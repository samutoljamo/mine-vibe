#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/worldsave.h"

/* ------------------------------------------------------------------ */
/*  Name sanitization                                                  */
/* ------------------------------------------------------------------ */

static void test_sanitize_basic(void) {
    char out[WORLDSAVE_NAME_MAX];

    worldsave_sanitize_name("My World", out, sizeof(out));
    assert(strcmp(out, "My_World") == 0);

    worldsave_sanitize_name("hello", out, sizeof(out));
    assert(strcmp(out, "hello") == 0);

    /* Path separators and traversal must be stripped/replaced — never allow a
     * name to escape the saves/ directory. */
    worldsave_sanitize_name("../etc/passwd", out, sizeof(out));
    assert(strchr(out, '/') == NULL);
    assert(strstr(out, "..") == NULL);

    worldsave_sanitize_name("a\\b/c", out, sizeof(out));
    assert(strchr(out, '/') == NULL);
    assert(strchr(out, '\\') == NULL);

    printf("PASS: sanitize_basic\n");
}

static void test_sanitize_empty_and_garbage(void) {
    char out[WORLDSAVE_NAME_MAX];

    /* Empty / all-illegal input falls back to a non-empty default. */
    worldsave_sanitize_name("", out, sizeof(out));
    assert(out[0] != '\0');

    worldsave_sanitize_name("///", out, sizeof(out));
    assert(out[0] != '\0');
    assert(strchr(out, '/') == NULL);

    /* Leading/trailing dots and spaces trimmed so we never make ".", "..". */
    worldsave_sanitize_name("  ..  ", out, sizeof(out));
    assert(out[0] != '\0');
    assert(strcmp(out, ".") != 0 && strcmp(out, "..") != 0);

    printf("PASS: sanitize_empty_and_garbage\n");
}

static void test_sanitize_truncates(void) {
    char out[WORLDSAVE_NAME_MAX];
    char big[512];
    memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1] = '\0';
    worldsave_sanitize_name(big, out, sizeof(out));
    assert(strlen(out) < WORLDSAVE_NAME_MAX);
    assert(out[0] == 'x');
    printf("PASS: sanitize_truncates\n");
}

/* ------------------------------------------------------------------ */
/*  Path building                                                      */
/* ------------------------------------------------------------------ */

static void test_paths(void) {
    char path[WORLDSAVE_PATH_MAX];

    worldsave_dir_path("Alpha", path, sizeof(path));
    assert(strcmp(path, WORLDSAVE_ROOT "/Alpha") == 0);

    worldsave_dat_path("Alpha", path, sizeof(path));
    assert(strcmp(path, WORLDSAVE_ROOT "/Alpha/world.dat") == 0);

    worldsave_meta_path("Alpha", path, sizeof(path));
    assert(strcmp(path, WORLDSAVE_ROOT "/Alpha/world.meta") == 0);

    /* Path builders sanitize the name too: a hostile name can't escape root. */
    worldsave_dat_path("../evil", path, sizeof(path));
    assert(strncmp(path, WORLDSAVE_ROOT "/", strlen(WORLDSAVE_ROOT) + 1) == 0);
    assert(strstr(path, "..") == NULL);

    printf("PASS: paths\n");
}

/* ------------------------------------------------------------------ */
/*  Meta format / parse round-trip                                     */
/* ------------------------------------------------------------------ */

static void test_meta_roundtrip(void) {
    WorldMeta m;
    worldsave_meta_init(&m, "Frontier", 12345, 99887766);

    char buf[WORLDSAVE_META_BUF];
    size_t n = worldsave_meta_format(&m, buf, sizeof(buf));
    assert(n > 0 && n < sizeof(buf));

    WorldMeta out;
    bool ok = worldsave_meta_parse(buf, &out);
    assert(ok);
    assert(strcmp(out.name, "Frontier") == 0);
    assert(out.seed == 12345);
    assert(out.last_played == 99887766);

    printf("PASS: meta_roundtrip\n");
}

static void test_meta_parse_rejects_garbage(void) {
    WorldMeta out;
    assert(!worldsave_meta_parse("", &out));
    assert(!worldsave_meta_parse("not a meta file\n", &out));
    assert(!worldsave_meta_parse("name=x\n", &out));   /* missing seed/last */
    printf("PASS: meta_parse_rejects_garbage\n");
}

/* ------------------------------------------------------------------ */
/*  Game mode: enum default, persistence round-trip, backward-compat   */
/* ------------------------------------------------------------------ */

static void test_gamemode_default_is_survival(void) {
    /* meta_init must default to survival for backward compatibility. */
    WorldMeta m;
    worldsave_meta_init(&m, "Default", 1, 2);
    assert(m.gamemode == GAMEMODE_SURVIVAL);
    assert(world_meta_gamemode(&m) == GAMEMODE_SURVIVAL);
    printf("PASS: gamemode_default_is_survival\n");
}

static void test_gamemode_roundtrips(void) {
    /* Creative gamemode survives a format/parse cycle. */
    WorldMeta m;
    worldsave_meta_init(&m, "Creative", 7, 8);
    m.gamemode = GAMEMODE_CREATIVE;

    char buf[WORLDSAVE_META_BUF];
    size_t n = worldsave_meta_format(&m, buf, sizeof(buf));
    assert(n > 0 && n < sizeof(buf));

    WorldMeta out;
    assert(worldsave_meta_parse(buf, &out));
    assert(out.gamemode == GAMEMODE_CREATIVE);
    assert(world_meta_gamemode(&out) == GAMEMODE_CREATIVE);
    printf("PASS: gamemode_roundtrips\n");
}

static void test_gamemode_old_save_defaults_survival(void) {
    /* An old meta file with no gamemode= line must load as survival, not
     * fail to parse and not corrupt the other fields. */
    WorldMeta out;
    bool ok = worldsave_meta_parse(
        "name=Legacy\nseed=99\nlast_played=5\n", &out);
    assert(ok);
    assert(strcmp(out.name, "Legacy") == 0);
    assert(out.seed == 99);
    assert(out.last_played == 5);
    assert(out.gamemode == GAMEMODE_SURVIVAL);
    printf("PASS: gamemode_old_save_defaults_survival\n");
}

static void test_gamemode_bad_value_defaults_survival(void) {
    /* Out-of-range / garbage gamemode value falls back to survival. */
    WorldMeta out;
    assert(worldsave_meta_parse(
        "name=X\nseed=1\nlast_played=1\ngamemode=99\n", &out));
    assert(out.gamemode == GAMEMODE_SURVIVAL);

    assert(worldsave_meta_parse(
        "name=X\nseed=1\nlast_played=1\ngamemode=junk\n", &out));
    assert(out.gamemode == GAMEMODE_SURVIVAL);
    printf("PASS: gamemode_bad_value_defaults_survival\n");
}

static void test_gamemode_allows_damage(void) {
    assert(gamemode_allows_damage(GAMEMODE_SURVIVAL) == true);
    assert(gamemode_allows_damage(GAMEMODE_CREATIVE) == false);
    printf("PASS: gamemode_allows_damage\n");
}

static void test_meta_parse_field_order_independent(void) {
    /* Parser keys off field names, not positions. */
    WorldMeta out;
    bool ok = worldsave_meta_parse(
        "last_played=7\nseed=-42\nname=Mixed Order\n", &out);
    assert(ok);
    assert(strcmp(out.name, "Mixed Order") == 0);
    assert(out.seed == -42);
    assert(out.last_played == 7);
    printf("PASS: meta_parse_field_order_independent\n");
}

/* ------------------------------------------------------------------ */
/*  New-world name + seed derivation (no rand/time globals)            */
/* ------------------------------------------------------------------ */

static void test_new_name_from_counter(void) {
    char a[WORLDSAVE_NAME_MAX], b[WORLDSAVE_NAME_MAX];
    worldsave_new_name(0, a, sizeof(a));
    worldsave_new_name(1, b, sizeof(b));
    assert(a[0] != '\0' && b[0] != '\0');
    assert(strcmp(a, b) != 0);   /* different counters -> different names */
    /* Generated names are already directory-safe. */
    assert(strchr(a, '/') == NULL && strchr(a, ' ') == NULL);
    printf("PASS: new_name_from_counter\n");
}

static void test_seed_from_counter_deterministic(void) {
    /* Pure: same counter -> same seed; nearby counters -> different seeds. */
    int32_t s0a = worldsave_seed_from_counter(7);
    int32_t s0b = worldsave_seed_from_counter(7);
    int32_t s1  = worldsave_seed_from_counter(8);
    assert(s0a == s0b);
    assert(s0a != s1);
    printf("PASS: seed_from_counter_deterministic\n");
}

/* ------------------------------------------------------------------ */
/*  World-list format/parse round-trip (the on-disk list view)         */
/* ------------------------------------------------------------------ */

static void test_world_list_roundtrip(void) {
    WorldList wl;
    worldsave_list_init(&wl);

    WorldMeta m0; worldsave_meta_init(&m0, "Alpha", 1, 100);
    WorldMeta m1; worldsave_meta_init(&m1, "Beta",  2, 200);
    assert(worldsave_list_add(&wl, &m0));
    assert(worldsave_list_add(&wl, &m1));
    assert(wl.count == 2);

    /* Sort by last_played descending: most-recent first. */
    worldsave_list_sort_recent(&wl);
    assert(wl.entries[0].last_played >= wl.entries[1].last_played);
    assert(strcmp(wl.entries[0].name, "Beta") == 0);

    printf("PASS: world_list_roundtrip\n");
}

static void test_world_list_capacity(void) {
    WorldList wl;
    worldsave_list_init(&wl);
    for (int i = 0; i < WORLDSAVE_LIST_MAX + 5; i++) {
        WorldMeta m; worldsave_meta_init(&m, "W", i, i);
        worldsave_list_add(&wl, &m);
    }
    assert(wl.count == WORLDSAVE_LIST_MAX);   /* never overflows */
    printf("PASS: world_list_capacity\n");
}

int main(void) {
    test_sanitize_basic();
    test_sanitize_empty_and_garbage();
    test_sanitize_truncates();
    test_paths();
    test_meta_roundtrip();
    test_meta_parse_rejects_garbage();
    test_meta_parse_field_order_independent();
    test_gamemode_default_is_survival();
    test_gamemode_roundtrips();
    test_gamemode_old_save_defaults_survival();
    test_gamemode_bad_value_defaults_survival();
    test_gamemode_allows_damage();
    test_new_name_from_counter();
    test_seed_from_counter_deterministic();
    test_world_list_roundtrip();
    test_world_list_capacity();
    printf("ALL WORLDSAVE TESTS PASSED\n");
    return 0;
}
