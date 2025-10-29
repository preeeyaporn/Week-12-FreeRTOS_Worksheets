#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "MEM_POOLS";

// ============================
//         GPIO Indicators
// ============================
#define LED_SMALL_POOL GPIO_NUM_2
#define LED_MEDIUM_POOL GPIO_NUM_4
#define LED_LARGE_POOL GPIO_NUM_5
#define LED_POOL_FULL GPIO_NUM_18
#define LED_POOL_ERROR GPIO_NUM_19

// ============================
//     Pool configuration (defaults)
// ============================
#define SMALL_POOL_BLOCK_SIZE 64
#define SMALL_POOL_BLOCK_COUNT 64
#define MEDIUM_POOL_BLOCK_SIZE 256
#define MEDIUM_POOL_BLOCK_COUNT 32
#define LARGE_POOL_BLOCK_SIZE 1024
#define LARGE_POOL_BLOCK_COUNT 24
#define HUGE_POOL_BLOCK_SIZE 4096
#define HUGE_POOL_BLOCK_COUNT 8

// ============================
//     Pool & Sync Structs
// ============================
typedef struct memory_block
{
    struct memory_block *next;
    uint32_t magic;
    uint32_t pool_id;
    uint64_t alloc_time;
    size_t size_used; // ขนาดข้อมูลจริงที่ผู้ใช้ใช้ (ช่วยเรื่อง migrate/realloc)
} memory_block_t;

#include "pool_sync.h" // ต้องมี pool_sync_t + SYNC_TIMEOUT_MS + ฟังก์ชัน lock ต่าง ๆ

typedef struct
{
    const char *name;
    size_t block_size;
    size_t block_count;
    size_t alignment;
    uint32_t caps;

    void *pool_memory;
    memory_block_t *free_list;
    uint8_t *usage_bitmap; // 1 bit ต่อ block

    // stats
    size_t allocated_blocks;
    size_t peak_usage;
    uint64_t total_allocations;
    uint64_t total_deallocations;
    uint64_t allocation_time_total;
    uint64_t deallocation_time_total;
    uint32_t allocation_failures;

    pool_sync_t sync;
    uint32_t pool_id;
} memory_pool_t;

typedef enum
{
    POOL_SMALL = 0,
    POOL_MEDIUM,
    POOL_LARGE,
    POOL_HUGE,
    POOL_COUNT
} pool_type_t;

static memory_pool_t pools[POOL_COUNT];

// Magic numbers
#define POOL_MAGIC_FREE 0xDEADBEEF
#define POOL_MAGIC_ALLOC 0xCAFEBABE

typedef struct
{
    const char *name;
    size_t block_size;
    size_t block_count;
    uint32_t caps;
    gpio_num_t led_pin;
} pool_config_t;

static const pool_config_t POOL_DEFAULTS[POOL_COUNT] = {
    {"Small", SMALL_POOL_BLOCK_SIZE, SMALL_POOL_BLOCK_COUNT, MALLOC_CAP_DEFAULT, LED_SMALL_POOL},
    {"Medium", MEDIUM_POOL_BLOCK_SIZE, MEDIUM_POOL_BLOCK_COUNT, MALLOC_CAP_DEFAULT, LED_MEDIUM_POOL},
    {"Large", LARGE_POOL_BLOCK_SIZE, LARGE_POOL_BLOCK_COUNT, MALLOC_CAP_DEFAULT, LED_LARGE_POOL},
    {"Huge", HUGE_POOL_BLOCK_SIZE, HUGE_POOL_BLOCK_COUNT, MALLOC_CAP_DEFAULT, LED_POOL_FULL},
};

// ============================
//    Persistence (NVS) config
// ============================
#define NVS_NS "mem_pools"
#define SAVE_INTERVAL_MS 30000ULL // อัตโนมัติทุก ~30s หากมีการเปลี่ยนแปลง
static nvs_handle_t g_nvs = 0;
static bool g_nvs_ready = false;
static uint64_t g_last_saved_stamp = 0;
static uint32_t g_dirty_epoch = 1; // เพิ่มเมื่อ stats เปลี่ยน เพื่อ trigger save

