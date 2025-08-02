#include "border.h"
#include "hashtable.h"
#include "windows.h"
#include <pthread.h>
#include "misc/yabai.h"
#include <time.h>
#include <dispatch/dispatch.h>

extern struct settings g_settings;
CGFloat indicator_offset = 20.0;

struct settings *border_get_settings(struct border *border) {
  assert(pthread_main_np() != 0);
  return border->setting_override.enabled ? &border->setting_override
                                          : &g_settings;
}

static void border_destroy_window(struct border *border)
{
    if (!border) return;

    /* CoreGraphics context */
    if (border->context) {
        CGContextRef ctx = border->context;
        border->context  = NULL;        /* prevent re-release */
        CGContextRelease(ctx);
    }

    /* SLS window */
    if (border->wid &&
        border->cid != 0 &&
        border->cid != SLSMainConnectionID())
    {
        uint32_t wid = border->wid;
        border->wid  = 0;               /* mark consumed */
        SLSReleaseWindow(border->cid, wid);
    }
}

static bool border_check_too_small(struct border *border, CGRect window_frame) {
  CGRect smallest_rect = CGRectInset(window_frame, 1.0, 1.0);
  if (smallest_rect.size.width < 2.f * BORDER_INNER_RADIUS ||
      smallest_rect.size.height < 2.f * BORDER_INNER_RADIUS) {
    return true;
  }
  return false;
}

static bool border_coalesce_resize_and_move_events(struct border *border,
                                                   CGRect *frame) {
  if (border->event_buffer.disable_coalescing || pthread_main_np() != 0 ||
      !border->wid) {
    debug("Coalescing disabled or not on main thread or no window id\n");
    SLSGetWindowBounds(border->cid, border->target_wid, frame);
    return true;
  }
  int64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW_APPROX);
  int64_t dt = now - border->event_buffer.last_coalesce_attempt;
  border->event_buffer.last_coalesce_attempt = now;

  if (border->event_buffer.is_coalescing){
    debug("Coalescing is already in progress for border %d\n", border->cid);
    return false;}

  CGRect window_frame;
  SLSGetWindowBounds(border->cid, border->target_wid, &window_frame);
  bool coalesceable =
      (CGPointEqualToPoint(window_frame.origin, border->target_bounds.origin) &&
       !CGSizeEqualToSize(window_frame.size, border->target_bounds.size)) ||
      (!CGPointEqualToPoint(window_frame.origin,
                            border->target_bounds.origin) &&
       CGSizeEqualToSize(window_frame.size, border->target_bounds.size));

  if (coalesceable && dt > (1ULL << 27)) {
    debug("Coalescing resize and move events for border %d\n", border->cid);
    border->event_buffer.is_coalescing = true;
    pthread_mutex_unlock(&border->mutex);
    usleep(20000);
    pthread_mutex_lock(&border->mutex);
    border->event_buffer.is_coalescing = false;
    debug("Coalescing complete for border %d\n", border->cid);
    if (border->external_proxy_wid)
      return false;
    SLSGetWindowBounds(border->cid, border->target_wid, frame);
    return true;
  }

  *frame = window_frame;
  return true;
}

static bool border_calculate_bounds(struct border *border, CGRect *frame,
                                    struct settings *settings) {
  CGRect window_frame;
  if (border->is_proxy){
    window_frame = border->target_bounds;
    debug("border_calculate_bounds > border->is_proxy > window frame is target bounds\n");
  } else if (
    !border_coalesce_resize_and_move_events(border, &window_frame)) {
    return false;
  }
  if (border->is_floating || border->is_sticky) {
    // Expand drawing_bounds, not the actual window frame
    //window_frame.origin.x -= 100.0;
    //window_frame.size.width += 100.0;
  }
  border->target_bounds = window_frame;

  border->too_small = border_check_too_small(border, window_frame);
  if (border->too_small) {
    border_hide(border);
    return false;
  }

  float border_offset = -settings->border_width - BORDER_PADDING;
  
  *frame = CGRectInset(window_frame, border_offset, border_offset);

  

  
  // adjust for indicators
  // if (border->stack_index > 1) {
  //   frame->origin.x -= indicator_offset;
  //   frame->size.width += indicator_offset;
  // }
 

  border->origin = frame->origin;
  frame->origin = CGPointZero;

  window_frame.origin = (CGPoint){-border_offset, -border_offset};
  
  border->drawing_bounds = window_frame;
  
  return true;
}

