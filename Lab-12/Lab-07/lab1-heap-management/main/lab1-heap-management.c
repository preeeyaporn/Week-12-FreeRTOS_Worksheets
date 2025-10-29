// lab1-heap-management.c — Unified Heap Lab + Secure Memory + Dynamic Pools (ESP32 / ESP-IDF v5.x)
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_random.h" // ✅ IDF 5+ ต้อง include แยก
#include "driver/gpio.h"

// (ถ้ามีในโปรเจ็กต์) เดโม shared memory
#include "shared_memory.h"

// mbedTLS AES (มากับ ESP-IDF อยู่แล้ว)
#include "mbedtls/aes.h"

/* =========================
 *      CONFIG / DEFINES
 * ========================= */

static const char *TAG = "HEAP_MGMT";

// GPIO แสดงสถานะ (ปรับตามบอร์ด)
#define LED_MEMORY_OK GPIO_NUM_2
#define LED_LOW_MEMORY GPIO_NUM_4
#define LED_MEMORY_ERROR GPIO_NUM_5
#define LED_FRAGMENTATION GPIO_NUM_18
#define LED_SPIRAM_ACTIVE GPIO_NUM_19

// Thresholds
#define LOW_MEMORY_THRESHOLD 50000      // 50KB
#define CRITICAL_MEMORY_THRESHOLD 20000 // 20KB
#define FRAGMENTATION_THRESHOLD 0.30f
#define MAX_ALLOCATIONS 100

#ifndef ALIGN_UP
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#endif

/* =========================
 *        STRUCTS
 * ========================= */
typedef struct
{
    void *ptr;
    size_t size;
    uint32_t caps;
    const char *description;
    uint64_t timestamp;
    bool is_active;
} memory_allocation_t;

typedef struct
{
    uint32_t total_allocations;
    uint32_t total_deallocations;
    uint32_t current_allocations;
    uint64_t total_bytes_allocated;
    uint64_t total_bytes_deallocated;
    uint64_t peak_usage;
    uint32_t allocation_failures;
    uint32_t fragmentation_events;
    uint32_t low_memory_events;
} memory_stats_t;

/* =========================
 *        GLOBALS
 * ========================= */
static memory_allocation_t allocations[MAX_ALLOCATIONS];
static memory_stats_t stats = {0};
static SemaphoreHandle_t memory_mutex = NULL;
static bool memory_monitoring_enabled = true;

/* =========================
 *  FORWARD DECLARATIONS
 * ========================= */
// tracking helpers
static int find_free_allocation_slot(void);
static int find_allocation_by_ptr(void *ptr);
static void *tracked_malloc(size_t size, uint32_t caps, const char *description);
static void tracked_free(void *ptr, const char *description);

// manual register helpers (สำหรับ memory pool หรือ arena)
static void register_allocation_manual(void *ptr, size_t size, uint32_t caps, const char *description);
static void unregister_allocation_manual(void *ptr, const char *description);

// memory analytics
static void analyze_memory_status(void);
static void print_allocation_summary(void);
static void detect_memory_leaks(void);

// RLE helpers
static size_t rle_compress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_capacity);
static size_t rle_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_capacity);

// secure memory helpers
static void secure_wipe(void *p, size_t n);

/* =========================
 *   TRACKING UTILITIES
 * ========================= */
static int find_free_allocation_slot(void)
{
    for (int i = 0; i < MAX_ALLOCATIONS; i++)
    {
        if (!allocations[i].is_active)
            return i;
    }
    return -1;
}

static int find_allocation_by_ptr(void *ptr)
{
    for (int i = 0; i < MAX_ALLOCATIONS; i++)
    {
        if (allocations[i].is_active && allocations[i].ptr == ptr)
            return i;
    }
    return -1;
}

static void *tracked_malloc(size_t size, uint32_t caps, const char *description)
{
    void *ptr = heap_caps_malloc(size, caps);

    if (memory_monitoring_enabled && memory_mutex)
    {
        if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            if (ptr)
            {
                int slot = find_free_allocation_slot();
                if (slot >= 0)
                {
                    allocations[slot].ptr = ptr;
                    allocations[slot].size = size;
                    allocations[slot].caps = caps;
                    allocations[slot].description = description;
                    allocations[slot].timestamp = esp_timer_get_time();
                    allocations[slot].is_active = true;

                    stats.total_allocations++;
                    stats.current_allocations++;
                    stats.total_bytes_allocated += size;

                    size_t current_usage = stats.total_bytes_allocated - stats.total_bytes_deallocated;
                    if (current_usage > stats.peak_usage)
                        stats.peak_usage = current_usage;

                    ESP_LOGI(TAG, "✅ Allocated %u bytes at %p (%s)",
                             (unsigned)size, ptr, description ? description : "-");
                }
                else
                {
                    ESP_LOGW(TAG, "⚠️ Allocation tracking full!");
                }
            }
            else
            {
                stats.allocation_failures++;
                ESP_LOGE(TAG, "❌ Failed to allocate %u bytes (%s)",
                         (unsigned)size, description ? description : "-");
            }
            xSemaphoreGive(memory_mutex);
        }
    }
    return ptr;
}

