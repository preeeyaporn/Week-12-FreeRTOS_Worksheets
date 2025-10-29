#include "pool_sync.h"

#define MAX_READERS 16
static const char *SYNC_TAG = "POOL_SYNC";

bool pool_sync_init(pool_sync_t *s)
{
    if (!s)
        return false;
    s->rw_mutex = xSemaphoreCreateMutex(); // priority inheritance
    s->readers_sem = xSemaphoreCreateCounting(MAX_READERS, MAX_READERS);
    s->readers = 0;
    s->spin = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;

    if (!s->rw_mutex || !s->readers_sem)
    {
        if (s->rw_mutex)
            vSemaphoreDelete(s->rw_mutex);
        if (s->readers_sem)
            vSemaphoreDelete(s->readers_sem);
        return false;
    }
    return true;
}

void pool_sync_deinit(pool_sync_t *s)
{
    if (!s)
        return;
    if (s->rw_mutex)
        vSemaphoreDelete(s->rw_mutex);
    if (s->readers_sem)
        vSemaphoreDelete(s->readers_sem);
    s->rw_mutex = NULL;
    s->readers_sem = NULL;
    s->readers = 0;
}

bool pool_sync_write_lock(pool_sync_t *s, TickType_t to_ticks)
{
    if (!s || !s->rw_mutex)
        return false;

    // block writers until all readers are out: เราใช้ readers_sem drain + mutex
    if (xSemaphoreTake(s->rw_mutex, to_ticks) != pdTRUE)
        return false;

    // ปิดทาง readers ใหม่: ดึง permits ออกให้หมด
    for (int i = 0; i < MAX_READERS; ++i)
    {
        if (xSemaphoreTake(s->readers_sem, 0) != pdTRUE)
        {
            // หมายความว่า permits หมดแล้ว (ดี) หรือมีคนยืมอยู่
            // ถ้ามี readers ค้าง: spin รอแบบสุภาพ
            // เราไม่ใช้ vTaskDelay(1) ตรงนี้ เพราะมีโอกาสเกิด priority inversion
            // ใช้ polling สั้น ๆ + yield
            while (s->readers > 0)
            {
                taskYIELD();
            }
            break;
        }
    }
    // ถึงตรงนี้ readers ใหม่จะเข้าไม่ได้, readers เดิมออกหมด (readers==0)
    return true;
}

void pool_sync_write_unlock(pool_sync_t *s)
{
    if (!s || !s->rw_mutex)
        return;

    // เปิดทาง readers: เติม permits กลับ
    UBaseType_t avail = uxSemaphoreGetCount(s->readers_sem);
    for (UBaseType_t i = avail; i < MAX_READERS; ++i)
    {
        xSemaphoreGive(s->readers_sem);
    }
    xSemaphoreGive(s->rw_mutex);
}

bool pool_sync_stats_lock(pool_sync_t *s, TickType_t to_ticks)
{
    if (!s || !s->readers_sem)
        return false;
    if (xSemaphoreTake(s->readers_sem, to_ticks) != pdTRUE)
        return false;

    // นับ readers
    taskENTER_CRITICAL(&s->spin);
    s->readers++;
    taskEXIT_CRITICAL(&s->spin);

    return true;
}

void pool_sync_stats_unlock(pool_sync_t *s)
{
    if (!s || !s->readers_sem)
        return;

    taskENTER_CRITICAL(&s->spin);
    if (s->readers > 0)
        s->readers--;
    taskEXIT_CRITICAL(&s->spin);

    xSemaphoreGive(s->readers_sem);
}