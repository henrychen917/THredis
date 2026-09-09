CXX      ?= g++
JEDIR    ?= /home/user/Projects/refs/jemalloc/_install
# jemalloc is optional: without it the tree still builds and runs, using the deterministic portable
# size-class table in alloc.h. Set JE=0 to force that path (useful for A/B-ing the allocator).
JE       ?= 1
ifeq ($(JE),1)
  # Prefer the system package; fall back to the copy built from refs/.
  ifneq ($(wildcard /usr/include/jemalloc/jemalloc.h),)
    JEFLAGS := -DTOMO_JEMALLOC
    JELIBS  := -ljemalloc
  else
    JEFLAGS := -DTOMO_JEMALLOC -I$(JEDIR)/include
    JELIBS  := $(JEDIR)/lib/libjemalloc.a -ldl
  endif
else
  JEFLAGS :=
  JELIBS  :=
endif
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -march=native -pthread
LDLIBS   ?= -luring -pthread
SRC      := src/main.cc src/net/tls.cc src/cmd/commands.cc src/cmd/glob.cc src/cmd/xshard.cc src/cmd/acl.cc src/cmd/hll.cc src/cmd/t_server.cc src/cmd/t_string.cc src/cmd/t_string_notify.cc src/cmd/t_hash.cc src/cmd/t_hash_ttl.cc \
            src/cmd/t_list.cc src/cmd/t_set.cc src/cmd/t_zset.cc src/cmd/t_zset_ops.cc src/cmd/geo.cc src/cmd/t_stream.cc src/cmd/t_stream_groups.cc src/cmd/scripting.cc src/cmd/functions.cc src/cmd/serialize.cc src/snapshot/snapshot.cc src/persist/aof.cc
SRC      += src/cmd/climon.cc src/cmd/tracking.cc
SRC      += src/cmd/server_tail.cc src/cmd/slowlog.cc src/cmd/lcs.cc src/cmd/info_stats.cc
SRC      += src/cmd/lbsignals.cc
SRC      += src/core/flipctl.cc
SRC      += src/core/genthread.cc
SRC      += src/core/rl2s.cc
SRC      += src/cmd/cmdgap.cc
SRC      += src/cmd/pfdebug.cc
SRC      += src/cmd/cmdmeta.cc
SRC      += src/cmd/t_sort.cc
LDLIBS   += -lssl -lcrypto
BIN      := build/tomokv
OBJ      := $(SRC:%.cc=build/%.o)

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(CXXFLAGS) $(OBJ) -o $@ $(JELIBS) $(LDLIBS) -lm

# The clean string family intentionally excludes the armed instantiations, but its parsed template
# bodies still move GCC just past the default large-unit threshold. 10600 restores the same inlining
# decisions as the base-420b4d492 translation unit; the objdump gate locks cmd_get/cmd_set to base.
build/src/cmd/t_string.o: CXXFLAGS += --param large-unit-insns=10600

