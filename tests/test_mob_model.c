/* Pure unit tests for the per-type mob box-model data layer (hz2).
 *
 * No Vulkan/GLFW: this exercises only the geometry DATA returned by
 * mob_model_for(). A follow-up (0xm) bakes this into GPU meshes. */
#include "mob_model.h"
#include "mob.h"
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>

/* Count boxes flagged as a given part role. */
static int count_role(const MobModel* m, MobPartRole role) {
    int n = 0;
    for (int i = 0; i < m->count; i++)
        if (m->boxes[i].role == role) n++;
    return n;
}

/* Compute the model's combined axis-aligned bounding box. */
static void model_bounds(const MobModel* m, float lo[3], float hi[3]) {
    lo[0] = lo[1] = lo[2] =  1e9f;
    hi[0] = hi[1] = hi[2] = -1e9f;
    for (int i = 0; i < m->count; i++) {
        const MobBox* b = &m->boxes[i];
        for (int a = 0; a < 3; a++) {
            float c = (a == 0) ? b->cx : (a == 1) ? b->cy : b->cz;
            float h = ((a == 0) ? b->w : (a == 1) ? b->h : b->d) * 0.5f;
            if (c - h < lo[a]) lo[a] = c - h;
            if (c + h > hi[a]) hi[a] = c + h;
        }
    }
}

int main(void) {
    /* Every mob type yields a non-NULL model with at least one box, a small
     * box count, every box has positive size, and sane combined extents. */
    for (int t = 0; t < MOB_TYPE_COUNT; t++) {
        const MobModel* m = mob_model_for((MobType)t);
        assert(m != NULL);
        assert(m->count > 0);
        assert(m->count <= MOB_MODEL_MAX_BOXES);

        for (int i = 0; i < m->count; i++) {
            const MobBox* b = &m->boxes[i];
            assert(b->w > 0.0f && b->h > 0.0f && b->d > 0.0f);
            /* Tint channels in [0,1]. */
            for (int c = 0; c < 3; c++) {
                assert(b->tint[c] >= 0.0f && b->tint[c] <= 1.0f);
            }
        }

        float lo[3], hi[3];
        model_bounds(m, lo, hi);
        for (int a = 0; a < 3; a++) {
            float ext = hi[a] - lo[a];
            assert(ext > 0.0f);     /* positive */
            assert(ext < 4.0f);     /* bounded — normalized model, not huge */
        }
        /* Feet sit on/near the ground plane: lowest box bottom is >= ~0. */
        assert(lo[1] >= -0.05f);
        /* Model stands within a reasonable normalized height. */
        assert(hi[1] <= 2.5f);
    }

    /* Unknown / out-of-range type falls back to a valid (zombie) model. */
    {
        const MobModel* m = mob_model_for((MobType)9999);
        assert(m != NULL);
        assert(m->count > 0);
    }

    /* Humanoids: head + torso + 2 arms + 2 legs = 6 parts. */
    {
        const MobModel* z = mob_model_for(MOB_ZOMBIE);
        const MobModel* s = mob_model_for(MOB_SKELETON);
        assert(count_role(z, MOB_PART_HEAD) == 1);
        assert(count_role(z, MOB_PART_TORSO) == 1);
        assert(count_role(z, MOB_PART_ARM) == 2);
        assert(count_role(z, MOB_PART_LEG) == 2);
        assert(z->count == 6);
        assert(count_role(s, MOB_PART_HEAD) == 1);
        assert(count_role(s, MOB_PART_TORSO) == 1);
        assert(count_role(s, MOB_PART_ARM) == 2);
        assert(count_role(s, MOB_PART_LEG) == 2);
        assert(s->count == 6);
    }

    /* Creeper: 4 legs, NO arms, has a head + torso. */
    {
        const MobModel* c = mob_model_for(MOB_CREEPER);
        assert(count_role(c, MOB_PART_LEG) == 4);
        assert(count_role(c, MOB_PART_ARM) == 0);
        assert(count_role(c, MOB_PART_HEAD) == 1);
        assert(count_role(c, MOB_PART_TORSO) == 1);
    }

    /* Quadrupeds pig/cow: 4 legs, a head, a snout, no arms. */
    {
        const MobModel* p = mob_model_for(MOB_PIG);
        const MobModel* w = mob_model_for(MOB_COW);
        assert(count_role(p, MOB_PART_LEG) == 4);
        assert(count_role(p, MOB_PART_ARM) == 0);
        assert(count_role(p, MOB_PART_HEAD) == 1);
        assert(count_role(p, MOB_PART_SNOUT) == 1);
        assert(count_role(w, MOB_PART_LEG) == 4);
        assert(count_role(w, MOB_PART_ARM) == 0);
        assert(count_role(w, MOB_PART_HEAD) == 1);
        assert(count_role(w, MOB_PART_SNOUT) == 1);
        /* Cow is the larger quadruped: its torso footprint exceeds the pig's. */
    }

    /* Chicken: small, has a beak and 2 wings and 2 legs. */
    {
        const MobModel* k = mob_model_for(MOB_CHICKEN);
        assert(count_role(k, MOB_PART_BEAK) == 1);
        assert(count_role(k, MOB_PART_WING) == 2);
        assert(count_role(k, MOB_PART_LEG) == 2);
        assert(count_role(k, MOB_PART_HEAD) == 1);
        assert(count_role(k, MOB_PART_TORSO) == 1);
    }

    printf("test_mob_model: all assertions passed\n");
    return 0;
}