static void tracked_free(void *ptr, const char *description)
{
    if (!ptr)
        return;

    if (memory_monitoring_enabled && memory_mutex)
    {
        if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            int slot = find_allocation_by_ptr(ptr);
            if (slot >= 0)
            {
                allocations[slot].is_active = false;
                stats.total_deallocations++;
                stats.current_allocations--;
                stats.total_bytes_deallocated += allocations[slot].size;

                ESP_LOGI(TAG, "🗑️ Freed %u bytes at %p (%s)",
                         (unsigned)allocations[slot].size, ptr, description ? description : "-");
            }
            else
            {
                ESP_LOGW(TAG, "⚠️ Freeing untracked pointer %p (%s)", ptr, description ? description : "-");
            }
            xSemaphoreGive(memory_mutex);
        }
    }
    heap_caps_free(ptr);
}

/* =========================
 *  MANUAL REGISTER HELPERS
 * ========================= */
static void register_allocation_manual(void *ptr, size_t size, uint32_t caps, const char *description)
{
    if (!ptr || !memory_mutex)
        return;
    if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        int slot = find_free_allocation_slot();
        if (slot >= 0)
        {
            allocations[slot].ptr = ptr;
            allocations[slot].size = size;
            allocations[slot].caps = caps;
            allocations[slot].description = description;
            allocations[slot].timestamp = esp_timer_get_time();
            allocations[slot].is_active = true;

            stats.total_allocations++;
            stats.current_allocations++;
            stats.total_bytes_allocated += size;

            size_t current_usage = stats.total_bytes_allocated - stats.total_bytes_deallocated;
            if (current_usage > stats.peak_usage)
                stats.peak_usage = current_usage;

            ESP_LOGI(TAG, "✅ Registered manual %u bytes at %p (%s)",
                     (unsigned)size, ptr, description ? description : "-");
        }
        else
        {
            ESP_LOGW(TAG, "⚠️ Manual tracking full for %p (%s)", ptr, description ? description : "-");
        }
        xSemaphoreGive(memory_mutex);
    }
}

static void unregister_allocation_manual(void *ptr, const char *description)
{
    if (!ptr || !memory_mutex)
        return;
    if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        int slot = find_allocation_by_ptr(ptr);
        if (slot >= 0)
        {
            allocations[slot].is_active = false;
            stats.total_deallocations++;
            stats.current_allocations--;
            stats.total_bytes_deallocated += allocations[slot].size;

            ESP_LOGI(TAG, "🗑️ Unregistered manual %u bytes at %p (%s)",
                     (unsigned)allocations[slot].size, ptr, description ? description : "-");
        }
        else
        {
            ESP_LOGW(TAG, "⚠️ Unregister: pointer not found %p (%s)", ptr, description ? description : "-");
        }
        xSemaphoreGive(memory_mutex);
    }
}

/* =========================
 *   MEMORY ANALYTICS
 * ========================= */
static void analyze_memory_status(void)
{
    size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    size_t spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t total_free = esp_get_free_heap_size();

    float internal_fragmentation = 0.0f;
    if (internal_free > 0)
        internal_fragmentation = 1.0f - ((float)internal_largest / (float)internal_free);

    ESP_LOGI(TAG, "\n📊 ═══ MEMORY STATUS ═══");
    ESP_LOGI(TAG, "Internal RAM Free:    %u", (unsigned)internal_free);
    ESP_LOGI(TAG, "Largest Free Block:   %u", (unsigned)internal_largest);
    ESP_LOGI(TAG, "SPIRAM Free:          %u", (unsigned)spiram_free);
    ESP_LOGI(TAG, "Total Free:           %u", (unsigned)total_free);
    ESP_LOGI(TAG, "Minimum Ever Free:    %u", (unsigned)esp_get_minimum_free_heap_size());
    ESP_LOGI(TAG, "Internal Fragmentation: %.1f%%", internal_fragmentation * 100.0f);

    if (internal_free < CRITICAL_MEMORY_THRESHOLD)
    {
        gpio_set_level(LED_MEMORY_ERROR, 1);
        gpio_set_level(LED_LOW_MEMORY, 1);
        gpio_set_level(LED_MEMORY_OK, 0);
        stats.low_memory_events++;
        ESP_LOGW(TAG, "🚨 CRITICAL: Very low memory!");
    }
    else if (internal_free < LOW_MEMORY_THRESHOLD)
    {
        gpio_set_level(LED_LOW_MEMORY, 1);
        gpio_set_level(LED_MEMORY_ERROR, 0);
        gpio_set_level(LED_MEMORY_OK, 0);
        stats.low_memory_events++;
        ESP_LOGW(TAG, "⚠️ WARNING: Low memory");
    }
    else
    {
        gpio_set_level(LED_MEMORY_OK, 1);
        gpio_set_level(LED_LOW_MEMORY, 0);
        gpio_set_level(LED_MEMORY_ERROR, 0);
    }

    if (internal_fragmentation > FRAGMENTATION_THRESHOLD)
    {
        gpio_set_level(LED_FRAGMENTATION, 1);
        stats.fragmentation_events++;
        ESP_LOGW(TAG, "⚠️ High fragmentation detected!");
    }
    else
    {
        gpio_set_level(LED_FRAGMENTATION, 0);
    }

    gpio_set_level(LED_SPIRAM_ACTIVE, spiram_free > 0 ? 1 : 0);
    ESP_LOGI(TAG, "═══════════════════════════════");
}

