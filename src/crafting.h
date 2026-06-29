#ifndef CRAFTING_H
#define CRAFTING_H

#include <stdint.h>
#include <stdbool.h>
#include "item.h"

/* ------------------------------------------------------------------ */
/*  Crafting recipe model                                              */
/*                                                                     */
/*  Recipes are *shapeless*: a recipe is a small set of ingredient     */
/*  stacks ({ItemId, count}) that produce one output stack. Matching   */
/*  is done against per-item counts (an "ItemCounts" snapshot of an     */
/*  inventory), not a positional grid, so it is order-independent and   */
/*  trivial to test. All functions here are PURE — they read their      */
/*  arguments and the static recipe table and have no side effects, so  */
/*  the same query always returns the same answer (determinism).        */
/*                                                                     */
/*  The recipe table is the single source of truth shared by the        */
/*  server (which validates + executes a craft) and the client UI       */
/*  (which lists affordable recipes). Recipe identity is its index in    */
/*  the table, which is what travels on the wire in PKT_CRAFT.           */
/* ------------------------------------------------------------------ */

#define CRAFT_MAX_INPUTS 3   /* max distinct ingredient stacks per recipe */

typedef struct {
    ItemId  item;
    uint8_t count;
} ItemStack;

typedef struct {
    const char* name;                       /* human label for the UI */
    ItemStack   inputs[CRAFT_MAX_INPUTS];    /* unused entries have count 0 */
    int         input_count;                 /* 1..CRAFT_MAX_INPUTS */
    ItemStack   output;
} Recipe;

/* ------------------------------------------------------------------ */
/*  Item-count snapshot                                                 */
/*                                                                     */
/*  A dense per-item tally used for affordability checks. Built from an  */
/*  inventory by summing matching stacks across all slots. ITEM_COUNT    */
/*  entries cover every block + tool + craftable id, so an ItemId can be  */
/*  used directly as the index. Counts saturate at UINT16_MAX.           */
/* ------------------------------------------------------------------ */
typedef struct {
    uint16_t n[ITEM_COUNT];
} ItemCounts;

/* Number of recipes in the table. */
int           crafting_recipe_count(void);

/* Recipe by index, or NULL if out of range. */
const Recipe* crafting_recipe(int index);

/* True iff `counts` holds at least every ingredient of `recipe`. Pure.
 * A NULL recipe returns false. */
bool          crafting_can_make(const Recipe* recipe, const ItemCounts* counts);

/* Find the first recipe in table order whose ingredients are all present in
 * `counts`, returning its index, or -1 if none is affordable. Deterministic
 * (table order is stable). */
int           crafting_find(const ItemCounts* counts);

/* Fill `out_indices` with the table indices of every recipe affordable given
 * `counts`, in stable table order. Writes at most `max` indices and returns the
 * number written. Pure. The client UI uses this so the listed rows and their
 * click targets share one ordering. */
int           crafting_affordable(const ItemCounts* counts,
                                  int* out_indices, int max);

/* Build an ItemCounts snapshot from an inventory (sum of matching stacks across
 * slots). Convenience for callers that have an Inventory; the pure matchers
 * above take ItemCounts so they stay inventory-agnostic and unit-testable. */
struct Inventory;
void          crafting_counts_from_inventory(const struct Inventory* inv,
                                             ItemCounts* out);

/* ================================================================== */
/*  Shaped 3x3 crafting (a4s.5.3 engine)                               */
/*                                                                     */
/*  This is the ADDITIVE grid-based crafting model that runs alongside  */
/*  the shapeless API above (which is unchanged and still used by the   */
/*  current server/HUD/client). A future ticket migrates the callers    */
/*  over to this matcher and bumps the protocol; until then both        */
/*  coexist.                                                            */
/*                                                                     */
/*  A ShapedRecipe is a 3x3 ItemId grid (row-major, index = y*3 + x;    */
/*  cell value 0 / BLOCK_AIR means empty). It produces output_count of   */
/*  output_item.                                                        */
/*                                                                     */
/*    - shapeless == false: a positional pattern. Matching is done      */
/*      modulo TRANSLATION (the pattern's bounding box may sit anywhere  */
/*      in the 3x3, as in vanilla) and modulo horizontal MIRROR.        */
/*    - shapeless == true: cells are an unordered ingredient multiset;   */
/*      matching ignores placement entirely.                            */
/*                                                                     */
/*  All functions are PURE + deterministic.                            */
/* ================================================================== */

#define CRAFT_GRID  3                    /* 3x3 grid */
#define CRAFT_CELLS (CRAFT_GRID * CRAFT_GRID)  /* 9 cells */

typedef struct {
    ItemId      cells[CRAFT_CELLS];  /* row-major; 0 / BLOCK_AIR == empty */
    ItemId      output_item;
    uint8_t     output_count;
    bool        shapeless;           /* true: cells are an unordered multiset */
    const char* name;                /* human label for the UI */
} ShapedRecipe;

/* Number of shaped recipes in the table. */
int  crafting_shaped_count(void);

/* Shaped recipe by index, or NULL if out of range. */
const ShapedRecipe* crafting_shaped(int index);

/* Match a filled 3x3 grid against the shaped recipe table.
 *
 * Returns the index of the FIRST matching shaped recipe (stable table order),
 * or -1 if none match. `grid` is row-major (index = y*CRAFT_GRID + x); a cell
 * of 0 / BLOCK_AIR is empty. Shaped recipes match modulo translation + mirror;
 * shapeless recipes match by ingredient multiset. An all-empty grid -> -1.
 * Pure. */
int  crafting_match_grid(const ItemId grid[CRAFT_CELLS]);

/* Output of a shaped recipe by index. Returns false (and leaves outputs
 * untouched) for an out-of-range index. Pure. */
bool shaped_recipe_output(int index, ItemId* out_item, uint8_t* out_count);

#endif /* CRAFTING_H */
