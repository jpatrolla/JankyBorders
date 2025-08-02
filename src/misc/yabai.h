#pragma once
#define _YABAI_INTEGRATION

#ifdef _YABAI_INTEGRATION
#include "extern.h"
#include "../windows.h"
#include "../mach.h"
#include <CoreVideo/CoreVideo.h>
#include <pthread.h>
#include "../yb_props.h"
// Additional border interfaces needed for the yabai integration
void border_init(struct border* border, int cid);
void border_create_window(struct border* border, CGRect frame, bool unmanaged, bool hidpi);
void border_update_internal(struct border* border, struct settings* settings);
/* Redraw helper defined in border.c */
void border_redraw_all_stacked(struct table *windows);
struct track_transform_payload {
  int cid;
  uint32_t border_wid;
  uint32_t proxy_wid;
  uint32_t target_wid;
  CGAffineTransform initial_transform;
};

typedef struct yb_props {
    bool is_floating;
    bool is_stacked;
    bool is_sticky;
    bool is_pip;
    uint32_t stack_index;  /* -1 if not stacked, otherwise the index in the stack */
} yb_props_t;

/* ---------- IPC payload definitions (shared with Yabai) -------------- */
struct yb_hdr {
    uint32_t event;
    uint32_t count;
};

/* Events 1325 / 1326  ─ proxy begin / end ------------------------------ */
struct payload {
    uint32_t event;
    uint32_t count;
    uint32_t proxy_wid[512];
    uint32_t real_wid[512];
};

/* Generic per‑window value payload (events 1008 / 1117 / 1227 / 1337) ---- */
struct yb_payload {
    uint32_t event;
    uint32_t count;
    uint32_t window_id;
    uint32_t value;          /* 0/1 or any 32‑bit value per window      */
};

/* Event 1338  ─ bundled window‑flags ----------------------------------- */
struct yb_flags {
    uint8_t is_floating : 1;
    uint8_t is_sticky   : 1;
    uint8_t is_stacked  : 1;
    uint8_t is_pip      : 1;
};

struct yb_flags_payload {
    uint32_t event;
    uint32_t count;
    uint32_t window_id[512];
    struct yb_flags flags[512];
};
struct yb_stack_payload {
    uint32_t event;
    uint32_t window_id;
    uint32_t stack_index;
    uint32_t stack_len;
    uint32_t stack_topmost_wid;
};
extern struct table yb_props;
void yabai_props_init(void);
void yabai_props_free(void);
void yb_props_bootstrap(void);
yb_props_t *yb_props_get(uint32_t wid, bool create_if_missing);

static inline void yb_props_refresh_flags(void) { yb_props_bootstrap(); }

struct yabai_proxy_payload {
  union { struct border* proxy; struct border* border; };
  struct settings settings;
  uint32_t border_wid;
  uint32_t real_wid;
  uint32_t external_proxy_wid;
};

static CVReturn track_transform(CVDisplayLinkRef display_link, const CVTimeStamp* now, const CVTimeStamp* output_time, CVOptionFlags flags, CVOptionFlags* flags_out, void* context) {
  struct animation* animation = context;
  usleep(0.25*animation->frame_time);

  struct track_transform_payload* payload = animation->context;
  CGAffineTransform target_transform, border_transform;
  CGError error = SLSGetWindowTransform(payload->cid,
                                        payload->target_wid,
                                        &target_transform   );

  if (error != kCGErrorSuccess) return kCVReturnSuccess;

  border_transform = CGAffineTransformConcat(target_transform,
                                             payload->initial_transform);

  CFTypeRef transaction = SLSTransactionCreate(payload->cid);
  if (transaction) {
    SLSTransactionSetWindowTransform(transaction, payload->proxy_wid, 0, 0, border_transform);
    SLSTransactionSetWindowTransform(transaction, payload->border_wid, 0, 0, border_transform);
    SLSTransactionCommit(transaction, 0);
    CFRelease(transaction);
  }
  return kCVReturnSuccess;
}