static void print_allocation_summary(void)
{
    if (!memory_mutex)
        return;
    if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        ESP_LOGI(TAG, "\n📈 ═══ ALLOCATION STATISTICS ═══");
        ESP_LOGI(TAG, "Total Allocations:    %lu", (unsigned long)stats.total_allocations);
        ESP_LOGI(TAG, "Total Deallocations:  %lu", (unsigned long)stats.total_deallocations);
        ESP_LOGI(TAG, "Current Allocations:  %lu", (unsigned long)stats.current_allocations);
        ESP_LOGI(TAG, "Total Allocated:      %llu", (unsigned long long)stats.total_bytes_allocated);
        ESP_LOGI(TAG, "Total Deallocated:    %llu", (unsigned long long)stats.total_bytes_deallocated);
        ESP_LOGI(TAG, "Peak Usage:           %llu", (unsigned long long)stats.peak_usage);
        ESP_LOGI(TAG, "Allocation Failures:  %lu", (unsigned long)stats.allocation_failures);
        ESP_LOGI(TAG, "Fragmentation Events: %lu", (unsigned long)stats.fragmentation_events);
        ESP_LOGI(TAG, "Low Memory Events:    %lu", (unsigned long)stats.low_memory_events);

        if (stats.current_allocations > 0)
        {
            ESP_LOGI(TAG, "\n🔍 ═══ ACTIVE ALLOCATIONS ═══");
            for (int i = 0; i < MAX_ALLOCATIONS; i++)
            {
                if (allocations[i].is_active)
                {
                    uint64_t age_ms = (esp_timer_get_time() - allocations[i].timestamp) / 1000ULL;
                    ESP_LOGI(TAG, "Slot %d: %u bytes @%p (%s) age=%llu ms",
                             i, (unsigned)allocations[i].size, allocations[i].ptr,
                             allocations[i].description ? allocations[i].description : "-",
                             (unsigned long long)age_ms);
                }
            }
        }
        xSemaphoreGive(memory_mutex);
    }
}

static void detect_memory_leaks(void)
{
    if (!memory_mutex)
        return;
    if (xSemaphoreTake(memory_mutex, pdMS_TO_TICKS(1000)) == pdTRUE)
    {
        uint64_t now = esp_timer_get_time();
        int leak_count = 0;
        size_t leaked_bytes = 0;
        for (int i = 0; i < MAX_ALLOCATIONS; i++)
        {
            if (allocations[i].is_active)
            {
                uint64_t age_ms = (now - allocations[i].timestamp) / 1000ULL;
                if (age_ms > 30000ULL)
                {
                    ESP_LOGW(TAG, "POTENTIAL LEAK: %u @%p (%s) age=%llu ms",
                             (unsigned)allocations[i].size, allocations[i].ptr,
                             allocations[i].description ? allocations[i].description : "-",
                             (unsigned long long)age_ms);
                    leak_count++;
                    leaked_bytes += allocations[i].size;
                }
            }
        }
        if (leak_count > 0)
        {
            ESP_LOGW(TAG, "Found %d potential leaks totaling %u bytes",
                     leak_count, (unsigned)leaked_bytes);
            gpio_set_level(LED_MEMORY_ERROR, 1);
        }
        else
        {
            gpio_set_level(LED_MEMORY_ERROR, 0);
            ESP_LOGI(TAG, "No memory leaks detected");
        }
        xSemaphoreGive(memory_mutex);
    }
}

/* =========================
 *       RLE HELPERS
 * ========================= */
static size_t rle_compress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_capacity)
{
    size_t in_pos = 0, out_pos = 0;
    while (in_pos < in_len)
    {
        size_t run_len = 1;
        while (in_pos + run_len < in_len && run_len < 127 && in[in_pos + run_len] == in[in_pos])
            run_len++;
        if (run_len >= 3)
        {
            uint8_t header = (uint8_t)(257 - run_len); // 129..255
            if (out_pos + 2 > out_capacity)
                break;
            out[out_pos++] = header;
            out[out_pos++] = in[in_pos];
            in_pos += run_len;
        }
        else
        {
            size_t lit_start = in_pos, lit_len = 0;
            while (in_pos < in_len && lit_len < 128)
            {
                if (in_pos + 2 < in_len && in[in_pos] == in[in_pos + 1] && in[in_pos] == in[in_pos + 2])
                    break;
                in_pos++;
                lit_len++;
            }
            if (lit_len > 0)
            {
                if (out_pos + 1 + lit_len > out_capacity)
                    break;
                out[out_pos++] = (uint8_t)(lit_len - 1);
                memcpy(out + out_pos, in + lit_start, lit_len);
                out_pos += lit_len;
            }
        }
    }
    return out_pos;
}

static size_t rle_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_capacity)
{
    size_t in_pos = 0, out_pos = 0;
    while (in_pos < in_len && out_pos < out_capacity)
    {
        uint8_t header = in[in_pos++];
        if (header <= 127)
        {
            size_t lit_len = (size_t)header + 1;
            if (in_pos + lit_len > in_len || out_pos + lit_len > out_capacity)
                break;
            memcpy(out + out_pos, in + in_pos, lit_len);
            in_pos += lit_len;
            out_pos += lit_len;
        }
        else if (header >= 129)
        {
            size_t rep_len = 257 - (size_t)header;
            if (in_pos >= in_len || out_pos + rep_len > out_capacity)
                break;
            uint8_t value = in[in_pos++];
            memset(out + out_pos, value, rep_len);
            out_pos += rep_len;
        }
        else
        {
            // 128 = NOP
        }
    }
    return out_pos;
}

