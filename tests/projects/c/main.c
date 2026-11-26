#include <stdio.h>
#include <string.h>
#include "util.h"

#ifdef BROKEN
int broken_entry(void);
#endif

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return add(2, 2) == 4 ? 0 : 1;
    printf("%s: %d\n", greeting(), add(20, 22));
#ifdef BROKEN
    return broken_entry();
#endif
    return 0;
}
