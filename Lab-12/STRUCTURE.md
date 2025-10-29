# โครงสร้างโฟลเดอร์ FreeRTOS Learning Materials

```
FreeRTOS/
├── README.md                          # เอกสารหลักและภาพรวมคอร์ส
│
├── 00-multitasking-evolution/         # วิวัฒนาการของ Multitasking
│   ├── README.md                      # ภาพรวมหัวข้อ
│   ├── theory.md                      # เนื้อหาทฤษฎี (1 ชม.)
│   └── practice/                      # การปฏิบัติ (2 ชม.)
│       ├── README.md
│       ├── lab1-single-vs-multi/
│       ├── lab2-time-sharing/
│       └── lab3-coop-vs-preemptive/
│
├── 01-freertos-overview/              # ภาพรวม FreeRTOS
│   ├── README.md
│   ├── theory.md                      # เนื้อหาทฤษฎี (1 ชม.)
│   └── practice/                      # การปฏิบัติ (2 ชม.)
│       ├── README.md
│       ├── lab1-setup-first-project/
│       ├── lab2-hello-world/
│       └── lab3-first-task/
│
├── 02-tasks-and-scheduling/           # Tasks และ Scheduling
│   ├── README.md
│   ├── theory.md                      # เนื้อหาทฤษฎี (1 ชม.)
│   └── practice/                      # การปฏิบัติ (2 ชม.)
│       ├── README.md
│       ├── lab1-task-priority/
│       ├── lab2-task-states/
│       └── lab3-stack-monitoring/
│
├── 03-queues/                         # Queues และการสื่อสาร
│   ├── README.md
│   ├── theory.md                      # เนื้อหาทฤษฎี (1 ชม.)
│   └── practice/                      # การปฏิบัติ (2 ชม.)
│       ├── README.md
│       ├── lab1-basic-queue/
│       ├── lab2-producer-consumer/
│       └── lab3-queue-sets/
│
├── 04-semaphores/                     # Semaphores และ Mutexes
│   ├── README.md
│   ├── theory.md                      # เนื้อหาทฤษฎี (1 ชม.)
│   └── practice/                      # การปฏิบัติ (2 ชม.)
│       ├── README.md
│       ├── lab1-binary-semaphore/
│       ├── lab2-counting-semaphore/
│       └── lab3-mutex-priority-inheritance/
│
├── 05-timers/                         # Software Timers
│   ├── README.md
│   ├── theory.md                      # เนื้อหาทฤษฎี (1 ชม.)
│   └── practice/                      # การปฏิบัติ (2 ชม.)
│       ├── README.md
│       ├── lab1-basic-timers/
│       ├── lab2-watchdog-system/
│       └── lab3-led-pattern-controller/
│
├── 06-event-groups/                   # Event Groups
│   ├── README.md
│   ├── theory.md                      # เนื้อหาทฤษฎี (1 ชม.)
│   └── practice/                      # การปฏิบัติ (2 ชม.)
│       ├── README.md
│       ├── lab1-basic-events/
│       ├── lab2-startup-orchestration/
│       └── lab3-multi-sensor-fusion/
│
├── 07-memory-management/              # การจัดการหน่วยความจำ
│   ├── README.md
│   ├── theory.md                      # เนื้อหาทฤษฎี (1 ชม.)
│   └── practice/                      # การปฏิบัติ (2 ชม.)
│       ├── README.md
│       ├── lab1-static-vs-dynamic/
│       ├── lab2-memory-pools/
│       └── lab3-memory-monitoring/
│
└── 08-esp-idf-specific/               # คุณสมบัติเฉพาะ ESP-IDF
    ├── README.md
    ├── theory.md                      # เนื้อหาทฤษฎี (1 ชม.)
    └── practice/                      # การปฏิบัติ (2 ชม.)
        ├── README.md
        ├── lab1-dual-core-smp/
        ├── lab2-task-affinity/
        └── lab3-ipc-optimization/
```

## สรุปโครงสร้าง

### รูปแบบการจัดเก็บ
- **แยกทฤษฎีและปฏิบัติ**: แต่ละหัวข้อมี `theory.md` และโฟลเดอร์ `practice/`
- **เวลาการเรียน**: ทฤษฎี 1 ชั่วโมง + ปฏิบัติ 2 ชั่วโมง ต่อหัวข้อ
- **การปฏิบัติ**: แต่ละหัวข้อมี 3 แลป เฉลี่ย 40 นาทีต่อแลป

### ประโยชน์ของโครงสร้างนี้
1. **การเรียนรู้เป็นขั้นตอน**: ทฤษฎีก่อน แล้วฝึกปฏิบัติ
2. **ง่ายต่อการติดตาม**: แต่ละหัวข้อมี README.md อธิบายชัดเจน
3. **ยืดหยุ่น**: สามารถเลือกเรียนตามความสนใจหรือข้ามหัวข้อได้
4. **พร้อมใช้งาน**: มีตัวอย่างโค้ดและแนวทางการทดลองครบถ้วน