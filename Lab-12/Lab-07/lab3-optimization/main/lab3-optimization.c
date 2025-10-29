#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h"
#include "driver/gpio.h"

static const char *TAG = "LAB6_MEMSYS";

/* =========================================================
 * CONFIGURATION
 * =======================================================*/
#define LED_STATIC_ALLOC GPIO_NUM_2
#define LED_MEMORY_SAVING GPIO_NUM_18
#define LED_OPTIMIZATION GPIO_NUM_19

#define STATIC_BUFFER_SIZE 4096
#define STATIC_BUFFER_COUNT 8
#define MAX_TASKS 4

#define STACK_OPTTEST 4096
#define STACK_MONITOR 3072
#define STACK_MEMUSAGE 3072

/* =========================================================
 * GLOBAL STATS
 * =======================================================*/
typedef struct
{
    size_t static_allocs;
    size_t dynamic_allocs;
    size_t memory_saved;
    uint64_t time_saved;
} mem_stats_t;
static mem_stats_t gstats = {0};

/* =========================================================
 * STATIC BUFFER SYSTEM
 * =======================================================*/
static uint8_t static_bufs[STATIC_BUFFER_COUNT][STATIC_BUFFER_SIZE] __attribute__((aligned(4)));
static bool static_used[STATIC_BUFFER_COUNT];
static SemaphoreHandle_t static_lock;

void *allocate_static_buffer(void)
{
    void *p = NULL;
    if (xSemaphoreTake(static_lock, pdMS_TO_TICKS(50)))
    {
        for (int i = 0; i < STATIC_BUFFER_COUNT; i++)
            if (!static_used[i])
            {
                static_used[i] = true;
                p = static_bufs[i];
                gstats.static_allocs++;
                break;
            }
        xSemaphoreGive(static_lock);
    }
    gpio_set_level(LED_STATIC_ALLOC, p != NULL);
    return p;
}
void free_static_buffer(void *p)
{
    if (!p)
        return;
    if (xSemaphoreTake(static_lock, pdMS_TO_TICKS(50)))
    {
        for (int i = 0; i < STATIC_BUFFER_COUNT; i++)
            if (p == static_bufs[i])
                static_used[i] = false;
        xSemaphoreGive(static_lock);
    }
}

/* =========================================================
 * TEMPLATE-BASED MEMORY POOLS
 * =======================================================*/
typedef struct
{
    void *blocks[64];
    bool used[64];
    int count;
    size_t size;
} pool_t;
typedef struct
{
    pool_t pools[8];
    size_t sizes[8];
    int counts[8];
} tplsys_t;

static tplsys_t tpl = {
    .sizes = {16, 32, 64, 128, 256, 512, 1024, 2048},
    .counts = {64, 32, 16, 16, 8, 8, 4, 2}};

void template_init(void)
{
    for (int i = 0; i < 8; i++)
    {
        tpl.pools[i].size = tpl.sizes[i];
        tpl.pools[i].count = tpl.counts[i];
        for (int j = 0; j < tpl.counts[i]; j++)
            tpl.pools[i].blocks[j] = malloc(tpl.sizes[i]);
        ESP_LOGI(TAG, "TPL[%d]: %d×%dB", i, tpl.counts[i], tpl.sizes[i]);
    }
}

void *template_malloc(size_t s)
{
    for (int i = 0; i < 8; i++)
        if (s <= tpl.sizes[i])
            for (int j = 0; j < tpl.pools[i].count; j++)
                if (!tpl.pools[i].used[j])
                {
                    tpl.pools[i].used[j] = true;
                    return tpl.pools[i].blocks[j];
                }
    return malloc(s);
}
void template_free(void *p)
{
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < tpl.pools[i].count; j++)
            if (p == tpl.pools[i].blocks[j])
            {
                tpl.pools[i].used[j] = false;
                return;
            }
    free(p);
}

/* =========================================================
 * AUTO TUNER
 * =======================================================*/
typedef struct
{
    uint64_t last;
    uint32_t interval;
    float grow, shrink;
    int gstep, sstep;
} tuner_t;
static tuner_t tuner = {0, 10 * 1000000ULL, 0.8f, 0.25f, 4, 2};