static void border_draw(struct border *border, CGRect frame,
                        struct settings *settings) {
  debug("border_draw: cid=%d wid=%d\n", border->cid, border->wid);
  struct color_style color_style;
  


  if(border->focused) {
    if(border->is_floating){
        color_style.color = 0xFF0A84FF;
    } else {
        color_style = settings->active_window;
    }
    
  } else {
    color_style = settings->inactive_window;
  }
  if (border->is_sticky){
        color_style.color = 0xFFFF9500;
  }
  if(border->last_focused){
    color_style.color = 0xFFFFFFFF; // white
  }
  uint32_t active   = color_style.color;
  uint32_t inactive = settings->inactive_window.color;
  uint32_t draw_color;
  float t = border->fade_value;             // 0→1
  if (border->stack_index > 0) {
      t = 1.0f;
  }

  if (border->focused) {
    draw_color = lerp_rgba(inactive, active, t);
  } else if(border->last_focused) {
    draw_color =lerp_rgba(inactive,active, t);
  } else {
        /* Normal unfocused window: just inactive */           // 0→1
        draw_color = inactive;
  }
  drawing_set_stroke_and_fill(border->context, draw_color, false);
  color_style.stype = COLOR_STYLE_SOLID;
  color_style.color = draw_color;          /* ← single source of truth */
  bool glow = false;

  CGGradientRef gradient = NULL;
  CGPoint gradient_dir[2];
  if (color_style.stype == COLOR_STYLE_SOLID ||
      color_style.stype == COLOR_STYLE_GLOW) {
    bool glow = color_style.stype == COLOR_STYLE_GLOW;
    drawing_set_stroke_and_fill(border->context, color_style.color, glow);
  } else if (color_style.stype == COLOR_STYLE_GRADIENT) {
    CGAffineTransform trans =
        CGAffineTransformMakeScale(frame.size.width, frame.size.height);
    gradient =
        drawing_create_gradient(&color_style.gradient, trans, gradient_dir);
  }

  CGContextSetLineWidth(border->context, settings->border_width);
  
  CGContextClearRect(border->context, frame);
  
  CGRect path_rect = border->drawing_bounds;
  
  CGMutablePathRef inner_clip_path = CGPathCreateMutable();
  
  if (settings->border_style == BORDER_STYLE_SQUARE &&
      settings->border_order == BORDER_ORDER_ABOVE &&
      settings->border_width >= BORDER_TSMW) {
    // Inset the frame to overlap the rounding of macOS windows to create a
    // truly square border
    path_rect = CGRectInset(border->drawing_bounds, BORDER_TSMN, BORDER_TSMN);

    CGPathAddRect(inner_clip_path, NULL, path_rect);
  } else {
    CGPathAddRoundedRect(inner_clip_path, NULL,
                         CGRectInset(path_rect, 1.0, 1.0), BORDER_INNER_RADIUS,
                         BORDER_INNER_RADIUS);
  }

  CGContextClearRect(border->context, frame);
  if (border->is_floating) {
    draw_floating_indicator(border);
}
  drawing_clip_between_rect_and_path(border->context, frame, inner_clip_path);
  
  if (settings->border_style == BORDER_STYLE_SQUARE) {
    if (color_style.stype == COLOR_STYLE_SOLID ||
        color_style.stype == COLOR_STYLE_GLOW) {
      drawing_draw_square_with_inset(border->context, path_rect,
                                     -settings->border_width / 2.f);
    } else if (color_style.stype == COLOR_STYLE_GRADIENT) {
      drawing_draw_square_gradient_with_inset(border->context, gradient,
                                              gradient_dir, path_rect,
                                              -settings->border_width / 2.f);
    }
  } else {
    if (color_style.stype == COLOR_STYLE_SOLID ||
        color_style.stype == COLOR_STYLE_GLOW) {
      drawing_draw_rounded_rect_with_inset(border->context, path_rect,
                                           BORDER_RADIUS);
    } else if (color_style.stype == COLOR_STYLE_GRADIENT) {
      drawing_draw_rounded_gradient_with_inset(
          border->context, gradient, gradient_dir, path_rect, BORDER_RADIUS);
    }
  }
  CGGradientRelease(gradient);

  if (settings->show_background && settings->border_order != 1) {
    CGContextRestoreGState(border->context);
    CGContextSaveGState(border->context);
    color_style = settings->background;
    if (color_style.stype == COLOR_STYLE_SOLID ||
        color_style.stype == COLOR_STYLE_GLOW) {
      drawing_draw_filled_path(border->context, inner_clip_path,
                               color_style.color);
    }
  }
  // 🟨🟨🟨 floating indicators
  if(border->is_floating || border->is_sticky){
      CGContextResetClip(border->context);

    draw_floating_indicator(border);
  }
  if(border->is_sticky){
  // 🟩🟩🟩 sticky indicators
    draw_sticky_indicator(border);
  }
  
  if(border->stack_index > 0) {
    draw_stack_indicators(border);
    debug("🐸 border_draw: wid=%d, stack_index: %d, is_topmost: %d\n",
        border->wid, border->stack_index, border->stack_is_topmost_wid);
  }
  CFRelease(inner_clip_path);
  CGContextFlush(border->context);
  CGContextResetClip(border->context);
  CGContextRestoreGState(border->context);
  
  SLSFlushWindowContentRegion(border->cid, border->wid, NULL);

}