static void* yabai_proxy_begin_proc(void* context) {
  struct yabai_proxy_payload* info = context;
  struct border* proxy = info->proxy;
  pthread_mutex_lock(&proxy->mutex);

  struct track_transform_payload* payload
                            = malloc(sizeof(struct track_transform_payload));

  CGRect proxy_frame;
  SLSGetWindowBounds(proxy->cid, info->external_proxy_wid, &proxy_frame);

  payload->proxy_wid = proxy->wid;
  payload->border_wid = info->border_wid;
  payload->target_wid = info->external_proxy_wid;
  payload->cid = proxy->cid;

  payload->initial_transform = CGAffineTransformIdentity;
  payload->initial_transform.a = proxy->target_bounds.size.width
                                / proxy_frame.size.width;
  payload->initial_transform.d = proxy->target_bounds.size.height
                                / proxy_frame.size.height;
  payload->initial_transform.tx = 0.5*(proxy->frame.size.width
                                 - proxy->target_bounds.size.width);
  payload->initial_transform.ty = 0.5*(proxy->frame.size.height
                                 - proxy->target_bounds.size.height);

  animation_stop(&proxy->animation);
  animation_start(&proxy->animation, track_transform, payload);

  if (!proxy->is_proxy) {
    proxy->is_proxy = true;
    proxy->frame = CGRectNull;
    border_update_internal(proxy, &info->settings);
  }

  CFTypeRef transaction = SLSTransactionCreate(proxy->cid);
  if (transaction) {
    SLSTransactionOrderWindow(transaction,
                              proxy->wid,
                              info->settings.border_order,
                              info->external_proxy_wid    );

    SLSTransactionSetWindowAlpha(transaction, info->border_wid, 0.f);
    SLSTransactionSetWindowAlpha(transaction, proxy->wid, 1.f);
    SLSTransactionCommit(transaction, 0);
    CFRelease(transaction);
  }

  pthread_mutex_unlock(&proxy->mutex);
  free(context);
  return NULL;
}

static void* yabai_proxy_end_proc(void* context) {
  struct yabai_proxy_payload* info = context;
  struct border* border = info->border;
  pthread_mutex_lock(&border->mutex);
  border->event_buffer.disable_coalescing = true;
  border->external_proxy_wid = 0;
  border_update_internal(border, &info->settings);
  border->event_buffer.disable_coalescing = false;
  pthread_mutex_unlock(&border->mutex);
  free(context);
  return NULL;
}

static inline void yabai_proxy_begin(struct table* windows, uint32_t wid, uint32_t real_wid) {
  if (!real_wid || !wid) return;
  struct border* border = table_find(windows, &real_wid);

  if (border) {
    pthread_mutex_lock(&border->mutex);
    border->external_proxy_wid = wid;
    if (!border->proxy) {
      border->proxy = malloc(sizeof(struct border));
      border_init(border->proxy, border->cid);
      border_create_window(border->proxy, CGRectNull, true, false);
      border->proxy->target_bounds = border->target_bounds;
      border->proxy->frame = border->frame;
      border->proxy->focused = border->focused;
      border->proxy->target_wid = border->target_wid;
      border->proxy->sid = border->sid;
    }

    struct yabai_proxy_payload* payload
                            = malloc(sizeof(struct yabai_proxy_payload));
    payload->proxy = border->proxy;
    payload->border_wid = border->wid;
    payload->external_proxy_wid = border->external_proxy_wid;
    payload->real_wid = real_wid;
    payload->settings = *border_get_settings(border);

    pthread_t thread;
    pthread_create(&thread, NULL, yabai_proxy_begin_proc, payload);
    pthread_detach(thread);
    pthread_mutex_unlock(&border->mutex);
  }
}

