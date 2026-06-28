#ifndef MINING_H
#define MINING_H

#include <stdbool.h>
#include "item.h"
#include "block.h"

/* ------------------------------------------------------------------ */
/*  Mining: tool/block speed multiplier + harvest-level drop gating    */
/*                                                                     */
/*  Pure module (no Vulkan/GLFW, no server/net). Two concerns:         */
/*                                                                     */
/*    1. Speed — how much faster the *correct* tool mines a block.     */
/*       mining_speed_multiplier(tool, block) returns the factor by    */
/*       which block_break_time(block) is divided to get the effective */
/*       break time. A wrong/no tool yields 1.0 (slow, hand speed).    */
/*                                                                     */
/*    2. Drops — whether breaking a block actually yields an item.     */
/*       A block has a required harvest level; a tool has a harvest    */
/*       level. block_drops_with(tool, block) is true only when the    */
/*       tool is the correct *kind* for the block AND its harvest      */
/*       level meets the block's requirement. Below that, the block    */
/*       can still be mined but drops nothing.                         */
/*                                                                     */
/*  Both are driven by small tables (see mining.c) so a new material   */
/*  tier (diamond) or a new gated block is a single row.               */
/* ------------------------------------------------------------------ */

/* Speed multiplier applied to mining when holding `tool` against `block`.
 * Returns 1.0 for a bare hand, a block item, or a wrong-category tool.
 * When the tool is the right kind for the block, returns the material tier's
 * multiplier (strictly increasing wood < stone < iron < ...). The effective
 * break time is block_break_time(block) / mining_speed_multiplier(...). */
float mining_speed_multiplier(ItemId tool, BlockID block);

/* Harvest level a tool provides (hand/block item = 0; wood < stone < iron).
 * Independent of the tool kind — gating also checks the kind separately. */
int tool_harvest_level(ItemId tool);

/* Harvest level required to obtain drops from `block` (0 = no requirement,
 * harvestable by hand). Stone needs a wooden pickaxe (1); iron/gold ore need
 * stone (2); diamond ore needs iron (3). */
int block_required_harvest_level(BlockID block);

/* True iff breaking `block` with `tool` yields a drop: the tool must be the
 * correct kind for the block (when the block requires one) AND its harvest
 * level must meet the block's requirement. Blocks with no requirement always
 * drop (even by hand). */
bool block_drops_with(ItemId tool, BlockID block);

#endif /* MINING_H */
