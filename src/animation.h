#pragma once
#include <CoreVideo/CoreVideo.h>
#include <pthread.h>

struct animation {
  void* context;
  double frame_time;
  CVDisplayLinkRef link;
};

void animation_init(struct animation* animation);
void animation_start(struct animation* animation, void* proc, void* context);
void animation_stop(struct animation* animation);
uint32_t lerp_rgba(uint32_t a, uint32_t b, float t);