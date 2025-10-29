// event_corr.c
#include "event_corr.h"
#include <string.h>
#include <stdlib.h>

static uint32_t  g_window_ms = 2000;
static int       g_bits      = 24;
static uint16_t *g_mat       = NULL;

typedef struct { uint32_t ts_ms; EventBits_t bits; } evstamp_t;
#define STAMP_MAX 64
static evstamp_t g_ring[STAMP_MAX];
static int g_head = 0, g_cnt = 0;

void evcorr_init(uint32_t window_ms, int bit_count) {
    g_window_ms = window_ms;
    g_bits = (bit_count > 24) ? 24 : bit_count;
    free(g_mat);
    g_mat = (uint16_t*)calloc((size_t)g_bits * (size_t)g_bits, sizeof(uint16_t));
    g_head = 0;
    g_cnt = 0;
}

int evcorr_bit_count(void) {
    return g_bits;
}

static inline void add_stamp(uint32_t ts_ms, EventBits_t bits) {
    g_ring[g_head] = (evstamp_t){ .ts_ms = ts_ms, .bits = bits };
    g_head = (g_head + 1) % STAMP_MAX;
    if (g_cnt < STAMP_MAX) g_cnt++;
}

void evcorr_on_set(EventGroupHandle_t group, EventBits_t set_bits) {
    (void)group;
    if (!g_mat || set_bits == 0) return;

    uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    for (int i = 0; i < g_cnt; i++) {
        int idx = (g_head - 1 - i + STAMP_MAX) % STAMP_MAX;
        if (now - g_ring[idx].ts_ms > g_window_ms) break;

        EventBits_t a = set_bits;
        EventBits_t b = g_ring[idx].bits;

        for (int bi = 0; bi < g_bits; bi++) {
            EventBits_t m1 = (1u << bi);
            if (!(a & m1)) continue;
            for (int bj = 0; bj < g_bits; bj++) {
                EventBits_t m2 = (1u << bj);
                if (!(b & m2)) continue;
                g_mat[bi * g_bits + bj]++;
            }
        }
    }

    add_stamp(now, set_bits);
}

size_t evcorr_dump(uint16_t *matrix) {
    if (!g_mat || !matrix) return 0;
    size_t n = (size_t)g_bits * (size_t)g_bits;
    memcpy(matrix, g_mat, n * sizeof(uint16_t));
    return n;
}