/* =========================
 *   SECURE MEMORY (AES-CTR)
 * ========================= */
typedef struct secure_blob_t
{
    uint8_t *cipher;
    size_t len;
    uint8_t key[16];
    uint8_t iv[16];
    bool in_use;
    const char *desc;
} secure_blob_t;

static void secure_wipe(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (n--)
        *v++ = 0;
}

static void fill_random(uint8_t *dst, size_t n)
{
    while (n >= 4)
    {
        uint32_t r = esp_random();
        memcpy(dst, &r, 4);
        dst += 4;
        n -= 4;
    }
    if (n)
    {
        uint32_t r = esp_random();
        memcpy(dst, &r, n);
    }
}

static int aes_ctr_crypt(uint8_t *in, size_t len, uint8_t *out, const uint8_t key[16], const uint8_t iv[16])
{
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);

    unsigned char stream_block[16] = {0};
    unsigned char nonce_counter[16];
    size_t nc_off = 0;
    memcpy(nonce_counter, iv, 16);

    int rc = mbedtls_aes_setkey_enc(&ctx, key, 128);
    if (rc != 0)
    {
        mbedtls_aes_free(&ctx);
        return rc;
    }

    rc = mbedtls_aes_crypt_ctr(&ctx, len, &nc_off, nonce_counter, stream_block, in, out);
    mbedtls_aes_free(&ctx);
    secure_wipe(stream_block, sizeof(stream_block));
    secure_wipe(nonce_counter, sizeof(nonce_counter));
    return rc;
}

static int secure_store(secure_blob_t *sb, const void *plaintext, size_t len, uint32_t caps_cipher, const char *desc)
{
    if (!sb || !plaintext || !len)
        return -1;
    memset(sb, 0, sizeof(*sb));
    fill_random(sb->key, sizeof(sb->key));
    fill_random(sb->iv, sizeof(sb->iv));
    sb->len = len;
    sb->desc = desc;

    if (caps_cipher == 0)
        caps_cipher = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    sb->cipher = (uint8_t *)tracked_malloc(len, caps_cipher, desc ? desc : "SecureCipher");
    if (!sb->cipher)
    {
        sb->cipher = (uint8_t *)tracked_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, desc ? desc : "SecureCipherInt");
        if (!sb->cipher)
            return -2;
    }

    int rc = aes_ctr_crypt((uint8_t *)plaintext, len, sb->cipher, sb->key, sb->iv);
    if (rc != 0)
    {
        tracked_free(sb->cipher, desc ? desc : "SecureCipher");
        memset(sb, 0, sizeof(*sb));
        return -3;
    }
    sb->in_use = true;
    ESP_LOGI(TAG, "🔒 secure_store: len=%u desc=%s", (unsigned)len, desc ? desc : "-");
    return 0;
}

static int secure_load(const secure_blob_t *sb, void *out_plain, size_t out_capacity)
{
    if (!sb || !sb->in_use || !sb->cipher || !sb->len)
        return -1;
    if (!out_plain || out_capacity < sb->len)
        return -2;
    int rc = aes_ctr_crypt(sb->cipher, sb->len, (uint8_t *)out_plain, sb->key, sb->iv);
    if (rc != 0)
        return -3;
    return (int)sb->len;
}

static void secure_free(secure_blob_t *sb)
{
    if (!sb)
        return;
    if (sb->cipher)
    {
        secure_wipe(sb->cipher, sb->len);
        tracked_free(sb->cipher, sb->desc ? sb->desc : "SecureCipher");
    }
    secure_wipe(sb->key, sizeof(sb->key));
    secure_wipe(sb->iv, sizeof(sb->iv));
    memset(sb, 0, sizeof(*sb));
}

/* =========================
 *   DYNAMIC MEMORY POOLS
 *   (1) Grow/Shrink Slab Pools
 *   (2) Bump-Pointer Arena
 * ========================= */
// ---- Slab Pools ----
typedef struct dpool_page_t
{
    struct dpool_page_t *next;
    uint8_t *base;
    uint16_t block_size;
    uint16_t blocks;
    uint16_t free_count;
    void *free_list;
} dpool_page_t;

typedef struct dpool_block_hdr_t
{
    dpool_page_t *owner;
    uint16_t sizeclass;
} dpool_block_hdr_t;

typedef struct dpool_class_t
{
    uint16_t size;
    uint16_t blocks_per_page;
    dpool_page_t *pages;
} dpool_class_t;

typedef struct dynamic_pool_mgr_t
{
    uint32_t caps;
    SemaphoreHandle_t lock;
#define DPOOL_NCLASS 6
    dpool_class_t cls[DPOOL_NCLASS];
} dynamic_pool_mgr_t;

static inline uint16_t _choose_blocks_per_page(uint16_t blk)
{
    if (blk <= 64)
        return 100;
    else if (blk <= 128)
        return 64;
    else if (blk <= 256)
        return 32;
    else if (blk <= 512)
        return 16;
    else if (blk <= 1024)
        return 8;
    else
        return 4;
}