static inline void yabai_proxy_end(struct table* windows, uint32_t wid, uint32_t real_wid) {
  if (!real_wid || !wid) return;
  struct border* border = (struct border*)table_find(windows, &real_wid);
  if (border) pthread_mutex_lock(&border->mutex);
  if (border && border->proxy && border->external_proxy_wid == wid) {
    struct border* proxy = border->proxy;
    border->proxy = NULL;

    CFTypeRef transaction = SLSTransactionCreate(border->cid);
    if (transaction) {
      SLSTransactionSetWindowAlpha(transaction, proxy->wid, 0.f);
      SLSTransactionSetWindowAlpha(transaction, border->wid, 1.f);
      SLSTransactionCommit(transaction, 0);
      CFRelease(transaction);
    }
    debug("destroy proxy\n");
    border_destroy(proxy);

    struct yabai_proxy_payload* payload
                                  = malloc(sizeof(struct yabai_proxy_payload));

    payload->border = border;
    payload->border_wid = border->wid;
    payload->settings = *border_get_settings(border);

    pthread_t thread;
    pthread_create(&thread, NULL, yabai_proxy_end_proc, payload);
    pthread_detach(thread);
  }
  if (border) pthread_mutex_unlock(&border->mutex);
}


static void yabai_message(CFMachPortRef port, void *data, CFIndex size, void *ctx)
{
  if (!data || size < sizeof(struct mach_message)) return;

    if (size != sizeof(struct mach_message)) return;

    struct mach_message *msg  = data;
    struct yb_hdr       *hdr  = msg->descriptor.address;
    switch (hdr->event) {

        case 1325:
        case 1326: {
            if (msg->descriptor.size != sizeof(struct payload)) break;
            struct payload *pl = (void *)hdr;
            for (uint32_t i = 0; i < pl->count; ++i) {
                if (hdr->event == 1325)
                    yabai_proxy_begin(ctx, pl->proxy_wid[i], pl->real_wid[i]);
                else
                    yabai_proxy_end  (ctx, pl->proxy_wid[i], pl->real_wid[i]);
            }
            break;
        }

        case 1008:  /* sticky */
        case 1117:  /* pip    */
        case 1227: {
            debug("🟨 Received window flags for event %d\n", hdr->event);
            if (msg->descriptor.size != sizeof(struct yb_payload)) break;
            struct yb_payload *pl = (void *)hdr;
            for (uint32_t i = 0; i < pl->count; ++i) {
                uint32_t wid = pl->window_id;
                uint32_t val = pl->value;
                yb_props_t *p = yb_props_get(wid, true);
                struct border *b = table_find(ctx, &wid);
                switch (hdr->event) {
                    case 1008:
                        debug("🟨🟨 1️⃣0️⃣0️⃣8️⃣ is_sticky: %d\n", val);
                        p->is_sticky            = val; 
                        if (b) b->is_sticky     = val;
                        break;
                    case 1117:
                        debug("🟨🟨 1️⃣1️⃣1️⃣7️⃣ is_pip: %d\n", val);
                        pthread_mutex_lock(&b->mutex);
                        p->is_pip               = val;
                        if (b) b->is_pip   = val;
                        pthread_mutex_unlock(&b->mutex);                               
                        break;
                    case 1227:
                        debug("🟨🟨 1️⃣2️⃣2️⃣7️⃣ is_floating: %d\n", val);
                        debug("🟨🟨 WID: %d\n", wid);
                        pthread_mutex_lock(&b->mutex);
                        p->is_floating          = val;
                        if (b){ 
                            b->is_floating   = val;
                        }
                        pthread_mutex_unlock(&b->mutex);
                        break;       
                }
                if (b) { b->needs_redraw = true; border_update(b, true); }
            }
            break;
        }
        case 1500: {
            debug("🟧 1️⃣5️⃣0️⃣0️⃣ Received bundled window flags\n");
            if (msg->descriptor.size != sizeof(struct yb_flags_payload)) break;
            struct yb_flags_payload *pl = (void *)hdr;
            debug("🟧🟧 ✅ pass count: %d\n", pl->count);
            for (uint32_t i = 0; i < pl->count; ++i) {
                uint32_t wid = pl->window_id[i];
                debug("🟧🟧🟧 wid: %d\n", wid);
                debug("🟧🟧🟧 is_floating: %d, is_sticky: %d, is_stacked: %d, is_pip: %d\n",
                      pl->flags[i].is_floating,
                      pl->flags[i].is_sticky,
                      pl->flags[i].is_stacked,
                      pl->flags[i].is_pip);
                struct yb_flags f = pl->flags[i];
                yb_props_t *p = yb_props_get(wid, true);
                p->is_floating = f.is_floating;
                p->is_sticky   = f.is_sticky;
                p->is_stacked  = f.is_stacked;
                p->is_pip      = f.is_pip;

                struct border *b = table_find(ctx, &wid);
                if (b) {
                    pthread_mutex_lock(&b->mutex);
                    b->is_floating  = p->is_floating;
                    b->is_sticky    = p->is_sticky;
                    b->needs_redraw = true;
                    border_update(b, true);
                    pthread_mutex_unlock(&b->mutex);
                }
            }
            break;
        }
        case 1337: 
        case 1338:
        case 1339: {
          // TODO: Handle events better:
          // TODO: when a window is added to existing stack, index needs to be updated 
          // TODO: "insert feedback" only  appears on stack/window index 0 
            debug("🟦🟦🟦 1339 - re-ordered 🟦🟦🟦\n");
            if (msg->descriptor.size != sizeof(struct yb_stack_payload)) break;
            struct yb_stack_payload *pl = (void *)hdr;
            uint32_t wid = pl->window_id;

            yb_props_t *p = yb_props_get(wid, true);
            struct border *b = table_find(ctx, &wid);
            debug("🟦🟦🟦 wid: %d, index: %d, len: %d, is_topmost: %d\n",
                  pl->window_id,
                  pl->stack_index,
                  pl->stack_len,
                  pl->stack_topmost_wid);
            if(b) {
                pthread_mutex_lock(&b->mutex);
                b->stack_index = pl->stack_index;
                b->stack_len = pl->stack_len;
                b->stack_is_topmost_wid = pl->stack_topmost_wid;
                border_redraw_all_stacked(ctx);      /* ctx is the windows table */
                pthread_mutex_unlock(&b->mutex);
                
            }
                
            break;
        }
        case 1340: {
            debug("🟩🟩🟩 1340 - left/removed 🟩🟩🟩\n");
            if (msg->descriptor.size != sizeof(struct yb_stack_payload)) break;

            struct yb_stack_payload *pl = (void *)hdr;
            uint32_t wid = pl->window_id;

            struct border *b = table_find(ctx, &wid);
            if (b) {
                pthread_mutex_lock(&b->mutex);
                border_clear_stack_state(b);
                pthread_mutex_unlock(&b->mutex);
                border_update(b, /*try_async=*/true);   /* repaint this border only */
            }
            /* Other stacked borders may have new indexes → redraw them too */
            border_redraw_all_stacked(ctx);
            break;
        }
        case 1400: {
            debug("🟩🟩🟩🟩🟩🟩🟩🟩🟩🟩\n");
            break;
        }
        default:
            break;
    }

    mach_msg_destroy(&msg->header);
}

static inline void yabai_register_mach_port(struct table* windows) {
  ipc_space_t task = mach_task_self();
  mach_port_t port;
  if (mach_port_allocate(task,
                         MACH_PORT_RIGHT_RECEIVE,
                         &port                   ) != KERN_SUCCESS) {
    return;
  }

  struct mach_port_limits limits = { 1 };
  if (mach_port_set_attributes(task,
                               port,
                               MACH_PORT_LIMITS_INFO,
                               (mach_port_info_t)&limits,
                               MACH_PORT_LIMITS_INFO_COUNT) != KERN_SUCCESS) {
    return;
  }

  if (!mach_register_port(port, "git.felix.jbevent")) return;

  CFMachPortContext context = {0, (void*)windows};

  CFMachPortRef cf_mach_port = CFMachPortCreateWithPort(NULL,
                                                        port,
                                                        yabai_message,
                                                        &context,
                                                        false         );

  CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(NULL,
                                                            cf_mach_port,
                                                            0            );

  CFRunLoopAddSource(CFRunLoopGetMain(), source, kCFRunLoopDefaultMode);
  CFRelease(source);
  CFRelease(cf_mach_port);
}
#endif