void draw_floating_indicator(struct border *border) {
  if (!(border->is_floating || border->is_sticky)) return;
  CGRect path_rect = border->drawing_bounds;

    path_rect = CGRectInset(border->drawing_bounds, BORDER_TSMN, BORDER_TSMN);

  CGRect bounds = border->drawing_bounds;
    CGRect shadow = CGRectInset(CGRectOffset(bounds, 10, -10), -2, -2);
    shadow = CGRectIntegral(shadow);  // avoid subpixel issues

    CGFloat radius = fmin(shadow.size.width, shadow.size.height) / 2.0;
    radius = fmin(radius, 9.0);  // corner radius

    CGRect borderRect = CGRectInset(path_rect, 1.0, 1.0);

    CGContextSaveGState(border->context);
    CGContextResetClip(border->context);
    CGContextClearRect(border->context, border->frame);

    // Create a compound path (outer shadow - inner border hole)
    CGMutablePathRef shadowPath = CGPathCreateMutable();
    CGPathAddRoundedRect(shadowPath, NULL, shadow, radius, radius);
    CGPathAddRoundedRect(shadowPath, NULL, borderRect, BORDER_INNER_RADIUS, BORDER_INNER_RADIUS);

    // Use even-odd rule to "punch out" the middle
    CGContextAddPath(border->context, shadowPath);
    CGContextSetRGBFillColor(border->context, 0.0, 0.0, 0.0, 0.7);
    CGContextEOFillPath(border->context);  // << important!

    CGPathRelease(shadowPath);
    CGContextRestoreGState(border->context);
}