static int dpool_init(dynamic_pool_mgr_t *pm, uint32_t caps)
{
    if (!pm)
        return -1;
    memset(pm, 0, sizeof(*pm));
    pm->caps = caps ? caps : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    pm->lock = xSemaphoreCreateMutex();
    if (!pm->lock)
        return -2;
    const uint16_t sizes[DPOOL_NCLASS] = {64, 128, 256, 512, 1024, 2048};
    for (int i = 0; i < DPOOL_NCLASS; i++)
    {
        pm->cls[i].size = sizes[i];
        pm->cls[i].blocks_per_page = _choose_blocks_per_page(sizes[i]);
        pm->cls[i].pages = NULL;
    }
    return 0;
}

static int _class_index_for_size(size_t size)
{
    if (size <= 64)
        return 0;
    if (size <= 128)
        return 1;
    if (size <= 256)
        return 2;
    if (size <= 512)
        return 3;
    if (size <= 1024)
        return 4;
    if (size <= 2048)
        return 5;
    return -1;
}

static dpool_page_t *_alloc_new_page(dynamic_pool_mgr_t *pm, int ci)
{
    dpool_class_t *c = &pm->cls[ci];
    const uint16_t bsz = c->size;
    const uint16_t blocks = c->blocks_per_page;

    const size_t hdr_sz = sizeof(dpool_block_hdr_t);
    const size_t one = ALIGN_UP(hdr_sz + bsz, 8);
    const size_t total = sizeof(dpool_page_t) + (one * blocks);

    uint8_t *mem = (uint8_t *)tracked_malloc(total, pm->caps, "DPOOL_PAGE");
    if (!mem)
        return NULL;

    dpool_page_t *pg = (dpool_page_t *)mem;
    memset(pg, 0, sizeof(*pg));
    pg->base = mem + sizeof(dpool_page_t);
    pg->block_size = bsz;
    pg->blocks = blocks;
    pg->free_count = blocks;
    pg->free_list = NULL;

    for (int i = 0; i < blocks; i++)
    {
        uint8_t *blk_base = pg->base + ((size_t)i * one);
        dpool_block_hdr_t *bh = (dpool_block_hdr_t *)blk_base;
        bh->owner = pg;
        bh->sizeclass = (uint16_t)ci;
        void *payload = (void *)(blk_base + hdr_sz);
        *((void **)payload) = pg->free_list;
        pg->free_list = payload;
    }

    pg->next = c->pages;
    c->pages = pg;

    ESP_LOGI(TAG, "🧩 DPOOL new page: class=%d size=%u blocks=%u total=%u@%p",
             ci, (unsigned)bsz, (unsigned)blocks, (unsigned)total, mem);
    return pg;
}

static void *_dpool_alloc_locked(dynamic_pool_mgr_t *pm, size_t size, const char *desc)
{
    int ci = _class_index_for_size(size);
    if (ci < 0)
        return NULL;
    dpool_class_t *c = &pm->cls[ci];

    dpool_page_t *pg = c->pages;
    while (pg && !pg->free_list)
        pg = pg->next;

    if (!pg)
    {
        pg = _alloc_new_page(pm, ci);
        if (!pg)
            return NULL;
    }

    void *payload = pg->free_list;
    pg->free_list = *((void **)payload);
    if (pg->free_count)
        pg->free_count--;

    register_allocation_manual(payload, c->size, pm->caps, desc ? desc : "DPOOL");
    return payload;
}

static void dpool_release_empty_pages_locked(dynamic_pool_mgr_t *pm, int ci)
{
    dpool_class_t *c = &pm->cls[ci];
    dpool_page_t **pp = &c->pages;
    while (*pp)
    {
        dpool_page_t *pg = *pp;
        if (pg->free_count == pg->blocks)
        {
            *pp = pg->next;
            tracked_free(pg, "DPOOL_PAGE");
        }
        else
        {
            pp = &pg->next;
        }
    }
}

