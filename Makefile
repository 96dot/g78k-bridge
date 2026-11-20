CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror=implicit-function-declaration -D_FORTIFY_SOURCE=2 -fstack-protector-strong

all: build/g78k-bridge build/g78k-fake

# Optional: needs the SDL3 development files (pkg-config sdl3).
sdl_check: build/sdl_check

build:
	mkdir -p build

build/g78k-bridge: src/g78k-bridge.c src/*.h | build
	$(CC) $(CFLAGS) -pthread -o $@ src/g78k-bridge.c

build/g78k-fake: src/g78k-fake.c src/gamesir.h src/uhid_util.h | build
	$(CC) $(CFLAGS) -o $@ src/g78k-fake.c

build/sdl_check: tests/sdl_check.c | build
	$(CC) $(CFLAGS) -o $@ tests/sdl_check.c $$(pkg-config --cflags --libs sdl3)

clean:
	rm -rf build

.PHONY: all clean sdl_check

build/map_test: tests/map_test.c src/g78k-bridge.c src/*.h | build
	$(CC) $(CFLAGS) -Wno-unused-function -pthread -o $@ tests/map_test.c

test: build/map_test
	build/map_test

.PHONY: test
