#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "shared_memory.h"

static const char *TAG = "SHM";

/* ---------- block layout ----------
   ต่อหนึ่งบล็อก: [ void* hdr ][ payload ... ]
   - ว่าง: hdr = next (ชี้ไปบล็อกถัดไปใน free_list)
   - ถูก publish: hdr = (void*)(uintptr_t)used_len
   ---------------------------------- */

#define BLK_HDR_SIZE (sizeof(void *))
#define PAYLOAD2BASE(p) ((uint8_t *)(p) - BLK_HDR_SIZE)
#define BASE2PAYLOAD(b) ((uint8_t *)(b) + BLK_HDR_SIZE)

/* ===== Zero-copy Block Pool ===== */

bool shm_pool_create(shm_pool_t *pool,
                     size_t block_size,
                     int num_blocks,
                     int queue_len,
                     uint32_t caps)
{
    if (!pool || num_blocks <= 0)
        return false;

    size_t aligned = block_size;
    if (aligned < BLK_HDR_SIZE)
        aligned = BLK_HDR_SIZE;
    aligned = (aligned + 3U) & ~3U; // align 4

    size_t block_bytes = BLK_HDR_SIZE + aligned;
    size_t total_bytes = block_bytes * (size_t)num_blocks;

    void *buf = heap_caps_malloc(total_bytes, caps);
    if (!buf)
        return false;

    memset(pool, 0, sizeof(*pool));
    pool->buffer = buf;
    pool->block_size = aligned; // payload bytes
    pool->num_blocks = num_blocks;
    pool->caps = caps;
    pool->lock = xSemaphoreCreateMutex();
    pool->q = xQueueCreate(queue_len > 0 ? queue_len : SHM_QUEUE_LENGTH, sizeof(void *));
    if (!pool->lock || !pool->q)
    {
        if (pool->lock)
            vSemaphoreDelete(pool->lock);
        if (pool->q)
            vQueueDelete(pool->q);
        heap_caps_free(buf);
        memset(pool, 0, sizeof(*pool));
        return false;
    }

    pool->free_list = NULL;
    for (int i = 0; i < num_blocks; i++)
    {
        uint8_t *blk_base = (uint8_t *)buf + (size_t)i * block_bytes;
        *((void **)blk_base) = pool->free_list; // push LIFO
        pool->free_list = blk_base;
    }

    ESP_LOGI(TAG, "Pool created: blocks=%d, payload=%u, total=%u",
             num_blocks, (unsigned)pool->block_size, (unsigned)total_bytes);
    return true;
}

void shm_pool_destroy(shm_pool_t *pool)
{
    if (!pool || !pool->buffer)
        return;
    if (pool->lock)
        vSemaphoreDelete(pool->lock);
    if (pool->q)
        vQueueDelete(pool->q);
    heap_caps_free(pool->buffer);
    memset(pool, 0, sizeof(*pool));
}

