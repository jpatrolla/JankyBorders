#include "misc/yabai.h"
#include "hashtable.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "misc/hash_util.h"       // hash_u32 / cmp_u32
#include <json-c/json.h>
struct table yb_props;

void yabai_props_init(void){table_init(&yb_props, 256, hash_u32, cmp_u32);}
void yabai_props_free(void){table_free(&yb_props);}

static inline yb_props_t * props_for(uint32_t wid, bool create_if_missing)
{
    yb_props_t *p = table_find(&yb_props, &wid);
    if (!p && create_if_missing) {
        p = calloc(1, sizeof(yb_props_t));
        _table_add(&yb_props, &wid, sizeof(uint32_t), p);
    }
    return p;
}
yb_props_t *yb_props_get(uint32_t wid, bool create_if_missing)
{
    return props_for(wid, create_if_missing);
}
static void mark_sticky_windows(void) {
	FILE *fp = popen(
        "yabai -m query --windows | "
        "jq -r '.[] | select(.\"is-sticky\"==true) | .id'", "r");
    if (!fp) return;

    char buf[32];
    while (fgets(buf, sizeof buf, fp)) {
        uint32_t wid = (uint32_t)strtoul(buf, NULL, 10);
        yb_props_t *p = props_for(wid, true);
        p->is_sticky     = true;
    }
    pclose(fp);
}
static void mark_pip_windows(void) {
	FILE *fp = popen(
        "yabai -m query --windows | "
        "jq -r '.[] | select(.\"is-pip\"==true) | .id'", "r");
    if (!fp) return;

    char buf[32];
    while (fgets(buf, sizeof buf, fp)) {
        uint32_t wid = (uint32_t)strtoul(buf, NULL, 10);
        yb_props_t *p = props_for(wid, true);
        p->is_pip        = true;
    }
    pclose(fp);
}
static void mark_stacked_windows(void) {
    FILE *fp = popen(
        "yabai -m query --windows | "
        "jq -r '.[] | select(.\"stack-index\" != null) | [.id, .\"stack-index\"] | @tsv'", "r");
    if (!fp) return;

    char buf[64];
    while (fgets(buf, sizeof buf, fp)) {
        uint32_t wid, index;
        if (sscanf(buf, "%u\t%u", &wid, &index) == 2) {
            yb_props_t *p = props_for(wid, true);
            if (p) p->stack_index = index;
        }
    }
    pclose(fp);
}
static void mark_floating_windows(void)
{
    debug("🟦🟦🟦🟦  Marking floating windows\n");

    FILE *fp = popen(
        "yabai -m query --windows | "
        "jq -r '.[] | [.id, .\"is-floating\"] | @tsv'", "r");
    if (!fp) return;

    char buf[64];
    while (fgets(buf, sizeof(buf), fp)) {
        uint32_t wid;
        int is_floating;

        if (sscanf(buf, "%u\t%d", &wid, &is_floating) == 2) {
            yb_props_t *p = props_for(wid, true);
            if (p) {
                p->is_floating = is_floating;
                debug("Window %d is floating: %d\n", wid, is_floating);
            }
        }
    }

    pclose(fp);
}
void mark_all_window_flags(void)
{
    debug("✅✅✅✅  Marking all window flags ✅✅✅✅\n");

    FILE *fp = popen(
    "yabai -m query --windows | "
    "jq -r '.[] | "
          "[ .id, "
          "  (.\"is-sticky\"   | if . then 1 else 0 end), "
          "  (.\"is-pip\"      | if . then 1 else 0 end), "
          "  (.\"stack-index\" // -1), "
          "  (.\"is-floating\" | if . then 1 else 0 end) ] "
          "| @tsv'", "r");
    if (!fp) return;

    char buf[128];
    while (fgets(buf, sizeof(buf), fp)) {
        uint32_t wid;
        int is_sticky = 0, is_pip = 0, stack_index = -1, is_floating = 0;

        // Handle optional fields by allowing nulls to be empty strings, and using sscanf carefully
        // Null stack-index becomes empty field, sscanf will fail with fewer matches
        int fields = sscanf(buf, "%u\t%d\t%d\t%d\t%d", &wid, &is_sticky, &is_pip, &stack_index, &is_floating);
        if (fields < 2) continue;

        yb_props_t *p = props_for(wid, true);
        if (!p) continue;

        if (fields >= 2) p->is_sticky   = is_sticky;
        if (fields >= 3) p->is_pip      = is_pip;
        if (fields >= 4 && stack_index >= 0) p->stack_index = stack_index;
        if (fields >= 5) p->is_floating = is_floating;

        debug("↪️ Window %u → sticky: %d, pip: %d, stack: %d, floating: %d\n",
              wid, is_sticky, is_pip, stack_index, is_floating);
    }

    pclose(fp);
}
void yb_props_bootstrap(void)
{
    table_clear(&yb_props);           /* One‑shot population at startup */
    mark_all_window_flags();
    // mark_floating_windows();
    // mark_sticky_windows();
    // mark_stacked_windows();
    // mark_pip_windows();
}