static void dpool_defrag_hint(dynamic_pool_mgr_t *pm)
{
    if (!pm || !pm->lock)
        return;
    if (xSemaphoreTake(pm->lock, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        for (int i = 0; i < DPOOL_NCLASS; i++)
            dpool_release_empty_pages_locked(pm, i);
        xSemaphoreGive(pm->lock);
    }
}

static void *dpool_alloc(dynamic_pool_mgr_t *pm, size_t size, const char *desc)
{
    if (!pm || !pm->lock || !size)
        return NULL;
    void *p = NULL;
    if (xSemaphoreTake(pm->lock, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        p = _dpool_alloc_locked(pm, size, desc);
        xSemaphoreGive(pm->lock);
    }
    return p;
}

static void dpool_free(dynamic_pool_mgr_t *pm, void *ptr, const char *desc)
{
    if (!pm || !pm->lock || !ptr)
        return;
    unregister_allocation_manual(ptr, desc ? desc : "DPOOL"); // สถิติ

    if (xSemaphoreTake(pm->lock, pdMS_TO_TICKS(100)) == pdTRUE)
    {
        dpool_block_hdr_t *bh = (dpool_block_hdr_t *)((uint8_t *)ptr - sizeof(dpool_block_hdr_t));
        dpool_page_t *pg = bh->owner;
        *((void **)ptr) = pg->free_list;
        pg->free_list = ptr;
        if (pg->free_count < 0xFFFF)
            pg->free_count++;
        xSemaphoreGive(pm->lock);
    }
}

static void dpool_dump(dynamic_pool_mgr_t *pm)
{
    if (!pm || !pm->lock)
        return;
    if (xSemaphoreTake(pm->lock, pdMS_TO_TICKS(200)) == pdTRUE)
    {
        ESP_LOGI(TAG, "🔎 DPOOL DUMP:");
        for (int i = 0; i < DPOOL_NCLASS; i++)
        {
            dpool_class_t *c = &pm->cls[i];
            int pages = 0, free_blocks = 0, total_blocks = 0;
            for (dpool_page_t *pg = c->pages; pg; pg = pg->next)
            {
                pages++;
                free_blocks += pg->free_count;
                total_blocks += pg->blocks;
            }
            ESP_LOGI(TAG, "  class[%d]: size=%u pages=%d free=%d/%d",
                     i, (unsigned)c->size, pages, free_blocks, total_blocks);
        }
        xSemaphoreGive(pm->lock);
    }
}

// ---- Bump Arena ----
typedef struct bump_arena_t
{
    uint8_t *buf;
    size_t cap;
    size_t off;
    uint32_t caps;
    SemaphoreHandle_t lock;
} bump_arena_t;

static int arena_init(bump_arena_t *ar, size_t capacity, uint32_t caps)
{
    if (!ar || !capacity)
        return -1;
    memset(ar, 0, sizeof(*ar));
    ar->caps = caps ? caps : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ar->buf = (uint8_t *)tracked_malloc(capacity, ar->caps, "ARENA_BUF");
    if (!ar->buf)
        return -2;
    ar->cap = capacity;
    ar->lock = xSemaphoreCreateMutex();
    return ar->lock ? 0 : -3;
}

static void *arena_alloc(bump_arena_t *ar, size_t size, size_t align, const char *desc)
{
    if (!ar || !ar->lock || !size)
        return NULL;
    void *p = NULL;
    if (xSemaphoreTake(ar->lock, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        size_t a = (align == 0) ? 8 : align;
        size_t off_aligned = ALIGN_UP(ar->off, a);
        if (off_aligned + size <= ar->cap)
        {
            p = ar->buf + off_aligned;
            ar->off = off_aligned + size;
            register_allocation_manual(p, size, ar->caps, desc ? desc : "ARENA");
        }
        xSemaphoreGive(ar->lock);
    }
    return p;
}

static void arena_reset(bump_arena_t *ar)
{
    if (!ar || !ar->lock)
        return;
    if (xSemaphoreTake(ar->lock, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        // บันทึกสถิติแบบง่าย: “free” ก้อนใหญ่
        ar->off = 0;
        xSemaphoreGive(ar->lock);
    }
}

static void arena_destroy(bump_arena_t *ar)
{
    if (!ar)
        return;
    if (ar->buf)
    {
        secure_wipe(ar->buf, ar->cap);
        tracked_free(ar->buf, "ARENA_BUF");
    }
    if (ar->lock)
        vSemaphoreDelete(ar->lock);
    memset(ar, 0, sizeof(*ar));
}

/* =========================
 *            TASKS
 * ========================= */
static void memory_stress_test_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🧪 Memory stress test started");
    void *test_ptrs[20] = {0};
    int count = 0;

    while (1)
    {
        int action = (int)(esp_random() % 3);
        if (action == 0 && count < 20)
        {
            size_t size = 100 + (esp_random() % 2000);
            uint32_t caps = (esp_random() % 2) ? MALLOC_CAP_INTERNAL : MALLOC_CAP_DEFAULT;
            test_ptrs[count] = tracked_malloc(size, caps, "StressTest");
            if (test_ptrs[count])
            {
                memset(test_ptrs[count], 0xAA, size);
                count++;
            }
        }
        else if (action == 1 && count > 0)
        {
            int idx = (int)(esp_random() % count);
            if (test_ptrs[idx])
            {
                tracked_free(test_ptrs[idx], "StressTest");
                for (int i = idx; i < count - 1; i++)
                    test_ptrs[i] = test_ptrs[i + 1];
                count--;
            }
        }
        else
        {
            analyze_memory_status();
        }
        vTaskDelay(pdMS_TO_TICKS(1000 + (esp_random() % 2000)));
    }
}

static void memory_pool_test_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🏊 Memory pool mini test started");
    const size_t sizes[] = {64, 128, 256, 512, 1024};
    void *p[5][6] = {{0}};
    while (1)
    {
        for (int s = 0; s < 5; s++)
            for (int i = 0; i < 6; i++)
            {
                char d[24];
                snprintf(d, sizeof(d), "Pool%u_%u", s, i);
                p[s][i] = tracked_malloc(sizes[s], MALLOC_CAP_INTERNAL, d);
                if (p[s][i])
                    memset(p[s][i], 0x55 + s, sizes[s]);
            }
        vTaskDelay(pdMS_TO_TICKS(3000));
        for (int s = 4; s >= 0; s--)
            for (int i = 5; i >= 0; i--)
            {
                if (p[s][i])
                {
                    tracked_free(p[s][i], "Pool");
                    p[s][i] = NULL;
                }
            }
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

static void large_allocation_test_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🐘 Large allocation test started");
    while (1)
    {
        size_t n = 50000 + (esp_random() % 100000);
        void *buf = tracked_malloc(n, MALLOC_CAP_INTERNAL, "LargeInternal");
        if (!buf)
            buf = tracked_malloc(n, MALLOC_CAP_SPIRAM, "LargeSPIRAM");
        if (buf)
        {
            // Fill then compress/decompress
            for (size_t i = 0; i < n;)
            {
                size_t run = 1 + (esp_random() % 256);
                uint8_t v = (uint8_t)(esp_random() & 0xFF);
                if (i + run > n)
                    run = n - i;
                memset((uint8_t *)buf + i, v, run);
                i += run;
                size_t mix = (esp_random() % 64);
                if (i + mix > n)
                    mix = n - i;
                for (size_t j = 0; j < mix; j++)
                    ((uint8_t *)buf)[i + j] = (uint8_t)(esp_random() & 0xFF);
                i += mix;
            }
            void *c = tracked_malloc(n + 64, MALLOC_CAP_INTERNAL, "RLE_C");
            void *d = tracked_malloc(n, MALLOC_CAP_INTERNAL, "RLE_D");
            if (c && d)
            {
                uint64_t cs = esp_timer_get_time();
                size_t clen = rle_compress(buf, n, c, n + 64);
                uint64_t ce = esp_timer_get_time();
                uint64_t ds = esp_timer_get_time();
                size_t dlen = rle_decompress(c, clen, d, n);
                uint64_t de = esp_timer_get_time();
                bool ok = (dlen == n) && (memcmp(buf, d, n) == 0);
                ESP_LOGI(TAG, "🔁 RLE: orig=%u comp=%u (%.2f%%) enc_us=%llu dec_us=%llu ok=%d",
                         (unsigned)n, (unsigned)clen, clen ? 100.f * (float)clen / (float)n : 0.f,
                         (unsigned long long)(ce - cs), (unsigned long long)(de - ds), ok);
            }
            if (c)
                tracked_free(c, "RLE_C");
            if (d)
                tracked_free(d, "RLE_D");
            tracked_free(buf, "Large");
        }
        else
        {
            ESP_LOGW(TAG, "Large alloc failed");
            analyze_memory_status();
        }
        vTaskDelay(pdMS_TO_TICKS(12000));
    }
}

static void memory_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "📊 Memory monitor started");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(10000));
        analyze_memory_status();
        print_allocation_summary();
        detect_memory_leaks();
        if (!heap_caps_check_integrity_all(true))
        {
            ESP_LOGE(TAG, "🚨 HEAP CORRUPTION DETECTED");
            gpio_set_level(LED_MEMORY_ERROR, 1);
        }
        ESP_LOGI(TAG, "Free heap: %u", (unsigned)esp_get_free_heap_size());
        ESP_LOGI(TAG, "Uptime: %llu ms\n", (unsigned long long)(esp_timer_get_time() / 1000ULL));
    }
}

