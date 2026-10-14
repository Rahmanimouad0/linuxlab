# LinuxLab - plain Makefile build (CMakeLists.txt provides the same targets).
#
#   make              build everything into build/bin
#   make test         build and run the whole test suite
#   make SAN=1        AddressSanitizer + UndefinedBehaviorSanitizer build
#   make CC=clang     build with clang
#   make bench        run the benchmarks and write CSVs to results/

CC      ?= cc
BUILD   ?= build
BIN     := $(BUILD)/bin
OBJ     := $(BUILD)/obj

CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
           -Wstrict-prototypes -Wmissing-prototypes -Wvla -Icommon
LDLIBS  += -pthread -lrt

ifeq ($(SAN),1)
  CFLAGS  += -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all
  LDFLAGS += -fsanitize=address,undefined
endif

LSH_SRC    := $(wildcard shell/src/*.c)
LSH_OBJ    := $(LSH_SRC:%.c=$(OBJ)/%.o)
COMMON_OBJ := $(OBJ)/common/util.o
PROG_SRC   := $(wildcard labs/*/*.c) $(wildcard tools/*.c) $(wildcard benchmarks/*.c)
PROGS      := $(foreach s,$(PROG_SRC),$(BIN)/$(basename $(notdir $(s))))
UNIT_TESTS := $(BIN)/test_parser

.PHONY: all test unit-test shell-test lab-test bench clean format tidy

all: $(BIN)/lsh $(PROGS) $(UNIT_TESTS)

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BIN)/lsh: $(LSH_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

# One executable per lab/tool/benchmark source file.
define PROG_RULE
$(BIN)/$(basename $(notdir $(1))): $(OBJ)/$(1:.c=.o) $(COMMON_OBJ)
	@mkdir -p $$(dir $$@)
	$$(CC) $$(LDFLAGS) $$^ -o $$@ $$(LDLIBS)
endef
$(foreach s,$(PROG_SRC),$(eval $(call PROG_RULE,$(s))))

$(BIN)/test_parser: $(OBJ)/tests/unit/test_parser.o $(OBJ)/shell/src/lexer.o $(OBJ)/shell/src/parser.o
	@mkdir -p $(dir $@)
	$(CC) $(LDFLAGS) $^ -o $@ $(LDLIBS)

test: all
	bash scripts/run_tests.sh $(BIN)

unit-test: $(UNIT_TESTS)
	$(BIN)/test_parser

shell-test: $(BIN)/lsh
	bash tests/shell/test_shell.sh $(BIN)/lsh

lab-test: all
	bash tests/labs/test_labs.sh $(BIN)

bench: all
	bash benchmarks/run_benchmarks.sh $(BIN)

format:
	clang-format -i $$(find . -path ./build -prune -o -name '*.[ch]' -print)

tidy:
	clang-tidy $(LSH_SRC) tools/*.c -- $(CFLAGS)

clean:
	rm -rf $(BUILD)

-include $(shell find $(OBJ) -name '*.d' 2>/dev/null)