void draw_sticky_indicator(struct border *border) {
  if (!border->is_sticky || !border->sticky) return;
  CGRect bounds = border->drawing_bounds;

  struct settings *settings = &g_settings;
  float frame_offset = settings->border_width + BORDER_PADDING;

  float ind_size = 28.0f;
  float x = 15;
  float y = bounds.size.height - 12;
  CGContextSaveGState(border->context);
    CGContextResetClip(border->context);
  CGContextTranslateCTM(border->context, x, y);
  CGContextRotateCTM(border->context, M_PI_4);
  CGContextTranslateCTM(border->context, -x, -y);


  CFStringRef emoji = CFSTR("📍");
  CTFontRef font = CTFontCreateWithName(CFSTR("Apple Color Emoji"), ind_size, NULL);
  const void *keys[] = { kCTFontAttributeName };
  const void *values[] = { font };
  CFDictionaryRef attrs = CFDictionaryCreate(NULL, keys, values, 1, NULL, NULL);
  CFAttributedStringRef attrStr = CFAttributedStringCreate(NULL, emoji, attrs);
  CTLineRef line = CTLineCreateWithAttributedString(attrStr);

  // Draw the emoji line
    CGContextSetTextPosition(border->context,x, y);
  //CGContextSetTextPosition(border->context, x, y);
    CTLineDraw(line, border->context);

  // Cleanup
  CFRelease(line);
  CFRelease(attrStr);
  CFRelease(attrs);
  CFRelease(font);

  CGContextRestoreGState(border->context);
}

void
border_clear_stack_state(struct border *b)
{
    b->stack_id             = 0;
    b->stack_index          = 0;
    b->stack_len            = 0;
    b->stack_is_topmost_wid = false;
    b->needs_redraw         = true;   /* one more paint clears indicators */
}

/* Force‑redraw every border that belongs to a stack (stack_index > 0). */
void border_redraw_all_stacked(struct table *windows)
{
    if (!windows) return;

    for (int i = 0; i < windows->capacity; ++i) {
        struct bucket *bucket = windows->buckets[i];
        while (bucket) {
            if (bucket->value) {
                struct border *b = bucket->value;
                if (b && b->stack_index > 0) {
                    b->needs_redraw = true;
                    border_update(b, /*try_async=*/true);
                }
            }
            bucket = bucket->next;
        }
    }
}
static inline void unpack_color(color_t color, float *r, float *g, float *b, float *a) {
    uint32_t v = color.value;
    *a = ((v >> 24) & 0xFF) / 255.0f;
    *r = ((v >> 16) & 0xFF) / 255.0f;
    *g = ((v >> 8)  & 0xFF) / 255.0f;
    *b = ((v >> 0)  & 0xFF) / 255.0f;
}
void draw_stack_indicators(struct border *border) {
  //TODO: styling
  /** TODO:
    [] styling
    [] interaction/on click
    */
  // placeholder for stack indicators
if(border->stack_index <= 0) return;
bool bactive = border->focused;
struct settings *settings = &g_settings;
float frame_offset = settings->border_width + BORDER_PADDING;
color_t active_color = { .value =  0xFF0A84FF }; // blue
//color_t active_color = { .value = 0xFFFFFFFF };
color_t inactive_color = { .value = 0x99CCFFFF }; // light blue
//color_t inactive_color = { .value = 0x99FFFFFF }; // 10% transparent white
float ind_height = 32.f;
float ind_width = 6.f;
float ind_gap = 2.0f;
float ind_y_offset = border->stack_index * (ind_height + ind_gap);
float ind_radius = 0;
float ind_x_offset = 0;  // offset from the right edge OR gap between border and indicator
CGRect bounds = border->drawing_bounds;
float x_pos = ( frame_offset  - ind_width   ) - ind_x_offset;
float y_pos = (bounds.size.height + frame_offset) - ind_y_offset - 28;

CGRect rect = CGRectMake(x_pos, y_pos,  ind_width, ind_height);
CGContextSaveGState(border->context);

CGPathRef path = CGPathCreateWithRoundedRect(rect, ind_radius, ind_radius, NULL);
CGContextAddPath(border->context, path);

float r, g, b, a;
if (border->stack_is_topmost_wid) {
    unpack_color(active_color, &r, &g, &b, &a);
} else {
    unpack_color(inactive_color, &r, &g, &b, &a);
}
CGContextSetRGBFillColor(border->context, r, g, b, a);

CGContextFillPath(border->context);
CGPathRelease(path);
CGContextRestoreGState(border->context);

}

