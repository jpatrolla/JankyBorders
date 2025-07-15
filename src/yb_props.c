#include "misc/yabai.h"
#include "hashtable.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include "misc/hash_util.h"       // hash_u32 / cmp_u32

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
        "jq -r '.[] | select(.\"is-stacked\"==true) | .id'", "r");
    if (!fp) return;

    char buf[32];
    while (fgets(buf, sizeof buf, fp)) {
        uint32_t wid = (uint32_t)strtoul(buf, NULL, 10);
        yb_props_t *p = props_for(wid, true);
        p->is_stacked    = true;
    }
    pclose(fp);
}
static void mark_floating_windows(void)
{
    FILE *fp = popen(
        "yabai -m query --windows | "
        "jq -r '.[] | select(.\"is-floating\"==true) | .id'", "r");
    if (!fp) return;

    char buf[32];
    while (fgets(buf, sizeof buf, fp)) {
        uint32_t wid = (uint32_t)strtoul(buf, NULL, 10);
        yb_props_t *p = props_for(wid, true);
        p->is_floating   = true;
    }
    pclose(fp);
}

void yb_props_bootstrap(void)
{
    table_clear(&yb_props);           /* One‑shot population at startup */
    mark_floating_windows();
    mark_sticky_windows();
    mark_stacked_windows();
    mark_pip_windows();
}