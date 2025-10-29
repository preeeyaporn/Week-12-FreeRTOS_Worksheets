// event_log.c
#include "event_log.h"
#include <string.h>
#include <stdlib.h>

static ev_record_t       *g_buf   = NULL;
static size_t             g_cap   = 0;
static size_t             g_head  = 0;  // ชี้ตำแหน่งจะเขียนถัดไป
static size_t             g_count = 0;  // จำนวนที่ใช้งานจริง (<= g_cap)
static SemaphoreHandle_t  g_mtx   = NULL;

bool evlog_init(size_t capacity){
    g_buf = (ev_record_t*)calloc(capacity, sizeof(ev_record_t));
    if (!g_buf) return false;
    g_cap = capacity;
    g_head = 0;
    g_count = 0;
    g_mtx = xSemaphoreCreateMutex();
    return (g_mtx != NULL);
}

void evlog_add(EventGroupHandle_t group, EventBits_t set_bits, const char* src){
    if (!g_buf || !g_mtx) {
        // fallback: ตั้งบิตอย่างน้อย
        xEventGroupSetBits(group, set_bits);
        return;
    }

    // อ่านสถานะก่อน
    EventBits_t before = xEventGroupGetBits(group);
    // ตั้งบิตจริง
    xEventGroupSetBits(group, set_bits);
    // อ่านสถานะหลัง
    EventBits_t after  = xEventGroupGetBits(group);

    // สร้าง record
    ev_record_t rec = {
        .ts_ms       = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
        .before_bits = before,
        .set_bits    = set_bits,
        .after_bits  = after,
        .source      = src
    };

    // เก็บลง ring buffer
    xSemaphoreTake(g_mtx, portMAX_DELAY);
    g_buf[g_head] = rec;
    g_head = (g_head + 1) % g_cap;
    if (g_count < g_cap) g_count++;
    xSemaphoreGive(g_mtx);
}

size_t evlog_dump(ev_record_t *out, size_t max){
    if (!g_buf || !g_mtx || !out || max == 0) return 0;

    xSemaphoreTake(g_mtx, portMAX_DELAY);
    size_t n = 0;
    size_t start = (g_head + g_cap - g_count) % g_cap; // ตำแหน่งรายการ “เก่าสุด”
    for (; n < g_count && n < max; n++){
        out[n] = g_buf[(start + n) % g_cap];
    }
    xSemaphoreGive(g_mtx);
    return n;
}