// ============================
//       Helpers / inline
// ============================
static inline memory_block_t *data_to_block(void *data)
{
    return (memory_block_t *)((uint8_t *)data - sizeof(memory_block_t));
}
static inline void *block_to_data(memory_block_t *blk)
{
    return (void *)((uint8_t *)blk + sizeof(memory_block_t));
}

static inline bool ptr_in_pool_range(const memory_pool_t *pool, const void *data_ptr)
{
    if (!pool || !pool->pool_memory || pool->block_count == 0 || !data_ptr)
        return false;

    size_t header = sizeof(memory_block_t);
    size_t aligned = (pool->block_size + pool->alignment - 1) & ~(pool->alignment - 1);
    size_t total = header + aligned;

    const uint8_t *base = (const uint8_t *)pool->pool_memory;
    const uint8_t *end = base + (total * pool->block_count);
    const uint8_t *blkp = (const uint8_t *)data_ptr - header;

    return (blkp >= base) && (blkp < end) && ((((size_t)(blkp - base)) % total) == 0);
}

// ============================
//   NVS helpers (open/close)
// ============================
static bool nvs_open_namespace(void)
{
    if (g_nvs_ready)
        return true;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_LOGW(TAG, "NVS re-init required...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = nvs_open(NVS_NS, NVS_READWRITE, &g_nvs);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "NVS open ns '%s' failed: %s", NVS_NS, esp_err_to_name(err));
        return false;
    }
    g_nvs_ready = true;
    return true;
}

static inline void nvs_mark_dirty(void) { g_dirty_epoch++; }

// ============================
//   Pool core (init/malloc/free)
// ============================
static bool init_memory_pool(memory_pool_t *pool, const pool_config_t *cfg, uint32_t pool_id)
{
    memset(pool, 0, sizeof(*pool));
    pool->name = cfg->name;
    pool->block_size = cfg->block_size;
    pool->block_count = cfg->block_count;
    pool->alignment = 4;
    pool->caps = cfg->caps;
    pool->pool_id = pool_id;

    size_t header = sizeof(memory_block_t);
    size_t aligned = (cfg->block_size + pool->alignment - 1) & ~(pool->alignment - 1);
    size_t total = header + aligned;
    size_t bytes = total * cfg->block_count;

    pool->pool_memory = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!pool->pool_memory)
    {
        ESP_LOGE(TAG, "Failed to alloc %s pool memory", cfg->name);
        return false;
    }

    size_t bits = cfg->block_count;
    size_t bbytes = (bits + 7) / 8;
    pool->usage_bitmap = heap_caps_calloc(1, bbytes, MALLOC_CAP_8BIT);
    if (!pool->usage_bitmap)
    {
        heap_caps_free(pool->pool_memory);
        ESP_LOGE(TAG, "Failed to alloc %s bitmap", cfg->name);
        return false;
    }

    // build free-list
    uint8_t *p = (uint8_t *)pool->pool_memory;
    for (size_t i = 0; i < cfg->block_count; i++)
    {
        memory_block_t *blk = (memory_block_t *)(p + i * total);
        blk->magic = POOL_MAGIC_FREE;
        blk->pool_id = pool->pool_id;
        blk->alloc_time = 0;
        blk->size_used = 0;
        blk->next = pool->free_list;
        pool->free_list = blk;
    }

    if (!pool_sync_init(&pool->sync))
    {
        heap_caps_free(pool->pool_memory);
        heap_caps_free(pool->usage_bitmap);
        ESP_LOGE(TAG, "Failed to init sync for %s", cfg->name);
        return false;
    }

    ESP_LOGI(TAG, "✅ Initialized %s: %d blocks × %d bytes", cfg->name, (int)cfg->block_count, (int)cfg->block_size);
    return true;
}

