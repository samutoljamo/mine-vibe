#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <volk.h>
#include "../src/ui/ui.h"

static void test_font_bake_succeeds(void)
{
    bool ok = ui_font_bake();
    assert(ok && "font bake must succeed");
    printf("PASS: test_font_bake_succeeds\n");
}

static void test_text_width_empty(void)
{
    ui_font_bake();
    float w = ui_text_width("", 20.0f);
    assert(w == 0.0f);
    printf("PASS: test_text_width_empty\n");
}

static void test_text_width_positive(void)
{
    ui_font_bake();
    float w = ui_text_width("Hello", 20.0f);
    assert(w > 0.0f && "non-empty string must have positive width");
    printf("PASS: test_text_width_positive\n");
}

static void test_text_width_scales(void)
{
    ui_font_bake();
    float w20 = ui_text_width("ABC", 20.0f);
    float w40 = ui_text_width("ABC", 40.0f);
    /* 40px text should be approximately 2x the width of 20px text */
    assert(fabsf(w40 - 2.0f * w20) < 2.0f && "text width must scale with size");
    printf("PASS: test_text_width_scales\n");
}

static void test_text_width_longer_is_wider(void)
{
    ui_font_bake();
    float w1 = ui_text_width("A", 20.0f);
    float w3 = ui_text_width("AAA", 20.0f);
    assert(w3 > w1 && "longer string must be wider");
    printf("PASS: test_text_width_longer_is_wider\n");
}

int main(void)
{
    test_font_bake_succeeds();
    test_text_width_empty();
    test_text_width_positive();
    test_text_width_scales();
    test_text_width_longer_is_wider();
    printf("All ui tests passed.\n");
    return 0;
}