static void heap_integrity_test_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🔍 Heap integrity/perf started");
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(30000));
        bool ok = heap_caps_check_integrity_all(false);
        ESP_LOGI(TAG, "Heap integrity: %s", ok ? "OK" : "FAIL");
        const size_t N = 4096;
        void *b = tracked_malloc(N, MALLOC_CAP_INTERNAL, "Perf");
        if (b)
        {
            uint64_t s = esp_timer_get_time();
            for (int i = 0; i < 100; i++)
                memset(b, i, N);
            uint64_t wt = esp_timer_get_time() - s;
            volatile uint8_t sum = 0;
            s = esp_timer_get_time();
            for (int i = 0; i < 100; i++)
            {
                uint8_t *p = b;
                for (size_t j = 0; j < N; j++)
                    sum += p[j];
            }
            uint64_t rt = esp_timer_get_time() - s;
            ESP_LOGI(TAG, "Perf: W %llu us, R %llu us (sum=%u)", (unsigned long long)wt, (unsigned long long)rt, sum);
            tracked_free(b, "Perf");
        }
    }
}

static void sensitive_data_demo_task(void *pvParameters)
{
    ESP_LOGI(TAG, "🔐 Sensitive data demo started");
    const char *secret = "User=Petch; Token=ABCD-1234-SECRET; Role=admin";
    size_t slen = strlen(secret);
    secure_blob_t sb;
    int rc = secure_store(&sb, secret, slen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, "SecureBlob");
    if (rc != 0)
    {
        ESP_LOGE(TAG, "secure_store rc=%d", rc);
        vTaskDelete(NULL);
        return;
    }
    uint8_t *tmp = (uint8_t *)tracked_malloc(slen + 1, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, "Tmp");
    if (!tmp)
    {
        secure_free(&sb);
        vTaskDelete(NULL);
        return;
    }
    int out = secure_load(&sb, tmp, slen + 1);
    if (out < 0)
    {
        ESP_LOGE(TAG, "secure_load rc=%d", out);
        tracked_free(tmp, "Tmp");
        secure_free(&sb);
        vTaskDelete(NULL);
        return;
    }
    tmp[slen] = '\0';
    ESP_LOGI(TAG, "🔓 Decrypted(use briefly): %s", (char *)tmp);
    secure_wipe(tmp, slen);
    tracked_free(tmp, "Tmp");
    secure_free(&sb);
    ESP_LOGI(TAG, "🔐 Sensitive data demo finished");
    vTaskDelete(NULL);
}