static void *pool_malloc(memory_pool_t *pool)
{
    uint64_t t0 = esp_timer_get_time();
    void *result = NULL;

    if (pool_sync_write_lock(&pool->sync, pdMS_TO_TICKS(SYNC_TIMEOUT_MS)))
    {
        if (pool->free_list)
        {
            memory_block_t *blk = pool->free_list;
            pool->free_list = blk->next;

            if (blk->magic != POOL_MAGIC_FREE || blk->pool_id != pool->pool_id)
            {
                ESP_LOGE(TAG, "🚨 Corruption in %s block %p", pool->name, (void *)blk);
                gpio_set_level(LED_POOL_ERROR, 1);
                pool_sync_write_unlock(&pool->sync);
                return NULL;
            }

            blk->magic = POOL_MAGIC_ALLOC;
            blk->next = NULL;
            blk->alloc_time = esp_timer_get_time();
            blk->size_used = 0;

            pool->allocated_blocks++;
            if (pool->allocated_blocks > pool->peak_usage)
                pool->peak_usage = pool->allocated_blocks;
            pool->total_allocations++;
            nvs_mark_dirty();

            // mark bitmap
            size_t header = sizeof(memory_block_t);
            size_t aligned = (pool->block_size + pool->alignment - 1) & ~(pool->alignment - 1);
            size_t total = header + aligned;
            size_t idx = ((size_t)((uint8_t *)blk - (uint8_t *)pool->pool_memory)) / total;
            if (idx < pool->block_count)
                pool->usage_bitmap[idx >> 3] |= (1 << (idx & 7));

            result = block_to_data(blk);
        }
        else
        {
            pool->allocation_failures++;
            nvs_mark_dirty();
            gpio_set_level(LED_POOL_FULL, 1);
            if ((pool->allocation_failures % 32) == 1)
            {
                ESP_LOGW(TAG, "🔴 %s exhausted! (%d/%d)", pool->name, (int)pool->allocated_blocks, (int)pool->block_count);
            }
        }
        pool_sync_write_unlock(&pool->sync);
    }

    pool->allocation_time_total += (esp_timer_get_time() - t0);
    return result;
}

static bool pool_free(memory_pool_t *pool, void *ptr)
{
    if (!ptr_in_pool_range(pool, ptr))
        return false;

    uint64_t t0 = esp_timer_get_time();
    bool ok = false;

    if (pool_sync_write_lock(&pool->sync, pdMS_TO_TICKS(SYNC_TIMEOUT_MS)))
    {
        memory_block_t *blk = data_to_block(ptr);

        if (blk->magic != POOL_MAGIC_ALLOC || blk->pool_id != pool->pool_id)
        {
            ESP_LOGE(TAG, "🚨 Invalid block %p for %s! magic=0x%08X pid=%lu",
                     ptr, pool->name, blk->magic, (unsigned long)blk->pool_id);
            gpio_set_level(LED_POOL_ERROR, 1);
            pool_sync_write_unlock(&pool->sync);
            return false;
        }

        size_t header = sizeof(memory_block_t);
        size_t aligned = (pool->block_size + pool->alignment - 1) & ~(pool->alignment - 1);
        size_t total = header + aligned;
        size_t idx = ((size_t)((uint8_t *)blk - (uint8_t *)pool->pool_memory)) / total;
        if (idx < pool->block_count)
            pool->usage_bitmap[idx >> 3] &= ~(1 << (idx & 7));

        blk->magic = POOL_MAGIC_FREE;
        blk->size_used = 0;
        blk->next = pool->free_list;
        pool->free_list = blk;

        if (pool->allocated_blocks)
            pool->allocated_blocks--;
        pool->total_deallocations++;
        nvs_mark_dirty();
        ok = true;

        pool_sync_write_unlock(&pool->sync);
    }

    pool->deallocation_time_total += (esp_timer_get_time() - t0);
    return ok;
}

