CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
           -Isrc -Ideps/quickjs
LDFLAGS := -lssl -lcrypto -lpthread -lm -ldl

# QuickJS from GitHub
QJS_REPO    := https://github.com/bellard/quickjs.git
QJS_DIR     := deps/quickjs
QJS_LIB     := $(QJS_DIR)/libquickjs.a

SRCS := src/main.c src/util.c src/stats.c src/http_parser.c src/tls.c \
        src/event_loop.c src/http_client.c src/fetch.c src/vm.c \
        src/cli.c src/worker.c src/bench.c
OBJS := $(SRCS:.c=.o)

BIN := jsb

TEST_SERVER_SRC := tests/test_server.c
TEST_SERVER_BIN := tests/test_server

.PHONY: all clean test deps

all: deps $(BIN)

$(BIN): $(OBJS) $(QJS_LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

src/%.o: src/%.c $(QJS_LIB)
	$(CC) $(CFLAGS) -c -o $@ $<

# QuickJS dependency
deps:
	@mkdir -p deps
	@if [ ! -f $(QJS_LIB) ]; then \
		echo "==> Cloning QuickJS from GitHub..."; \
		git clone --depth 1 $(QJS_REPO) $(QJS_DIR) 2>/dev/null || true; \
		echo "==> Building QuickJS..."; \
		$(MAKE) -C $(QJS_DIR) libquickjs.a CC=$(CC); \
	fi

$(QJS_LIB): deps

# Test server
$(TEST_SERVER_BIN): $(TEST_SERVER_SRC)
	$(CC) $(CFLAGS) -o $@ $< -lpthread

test: all $(TEST_SERVER_BIN)
	@bash tests/run_tests.sh

clean:
	rm -f $(OBJS) $(BIN) $(TEST_SERVER_BIN)

distclean: clean
	rm -rf deps/quickjs
