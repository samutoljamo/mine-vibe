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

/* ------------------------------------------------------------------ */
/*  Input handler tests                                                */
/* ------------------------------------------------------------------ */

static void test_hittest_inside(void)
{
    ui_input_begin();
    ui_add_element(7, 100.0f, 100.0f, 50.0f, 50.0f);
    /* cursor inside, not clicked */
    ui_handle_mouse(120.0f, 120.0f, false);
    assert(ui_hovered_element() == 7 && "cursor inside element must hover it");
    assert(ui_clicked_element() == -1 && "no click means no clicked element");
    printf("PASS: test_hittest_inside\n");
}

static void test_hittest_outside(void)
{
    ui_input_begin();
    ui_add_element(7, 100.0f, 100.0f, 50.0f, 50.0f);
    ui_handle_mouse(10.0f, 10.0f, false);
    assert(ui_hovered_element() == -1 && "cursor outside any element must not hover");
    printf("PASS: test_hittest_outside\n");
}

static void test_hittest_edges(void)
{
    ui_input_begin();
    ui_add_element(3, 100.0f, 100.0f, 50.0f, 50.0f);
    /* top-left corner is inside (inclusive), bottom-right edge is exclusive */
    ui_handle_mouse(100.0f, 100.0f, false);
    assert(ui_hovered_element() == 3 && "top-left corner is inside");
    ui_handle_mouse(150.0f, 150.0f, false);
    assert(ui_hovered_element() == -1 && "bottom-right edge is exclusive");
    printf("PASS: test_hittest_edges\n");
}

static void test_topmost_on_overlap(void)
{
    ui_input_begin();
    ui_add_element(1, 100.0f, 100.0f, 100.0f, 100.0f);
    ui_add_element(2, 120.0f, 120.0f, 100.0f, 100.0f); /* added later = on top */
    ui_handle_mouse(130.0f, 130.0f, false); /* inside both */
    assert(ui_hovered_element() == 2 && "later-added element wins on overlap");
    printf("PASS: test_topmost_on_overlap\n");
}

static void test_click_dispatch(void)
{
    ui_input_begin();
    ui_add_element(42, 0.0f, 0.0f, 200.0f, 200.0f);
    ui_handle_mouse(50.0f, 50.0f, true);
    assert(ui_hovered_element() == 42 && "clicked element is also hovered");
    assert(ui_clicked_element() == 42 && "click inside element sets clicked id");
    printf("PASS: test_click_dispatch\n");
}

static void test_click_empty_space_noop(void)
{
    ui_input_begin();
    ui_add_element(42, 0.0f, 0.0f, 50.0f, 50.0f);
    ui_handle_mouse(500.0f, 500.0f, true); /* click outside */
    assert(ui_hovered_element() == -1 && "no element hovered in empty space");
    assert(ui_clicked_element() == -1 && "click in empty space dispatches nothing");
    printf("PASS: test_click_empty_space_noop\n");
}

static void test_input_begin_resets(void)
{
    ui_input_begin();
    ui_add_element(9, 0.0f, 0.0f, 100.0f, 100.0f);
    ui_handle_mouse(10.0f, 10.0f, true);
    assert(ui_clicked_element() == 9);
    /* new frame clears registered elements and state */
    ui_input_begin();
    ui_handle_mouse(10.0f, 10.0f, true);
    assert(ui_hovered_element() == -1 && "elements cleared after input_begin");
    assert(ui_clicked_element() == -1 && "clicked state cleared after input_begin");
    printf("PASS: test_input_begin_resets\n");
}

static int g_cb_id = -1;
static void* g_cb_user = NULL;
static void click_cb(int id, void* user) { g_cb_id = id; g_cb_user = user; }

static void test_click_callback(void)
{
    int marker = 0;
    g_cb_id = -1; g_cb_user = NULL;
    ui_input_begin();
    ui_add_element_cb(5, 0.0f, 0.0f, 100.0f, 100.0f, click_cb, &marker);
    /* hover only: callback must not fire */
    ui_handle_mouse(10.0f, 10.0f, false);
    assert(g_cb_id == -1 && "callback must not fire on hover");
    /* click: callback fires with id and user pointer */
    ui_handle_mouse(10.0f, 10.0f, true);
    assert(g_cb_id == 5 && "click invokes callback with element id");
    assert(g_cb_user == &marker && "callback receives user pointer");
    printf("PASS: test_click_callback\n");
}

int main(void)
{
    test_font_bake_succeeds();
    test_text_width_empty();
    test_text_width_positive();
    test_text_width_scales();
    test_text_width_longer_is_wider();

    test_hittest_inside();
    test_hittest_outside();
    test_hittest_edges();
    test_topmost_on_overlap();
    test_click_dispatch();
    test_click_empty_space_noop();
    test_input_begin_resets();
    test_click_callback();

    printf("All ui tests passed.\n");
    return 0;
}
