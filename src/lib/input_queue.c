#include <stdint.h>

#include <robco/input.h>

/*
 * Ring of INPUT_QUEUE_LEN events. head and tail run freely, so
 * head - tail is the count even after they wrap. The caller provides
 * mutual exclusion; src/drv/input.c masks IRQs around each call.
 */

void input_queue_init(input_queue_t *q)
{
    q->head = 0;
    q->tail = 0;
    q->pushed = 0;
    q->dropped = 0;
}

int input_queue_push(input_queue_t *q, const input_event_t *ev)
{
    if (q->head - q->tail >= INPUT_QUEUE_LEN) {
        q->dropped++;
        return INPUT_ERR_FULL;
    }
    q->buf[q->head % INPUT_QUEUE_LEN] = *ev;
    q->head++;
    q->pushed++;
    return 0;
}

int input_queue_pop(input_queue_t *q, input_event_t *ev)
{
    if (q->head == q->tail) {
        return 0;
    }
    *ev = q->buf[q->tail % INPUT_QUEUE_LEN];
    q->tail++;
    return 1;
}

unsigned input_queue_count(const input_queue_t *q)
{
    return (unsigned)(q->head - q->tail);
}

int input_queue_push_oldest(input_queue_t *q, const input_event_t *ev)
{
    int ret = 0;

    if (q->head - q->tail >= INPUT_QUEUE_LEN) {
        q->tail++;
        q->dropped++;
        ret = INPUT_DROPPED_OLDEST;
    }
    q->buf[q->head % INPUT_QUEUE_LEN] = *ev;
    q->head++;
    q->pushed++;
    return ret;
}
