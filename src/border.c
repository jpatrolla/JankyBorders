#include "border.h"
#include "hashtable.h"
#include "windows.h"
#include <pthread.h>
#include <time.h>

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
    SLSGetWindowBounds(border->cid, border->target_wid, frame);
    return true;
  }
  int64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW_APPROX);
  int64_t dt = now - border->event_buffer.last_coalesce_attempt;
  border->event_buffer.last_coalesce_attempt = now;

  if (border->event_buffer.is_coalescing)
    return false;

  CGRect window_frame;
  SLSGetWindowBounds(border->cid, border->target_wid, &window_frame);
  bool coalesceable =
      (CGPointEqualToPoint(window_frame.origin, border->target_bounds.origin) &&
       !CGSizeEqualToSize(window_frame.size, border->target_bounds.size)) ||
      (!CGPointEqualToPoint(window_frame.origin,
                            border->target_bounds.origin) &&
       CGSizeEqualToSize(window_frame.size, border->target_bounds.size));

  if (coalesceable && dt > (1ULL << 27)) {
    border->event_buffer.is_coalescing = true;
    pthread_mutex_unlock(&border->mutex);
    usleep(20000);
    pthread_mutex_lock(&border->mutex);
    border->event_buffer.is_coalescing = false;
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
  if (border->is_proxy)
    window_frame = border->target_bounds;
  else if (!border_coalesce_resize_and_move_events(border, &window_frame)) {
    return false;
  }
  border->target_bounds = window_frame;

  border->too_small = border_check_too_small(border, window_frame);
  if (border->too_small) {
    border_hide(border);
    return false;
  }

  float border_offset = -settings->border_width - BORDER_PADDING;
  *frame = CGRectInset(window_frame, border_offset, border_offset);

  if (border->stack_index > 1) {

    frame->origin.x -= indicator_offset;
    frame->size.width += indicator_offset;
  }

  border->origin = frame->origin;
  frame->origin = CGPointZero;

  window_frame.origin = (CGPoint){-border_offset, -border_offset};
  border->drawing_bounds = window_frame;

  return true;
}

static void border_draw(struct border *border, CGRect frame,
                        struct settings *settings) {

  CGContextSaveGState(border->context);
  border->needs_redraw = false;
  //struct color_style color_style =
  //    border->focused ? settings->active_window : settings->inactive_window;
  struct color_style color_style;
  if (border->is_floating) {
    debug("BORDER IS FLOATING: 💛💛💛💛💛\n");
    color_style.stype = COLOR_STYLE_SOLID;
    color_style.color = 0xffffff00; // yellow
  } else {
    color_style = border->focused ? settings->active_window : settings->inactive_window;
  }

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
  if (border->stack_index > 1) {
    path_rect.origin.x +=
        indicator_offset; // shift right by 10 to realign the left clip edge
    path_rect.size.width += indicator_offset;
  }

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
  CFRelease(inner_clip_path);
  CGContextFlush(border->context);
  CGContextRestoreGState(border->context);
  SLSFlushWindowContentRegion(border->cid, border->wid, NULL);
}