// ============================
//   Smart API (malloc/free)
// ============================
static void *smart_pool_malloc(size_t size)
{
    size_t need = size + 16; // safety margin
    for (int i = 0; i < POOL_COUNT; i++)
    {
        if (need <= pools[i].block_size)
        {
            void *p = pool_malloc(&pools[i]);
            if (p)
            {
                memory_block_t *blk = data_to_block(p);
                size_t cap = pools[i].block_size;
                blk->size_used = (size <= cap) ? size : cap;

                // pulse LED
                gpio_set_level(((gpio_num_t)POOL_DEFAULTS[i].led_pin), 1);
                vTaskDelay(1);
                gpio_set_level(((gpio_num_t)POOL_DEFAULTS[i].led_pin), 0);
                return p;
            }
        }
    }
    return heap_caps_malloc(size, MALLOC_CAP_DEFAULT); // fallback heap
}

static bool smart_pool_free(void *ptr)
{
    if (!ptr)
        return false;
    for (int i = 0; i < POOL_COUNT; i++)
    {
        if (ptr_in_pool_range(&pools[i], ptr))
            return pool_free(&pools[i], ptr);
    }
    heap_caps_free(ptr);
    return true;
}

// ============================
//        (Optional) Migration
// ============================
static void *pool_realloc_smart(void *old_ptr, size_t new_size)
{
    if (old_ptr == NULL)
        return smart_pool_malloc(new_size);
    if (new_size == 0)
    {
        smart_pool_free(old_ptr);
        return NULL;
    }

    memory_pool_t *owner = NULL;
    for (int i = 0; i < POOL_COUNT; i++)
    {
        if (ptr_in_pool_range(&pools[i], old_ptr))
        {
            owner = &pools[i];
            break;
        }
    }
    if (!owner)
    {
        void *np = smart_pool_malloc(new_size);
        if (!np)
            return NULL;
        memcpy(np, old_ptr, new_size); // ผู้เรียกควรรู้ขนาดเดิม
        heap_caps_free(old_ptr);
        return np;
    }

    size_t need = new_size + 16;
    if (need <= owner->block_size)
    {
        data_to_block(old_ptr)->size_used = new_size;
        return old_ptr;
    }

    void *np = smart_pool_malloc(new_size);
    if (!np)
        return NULL;
    size_t old_used = data_to_block(old_ptr)->size_used ? data_to_block(old_ptr)->size_used : owner->block_size;
    memcpy(np, old_ptr, (new_size < old_used) ? new_size : old_used);
    smart_pool_free(old_ptr);
    data_to_block(np)->size_used = new_size;
    return np;
}

// ============================
//     Resize (boot-time safe)
// ============================
static bool resize_pool_boot(memory_pool_t *pool, size_t new_count)
{
    // เรียกตอนบูตเท่านั้น (ยังไม่มี allocation)
    if (!pool || new_count == 0)
        return false;
    if (pool->allocated_blocks != 0)
    {
        ESP_LOGW(TAG, "resize_pool_boot(%s): busy", pool->name);
        return false;
    }

    size_t header = sizeof(memory_block_t);
    size_t aligned = (pool->block_size + pool->alignment - 1) & ~(pool->alignment - 1);
    size_t total = header + aligned;
    size_t bytes = total * new_count;

    void *new_mem = heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    size_t bits = new_count;
    size_t bbytes = (bits + 7) / 8;
    uint8_t *bitmap = (uint8_t *)heap_caps_calloc(1, bbytes, MALLOC_CAP_8BIT);
    if (!new_mem || !bitmap)
    {
        if (new_mem)
            heap_caps_free(new_mem);
        if (bitmap)
            heap_caps_free(bitmap);
        ESP_LOGE(TAG, "resize_pool_boot(%s): alloc fail", pool->name);
        return false;
    }

    // free list ใหม่
    memory_block_t *new_free = NULL;
    uint8_t *p = (uint8_t *)new_mem;
    for (size_t i = 0; i < new_count; i++)
    {
        memory_block_t *blk = (memory_block_t *)(p + i * total);
        blk->magic = POOL_MAGIC_FREE;
        blk->pool_id = pool->pool_id;
        blk->alloc_time = 0;
        blk->size_used = 0;
        blk->next = new_free;
        new_free = blk;
    }

    // ทิ้งของเก่า
    if (pool->pool_memory)
        heap_caps_free(pool->pool_memory);
    if (pool->usage_bitmap)
        heap_caps_free(pool->usage_bitmap);

    pool->pool_memory = new_mem;
    pool->usage_bitmap = bitmap;
    pool->free_list = new_free;
    pool->block_count = new_count;
    pool->allocated_blocks = 0;
    pool->peak_usage = 0;

    ESP_LOGI(TAG, "Boot-resized %s → %d blocks", pool->name, (int)new_count);
    return true;
}

