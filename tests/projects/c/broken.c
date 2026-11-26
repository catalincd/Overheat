/* Intentional errors for Overheat's problem matchers (built with `make BROKEN=1`). */
#include "util.h"

static int unused_helper(int x) { return x * 2; }

int broken_entry(void) {
    int unused;
    int total = add(1, "two");
    undeclared_function(total);
    return total
}