void border_create_window(struct border *border, CGRect frame, bool unmanaged,
                          bool hidpi) {
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
  debug("[BADGES DEBUG] border_update_internal\n");
  if (border->external_proxy_wid)
    return;

  int cid = border->cid;
  CGRect frame;
  if (!border_calculate_bounds(border, &frame, settings))
    return;

  uint64_t tags = window_tags(cid, border->target_wid);
  border->sticky = tags & WINDOW_TAG_STICKY;
  if (!border->sticky && !is_space_visible(cid, border->sid))
    return;

  bool shown = false;
  SLSWindowIsOrderedIn(cid, border->target_wid, &shown);
  if (!shown && !border->is_proxy) {
    border_hide(border);
    return;
  }

  int level = window_level(cid, border->target_wid);
  int sub_level = window_sub_level(border->target_wid);

  if (!border->wid) {
    debug("[BADGES DEBUG] !border->wid, creating window\n");
    border_create_window(border, frame, border->is_proxy, settings->hidpi);
  }

  bool disabled_update = false;
  if (!CGRectEqualToRect(frame, border->frame)) {
    debug("!CGRectEqualToRect(frame, border->frame) ❤️ \n");
    CFTypeRef transaction = SLSTransactionCreate(cid);
    if (!transaction)
      return;

    CFTypeRef frame_region;
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
    debug("[BADGES DEBUG] NEEDS REDRAW @ border_update_internal");

  if (border && border->needs_redraw){
    debug("[BADGES DEBUG] NEEDS REDRAW @ border_update_internal");
    border_draw(border, frame, settings);};

  CFTypeRef transaction = SLSTransactionCreate(cid);
  if (!transaction)
    return;
  SLSTransactionMoveWindowWithGroup(transaction, border->wid, border->origin);

  if (border && !border->is_proxy) {
    debug("CGAffineTransform transform = CGAffineTransformIdentity;");
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

  if (border && border->sticky) {
    debug("border->sticky\n");
    set_tags |= WINDOW_TAG_STICKY;
    clear_tags |= (1ULL << 45);
  }
  if (border->stack_index > 1) {
    debug("BORDER HAS STACK...\n");
    CGContextSaveGState(border->context);
    CGFloat indicator_offset = 50;

    CGFloat indicator_size = 20;
    CGFloat y = (frame.size.height / 2) - (indicator_size / 2);
    drawing_draw_filled_circle(
        border->context,
        CGRectMake((indicator_offset / 2), y, indicator_size, indicator_size),
        0xffff0000);
    CGContextSetRGBFillColor(border->context, 1, 1, 1, 0.5);
    CGContextFillRect(border->context,
                      CGRectMake(0, 0, frame.size.width, frame.size.height));
    // if (!border->stack_indicator_overlay) {
    //   border->stack_indicator_overlay = malloc(sizeof(struct border));
    //   border_init(border->stack_indicator_overlay, border->cid);
    //   CGRect overlay_rect = CGRectMake(0, 0, 20, 20);
    //   border_create_window(border->stack_indicator_overlay, overlay_rect,
    //   true,
    //                        false);
    // }
    // CGFloat dot_size = 6.0;
    // CGFloat x = frame.size.width - dot_size - 3;
    // CGFloat y = frame.size.height / 2 - dot_size / 2;

    // CGContextSaveGState(border->context);
    // drawing_set_fill(border->context, 0xffff0000);
    // CGContextFillEllipseInRect(border->context,
    //                            CGRectMake(x, y, dot_size, dot_size));
    // CGContextRestoreGState(border->context);

    // CGRect overlay_frame =
    //     CGRectMake(border->target_bounds.origin.x - 5,
    //                border->target_bounds.origin.y + 5, 20, 20);
    // border->stack_indicator_overlay->target_bounds = overlay_frame;
    // border_update_internal(border->stack_indicator_overlay, settings);

  } else if (border->stack_indicator_overlay) {
    debug("border->stack_indicator_overlay");

    border_destroy(border->stack_indicator_overlay);
    border->stack_indicator_overlay = NULL;
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
  if (cid)
    border->cid = cid;
  else
    border->cid = SLSMainConnectionID();
}

struct border *border_create() {
  struct border *border = malloc(sizeof(struct border));
  int cid = 0;
  SLSNewConnection(0, &cid);
  border_init(border, cid);
  return border;
}

void border_destroy(struct border *border)
{
    /* ①  Return immediately if we’ve already queued a destroy */
    if (border->destroy_queued) return;
    border->destroy_queued = true;

    border_hide(border);

    dispatch_async(dispatch_get_main_queue(), ^{
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
        debug("border_destroy: cid=%d is_proxy=%d\n", border->cid, border->is_proxy);
        free(border);
    });
}

void border_move(struct border *border) {
  pthread_mutex_lock(&border->mutex);
  if (border->external_proxy_wid) {
    pthread_mutex_unlock(&border->mutex);
    return;
  }
  pthread_mutex_unlock(&border->mutex);

  struct settings *settings = border_get_settings(border);
  dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^{
    pthread_mutex_lock(&border->mutex);
    CGRect window_frame;
    if (!border_coalesce_resize_and_move_events(border, &window_frame)) {
      pthread_mutex_unlock(&border->mutex);
      return;
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
  });
}

void border_update(struct border *border, bool try_async) {
  pthread_mutex_lock(&border->mutex);
  struct settings *settings = border_get_settings(border);
  if (!border->wid || !try_async) {
    debug("[BADGES DEBUG] border_update -> border_update_internal\n");
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
