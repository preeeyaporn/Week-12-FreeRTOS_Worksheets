// dynamic_events.h
#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdbool.h>
#include <stdint.h>

// เริ่มระบบ allocator โดยระบุ mask ของบิตที่ "ห้ามแตะ" (เช่นบิตระบบที่ใช้อยู่แล้ว)
bool        dyn_init(EventBits_t reserved_mask);

// ขอจองบิตว่าง 1 บิต (คืน 0 ถ้าไม่มีว่าง) — ไม่ copy ชื่อ แค่เก็บ pointer
EventBits_t dyn_acquire(const char* name);

// คืนบิตที่เคยจอง
bool        dyn_release(EventBits_t bit);

// อ่านชื่อที่ผูกกับบิตนั้น (ถ้าเคยจอง)
const char* dyn_name(EventBits_t bit);