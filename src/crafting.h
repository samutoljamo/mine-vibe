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

#endif /* CRAFTING_H */
