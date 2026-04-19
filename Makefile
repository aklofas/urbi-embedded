SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)

CFLAGS ?= -std=c99 -Wall -Wextra -Wpedantic -Os
CPPFLAGS += -Isrc

liburbi.a: $(OBJ)
	$(AR) rcs $@ $^

clean:
	rm -f $(OBJ) liburbi.a

.PHONY: clean
