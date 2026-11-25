#include <stdio.h>

static int counter = 42;

int add(int a, int b) {
    return a + b; /* sum */
}

int main(void) {
    const char *msg = "hello";
    printf("%s %d\n", msg, add(counter, 7));
    return 0;
}
