#include "server_block_entity.h"
#include "server.h"        /* Server: block_entities array + count/cap */
#include "block.h"         /* BLOCK_FURNACE / BLOCK_CHEST */
#include "smelting.h"      /* FurnaceState + furnace_tick */
#include "container.h"     /* Container + transfer helpers (ItemStack lives here) */
#include <stdlib.h>
#include <string.h>

/* Full definition of the opaque BlockEntity (see server_block_entity.h for why
 * it is here and not in server.h). `viewers` is a bitmask of client indices. */
struct BlockEntity {
    int     x, y, z;
    SbeType type;
    uint32_t viewers;
    union {
        FurnaceState furnace;
        Container    chest;
    } u;
};

_Static_assert(CHEST_SLOTS == CONTAINER_NET_CHEST_SLOTS,
    "chest wire slot count must match the container model");

BlockEntity* sbe_find(Server* s, int x, int y, int z) {
    for (size_t i = 0; i < s->block_entity_count; i++) {
        BlockEntity* be = &s->block_entities[i];
        if (be->x == x && be->y == y && be->z == z) return be;
    }
    return NULL;
}

BlockEntity* sbe_create(Server* s, int x, int y, int z, SbeType type) {
    BlockEntity* ex = sbe_find(s, x, y, z);
    if (ex) return ex;
    if (s->block_entity_count == s->block_entity_cap) {
        size_t ncap = s->block_entity_cap ? s->block_entity_cap * 2 : 8;
        BlockEntity* na = realloc(s->block_entities, ncap * sizeof(*na));
        if (!na) return NULL;        /* OOM: skip the entity (block still placed) */
        s->block_entities  = na;
        s->block_entity_cap = ncap;
    }
    BlockEntity* be = &s->block_entities[s->block_entity_count++];
    memset(be, 0, sizeof(*be));
    be->x = x; be->y = y; be->z = z; be->type = type; be->viewers = 0;
    if (type == SBE_CHEST) container_init(&be->u.chest);
    /* FURNACE: a zeroed FurnaceState is a valid empty furnace. */
    return be;
}

/* Pour an entity's contents into `inv` (anything that doesn't fit is lost). */
static void sbe_pour_contents(BlockEntity* be, Inventory* inv) {
    if (!inv) return;
    if (be->type == SBE_CHEST) {
        for (int i = 0; i < CHEST_SLOTS; i++) {
            ItemStack* st = &be->u.chest.slots[i];
            if (st->count == 0) continue;
            inventory_add_item(inv, st->item, st->count);
        }
    } else {
        FurnaceState* f = &be->u.furnace;
        if (f->input_count)  inventory_add_item(inv, f->input,  f->input_count);
        if (f->fuel_count)   inventory_add_item(inv, f->fuel,   f->fuel_count);
        if (f->output_count) inventory_add_item(inv, f->output, f->output_count);
    }
}

void sbe_destroy_return(Server* s, int x, int y, int z, Inventory* inv) {
    for (size_t i = 0; i < s->block_entity_count; i++) {
        BlockEntity* be = &s->block_entities[i];
        if (be->x == x && be->y == y && be->z == z) {
            sbe_pour_contents(be, inv);
            /* swap-remove (order doesn't matter — found by position). */
            s->block_entities[i] = s->block_entities[s->block_entity_count - 1];
            s->block_entity_count--;
            return;
        }
    }
}

void sbe_free_all(Server* s) {
    free(s->block_entities);
    s->block_entities    = NULL;
    s->block_entity_count = 0;
    s->block_entity_cap   = 0;
}

bool sbe_block_is_container(uint8_t b, SbeType* out_type) {
    if (b == BLOCK_FURNACE) { if (out_type) *out_type = SBE_FURNACE; return true; }
    if (b == BLOCK_CHEST)   { if (out_type) *out_type = SBE_CHEST;   return true; }
    return false;
}

void sbe_add_viewer(BlockEntity* be, int idx)    { be->viewers |=  (1u << idx); }
void sbe_remove_viewer(BlockEntity* be, int idx) { be->viewers &= ~(1u << idx); }

void sbe_clear_viewer_all(Server* s, int idx) {
    uint32_t bit = ~(1u << idx);
    for (size_t i = 0; i < s->block_entity_count; i++)
        s->block_entities[i].viewers &= bit;
}

int sbe_slot_count(const BlockEntity* be) {
    return (be->type == SBE_CHEST) ? CHEST_SLOTS : 3;  /* furnace: in/fuel/out */
}

uint32_t sbe_viewers(const BlockEntity* be) { return be->viewers; }

void sbe_fill_state(const BlockEntity* be, ContainerStatePacket* p) {
    memset(p, 0, sizeof(*p));
    p->x = be->x; p->y = be->y; p->z = be->z;
    if (be->type == SBE_FURNACE) {
        const FurnaceState* f = &be->u.furnace;
        p->type = CONTAINER_NET_FURNACE;
        p->f_input  = (uint16_t)f->input;  p->f_input_count  = f->input_count;
        p->f_fuel   = (uint16_t)f->fuel;   p->f_fuel_count   = f->fuel_count;
        p->f_output = (uint16_t)f->output; p->f_output_count = f->output_count;
        p->f_burn_ticks_left = f->burn_ticks_left;
        p->f_cook_progress   = f->cook_progress;
    } else {
        p->type = CONTAINER_NET_CHEST;
        for (int i = 0; i < CHEST_SLOTS; i++) {
            p->slots[i].item  = (uint16_t)be->u.chest.slots[i].item;
            p->slots[i].count = be->u.chest.slots[i].count;
        }
    }
}

void sbe_transfer(BlockEntity* be, uint8_t slot, uint8_t dir, uint8_t count,
                  Inventory* inv) {
    if (be->type == SBE_CHEST) {
        if (dir == CONTAINER_DIR_TO_INV)
            container_transfer_to_inventory(&be->u.chest, slot, inv, count);
        else
            container_transfer_from_inventory(inv, slot, &be->u.chest, count);
        return;
    }
    /* Furnace: map its 3 slots onto a temporary Container so we can reuse the
     * same transfer helpers, then copy the touched slots back. */
    FurnaceState* f = &be->u.furnace;
    Container tmp; container_init(&tmp);
    tmp.slots[0].item = f->input;  tmp.slots[0].count = f->input_count;
    tmp.slots[1].item = f->fuel;   tmp.slots[1].count = f->fuel_count;
    tmp.slots[2].item = f->output; tmp.slots[2].count = f->output_count;
    if (dir == CONTAINER_DIR_TO_INV)
        container_transfer_to_inventory(&tmp, slot, inv, count);
    else
        container_transfer_from_inventory(inv, slot, &tmp, count);
    f->input  = tmp.slots[0].item;  f->input_count  = tmp.slots[0].count;
    f->fuel   = tmp.slots[1].item;  f->fuel_count   = tmp.slots[1].count;
    f->output = tmp.slots[2].item;  f->output_count = tmp.slots[2].count;
}

void sbe_tick_furnaces(Server* s, int dt_ticks,
                       void (*on_change)(void* user, const BlockEntity* be),
                       void* user) {
    if (dt_ticks <= 0) return;
    for (size_t i = 0; i < s->block_entity_count; i++) {
        BlockEntity* be = &s->block_entities[i];
        if (be->type != SBE_FURNACE) continue;
        FurnaceState before = be->u.furnace;
        furnace_tick(&be->u.furnace, dt_ticks);
        if (be->viewers && on_change &&
            memcmp(&before, &be->u.furnace, sizeof(FurnaceState)) != 0)
            on_change(user, be);
    }
}