void auto_tune_pools(void)
{
    uint64_t now = esp_timer_get_time();
    if (now - tuner.last < tuner.interval)
        return;
    tuner.last = now;
    ESP_LOGI(TAG, "🔧 Auto tuning...");
    for (int i = 0; i < 8; i++)
    {
        int used = 0;
        for (int j = 0; j < tpl.pools[i].count; j++)
            if (tpl.pools[i].used[j])
                used++;
        float ratio = (float)used / tpl.pools[i].count;
        if (ratio > tuner.grow && tpl.pools[i].count + tuner.gstep <= 64)
        {
            int n = tpl.pools[i].count + tuner.gstep;
            for (int j = tpl.pools[i].count; j < n; j++)
                tpl.pools[i].blocks[j] = malloc(tpl.sizes[i]);
            tpl.pools[i].count = n;
            ESP_LOGW(TAG, "Grow pool[%d] → %d", i, n);
        }
        else if (ratio < tuner.shrink && tpl.pools[i].count > tuner.sstep)
        {
            int n = tpl.pools[i].count - tuner.sstep;
            for (int j = n; j < tpl.pools[i].count; j++)
                free(tpl.pools[i].blocks[j]);
            tpl.pools[i].count = n;
            ESP_LOGI(TAG, "Shrink pool[%d] → %d", i, n);
        }
    }
}

/* =========================================================
 * CUSTOM ALLOCATORS
 * =======================================================*/
typedef struct
{
    uint8_t *buf;
    size_t cap, off;
} linear_t;
void linear_init(linear_t *a, void *m, size_t s)
{
    a->buf = m;
    a->cap = s;
    a->off = 0;
}
void *linear_alloc(linear_t *a, size_t s)
{
    if (a->off + s > a->cap)
        return NULL;
    void *p = a->buf + a->off;
    a->off += s;
    return p;
}
void linear_reset(linear_t *a) { a->off = 0; }

typedef struct
{
    uint8_t *buf;
    size_t cap, top;
} stack_t;
void stack_init(stack_t *a, void *m, size_t s)
{
    a->buf = m;
    a->cap = s;
    a->top = 0;
}
void *stack_alloc(stack_t *a, size_t s)
{
    if (a->top + s > a->cap)
        return NULL;
    void *p = a->buf + a->top;
    a->top += s;
    return p;
}
void stack_free(stack_t *a, size_t s) { a->top = (s > a->top) ? 0 : a->top - s; }

typedef struct
{
    uint8_t *buf;
    size_t cap, head, tail;
} ring_t;
void ring_init(ring_t *r, void *m, size_t s)
{
    r->buf = m;
    r->cap = s;
    r->head = r->tail = 0;
}
void *ring_alloc(ring_t *r, size_t s)
{
    if (s > r->cap)
        return NULL;
    if (r->head + s <= r->cap)
    {
        void *p = r->buf + r->head;
        r->head = (r->head + s) % r->cap;
        return p;
    }
    return NULL;
}
void ring_clear(ring_t *r) { r->head = r->tail = 0; }

/* =========================================================
 * NUMA-AWARE ALLOCATION
 * =======================================================*/
typedef struct
{
    void *blocks[32];
    bool used[32];
    int count;
    size_t size;
    SemaphoreHandle_t lock;
} numa_pool_t;
static numa_pool_t numa[2];
void numa_init(void)
{
    for (int c = 0; c < 2; c++)
    {
        numa[c].count = 32;
        numa[c].size = 256;
        numa[c].lock = xSemaphoreCreateMutex();
        for (int i = 0; i < numa[c].count; i++)
            numa[c].blocks[i] = malloc(numa[c].size);
        ESP_LOGI(TAG, "NUMA[%d]:%d×%dB", c, numa[c].count, numa[c].size);
    }
}
void *numa_malloc(size_t s)
{
    int core = xPortGetCoreID();
    numa_pool_t *p = &numa[core];
    void *r = NULL;
    if (xSemaphoreTake(p->lock, pdMS_TO_TICKS(10)))
    {
        for (int i = 0; i < p->count; i++)
            if (!p->used[i])
            {
                p->used[i] = true;
                r = p->blocks[i];
                break;
            }
        xSemaphoreGive(p->lock);
    }
    if (!r)
        r = malloc(s);
    ESP_LOGI(TAG, "NUMA alloc on core%d", core);
    return r;
}
void numa_free(void *p)
{
    for (int c = 0; c < 2; c++)
        if (xSemaphoreTake(numa[c].lock, pdMS_TO_TICKS(10)))
        {
            for (int i = 0; i < numa[c].count; i++)
                if (numa[c].blocks[i] == p)
                {
                    numa[c].used[i] = false;
                    xSemaphoreGive(numa[c].lock);
                    return;
                }
            xSemaphoreGive(numa[c].lock);
        }
    free(p);
}

/* =========================================================
 * PREDICTIVE MEMORY MANAGEMENT
 * =======================================================*/
#define PRED_WINDOW 8
typedef struct
{
    size_t heap[PRED_WINDOW];
    size_t pool[PRED_WINDOW];
    int idx;
    bool full;
} predictor_t;
static predictor_t pred = {0};

