#include "animation.h"

void animation_init(struct animation* animation) {
  memset(animation, 0, sizeof(struct animation));
}


#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
void animation_start(struct animation* animation, void* proc, void* context) {
  assert(animation->link == NULL);
  assert(animation->context == NULL);
  CVDisplayLinkCreateWithActiveCGDisplays(&animation->link);
  CVTime refresh_period
            = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(animation->link);
  animation->frame_time = 1e6 * (double)refresh_period.timeValue
                        / (double)refresh_period.timeScale;

  animation->context = context;
  CVDisplayLinkSetOutputCallback(animation->link, proc, animation);
  CVDisplayLinkStart(animation->link);
}

void animation_stop(struct animation* animation) {
  if (animation->link) {
    CVDisplayLinkStop(animation->link);
    CVDisplayLinkRelease(animation->link);
    animation->link = NULL;
  }
  if (animation->context) free(animation->context);
  animation->context = NULL;
}

uint32_t lerp_rgba(uint32_t a, uint32_t b, float t)
{
    uint8_t ar = (a >> 24) & 0xFF, ag = (a >> 16) & 0xFF,
            ab = (a >>  8) & 0xFF, aa =  a        & 0xFF;
    uint8_t br = (b >> 24) & 0xFF, bg = (b >> 16) & 0xFF,
            bb = (b >>  8) & 0xFF, ba =  b        & 0xFF;

    uint8_t cr = ar + (br - ar) * t;
    uint8_t cg = ag + (bg - ag) * t;
    uint8_t cb = ab + (bb - ab) * t;
    uint8_t ca = aa + (ba - aa) * t;

    return (cr << 24) | (cg << 16) | (cb << 8) | ca;
}
#pragma clang diagnostic pop
