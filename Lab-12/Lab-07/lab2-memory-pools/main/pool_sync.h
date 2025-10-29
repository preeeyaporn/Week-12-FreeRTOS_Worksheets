#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_attr.h"

// ===== Config =====
#ifndef SYNC_TIMEOUT_MS
#define SYNC_TIMEOUT_MS 200 // ค่า default ถ้า caller ไม่กำหนด
#endif

// กลยุทธ์ซิงก์:
// - ใช้ mutex (priority-inheritance) สำหรับ write (ปรับโครงสร้าง/แก้ freelist)
// - ใช้ counting semaphore สำหรับ read (อ่านสถิติได้พร้อมกันหลาย task)
// - ใช้ spinlock แบบ portMUX_TYPE ป้องกัน race สั้น ๆ ที่ต้อง atomic ใน ISR/critical
typedef struct
{
    SemaphoreHandle_t rw_mutex;    // exclusive writers
    SemaphoreHandle_t readers_sem; // permits for readers (MAX_READERS)
    volatile int readers;          // readers in critical
    portMUX_TYPE spin;             // short critical sections / ISR
} pool_sync_t;

#ifdef __cplusplus
extern "C"
{
#endif

    // Init/Deinit
    bool pool_sync_init(pool_sync_t *s);
    void pool_sync_deinit(pool_sync_t *s);

    // Write lock (exclusive)
    bool pool_sync_write_lock(pool_sync_t *s, TickType_t to_ticks);
    void pool_sync_write_unlock(pool_sync_t *s);

    // Read (stats) lock (shared)
    bool pool_sync_stats_lock(pool_sync_t *s, TickType_t to_ticks);
    void pool_sync_stats_unlock(pool_sync_t *s);

    // Deadlock symptom checker (best-effort)
    static inline bool pool_sync_check_deadlock(pool_sync_t *s)
    {
        (void)s;
        // สามารถใส่เงื่อนไขเฉพาะงานได้ภายหลัง ตอนนี้คืน false ไป
        return false;
    }

    // ===== ISR-safe criticals =====
    // หมายเหตุ: หลีกเลี่ยงการเรียก API FreeRTOS ที่ block/alloc ใน ISR
    // ส่วนนี้มีไว้ให้ allocator ใช้ปกป้อง Treiber-like push/pop สั้น ๆ
    static inline void pool_sync_isr_enter(pool_sync_t *s)
    {
        portENTER_CRITICAL(&s->spin);
    }
    static inline void pool_sync_isr_exit(pool_sync_t *s)
    {
        portEXIT_CRITICAL(&s->spin);
    }

#ifdef __cplusplus
}
#endif