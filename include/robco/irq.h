#ifndef ROBCO_IRQ_H
#define ROBCO_IRQ_H

#include <stdint.h>

/*
 * BCM283x legacy interrupt controller. IRQ numbers:
 *   0..63   GPU IRQs (pending1/pending2), e.g. system timer channel n = n
 *   64..71  ARM basic IRQs (bits 0..7 of basic pending)
 * Handlers run in IRQ mode with IRQs masked. No nesting, no FIQ.
 */
#define IRQ_GPU_COUNT       64
#define IRQ_BASIC_FIRST     64
#define IRQ_COUNT           72

#define IRQ_TIMER1          1
#define IRQ_TIMER3          3

typedef void (*irq_handler_t)(unsigned irq, void *arg);

/* Masks every source and routes the controller to core 0. Call once. */
void irq_init(void);

/* Installs a handler. Returns 0, or -1 if `irq` is invalid or taken. */
int irq_register(unsigned irq, irq_handler_t fn, void *arg);

/* Disables the source and removes its handler. */
void irq_unregister(unsigned irq);

/* Unmasks the source in the controller. */
void irq_enable(unsigned irq);

/* Masks the source in the controller. */
void irq_disable(unsigned irq);

/* Masks IRQs on the CPU and returns the previous state for irq_restore(). */
uint32_t irq_save(void);

/* Restores the CPU IRQ mask saved by irq_save(). */
void irq_restore(uint32_t state);

/* Unmasks IRQs on the CPU (clears CPSR.I). */
void irq_cpu_enable(void);

/* Masks IRQs on the CPU (sets CPSR.I). */
void irq_cpu_disable(void);

/* Number of sources that fired with no handler. Each one is disabled. */
uint32_t irq_spurious_count(void);

/* Called by the IRQ vector. `pc` and `spsr` describe the interrupted code. */
void robco_irq_dispatch(uint32_t pc, uint32_t spsr);

#endif