build/%.o: %.cc $(wildcard src/*/*.h) $(wildcard src/*/*.inc) $(wildcard third_party/lua/*) Makefile
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -I. -c $< -o $@

asan: CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
asan:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. $(SRC) -o build/tomokv-asan $(LDLIBS) -lm

tsan:
	@mkdir -p build
	$(CXX) -std=c++20 -O1 -g -Wall -Wextra -pthread -fsanitize=thread \
	  -I. $(SRC) -o build/tomokv-tsan $(LDLIBS) -lm

# The armed-write block cache's ownership laws as assertions: the cache and the QSBR retire ring
# are owner-private (one thread, no lock), each cached block is resident exactly once, each class
# list matches its counter, and every shard's retire sink names its CURRENT owner. Debug-only: the
# residency set and the sampled list walk cost far more than the path they guard. DESIGN-P0REPLY.md.
rlcachedbg:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -DTOMO_RL_CACHE_DEBUG -I. $(SRC) \
	  -o build/tomokv-rlcachedbg $(JELIBS) $(LDLIBS) -lm

# NEGATIVE-CONTROL BUILD for the row above: the same assertions with the ownership-edge rebind
# removed, i.e. the pre-fix behaviour. tests/rlcache_churn.py MUST fail against this binary; a
# detector that cannot report failure proves nothing about the runs that pass.
rlcache-nofix:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -DTOMO_RL_CACHE_DEBUG -DTOMO_RL_CACHE_NO_EAGER_ADOPT -I. $(SRC) \
	  -o build/tomokv-rlcache-nofix $(JELIBS) $(LDLIBS) -lm

# NEGATIVE-CONTROL BUILD for the cross-owner script reservation sub-wave. Identical to the release
# build except that ScriptPhase::Pin arms nothing, so tests/xscript.py counterexample MUST fail
# against it. A detector that cannot report failure proves nothing about the runs that pass.
noreserve:
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -DTOMO_XSCRIPT_NO_RESERVE -I. $(SRC) \
	  -o build/tomokv-noreserve $(JELIBS) $(LDLIBS) -lm

# Server-less unit binaries: the config parser and the flip controller. `make unit` builds and
# runs both (neither boots a server). tests/gate.sh's parser row is the same program.
build/config-parser-test: tests/config_parser_test.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/config_parser_test.cc -o $@
build/flipctl-unit: tests/flipctl_unit.cc src/core/flipctl.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/flipctl_unit.cc src/core/flipctl.cc -o $@
# The fused owner's deferred-reclaim ring (QSBR batches) against a per-entry reference model.
build/read-local-ring-unit: tests/read_local_ring_unit.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/read_local_ring_unit.cc -o $@
build/read-local-write-ring-unit: tests/read_local_write_ring_unit.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/read_local_write_ring_unit.cc -o $@
# The production cross-connection scheduler, with real ROB tasks at both executor capacities.
build/reorder-unit: tests/reorder_unit.cc $(wildcard src/*/*.h) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/reorder_unit.cc -o $@
STORE_REGRESSION_SRC := tests/store_regression.cc src/cmd/t_hash.cc src/cmd/t_hash_ttl.cc
build/store-regression: $(STORE_REGRESSION_SRC) $(wildcard src/*/*.h) $(wildcard src/*/*.inc) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -ffunction-sections -fdata-sections -DTOMO_STORE_REGRESSION_TEST -I. \
	  $(STORE_REGRESSION_SRC) -Wl,--gc-sections -o $@
build/store-regression-sidecar: $(STORE_REGRESSION_SRC) $(wildcard src/*/*.h) $(wildcard src/*/*.inc) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -ffunction-sections -fdata-sections -DTOMO_STORE_REGRESSION_TEST \
	  -DTOMO_TTL_DEADLINE_SIDECAR=1 -I. $(STORE_REGRESSION_SRC) -Wl,--gc-sections -o $@
build/store-regression-tsan: $(STORE_REGRESSION_SRC) $(wildcard src/*/*.h) $(wildcard src/*/*.inc) Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -O1 -fsanitize=thread -fno-omit-frame-pointer -no-pie \
	  -ffunction-sections -fdata-sections -DTOMO_STORE_REGRESSION_TEST -I. \
	  $(STORE_REGRESSION_SRC) -Wl,--gc-sections -o $@
unit: build/config-parser-test build/flipctl-unit build/read-local-ring-unit build/read-local-write-ring-unit build/reorder-unit
	./build/config-parser-test
	./build/flipctl-unit
	./build/read-local-ring-unit
	./build/read-local-write-ring-unit
	./build/reorder-unit

# Deterministic core regressions: the test TU instantiates the real executor/IO methods
# with ASAN/UBSAN and test-only interleaving hooks. No server or ring is started.
CORE_TEST_OBJ := $(filter-out build/src/main.o build/src/core/genthread.o,$(OBJ))
build/core-concurrency-unit: tests/core_concurrency_unit.cc $(CORE_TEST_OBJ) $(wildcard src/*/*.h)
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -O1 -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -DTOMO_CORE_CONCURRENCY_TEST -I. $< $(CORE_TEST_OBJ) -o $@ \
	  $(JELIBS) $(LDLIBS) -lm

# Directed owner-phase tests. The test includes xshard.cc to drive the real private phases
# without starting worker threads or opening a listener; all other code is the release objects.
build/atomic-survivors-unit: tests/atomic_survivors_unit.cc src/cmd/xshard.cc $(filter-out build/src/main.o build/src/cmd/xshard.o,$(OBJ)) $(wildcard src/*/*.inc) $(wildcard src/*/*.h) Makefile
	$(CXX) $(CXXFLAGS) $(JEFLAGS) -I. tests/atomic_survivors_unit.cc \
	  $(filter-out build/src/main.o build/src/cmd/xshard.o,$(OBJ)) -o $@ $(JELIBS) $(LDLIBS) -lm

# Load drivers: not part of `all`, kept compiling here so they cannot rot unnoticed.
build/benchtxn: tools/benchtxn.cc Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tools/benchtxn.cc -o $@
build/broaden-bench: tests/broaden_bench.cc Makefile
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -I. tests/broaden_bench.cc -o $@
tools: build/benchtxn build/broaden-bench

clean:
	rm -rf build
.PHONY: all asan tsan noreserve clean unit tools
