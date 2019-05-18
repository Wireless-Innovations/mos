#ifndef SYSTEM_TASK_H
#define SYSTEM_TASK_H

#include <stdint.h>

typedef struct trap_frame
{
  uint32_t gs, fs, es, ds;                         // Data segment selector
  uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax; // Pushed by pusha.
  uint32_t eip, cs, eflags, useresp, ss;           // Pushed by the processor automatically.
} trap_frame;

typedef struct thread
{
  uint32_t esp;
} thread;

void task_init();
void task_start();
void task_schedule(uint32_t esp);

thread create_thread(void *func, uint32_t esp);

bool queue_push(thread t);

#endif