SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

TEST_SRC := $(wildcard tests/unit/test_*.c) tests/unit/runner.c
TEST_OBJ := $(TEST_SRC:.c=.o)

CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -Os
CPPFLAGS += -Isrc -Itests/unit

liburbi.a: $(OBJ)
	$(AR) rcs $@ $^

test: liburbi.a $(TEST_OBJ)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o tests/unit/runner $(TEST_OBJ) liburbi.a
	./tests/unit/runner

test-asan: clean
	$(MAKE) CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address -fno-omit-frame-pointer" test

test-ubsan: clean
	$(MAKE) CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=undefined -fno-omit-frame-pointer" test

test-debug: clean
	$(MAKE) CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -O0 -g" test

clean:
	rm -f $(OBJ) $(TEST_OBJ) liburbi.a tests/unit/runner

.PHONY: test test-asan test-ubsan test-debug clean