void border_create_window(struct border *border, CGRect frame, bool unmanaged,
                          bool hidpi) {
  debug("border_create_window: cid=%d wid=%d\n", border->cid, border->wid);
  pthread_mutex_lock(&border->mutex);
  int cid = border->cid;
  border->wid = window_create(cid, frame, hidpi, unmanaged);
  
  border->frame = frame;
  border->needs_redraw = true;
  border->context = SLWindowContextCreate(cid, border->wid, NULL);
  CGContextSetInterpolationQuality(border->context, kCGInterpolationNone);

  if (!border->sid)
    border->sid = window_space_id(cid, border->target_wid);
  window_send_to_space(cid, border->wid, border->sid);
  pthread_mutex_unlock(&border->mutex);
}

void border_update_internal(struct border *border, struct settings *settings) {

  if (border->external_proxy_wid){
    return;}

  int cid = border->cid;
  CGRect frame;
  if (!border_calculate_bounds(border, &frame, settings)){
    return;}
  yb_props_t *prop = table_find(&yb_props, &border->target_wid);
  yb_props_t *propswid = table_find(&yb_props, &border->wid);

  uint64_t tags = window_tags(cid, border->target_wid);
  border->sticky = tags & WINDOW_TAG_STICKY;
  border->is_sticky = prop && prop->is_sticky;
  border->floating = tags & WINDOW_TAG_FLOATING;
  border->is_floating = prop && prop->is_floating;
  border->attached = tags & WINDOW_TAG_ATTACHED;
  border->modal = tags & WINDOW_TAG_MODAL; 
  border->document = tags & WINDOW_TAG_DOCUMENT;

  if (!border->sticky && !is_space_visible(cid, border->sid))
    return;
  // if(border->is_floating ) draw_floating_indicator(border);
  bool shown = false;
  SLSWindowIsOrderedIn(cid, border->target_wid, &shown);
  if (!shown && !border->is_proxy) {
    border_hide(border);
    return;
  }
  
  int level = window_level(cid, border->target_wid);
  int sub_level = window_sub_level(border->target_wid);

  if (!border->wid){
    border_create_window(border, frame, border->is_proxy, settings->hidpi);
  }

  bool disabled_update = false;
  if (!CGRectEqualToRect(frame, border->frame)) {
    
    CFTypeRef transaction = SLSTransactionCreate(cid);
    if (!transaction)
      return;

    CFTypeRef frame_region;
    //if(border->is_floating){
    //  frame.origin.x -= 100;
    //  frame.size.width += 100;
    //}
    CGSNewRegionWithRect(&frame, &frame_region);
    SLSTransactionOrderWindow(transaction, border->wid, 0, border->target_wid);
    SLSTransactionSetWindowShape(transaction, border->wid, -9999, -9999,
                                 frame_region);
    CFRelease(frame_region);
    
    border->needs_redraw = true;
    border->frame = frame;
    disabled_update = true;
   
    SLSDisableUpdate(cid);
    SLSTransactionCommit(transaction, 0);
    CFRelease(transaction);
  }

  if (border->needs_redraw){
    border_draw(border, frame, settings);
  };
  
  CFTypeRef transaction = SLSTransactionCreate(cid);
  if (!transaction)
    return;

  SLSTransactionMoveWindowWithGroup(transaction, border->wid, border->origin);
  if (!border->is_proxy) {
    CGAffineTransform transform = CGAffineTransformIdentity;
    transform.tx = -border->origin.x;
    transform.ty = -border->origin.y;
    SLSTransactionSetWindowTransform(transaction, border->wid, 0, 0, transform);
  }
  SLSTransactionSetWindowLevel(transaction, border->wid, level);
  SLSTransactionSetWindowSubLevel(transaction, border->wid, sub_level);
  SLSTransactionOrderWindow(transaction, border->wid, settings->border_order,
                            border->target_wid);
  SLSTransactionCommit(transaction, 0);
  CFRelease(transaction);

  uint64_t set_tags = (1ULL << 1) | (1ULL << 9);
  uint64_t clear_tags = 0;
  
  CGContextSaveGState(border->context);
  struct color_style color_style;
  bool was_sticky = border->sticky;  

  if (border && border->sticky) {
    set_tags |= WINDOW_TAG_STICKY;
    clear_tags |= (1ULL << 45);
  }
   
  SLSSetWindowTags(cid, border->wid, &set_tags, 0x40);
  SLSClearWindowTags(cid, border->wid, &clear_tags, 0x40);
  if (disabled_update)
    SLSReenableUpdate(cid);
  
}

