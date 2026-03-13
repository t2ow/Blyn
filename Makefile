CC     = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -D_DEFAULT_SOURCE
LIBS   = -lncurses
SRC    = src/main.c
OUT    = blyn

all:
	$(CC) $(CFLAGS) $(SRC) -o $(OUT) $(LIBS)

install: all
	cp $(OUT) /usr/local/bin/blyn
	@echo "Installed! You can now run: blyn <file>"

clean:
	rm -f $(OUT)
