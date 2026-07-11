# Sentinel hub — build on the Pi (QNX 8.0) via SSH:
#   ssh qnxuser@172.20.10.9
#   cd ~/sentinel/hub
#   make
#   ./hub
#
# Persists to SQLite at $HOME/sentinel/sentinel.db by default (override
# with SENTINEL_DB_PATH). Needs sqlite-dev's headers/libs, already
# installed on this Pi image (sqlite3, sqlite-dev, sqlite-libs via apk).
#
# If clang complains about missing socket symbols, try uncommenting
# LDFLAGS_SOCKET below (some QNX images split networking into libsocket).

CC      = clang
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lpthread -lsqlite3
# LDFLAGS_SOCKET = -lsocket
# LDFLAGS += $(LDFLAGS_SOCKET)

BIN = hub

all: $(BIN)

$(BIN): main.c
	$(CC) $(CFLAGS) -o $(BIN) main.c $(LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN)

.PHONY: all run clean