static void *border_update_async_proc(void *context) {
  struct {
    struct border *border;
    struct settings settings;
  } *payload = context;

  pthread_mutex_lock(&payload->border->mutex);
  border_update_internal(payload->border, &payload->settings);
  pthread_mutex_unlock(&payload->border->mutex);
  free(payload);
  return NULL;
}

void border_init(struct border *border, int cid) {
  memset(border, 0, sizeof(struct border));
  pthread_mutexattr_t mattr;
  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&border->mutex, &mattr);
  animation_init(&border->animation);


  if (cid){
    border->cid = cid;
    border->fade_value = 0.0f;
    border->fade_start = 0.0;
    border->fade_in    = false;
    border->last_focus_ts = 0.0;
  } else {
    border->cid = SLSMainConnectionID();
  }
}
struct border *border_create() {
  struct border *border = malloc(sizeof(struct border));
  int cid = 0;
  SLSNewConnection(0, &cid);
  border_init(border, cid);
  return border;
}

static void border_destroy_async(void *context) {
    struct border *border = (struct border *)context;
    /* ②  Second-level guard in case another async block sneaks in */
    if (border->destroyed) return;
    border->destroyed = true;

    pthread_mutex_lock(&border->mutex);

    /* ---- tear-down begins ---- */
    border_destroy_window(border);

    if (border->proxy) {
        struct border *child = border->proxy;
        border->proxy = NULL;          /* break link first */
        border_destroy(child);
    }

    animation_stop(&border->animation);

    if (!border->is_proxy &&
        border->cid != 0 &&
        border->cid != SLSMainConnectionID())
    {
        SLSReleaseConnection(border->cid);
        border->cid = 0;
    }

    pthread_mutex_unlock(&border->mutex);
    free(border);
}

static inline float ease_out_cubic(float t)
{
    return 1.0f - powf(1.0f - t, 3.0f);   // t in [0,1]
}
static inline float smoothstep(float t)
{
    return t*t*(3 - 2*t);

}
void borders_fade_tick(struct table *windows)
{
    if (!windows || windows->count == 0) return;

    double now       = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW_APPROX) / 1e9;
    float  fade_time = 0.15f;         /* seconds  */
    float  idle_out  = 2.0f;    /* seconds  */
    bool idle_out_enabled = false;
    for (int i = 0; i < windows->capacity; ++i) {
        struct bucket *bucket = windows->buckets[i];
        while (bucket) {
            struct border *b = bucket->value;

            if (b->fade_running &&( b->focused || b->last_focused)){
                double dt = now - b->fade_start;
                float t_linear = fminf(dt / fade_time, 1.0f);
                float t_eased  = ease_out_cubic(t_linear);

                b->fade_value = b->fade_in ? t_eased         
                                          : 1.0f - t_linear;
                if (dt >= fade_time) {             
                    b->fade_running = false;
                    b->fade_start   = 0.0;
                    b->fade_value   = b->fade_in ? 1.0f : 0.0f;
                    b->last_focused = false;
                    if (!b->fade_in && b->focused) {
                      b->last_focus_ts = now + 999999;  // ensure we don’t retrigger
                  }
                }
                border_update(b, true);
            }

            if (idle_out_enabled && idle_out > 0 &&
                b->focused &&               /* still the focused window   */
                !b->fade_running &&         /* not already animating      */
                (now - b->last_focus_ts) > idle_out)
            {
                debug("⏰⏰⏰ idle fade‑out start (wid:%u)\n", b->wid);
                b->fade_in      = false;    /* fade toward inactive       */
                b->fade_start   = now;
                b->fade_value   = 1.0f;     /* start at fully‑active      */
                /* keep last_focused = false — this is *current* focus */
                b->fade_running = true;     /* start animation            */
                border_update(b, true);     /* schedule first repaint     */
            }

            bucket = bucket->next;
        }
    }
}
void border_destroy(struct border *border)
{
    /* ①  Return immediately if we’ve already queued a destroy */
    if (border->destroy_queued) return;
    border->destroy_queued = true;

    border_hide(border);

    dispatch_async_f(dispatch_get_main_queue(), border, border_destroy_async);
}

