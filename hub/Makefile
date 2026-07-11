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
#
# capture_snapshot is a SEPARATE binary (see capture_snapshot.c) that
# hub shells out to via system() right before running the OpenCV
# classifier — it needs -lcamera, which `hub` itself does not link.
# Field names inside capture_snapshot.c are best-effort against the real
# QNX camera_api — check the header comment there before trusting `make`
# to succeed on the first try.

CC      = clang
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lpthread -lsqlite3
CAMERA_LDFLAGS = -lcamera
# LDFLAGS_SOCKET = -lsocket
# LDFLAGS += $(LDFLAGS_SOCKET)

BIN = hub
SNAPSHOT_BIN = capture_snapshot

all: $(BIN) $(SNAPSHOT_BIN)

$(BIN): main.c
	$(CC) $(CFLAGS) -o $(BIN) main.c $(LDFLAGS)

$(SNAPSHOT_BIN): capture_snapshot.c
	$(CC) $(CFLAGS) -o $(SNAPSHOT_BIN) capture_snapshot.c $(CAMERA_LDFLAGS)

run: $(BIN)
	./$(BIN)

clean:
	rm -f $(BIN) $(SNAPSHOT_BIN)

.PHONY: all run clean
