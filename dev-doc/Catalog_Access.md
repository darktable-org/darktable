# Catalog Access - Reading and Writing the Library from Code

How code outside `src/common/` reads and writes darktable's library (the
SQLite databases holding images, film rolls, tags, metadata, styles,
presets, history): where the SQL goes, what a catalog function looks like,
and how to test it.

This is the first step of darktable-org/darktable#22574. A catalog API
that all these functions would later be grouped behind is proposed there;
until it exists, the rules below are the ones new code follows.

See also:
- [Core_Concepts.md](Core_Concepts.md) - image, dev, history and the other core objects
- [GUI_Threading.md](GUI_Threading.md) - which thread a callback runs on, and the GUI critical section

---

## The Rules, in Short

- [SQL lives in `src/common/`](#where-the-sql-goes). GUI modules, views,
  Lua, `develop/` and `control/` call a function there; they never prepare a
  statement themselves.
- [A function is an operation of its domain](#what-a-catalog-function-looks-like),
  named `dt_<domain>_<verb>...()`, never a thin wrapper around a query
  string.
- [The header hides the database](#what-a-catalog-function-looks-like): no
  `sqlite3_stmt`, no SQL, no table name in a signature. Rows come back as
  values or as a `GList` of structs with a matching free function.
- [Moving is not changing](#moving-existing-sql). Moved SQL keeps its query,
  its order and its results; a fix found on the way gets its own commit.
- [Say who owns the cache](#caches). A function that only touches the
  database says so, and which in-memory cache the caller refreshes.
- [Every function has a test](#tests) in `src/tests/unittests/catalog/`.

## Where the SQL Goes

Each domain has its file in `src/common/`, and its functions go there:

| Domain | File | Main tables |
|---|---|---|
| color labels | `colorlabels.c` | `main.color_labels` |
| ratings | `ratings.c` | `main.images` (flags) |
| metadata | `metadata.c` | `data.meta_data`, `main.meta_data` |
| tags | `tags.c` | `data.tags`, `main.tagged_images` |
| styles | `styles.c` | `data.styles`, `data.style_items` |
| images, film rolls | `image.c`, `film.c` | `main.images`, `main.film_rolls` |
| presets | `presets.c` | `data.presets` |
| history | `history.c` | `main.history`, `main.masks_history`, `main.history_hash` |
| collection, selection | `collection.c`, `selection.c` | `memory.collected_images`, `main.selected_images` |

The schema itself, its migrations and maintenance stay in
`common/database.c`; they are not catalog functions.

Inside these files the usual conventions hold: statements go through
`dt_database_get(darktable.db)` and the `DT_DEBUG_SQLITE3_*` macros,
table names carry their schema (`main.`, `data.`, `memory.`), and SQL is
formatted by hand between `// clang-format off` and `// clang-format on`.

## What a Catalog Function Looks Like

It is named after its domain and what it does, and its header comment says
what comes back:

```c
// src/common/tags.h
/** get the ids of the tags attached to an image, darktable tags included */
GList *dt_tag_get_tag_ids(const dt_imgid_t imgid);

/** get the number of tags in the tag dictionary */
uint32_t dt_tag_count_tags(void);

// src/common/styles.h
/** name of the n-th style (from 0, by name), or NULL; free with g_free() */
gchar *dt_styles_get_nth_name(const int n);
```

When a caller needs several columns, return a list of small structs and a
function that frees one element, to pass to `g_list_free_full()`:

```c
// src/common/metadata.h
typedef struct dt_metadata_count_t
{
  uint32_t key;
  gchar *value;
  uint32_t count;
} dt_metadata_count_t;

GList *dt_metadata_get_counts(const char *images);
void dt_metadata_count_free(gpointer data);

// caller
GList *counts = dt_metadata_get_counts(images);
for(GList *c = counts; c; c = g_list_next(c)) { ... }
g_list_free_full(counts, dt_metadata_count_free);
```

Prefer an existing function to a new one, and one function that serves
several callers to one per caller. A trailing comment naming the caller
(`// libs/metadata.c`) helps while a function has a single one.

## Moving Existing SQL

Most catalog functions today come from moving a query out of a caller. The
move is a refactoring, reviewed as such:

- the query text, the bound values and the order of the results stay the
  same, so the caller sees exactly what it saw before;
- the caller's loop, locking and GUI updates stay in the caller; only the
  database access moves;
- a bug found while moving is fixed in a separate commit, or a separate
  PR, so that `git blame` on the moved code shows a move.

## Caches

Several domains keep an in-memory copy of what the database holds: the
image cache (`dt_image_cache_get()` and friends) and the metadata
definitions (`dt_metadata_get_metadata_by_keyid()`) are two. A catalog function that writes the database without updating that
copy says so in its header, and names what the caller refreshes:

```c
/** remove the metadata entries in the list and their image assignments,
    in the database only: the caller refreshes the cached list with
    dt_metadata_init() */
void dt_metadata_remove_keys(const GList *keys);
```

Keeping the refresh in the caller is fine when it runs under a lock or
next to GUI work that does not belong in `src/common/`; say it rather than
leave it implied.

## Tests

`src/tests/unittests/catalog/` starts darktable without a GUI on an
in-memory library, as darktable-cli does, imports a few images of the
integration test suite, and calls the catalog functions directly:

```c
#include "catalog_fixture.h"
#include "common/styles.h"

static void test_count_and_nth(void **state)
{
  const dt_imgid_t cr2 = dt_test_catalog_image("mire1.cr2");
  ...
}

int main(int argc, char *argv[])
{
  const struct CMUnitTest tests[] = {
    cmocka_unit_test(test_count_and_nth),
  };
  return cmocka_run_group_tests(tests, dt_test_catalog_setup, dt_test_catalog_teardown);
}
```

- add the suite to the `foreach` list of
  `src/tests/unittests/catalog/CMakeLists.txt`;
- check results against the database itself (a direct `SELECT COUNT(*)`
  in the test), not only through the function under test;
- the tests need the images of the `src/tests/integration` submodule and
  are configured out without them; they run with `ctest` after a build
  with `-DBUILD_TESTING=ON`.

## Common Mistakes

| Mistake | Instead |
|---|---|
| preparing a statement in `libs/`, `views/`, `lua/`, `develop/` or `control/` | call, or add, a function in the domain's `src/common/` file |
| returning a `sqlite3_stmt *`, or taking a `WHERE` clause as a string | return values or a list of row structs; take ids or flags |
| "fixing" the query while moving it | move it unchanged, fix in a separate commit |
| returning a list without saying how to free it | add the `*_free()` function and say so in the header |
| writing the database and leaving a cache silently stale | update the cache, or document who refreshes it |
| testing a function only through itself | compare with a direct query in the test |