void *shm_pool_acquire(shm_pool_t *pool, TickType_t to_ticks)
{
    if (!pool || !pool->lock)
        return NULL;
    void *blk_base = NULL;

    TickType_t start = xTaskGetTickCount();
    while (1)
    {
        if (xSemaphoreTake(pool->lock, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            if (pool->free_list)
            {
                blk_base = pool->free_list;
                pool->free_list = *((void **)blk_base);
                xSemaphoreGive(pool->lock);
                break;
            }
            xSemaphoreGive(pool->lock);
        }
        if ((xTaskGetTickCount() - start) >= to_ticks)
            break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    return blk_base ? BASE2PAYLOAD(blk_base) : NULL;
}

bool shm_pool_publish(shm_pool_t *pool, void *blk_payload, size_t used_len, TickType_t to_ticks)
{
    if (!pool || !blk_payload)
        return false;
    if (used_len > pool->block_size)
        return false;

    uint8_t *base = PAYLOAD2BASE(blk_payload);
    *((void **)base) = (void *)(uintptr_t)used_len; // เก็บ used_len ไว้ใน header

    return xQueueSend(pool->q, &blk_payload, to_ticks) == pdTRUE;
}

bool shm_pool_consume(shm_pool_t *pool, void **out_payload, size_t *out_len, TickType_t to_ticks)
{
    if (!pool || !out_payload)
        return false;
    void *payload = NULL;
    if (xQueueReceive(pool->q, &payload, to_ticks) != pdTRUE)
        return false;

    if (out_len)
    {
        uint8_t *base = PAYLOAD2BASE(payload);
        *out_len = (size_t)(uintptr_t)(*((void **)base));
    }
    *out_payload = payload;
    return true;
}

void shm_pool_release(shm_pool_t *pool, void *blk_payload)
{
    if (!pool || !blk_payload || !pool->lock)
        return;
    uint8_t *base = PAYLOAD2BASE(blk_payload);

    if (xSemaphoreTake(pool->lock, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        *((void **)base) = pool->free_list;
        pool->free_list = base;
        xSemaphoreGive(pool->lock);
    }
}

/* ===== Simple SPSC Ring Buffer ===== */

typedef struct
{
    uint8_t hdr[2];
} __packed_hdr_t;

static inline size_t _space(const shm_ring_t *rb)
{
    size_t h = rb->head, t = rb->tail, c = rb->cap;
    return (h >= t) ? (c - (h - t) - 1) : ((t - h) - 1);
}

static inline size_t _used(const shm_ring_t *rb)
{
    size_t h = rb->head, t = rb->tail, c = rb->cap;
    return (h >= t) ? (h - t) : (c - (t - h));
}

bool shm_ring_create(shm_ring_t *rb, size_t capacity, uint32_t caps)
{
    if (!rb || capacity < 64)
        return false;
    size_t cap = capacity + 1; // กัน full=empty
    uint8_t *buf = (uint8_t *)heap_caps_malloc(cap, caps);
    if (!buf)
        return false;

    memset(rb, 0, sizeof(*rb));
    rb->buf = buf;
    rb->cap = cap;
    rb->head = rb->tail = 0;
    rb->mtx = xSemaphoreCreateMutex();
    rb->caps = caps;
    rb->can_read = xSemaphoreCreateBinary();
    rb->can_write = xSemaphoreCreateBinary();
    if (rb->can_read)
        xSemaphoreGive(rb->can_read);
    if (rb->can_write)
        xSemaphoreGive(rb->can_write);
    return rb->mtx != NULL;
}

void shm_ring_destroy(shm_ring_t *rb)
{
    if (!rb)
        return;
    if (rb->mtx)
        vSemaphoreDelete(rb->mtx);
    if (rb->can_read)
        vSemaphoreDelete(rb->can_read);
    if (rb->can_write)
        vSemaphoreDelete(rb->can_write);
    if (rb->buf)
        heap_caps_free(rb->buf);
    memset(rb, 0, sizeof(*rb));
}

static void _ring_write_bytes(shm_ring_t *rb, const uint8_t *src, size_t n)
{
    size_t cap = rb->cap, h = rb->head;
    size_t first = (n <= (cap - h)) ? n : (cap - h);
    memcpy(rb->buf + h, src, first);
    if (n > first)
        memcpy(rb->buf, src + first, n - first);
    rb->head = (h + n) % cap;
}

static void _ring_read_bytes(shm_ring_t *rb, uint8_t *dst, size_t n)
{
    size_t cap = rb->cap, t = rb->tail;
    size_t first = (n <= (cap - t)) ? n : (cap - t);
    memcpy(dst, rb->buf + t, first);
    if (n > first)
        memcpy(dst + first, rb->buf, n - first);
    rb->tail = (t + n) % cap;
}

bool shm_ring_write(shm_ring_t *rb, const void *data, uint16_t len, TickType_t to_ticks)
{
    if (!rb || !rb->mtx || !data)
        return false;
    uint8_t hdr[2] = {(uint8_t)(len & 0xFF), (uint8_t)(len >> 8)};
    size_t need = 2 + (size_t)len;

    TickType_t start = xTaskGetTickCount();
    while (1)
    {
        if (xSemaphoreTake(rb->mtx, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            if (_space(rb) >= need)
            {
                _ring_write_bytes(rb, hdr, 2);
                _ring_write_bytes(rb, (const uint8_t *)data, len);
                xSemaphoreGive(rb->mtx);
                return true;
            }
            xSemaphoreGive(rb->mtx);
        }
        if ((xTaskGetTickCount() - start) >= to_ticks)
            break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

bool shm_ring_read(shm_ring_t *rb, void *out, uint16_t *inout_len, TickType_t to_ticks)
{
    if (!rb || !rb->mtx || !out || !inout_len)
        return false;

    TickType_t start = xTaskGetTickCount();
    while (1)
    {
        if (xSemaphoreTake(rb->mtx, pdMS_TO_TICKS(20)) == pdTRUE)
        {
            if (_used(rb) >= 2)
            {
                // peek header
                uint8_t hdr[2];
                size_t cap = rb->cap, t = rb->tail;
                size_t first = (2 <= (cap - t)) ? 2 : (cap - t);
                memcpy(hdr, rb->buf + t, first);
                if (2 > first)
                    memcpy(hdr + first, rb->buf, 2 - first);

                uint16_t frame_len = (uint16_t)(hdr[0] | (hdr[1] << 8));
                size_t need = 2 + (size_t)frame_len;

                if (_used(rb) >= need)
                {
                    // consume header
                    rb->tail = (rb->tail + 2) % cap;

                    size_t to_copy = frame_len;
                    if (to_copy > *inout_len)
                        to_copy = *inout_len;
                    _ring_read_bytes(rb, (uint8_t *)out, to_copy);

                    size_t remain = (size_t)frame_len - to_copy;
                    if (remain)
                    {
                        uint8_t tmp[32];
                        while (remain)
                        {
                            size_t chunk = remain > sizeof(tmp) ? sizeof(tmp) : remain;
                            _ring_read_bytes(rb, tmp, chunk);
                            remain -= chunk;
                        }
                    }

                    *inout_len = to_copy;
                    xSemaphoreGive(rb->mtx);
                    return true;
                }
            }
            xSemaphoreGive(rb->mtx);
        }
        if ((xTaskGetTickCount() - start) >= to_ticks)
            break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

/* ===== Demo tasks ===== */

static shm_pool_t g_pool;
static shm_ring_t g_ring;

static void shm_pool_producer(void *arg)
{
    const char *name = (const char *)arg;
    uint32_t seq = 0;

    while (1)
    {
        void *p = shm_pool_acquire(&g_pool, pdMS_TO_TICKS(50));
        if (p)
        {
            int n = snprintf((char *)p, g_pool.block_size,
                             "[%s] msg #%lu @%llu us",
                             name, (unsigned long)seq++,
                             (unsigned long long)esp_timer_get_time());
            if (n < 0)
                n = 0;
            if (n > (int)g_pool.block_size)
                n = (int)g_pool.block_size;

            if (!shm_pool_publish(&g_pool, p, (size_t)n + 1, pdMS_TO_TICKS(50)))
            {
                shm_pool_release(&g_pool, p);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void shm_pool_consumer(void *arg)
{
    (void)arg;
    for (;;)
    {
        void *payload = NULL;
        size_t used = 0;
        if (shm_pool_consume(&g_pool, &payload, &used, pdMS_TO_TICKS(1000)))
        {
            ESP_LOGI(TAG, "POOL RX: \"%s\" (len=%u)", (char *)payload, (unsigned)used);
            shm_pool_release(&g_pool, payload);
        }
    }
}

static void shm_ring_producer(void *arg)
{
    const char *name = (const char *)arg;
    uint32_t seq = 0;
    char msg[64];

    while (1)
    {
        int n = snprintf(msg, sizeof(msg), "[%s-RING] %lu", name, (unsigned long)seq++);
        if (n < 0)
            n = 0;
        if (n > (int)sizeof(msg))
            n = (int)sizeof(msg);

        if (!shm_ring_write(&g_ring, msg, (uint16_t)n, pdMS_TO_TICKS(100)))
        {
            ESP_LOGW(TAG, "RING TX timeout");
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

static void shm_ring_consumer(void *arg)
{
    (void)arg;
    char buf[80];

    while (1)
    {
        uint16_t len = sizeof(buf);
        if (shm_ring_read(&g_ring, buf, &len, pdMS_TO_TICKS(1000)))
        {
            buf[len < sizeof(buf) ? len : (sizeof(buf) - 1)] = '\0';
            ESP_LOGI(TAG, "RING RX: \"%s\" (len=%u)", buf, (unsigned)len);
        }
    }
}

void shm_demo_start(void)
{
    static bool started = false;
    if (started)
        return;
    started = true;

    // ✅ บังคับให้หน่วยความจำเป็นชนิด 8-bit
    uint32_t caps = MALLOC_CAP_8BIT | (SHM_USE_SPIRAM ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);

    if (!shm_pool_create(&g_pool,
                         SHM_DEFAULT_BLOCK_SIZE,
                         SHM_DEFAULT_BLOCK_COUNT,
                         SHM_QUEUE_LENGTH,
                         caps))
    {
        ESP_LOGE(TAG, "create shm pool failed");
        return;
    }

    if (!shm_ring_create(&g_ring, SHM_RING_CAPACITY, caps))
    {
        ESP_LOGE(TAG, "create ring failed");
        return;
    }

    xTaskCreate(shm_pool_producer, "shm_pool_tx", 4096, (void *)"P1", 5, NULL);
    xTaskCreate(shm_pool_consumer, "shm_pool_rx", 4096, NULL, 5, NULL);
    xTaskCreate(shm_ring_producer, "shm_ring_tx", 4096, (void *)"R1", 5, NULL);
    xTaskCreate(shm_ring_consumer, "shm_ring_rx", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Shared-memory demo started");
}