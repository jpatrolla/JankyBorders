#pragma once
#include <stdint.h>

static inline unsigned long hash_u32(void* k) {
    return *(uint32_t*)k;
}

static inline int cmp_u32(void* a, void* b) {
    return *(uint32_t*)a == *(uint32_t*)b;
}
