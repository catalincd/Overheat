# Tiny C project for Overheat's build-task tests. `make BROKEN=1` also builds
# broken.c, which has intentional errors and warnings.
CC ?= cc
CFLAGS ?= -Wall -Wextra -g
SRCS = main.c util.c
ifdef BROKEN
SRCS += broken.c
CFLAGS += -DBROKEN
endif

app: $(SRCS) util.h
	$(CC) $(CFLAGS) -o $@ $(SRCS)

run: app
	./app

test: app
	./app --selftest

clean:
	rm -f app

.PHONY: run test clean
