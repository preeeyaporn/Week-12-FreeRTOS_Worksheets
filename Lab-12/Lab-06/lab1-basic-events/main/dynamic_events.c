// dynamic_events.c
#include "dynamic_events.h"
#include <string.h>

#ifndef DYN_MAX_BITS
#define DYN_MAX_BITS 24   // ใช้บิต 0..23 ของ EventGroup (FreeRTOS doc: ~24 usable bits)
#endif

typedef struct {
    EventBits_t bit;
    const char* name; // ไม่ copy string
    bool        used;
} dyn_rec_t;

static dyn_rec_t  g_map[DYN_MAX_BITS];
static EventBits_t g_forbid = 0;

bool dyn_init(EventBits_t reserved_mask) {
    memset(g_map, 0, sizeof(g_map));
    g_forbid = reserved_mask;
    return true;
}

EventBits_t dyn_acquire(const char* name) {
    for (int i = 0; i < DYN_MAX_BITS; i++) {
        EventBits_t b = (1u << i);
        if ((g_forbid & b) == 0 && !g_map[i].used) {
            g_map[i].used = true;
            g_map[i].bit  = b;
            g_map[i].name = name;
            return b;
        }
    }
    return 0; // หมดบิตว่าง
}

bool dyn_release(EventBits_t bit) {
    for (int i = 0; i < DYN_MAX_BITS; i++) {
        if (g_map[i].used && g_map[i].bit == bit) {
            g_map[i].used = false;
            g_map[i].bit  = 0;
            g_map[i].name = NULL;
            return true;
        }
    }
    return false;
}

const char* dyn_name(EventBits_t bit) {
    for (int i = 0; i < DYN_MAX_BITS; i++) {
        if (g_map[i].used && g_map[i].bit == bit) {
            return g_map[i].name;
        }
    }
    return NULL;
}