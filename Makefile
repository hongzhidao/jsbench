CC      ?= gcc
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11 \
           -Isrc -Ideps/quickjs
LDFLAGS := -lssl -lcrypto -lpthread -lm -ldl

# QuickJS from GitHub
QJS_REPO    := https://github.com/bellard/quickjs.git
QJS_DIR     := deps/quickjs
QJS_LIB     := $(QJS_DIR)/libquickjs.a

SRCS := src/js_main.c src/js_time.c src/js_rbtree.c src/js_timer.c src/js_engine.c src/js_util.c src/js_stats.c src/js_http_parser.c \
        src/js_tls.c src/js_epoll.c src/js_conn.c src/js_web.c src/js_headers.c src/js_response.c src/js_fetch.c \
        src/js_loop.c src/js_vm.c src/js_runtime.c src/js_worker.c
OBJS := $(patsubst src/%.c,build/%.o,$(SRCS))
DEPS := $(OBJS:.o=.d)

BIN := jsb

TEST_SERVER_SRC := tests/test_server.c
TEST_SERVER_BIN := tests/test_server

.PHONY: all clean test deps

all: deps $(BIN)

$(BIN): $(OBJS) $(QJS_LIB)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.c $(QJS_LIB) | build
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEPS)

build:
	@mkdir -p build

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
	rm -rf build $(BIN) $(TEST_SERVER_BIN)

distclean: clean
	rm -rf deps/quickjs