// ============================
//   Persistence: SAVE / LOAD
// ============================
static esp_err_t pools_persist_load_counts_from_nvs(size_t out_counts[POOL_COUNT])
{
    if (!nvs_open_namespace())
        return ESP_FAIL;

    for (int i = 0; i < POOL_COUNT; i++)
    {
        char key[32];
        snprintf(key, sizeof(key), "%s_blocks", POOL_DEFAULTS[i].name); // "Small_blocks" ...
        uint32_t val = 0;
        esp_err_t err = nvs_get_u32(g_nvs, key, &val);
        if (err == ESP_OK && val > 0)
        {
            out_counts[i] = val;
        }
        else
        {
            out_counts[i] = POOL_DEFAULTS[i].block_count; // default
        }
    }
    return ESP_OK;
}

static esp_err_t pools_persist_save_counts_to_nvs(void)
{
    if (!nvs_open_namespace())
        return ESP_FAIL;

    for (int i = 0; i < POOL_COUNT; i++)
    {
        char key[32];
        snprintf(key, sizeof(key), "%s_blocks", pools[i].name);
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u32(g_nvs, key, (uint32_t)pools[i].block_count));
    }
    return nvs_commit(g_nvs);
}

static esp_err_t pools_persist_save_stats_to_nvs(void)
{
    if (!nvs_open_namespace())
        return ESP_FAIL;

    for (int i = 0; i < POOL_COUNT; i++)
    {
        char k1[32], k2[32], k3[32], k4[32];
        snprintf(k1, sizeof(k1), "%s_peak", pools[i].name);
        snprintf(k2, sizeof(k2), "%s_alloc", pools[i].name);
        snprintf(k3, sizeof(k3), "%s_freec", pools[i].name);
        snprintf(k4, sizeof(k4), "%s_fail", pools[i].name);

        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u32(g_nvs, k1, (uint32_t)pools[i].peak_usage));
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u64(g_nvs, k2, pools[i].total_allocations));
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u64(g_nvs, k3, pools[i].total_deallocations));
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_set_u32(g_nvs, k4, pools[i].allocation_failures));
    }
    return nvs_commit(g_nvs);
}

// Public: on-demand save (เรียกจากที่ไหนก็ได้)
void pools_persist_save_now(void)
{
    if (!g_nvs_ready && !nvs_open_namespace())
        return;

    ESP_LOGI(TAG, "MON | Persist: saving counts+stats to NVS...");
    ESP_ERROR_CHECK_WITHOUT_ABORT(pools_persist_save_counts_to_nvs());
    ESP_ERROR_CHECK_WITHOUT_ABORT(pools_persist_save_stats_to_nvs());
    g_last_saved_stamp = esp_timer_get_time() / 1000ULL;
    ESP_LOGI(TAG, "MON | Persist: done");
}

// ============================
//     Monitor / utilities
// ============================
static void print_pool_statistics(void)
{
    ESP_LOGI(TAG, "\n📊 == MEMORY POOL STATISTICS ==");
    for (int i = 0; i < POOL_COUNT; i++)
    {
        memory_pool_t *pool = &pools[i];
        if (pool_sync_stats_lock(&pool->sync, pdMS_TO_TICKS(SYNC_TIMEOUT_MS)))
        {
            int used = (int)pool->allocated_blocks;
            int tot = (int)pool->block_count;
            ESP_LOGI(TAG, "%-6s | used %2d/%-2d  peak=%-2d  alloc=%llu  free=%llu  fail=%lu",
                     pool->name, used, tot, (int)pool->peak_usage,
                     (unsigned long long)pool->total_allocations,
                     (unsigned long long)pool->total_deallocations,
                     (unsigned long)pool->allocation_failures);
            pool_sync_stats_unlock(&pool->sync);
        }
    }
}

