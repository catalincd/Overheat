// A fixed-size ring buffer of samples, with a moving average.
#include <stdio.h>
#include <stdlib.h>

#define CAPACITY 8

typedef struct {
    double data[CAPACITY];
    size_t head, count;
} Ring;

static void ring_push(Ring *r, double value) {
    r->data[r->head] = value;
    r->head = (r->head + 1) % CAPACITY;
    if (r->count < CAPACITY) r->count++;
}

static double ring_average(const Ring *r) {
    if (r->count == 0) return 0.0;
    double sum = 0.0;
    for (size_t i = 0; i < r->count; i++) {
        sum += r->data[i];
    }
    return sum / (double)r->count;
}

int main(int argc, char **argv) {
    Ring ring = {.count = 0};
    int samples = argc > 1 ? atoi(argv[1]) : 20;
    for (int i = 0; i < samples; i++) {
        ring_push(&ring, i * 0.5);
        printf("sample %2d -> avg %.2f\n", i, ring_average(&ring));
    }
    return EXIT_SUCCESS;
}