static float avg(const size_t *a, int len, bool full, int idx)
{
    int n = full ? len : idx;
    if (n <= 0)
        return 0;
    float s = 0;
    for (int i = 0; i < n; i++)
        s += a[i];
    return s / n;
}
static void record_usage(void)
{
    size_t free = esp_get_free_heap_size(), total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    size_t used = total - free, pu = 0;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < tpl.pools[i].count; j++)
            if (tpl.pools[i].used[j])
                pu++;
    pred.heap[pred.idx] = used;
    pred.pool[pred.idx] = pu;
    pred.idx++;
    if (pred.idx >= PRED_WINDOW)
    {
        pred.idx = 0;
        pred.full = true;
    }
}
static void analyze_trend(void)
{
    float avgh = avg(pred.heap, PRED_WINDOW, pred.full, pred.idx);
    float avgp = avg(pred.pool, PRED_WINDOW, pred.full, pred.idx);
    size_t free = esp_get_free_heap_size(), total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    float now = (float)(total - free), diff = ((now - avgh) / avgh) * 100.0f;
    ESP_LOGI(TAG, "\n📊 Predictive: heapΔ=%.2f%% poolΔ=%.2f%%", diff,
             ((float)pred.pool[(pred.idx + PRED_WINDOW - 1) % PRED_WINDOW] - avgp) / avgp * 100.0f);
    if (diff > 10.0f)
    {
        ESP_LOGW(TAG, "⚠️ Rising usage!");
        gpio_set_level(LED_MEMORY_SAVING, 1);
    }
    else if (diff < -10.0f)
    {
        ESP_LOGI(TAG, "✅ Stable usage");
        gpio_set_level(LED_MEMORY_SAVING, 0);
    }
}
static void predictive_update(void)
{
    record_usage();
    if (pred.full)
        analyze_trend();
}

/* =========================================================
 * TASKS
 * =======================================================*/
void opt_task(void *arg)
{
    linear_t L;
    stack_t S;
    ring_t R;
    uint8_t lb[512], sb[512], rb[512];
    linear_init(&L, lb, sizeof(lb));
    stack_init(&S, sb, sizeof(sb));
    ring_init(&R, rb, sizeof(rb));
    while (1)
    {
        linear_alloc(&L, 100);
        stack_alloc(&S, 128);
        ring_alloc(&R, 128);
        uint8_t sample[32];
        for (int i = 0; i < 32; i++)
            sample[i] = (i < 16) ? 0xAA : 0x55;
        uint8_t comp[64], decomp[64];
        size_t clen = 0; // simple compression demo
        for (size_t i = 0; i < 32; i += 2)
        {
            comp[clen++] = sample[i];
            comp[clen++] = 2;
        }
        memcpy(decomp, sample, sizeof(sample));
        void *n1 = numa_malloc(200);
        vTaskDelay(pdMS_TO_TICKS(100));
        numa_free(n1);
        ESP_LOGI(TAG, "OptTask done, stackHW=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(8000));
    }
}
void mem_task(void *arg)
{
    while (1)
    {
        void *p = allocate_static_buffer();
        vTaskDelay(pdMS_TO_TICKS(1000));
        free_static_buffer(p);
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}
void mon_task(void *arg)
{
    while (1)
    {
        auto_tune_pools();
        predictive_update();
        ESP_LOGI(TAG, "Monitor stackHW=%u", (unsigned)uxTaskGetStackHighWaterMark(NULL));
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* =========================================================
 * APP MAIN
 * =======================================================*/
static StackType_t st[MAX_TASKS][STACK_OPTTEST];
static StaticTask_t buf[MAX_TASKS];
int slot = 0;
BaseType_t create_static(TaskFunction_t f, const char *n, int p, void *a, int sz)
{
    if (slot >= MAX_TASKS)
        return pdFAIL;
    xTaskCreateStatic(f, n, sz, a, p, st[slot], &buf[slot]);
    ESP_LOGI(TAG, "Task[%d]=%s", slot, n);
    slot++;
    return pdPASS;
}
void app_main(void)
{
    ESP_LOGI(TAG, "🚀 LAB6 Intelligent Memory System Start");
    gpio_set_direction(LED_STATIC_ALLOC, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MEMORY_SAVING, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_OPTIMIZATION, GPIO_MODE_OUTPUT);
    static_lock = xSemaphoreCreateMutex();
    template_init();
    numa_init();
    create_static(opt_task, "OptTask", 5, NULL, STACK_OPTTEST);
    create_static(mem_task, "MemTask", 4, NULL, STACK_MEMUSAGE);
    create_static(mon_task, "MonTask", 3, NULL, STACK_MONITOR);
    ESP_LOGI(TAG, "✅ System Ready");
}