// Monitor task: แสดงผล + autosave NVS เป็นระยะเมื่อมี “dirty”
static void pool_monitor_task(void *arg)
{
    ESP_LOGI(TAG, "MON | started");
    uint32_t last_seen_epoch = g_dirty_epoch;

    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(5000));

        print_pool_statistics();

        // LED เตือน
        bool any_full = false;
        for (int i = 0; i < POOL_COUNT; i++)
        {
            if (pools[i].allocated_blocks >= pools[i].block_count)
            {
                any_full = true;
                break;
            }
        }
        gpio_set_level(LED_POOL_FULL, any_full ? 1 : 0);

        // Autosave ถ้ามีสถิติเปลี่ยน หรือผ่านไปนานกว่า SAVE_INTERVAL_MS
        uint64_t now_ms = esp_timer_get_time() / 1000ULL;
        bool epoch_changed = (g_dirty_epoch != last_seen_epoch);
        bool timed = (now_ms - g_last_saved_stamp) > SAVE_INTERVAL_MS;
        if (epoch_changed || timed)
        {
            pools_persist_save_now();
            last_seen_epoch = g_dirty_epoch;
        }
    }
}

// ============================
//      Demo / test task
// ============================
static void persistence_demo_task(void *arg)
{
    // ตัวอย่างการใช้งาน: จอง/คืน/ปรับขนาด แล้วปล่อยให้ monitor บันทึกลง NVS ให้อัตโนมัติ
    void *p = smart_pool_malloc(120); // น่าจะได้ Medium
    vTaskDelay(100 / portTICK_PERIOD_MS);

    p = pool_realloc_smart(p, 1800); // ย้ายไป Large
    vTaskDelay(100 / portTICK_PERIOD_MS);

    smart_pool_free(p);

    // บันทึก on-demand (สมมุติว่าเป็นการกดปุ่ม)
    pools_persist_save_now();

    vTaskDelete(NULL);
}

// ============================
//            app_main
// ============================
void app_main(void)
{
    ESP_LOGI(TAG, "🚀 Memory Pools + Persistence starting...");

    // GPIO
    gpio_set_direction(LED_SMALL_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MEDIUM_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_LARGE_POOL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_POOL_FULL, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_POOL_ERROR, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_SMALL_POOL, 0);
    gpio_set_level(LED_MEDIUM_POOL, 0);
    gpio_set_level(LED_LARGE_POOL, 0);
    gpio_set_level(LED_POOL_FULL, 0);
    gpio_set_level(LED_POOL_ERROR, 0);

    // ----- Load desired block counts from NVS (ถ้ามี) -----
    size_t boot_counts[POOL_COUNT];
    pools_persist_load_counts_from_nvs(boot_counts);

    // ----- Init pools (ใช้จำนวนที่โหลดมา) -----
    for (int i = 0; i < POOL_COUNT; i++)
    {
        pool_config_t cfg = POOL_DEFAULTS[i]; // copy
        cfg.block_count = boot_counts[i];     // override from NVS
        if (!init_memory_pool(&pools[i], &cfg, (uint32_t)(i + 1)))
        {
            ESP_LOGE(TAG, "Init %s failed", cfg.name);
            return;
        }
    }
    print_pool_statistics();

    // ----- Tasks -----
    xTaskCreate(pool_monitor_task, "PoolMon", 4096, NULL, 5, NULL);
    xTaskCreate(persistence_demo_task, "PersistD", 4096, NULL, 6, NULL);

    ESP_LOGI(TAG, "System up.");
}