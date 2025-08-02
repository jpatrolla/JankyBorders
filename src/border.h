#pragma once
#include <pthread.h>
#include "misc/helpers.h"
#include "misc/window.h"
#include "misc/drawing.h"
#include "animation.h"
#include "hashtable.h"
#include <stdint.h>
#include <CoreText/CoreText.h>
typedef union {
    uint32_t value;
    struct {
#if __BIG_ENDIAN__
        uint8_t a, r, g, b;  // adjust depending on platform endianness
#else
        uint8_t b, g, r, a;
#endif
    };
} color_t;

#define BORDER_ORDER_ABOVE 1
#define BORDER_ORDER_BELOW -1
#define BORDER_STYLE_ROUND  'r'
#define BORDER_STYLE_SQUARE 's'
#define BORDER_PADDING 20.0
#define BORDER_TSMN 3.27f
#define BORDER_TSMW 8.f
#define BORDER_RADIUS 9.f
#define BORDER_INNER_RADIUS 10.f

struct color_style {
  enum { COLOR_STYLE_GRADIENT, COLOR_STYLE_SOLID, COLOR_STYLE_GLOW } stype;
  union {
    uint32_t color;
    struct gradient gradient;
  };
};

struct settings {
  bool enabled;
  uint32_t apply_to;

  struct color_style active_window;
  struct color_style inactive_window;
  struct color_style background;

  float border_width;
  float blur_radius;
  char border_style;
  bool hidpi;
  bool show_background;
  int border_order;
  bool ax_focus;

  bool blacklist_enabled;
  struct table blacklist;

  bool whitelist_enabled;
  struct table whitelist;

  float fade_time;        /* duration of fade  (e.g. 0.20) */
  float fade_out_after;   /* idle timeout (e.g. 5.0)       */
};

struct event_buffer {
  bool disable_coalescing;
  volatile bool is_coalescing;
  int64_t last_coalesce_attempt;
};

struct border {
  pthread_mutex_t mutex;
  int cid;

  bool focused;
  bool needs_redraw;
  bool too_small;
  bool sticky;
  bool floating;
  bool attached; 
  bool modal;
  bool document;
  bool is_floating;
  bool is_sticky;
  bool is_pip;
  bool is_stack;
  bool zoom_level;
  bool destroy_queued;
  bool destroyed;

  uint64_t sid;
  uint32_t wid;
uint32_t target_wid;

  CGPoint origin;
  CGRect frame;
  CGRect target_bounds;
  CGRect drawing_bounds;
  CGContextRef context;

  struct animation animation;
  struct event_buffer event_buffer;

  bool is_proxy;
  struct border* proxy;
  volatile uint32_t external_proxy_wid;

  struct settings setting_override;
  uint32_t stack_id;
  int stack_index;
  int stack_len;
  bool stack_is_topmost_wid;
  struct border *stack_indicator_overlay;

  /* ───── Fade animation ────────────────────────────── */
  double   fade_start;    /* monotonic seconds when fade began      */
  float    fade_value;    /* 0-1: 0 = fully inactive, 1 = fully active */
  bool     fade_in;       /* true = fading to active, false = to inactive */
  double   last_focus_ts; /* time of last explicit focus event      */
  bool  fade_running;
  bool last_focused;
  };

struct border* border_create();
void border_init(struct border *border, int cid);
void border_destroy(struct border* border);

void border_move(struct border* border);
void border_update(struct border* border, bool try_async);
void border_hide(struct border* border);
void border_unhide(struct border* border);
void draw_floating_indicator(struct border *border);
void draw_sticky_indicator(struct border *border);
void draw_stack_indicators(struct border *border);
void border_redraw_all_stacked(struct table *windows);
void border_clear_stack_state(struct border *b);
struct settings* border_get_settings(struct border* border);
void borders_fade_tick(struct table *windows);

