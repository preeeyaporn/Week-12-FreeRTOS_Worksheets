#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

/* ======================
 * Config (ปรับได้)
 * ====================== */
#ifndef SHM_DEFAULT_BLOCK_SIZE
#define SHM_DEFAULT_BLOCK_SIZE 128 // payload ต่อบล็อก
#endif

#ifndef SHM_DEFAULT_BLOCK_COUNT
#define SHM_DEFAULT_BLOCK_COUNT 16 // จำนวนบล็อกในพูล
#endif

#ifndef SHM_QUEUE_LENGTH
#define SHM_QUEUE_LENGTH 8 // ความยาวคิวของพูล
#endif

#ifndef SHM_RING_CAPACITY
#define SHM_RING_CAPACITY 1024 // ขนาดบัฟเฟอร์ริง (ไบต์)
#endif

#ifndef SHM_USE_SPIRAM
#define SHM_USE_SPIRAM 0 // 1 = ใช้ SPIRAM ถ้ามี
#endif

/* ======================
 * Zero-copy block pool
 * ====================== */
typedef struct
{
    void *buffer;           // memory backing ทั้งก้อน
    size_t block_size;      // ขนาด payload ต่อบล็อก (ไม่รวม header)
    int num_blocks;         // จำนวนบล็อก
    void *free_list;        // LIFO free list (เก็บ pointer base ของบล็อก)
    SemaphoreHandle_t lock; // ป้องกัน free_list
    QueueHandle_t q;        // คิวส่ง payload ptr ที่ publish แล้ว
    uint32_t caps;          // heap capabilities
} shm_pool_t;

bool shm_pool_create(shm_pool_t *pool, size_t block_size, int num_blocks, int queue_len, uint32_t caps);
void shm_pool_destroy(shm_pool_t *pool);

void *shm_pool_acquire(shm_pool_t *pool, TickType_t to_ticks);                                     // ได้ ptr ไปเขียน (payload)
bool shm_pool_publish(shm_pool_t *pool, void *blk_payload, size_t used_len, TickType_t to_ticks);  // ส่งเข้าคิว
bool shm_pool_consume(shm_pool_t *pool, void **out_payload, size_t *out_len, TickType_t to_ticks); // รับจากคิว
void shm_pool_release(shm_pool_t *pool, void *blk_payload);                                        // คืนบล็อกเข้าพูล

/* ======================
 * Simple ring buffer (SPSC)
 * frame = [uint16_t len][payload bytes]
 * ====================== */
typedef struct
{
    uint8_t *buf;
    size_t cap; // ขนาดบัฟเฟอร์ (มีเว้น 1 ไบต์ไว้กัน full=empty)
    size_t head;
    size_t tail;
    SemaphoreHandle_t mtx;       // ล็อกสำหรับอ่าน/เขียน
    SemaphoreHandle_t can_read;  // เผื่อใช้ในอนาคต
    SemaphoreHandle_t can_write; // เผื่อใช้ในอนาคต
    uint32_t caps;
} shm_ring_t;

bool shm_ring_create(shm_ring_t *rb, size_t capacity, uint32_t caps);
void shm_ring_destroy(shm_ring_t *rb);
bool shm_ring_write(shm_ring_t *rb, const void *data, uint16_t len, TickType_t to_ticks);
bool shm_ring_read(shm_ring_t *rb, void *out, uint16_t *inout_len, TickType_t to_ticks);

/* ======================
 * Demo starter
 * ====================== */
void shm_demo_start(void);

#endif /* SHARED_MEMORY_H */