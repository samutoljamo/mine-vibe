#include "smelting.h"
#include "block.h"
#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Smelting recipe table — the single source of truth                 */
/*                                                                     */
/*  Each entry maps one smeltable input ItemId to one result ItemId.    */
/*  Data-driven so new recipes are a one-line append. Only existing      */
/*  ItemIds / BlockIDs are used: gold ore is intentionally absent until  */
/*  a gold-ingot item exists. Coal is represented by BLOCK_COAL_ORE      */
/*  (there is no separate coal item yet), matching crafting.c.           */
/* ------------------------------------------------------------------ */
typedef struct {
    ItemId input;
    ItemId result;
} SmeltRecipe;

static const SmeltRecipe g_smelt_table[] = {
    { (ItemId)BLOCK_IRON_ORE, ITEM_IRON_INGOT     },
    { (ItemId)BLOCK_SAND,     (ItemId)BLOCK_GLASS },
    { ITEM_RAW_PORK,          ITEM_COOKED_PORK    },
    { ITEM_RAW_BEEF,          ITEM_COOKED_BEEF    },
    { ITEM_RAW_CHICKEN,       ITEM_COOKED_CHICKEN },
};

#define SMELT_COUNT ((int)(sizeof(g_smelt_table) / sizeof(g_smelt_table[0])))

bool smelting_result(ItemId input, ItemId* out_result) {
    for (int i = 0; i < SMELT_COUNT; i++) {
        if (g_smelt_table[i].input == input) {
            if (out_result) *out_result = g_smelt_table[i].result;
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ */
/*  Fuel table                                                         */
/*                                                                     */
/*  Burn ticks per unit. Tuned relative to SMELT_TICKS_PER_ITEM (200):   */
/*  coal smelts several items per unit; a plank burns less than one full  */
/*  smelt; a wood log sits between. Non-fuel items return 0.             */
/* ------------------------------------------------------------------ */
typedef struct {
    ItemId fuel;
    int    ticks;
} FuelEntry;

static const FuelEntry g_fuel_table[] = {
    { (ItemId)BLOCK_COAL_ORE, 800 },  /* coal: ~4 smelts per unit */
    { (ItemId)BLOCK_WOOD,     300 },  /* log: 1.5 smelts per unit */
    { (ItemId)BLOCK_PLANKS,   150 },  /* plank: < 1 smelt per unit */
};

#define FUEL_COUNT ((int)(sizeof(g_fuel_table) / sizeof(g_fuel_table[0])))

int fuel_burn_ticks(ItemId fuel) {
    if (fuel == (ItemId)BLOCK_AIR) return 0;
    for (int i = 0; i < FUEL_COUNT; i++) {
        if (g_fuel_table[i].fuel == fuel) return g_fuel_table[i].ticks;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Furnace state machine                                              */
/* ------------------------------------------------------------------ */

bool furnace_can_smelt(const FurnaceState* f) {
    if (!f || f->input_count == 0) return false;

    ItemId result;
    if (!smelting_result(f->input, &result)) return false;

    /* Output must be empty, or already holding the same result below the cap. */
    if (f->output_count == 0) return true;
    if (f->output != result) return false;
    if (f->output_count >= FURNACE_STACK_MAX) return false;
    return true;
}

void furnace_tick(FurnaceState* f, int dt_ticks) {
    if (!f || dt_ticks <= 0) return;

    for (int t = 0; t < dt_ticks; t++) {
        bool smeltable = furnace_can_smelt(f);

        /* No valid smelt: the furnace idles. We do not advance cook progress,
         * and we do not start a new fuel unit (a lit furnace keeps any
         * remaining burn ticks but they only count down while it cooks). */
        if (!smeltable) {
            /* Without a pending smelt, burn time is not consumed: a furnace
             * only burns while actually smelting. */
            continue;
        }

        /* Light a fresh fuel unit if the furnace is not currently lit. */
        if (f->burn_ticks_left <= 0) {
            int bt = fuel_burn_ticks(f->fuel);
            if (bt <= 0 || f->fuel_count == 0) {
                /* No usable fuel: cannot advance, progress preserved. */
                continue;
            }
            f->fuel_count--;
            if (f->fuel_count == 0) f->fuel = (ItemId)BLOCK_AIR;
            f->burn_ticks_left = bt;
        }

        /* Burn one tick and advance cook progress one tick. */
        f->burn_ticks_left--;
        f->cook_progress++;

        if (f->cook_progress >= SMELT_TICKS_PER_ITEM) {
            ItemId result;
            /* furnace_can_smelt guaranteed a valid recipe + output space. */
            smelting_result(f->input, &result);

            f->cook_progress -= SMELT_TICKS_PER_ITEM;  /* carry overflow */

            /* Consume one input. */
            f->input_count--;
            if (f->input_count == 0) f->input = (ItemId)BLOCK_AIR;

            /* Produce one result into the output slot. */
            if (f->output_count == 0) f->output = result;
            f->output_count++;
        }
    }
}