struct border_move_payload {
  struct border *border;
  struct settings *settings;
};

void *border_move_async_proc(void *context) {
  struct border_move_payload *payload = (struct border_move_payload *)context;
  struct border *border = payload->border;
  struct settings *settings = payload->settings;
  pthread_mutex_lock(&border->mutex);
  CGRect window_frame;
  if (!border_coalesce_resize_and_move_events(border, &window_frame)) {
    pthread_mutex_unlock(&border->mutex);
    free(payload);
    return NULL;
  }

  CGPoint origin = {
      .x = window_frame.origin.x - settings->border_width - BORDER_PADDING,
      .y = window_frame.origin.y - settings->border_width - BORDER_PADDING};

  CFTypeRef transaction = SLSTransactionCreate(border->cid);
  if (transaction) {
    SLSTransactionMoveWindowWithGroup(transaction, border->wid, origin);
    SLSTransactionCommit(transaction, 0);
    CFRelease(transaction);
  }
  border->target_bounds = window_frame;
  border->origin = origin;
  pthread_mutex_unlock(&border->mutex);
  free(payload);
  return NULL;
}

void border_move(struct border *border) {
  pthread_mutex_lock(&border->mutex);
  if (border->external_proxy_wid) {
    pthread_mutex_unlock(&border->mutex);
    return;
  }
  pthread_mutex_unlock(&border->mutex);

  struct settings *settings = border_get_settings(border);

  struct border_move_payload *payload = malloc(sizeof(struct border_move_payload));
  payload->border = border;
  payload->settings = settings;
  pthread_t thread;
  pthread_create(&thread, NULL, border_move_async_proc, payload);
  pthread_detach(thread);
}

void border_update(struct border *border, bool try_async) {
  char *window_title(int cid, uint32_t wid);
  pthread_mutex_lock(&border->mutex);
  yb_props_t *prop = table_find(&yb_props, &border->target_wid);
  border->is_floating = prop && prop->is_floating;
  struct settings *settings = border_get_settings(border);
  if (!border->wid || !try_async) {
    border_update_internal(border, settings);
    pthread_mutex_unlock(&border->mutex);
    return;
  }

  struct payload {
    struct border *border;
    struct settings settings;
  } *payload = malloc(sizeof(struct payload));

  payload->border = border;
  payload->settings = *settings;
  pthread_t thread;
  pthread_create(&thread, NULL, border_update_async_proc, payload);
  pthread_detach(thread);
  pthread_mutex_unlock(&border->mutex);
}

void border_hide(struct border *border) {
  pthread_mutex_lock(&border->mutex);
  if (border->wid) {
    CFTypeRef transaction = SLSTransactionCreate(border->cid);
    if (transaction) {
      SLSTransactionOrderWindow(transaction, border->wid, 0,
                                border->target_wid);
      SLSTransactionCommit(transaction, 0);
      CFRelease(transaction);
    }
  }
  pthread_mutex_unlock(&border->mutex);
}

void border_unhide(struct border *border) {
  pthread_mutex_lock(&border->mutex);
  if (border->too_small || border->external_proxy_wid ||
      (!border->sticky && !is_space_visible(border->cid, border->sid))) {
    pthread_mutex_unlock(&border->mutex);
    return;
  }

  if (border->wid) {
    struct settings *settings = border_get_settings(border);
    CFTypeRef transaction = SLSTransactionCreate(border->cid);
    if (transaction) {
      SLSTransactionOrderWindow(transaction, border->wid,
                                settings->border_order, border->target_wid);
      SLSTransactionCommit(transaction, 0);
      CFRelease(transaction);
    }
  }
  pthread_mutex_unlock(&border->mutex);
}
