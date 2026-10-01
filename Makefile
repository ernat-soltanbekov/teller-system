CC ?= cc
PKG_CONFIG ?= pkg-config
CPPFLAGS += -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE -Isrc $(shell $(PKG_CONFIG) --cflags sqlite3 openssl)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += $(shell $(PKG_CONFIG) --libs sqlite3 openssl)
CORE = src/util.c src/auth.c src/store.c src/snapshot.c src/import.c src/system.c src/analyzer.c src/ui.c
OBJECTS = $(patsubst src/%.c,build/%.o,$(CORE) src/main.c)

.PHONY: all clean test audit sanitize fuzz stress analyze check
all: teller-system atm

teller-system: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LDLIBS) -o $@

atm: teller-system
	ln -sf teller-system atm

build/%.o: src/%.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

build/unit-tests: tests/unit.c $(CORE) src/header.h src/analyzer.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) tests/unit.c $(CORE) $(LDLIBS) -o $@

build/teller-test: $(CORE) src/main.c src/header.h src/analyzer.h
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTELLER_TESTING $(LDFLAGS) $(CORE) src/main.c $(LDLIBS) -o $@

test: build/unit-tests
	./build/unit-tests

audit: all build/teller-test
	python3 tests/audit.py

sanitize:
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined tests/unit.c $(CORE) $(LDLIBS) -o build/unit-sanitize
	./build/unit-sanitize

fuzz:
	@mkdir -p build/fuzz-corpus
	clang $(CPPFLAGS) -std=c11 -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined tests/fuzz.c src/util.c src/analyzer.c $(LDLIBS) -o build/fuzz
	./build/fuzz build/fuzz-corpus -max_total_time=15 -max_len=512 -timeout=2

analyze:
	clang --analyze $(CPPFLAGS) -std=c11 -Xanalyzer -analyzer-output=text $(CORE) src/main.c

stress:
	@mkdir -p build
	$(CC) $(CPPFLAGS) -std=c11 -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined tests/fuzz_driver.c tests/fuzz.c src/util.c src/analyzer.c $(LDLIBS) -o build/parser-stress
	./build/parser-stress

check: test audit sanitize

clean:
	rm -rf build teller-system atm

-include $(OBJECTS:.o=.d)
