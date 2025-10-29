// event_corr.h
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdint.h>
#include <stddef.h>

// กำหนดหน้าต่างเวลาสำหรับนับร่วมกัน (ms) และจำนวนบิตสูงสุดที่จะติดตาม (<=24)
void   evcorr_init(uint32_t window_ms, int bit_count);

/* เรียกทุกครั้ง "หลัง" มีการตั้งบิตใน EventGroup
   (เช่น เรียกต่อจาก evlog_add หรือใน dispatcher) */
void   evcorr_on_set(EventGroupHandle_t group, EventBits_t set_bits);

// ดึงเมทริกซ์ co-occurrence ออกมา (ขนาด bit_count x bit_count) ; คืนจำนวนช่องที่คัดลอก
size_t evcorr_dump(uint16_t *matrix /* len >= bit_count*bit_count */);

// อ่านค่า bit_count ปัจจุบัน (สะดวกเวลาพิมพ์)
int    evcorr_bit_count(void);