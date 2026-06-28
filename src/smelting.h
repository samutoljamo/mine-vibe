#ifndef SMELTING_H
#define SMELTING_H

#include <stdint.h>
#include <stdbool.h>
#include "item.h"

/* ------------------------------------------------------------------ */
/*  Smelting / furnace model                                           */
/*                                                                     */
/*  All functions here are PURE — they read their arguments and the     */
/*  static recipe / fuel tables and have no I/O, so the same query       */
/*  always returns the same answer (determinism). This mirrors the       */
/*  crafting module: a data-driven table is the single source of truth   */
/*  shared by the server (which validates + executes smelting) and the   */
/*  client UI. The furnace tick is a deterministic state transition so   */
/*  it can be unit-tested and replayed identically on client + server.   */
/* ------------------------------------------------------------------ */

/* Ticks of cook progress required to complete one smelt (vanilla: 200). */
#define SMELT_TICKS_PER_ITEM 200

/* Look up the smelting result for `input`. Returns true and writes the
 * result ItemId to *out_result when `input` is a smeltable item; returns
 * false (and leaves *out_result untouched) otherwise. out_result may be
 * NULL to query only smeltability. Pure. */
bool smelting_result(ItemId input, ItemId* out_result);

/* Burn time in ticks contributed by one unit of `fuel`. Non-fuel items
 * (including BLOCK_AIR) return 0. Pure lookup. */
int  fuel_burn_ticks(ItemId fuel);

/* Convenience predicate: true iff `fuel` yields a positive burn time. */
static inline bool is_fuel(ItemId fuel) { return fuel_burn_ticks(fuel) > 0; }

/* ------------------------------------------------------------------ */
/*  Furnace container state                                            */
/*                                                                     */
/*  Three logical slots: input (the item being smelted), fuel, and       */
/*  output (the produced item). Each carries an ItemId + count. The       */
/*  furnace also tracks remaining burn ticks from the currently-lit fuel  */
/*  unit and the cook progress of the in-flight smelt. A furnace is        */
/*  "fully empty" with all counts 0 / items BLOCK_AIR and both timers 0.   */
/* ------------------------------------------------------------------ */
typedef struct FurnaceState {
    ItemId  input;            /* item in the input slot (BLOCK_AIR if empty) */
    uint8_t input_count;

    ItemId  fuel;             /* item in the fuel slot (BLOCK_AIR if empty) */
    uint8_t fuel_count;

    ItemId  output;           /* item in the output slot (BLOCK_AIR if empty) */
    uint8_t output_count;

    int     burn_ticks_left;  /* ticks of fuel burn remaining from a lit unit */
    int     cook_progress;    /* ticks of cook accumulated toward current smelt */
} FurnaceState;

/* Max items a furnace output slot can hold before it is "full" and stalls. */
#define FURNACE_STACK_MAX 64

/* Returns true iff the furnace currently has a valid smelt available: the
 * input is smeltable, there is at least one input item, and the output slot
 * has room for the result (empty, or same item below the stack cap). Pure. */
bool furnace_can_smelt(const FurnaceState* f);

/* Advance the furnace by `dt_ticks` ticks (dt_ticks >= 0). Deterministic and
 * pure aside from mutating *f:
 *
 *   - Each tick, if there is a valid smelt and the furnace is lit (or can be
 *     lit by consuming one fuel unit), it burns fuel and advances cook
 *     progress; otherwise it idles.
 *   - Lighting consumes exactly one fuel unit, setting burn_ticks_left to that
 *     fuel's burn time. Fuel is only consumed when a smelt is actually pending
 *     (a lit-but-idle furnace does not waste fuel starting a new unit).
 *   - When cook_progress reaches SMELT_TICKS_PER_ITEM, one input is consumed,
 *     one result is added to the output slot, and cook_progress resets,
 *     carrying any overflow ticks into the next smelt.
 *   - Without fuel, or with a full output, cook progress does not advance;
 *     partially-cooked progress is preserved so it resumes when unblocked.
 *
 * Processing one tick at a time keeps the result identical whether called with
 * dt_ticks=N once or 1 tick N times. */
void furnace_tick(FurnaceState* f, int dt_ticks);

#endif /* SMELTING_H */
