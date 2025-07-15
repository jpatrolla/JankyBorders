#include "sidebar.h"
#include "misc/connection.h"
#include "misc/helpers.h"
#include <ApplicationServices/ApplicationServices.h>
#include <CoreGraphics/CoreGraphics.h>
extern struct table g_windows;

/*
struct indicator:
- wid
- sid
- icon file/graphic
- focused
- hidden
- sticky
*/

// Creates a 50 px × 400 px magenta sidebar that hugs the left edge of the
// main display, is vertically centred, and always stays on top.
static uint32_t g_sidebar_wid = 0;

// Drww the UI and load existing windows
void sidebar_init(void) {
  debug("creating sidebar\n");
  /* Obtain a SkyLight connection for the current process */
  int cid = SLSMainConnectionID();
  if (!cid)
    return;

  /* Geometry for the primary display */
  CGRect d_bounds = CGDisplayBounds(CGMainDisplayID());

  const CGFloat SB_WIDTH = 50.0f;
  const CGFloat SB_HEIGHT = 400.0f;

  float x = d_bounds.origin.x;
  // float y = (d_bounds.size.height / 2.0f) - (SB_HEIGHT / 2.0f);
  float y = d_bounds.origin.y;
  CGRect frame = CGRectMake(x, y, SB_WIDTH, SB_HEIGHT);

  /* Create a region matching the sidebar’s frame */
  CFTypeRef frame_region = NULL;
  CGSNewRegionWithRect(&frame, &frame_region);

  /* Empty region for the opaque‑shape parameter (required by the API) */
  CFTypeRef empty_region = CGRegionCreateEmptyRegion();

  uint64_t set_tags = 0; /* No special tags at creation */
  SLSNewWindowWithOpaqueShapeAndContext(
      cid, kCGBackingStoreBuffered, /* window backing type */
      frame_region,                 /* frame region */
      empty_region,                 /* opaque shape (none) */
      0,                            /* options */
      &set_tags,                    /* tags */
      x,                            /* x position */
      32,                           /* y position */
      sizeof(set_tags) * 8,         /* tag size in bits */
      &g_sidebar_wid,               /* out: window id */
      NULL);                        /* context (unused) */

  /* Release temporary CoreFoundation objects */
  CFRelease(empty_region);
  CFRelease(frame_region);

  SLSSetWindowAlpha(cid, g_sidebar_wid, .3f);

  /* Pin window above everything else */
  int64_t level = CGWindowLevelForKey(kCGMaximumWindowLevelKey);
  SLSSetWindowLevel(cid, g_sidebar_wid, &level);

  /* Remove window shadow */
  uint64_t tags[2] = {0};
  tags[0] = (1 << 3); /* kCGSDisableShadowTagBit is bit 3 */
  SLSSetWindowTags(cid, g_sidebar_wid, tags, 32);

  /* Draw opaque magenta rectangle */
  CGContextRef ctx = SLWindowContextCreate(cid, g_sidebar_wid, NULL);
  if (ctx) {
    CGContextSetRGBFillColor(ctx, 1.0, 0.0, 1.0, 0.4);
    CGContextFillRect(ctx, CGRectMake(0, 0, SB_WIDTH, SB_HEIGHT));
    CGContextFlush(ctx);
    CGContextRelease(ctx);
  }
  SLSOrderWindow(cid, g_sidebar_wid, 1, 0); /* 1 = above */
}

//create icon

// update icon

// destroy icon