static void dynamic_pools_demo_task(void *arg)
{
    ESP_LOGI(TAG, "🧪 Dynamic Pools demo started");
    dynamic_pool_mgr_t dpm;
    uint32_t caps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                                                               : (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (dpool_init(&dpm, caps) != 0)
    {
        ESP_LOGE(TAG, "dpool_init failed");
        vTaskDelete(NULL);
        return;
    }

    bump_arena_t arena;
    if (arena_init(&arena, 64 * 1024, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != 0)
    {
        ESP_LOGE(TAG, "arena_init failed");
        vTaskDelete(NULL);
        return;
    }

    void *h[30] = {0};

    while (1)
    {
        // Slab allocations
        for (int i = 0; i < 30; i++)
        {
            size_t want = 1 + (esp_random() % 1800);
            h[i] = dpool_alloc(&dpm, want, "DPOOL_DEMO");
            if (h[i])
                memset(h[i], 0xCD, want < 64 ? want : 64);
        }
        // Arena batch then reset
        for (int k = 0; k < 10; k++)
        {
            size_t ask = 1024 + (esp_random() % 2048);
            void *p = arena_alloc(&arena, ask, 16, "ARENA_DEMO");
            if (p)
                memset(p, 0xA5, ask < 128 ? ask : 128);
        }
        arena_reset(&arena);

        dpool_dump(&dpm);
        analyze_memory_status();

        for (int i = 0; i < 30; i += 2)
            if (h[i])
            {
                dpool_free(&dpm, h[i], "DPOOL_DEMO");
                h[i] = NULL;
            }
        dpool_defrag_hint(&dpm);

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* =========================
 *         app_main
 * ========================= */
void app_main(void)
{
    ESP_LOGI(TAG, "🚀 Heap Management Lab Starting...");

    // GPIO init
    gpio_set_direction(LED_MEMORY_OK, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_LOW_MEMORY, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_MEMORY_ERROR, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_FRAGMENTATION, GPIO_MODE_OUTPUT);
    gpio_set_direction(LED_SPIRAM_ACTIVE, GPIO_MODE_OUTPUT);

    gpio_set_level(LED_MEMORY_OK, 0);
    gpio_set_level(LED_LOW_MEMORY, 0);
    gpio_set_level(LED_MEMORY_ERROR, 0);
    gpio_set_level(LED_FRAGMENTATION, 0);
    gpio_set_level(LED_SPIRAM_ACTIVE, 0);

    // Mutex + init
    memory_mutex = xSemaphoreCreateMutex();
    if (!memory_mutex)
    {
        ESP_LOGE(TAG, "Failed to create memory mutex!");
        return;
    }
    memset(allocations, 0, sizeof(allocations));
    ESP_LOGI(TAG, "Memory tracking system initialized");

    // Snapshot
    analyze_memory_status();
    ESP_LOGI(TAG, "\n🏗️ ═══ INITIAL HEAP INFORMATION ═══");
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL);
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0)
    {
        ESP_LOGI(TAG, "\n🏗️ ═══ SPIRAM INFORMATION ═══");
        heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    }

    // Tasks
    xTaskCreate(memory_monitor_task, "MemMon", 4096, NULL, 6, NULL);
    xTaskCreate(memory_stress_test_task, "Stress", 3072, NULL, 5, NULL);
    xTaskCreate(memory_pool_test_task, "PoolMini", 3072, NULL, 5, NULL);
    xTaskCreate(large_allocation_test_task, "Large", 3072, NULL, 4, NULL);
    xTaskCreate(heap_integrity_test_task, "Integrity", 3072, NULL, 3, NULL);
    xTaskCreate(sensitive_data_demo_task, "Secure", 4096, NULL, 7, NULL);
    xTaskCreate(dynamic_pools_demo_task, "DynPools", 4096, NULL, 5, NULL);

// Shared memory demo (ถ้ามีในโปรเจ็กต์)
#ifdef CONFIG_APP_HAVE_SHARED_MEMORY_DEMO
    shm_demo_start();
#else
// เรียกตรง ๆ ได้ถ้ามีซอร์สของคุณ
// shm_demo_start();
#endif

    ESP_LOGI(TAG, "All tasks created successfully");

    ESP_LOGI(TAG, "\n🎯 LED Indicators:");
    ESP_LOGI(TAG, "  GPIO2  - Memory System OK (Green)");
    ESP_LOGI(TAG, "  GPIO4  - Low Memory Warning (Yellow)");
    ESP_LOGI(TAG, "  GPIO5  - Memory Error/Leak (Red)");
    ESP_LOGI(TAG, "  GPIO18 - High Fragmentation (Orange)");
    ESP_LOGI(TAG, "  GPIO19 - SPIRAM Active (Blue)");

    ESP_LOGI(TAG, "\n🔬 Features:");
    ESP_LOGI(TAG, "  • Heap Tracking / Monitor / Leak detection / Fragmentation");
    ESP_LOGI(TAG, "  • RLE compress/decompress demo");
    ESP_LOGI(TAG, "  • 🔒 Secure In-RAM Encryption (AES-CTR)");
    ESP_LOGI(TAG, "  • 🧰 Dynamic Pools: Slab grow/shrink + Bump Arena");
}