# Makefile - Member 2's LOCAL unit test only.
#
# This is NOT one of the four submission Makefiles (those are Member 6's, PR #2,
# and must build exactly "server" and "client"). Nothing in this folder is
# committed in PR #1 - only protocol.h and protocol.cpp go into the four
# code folders.
#
#   make        - build the test binary
#   make test   - build and run it
#   make asan   - build and run under AddressSanitizer + UBSan
#   make modes  - check protocol.cpp compiles under all 4 Makefile defines
#   make clean  - remove binaries

CXX      := g++
CXXFLAGS := -O2 -Wall -Wextra -Werror -pthread -std=c++17

# protocol.h / protocol.cpp are read from Part1/ so this test always exercises
# the exact file that gets committed, never a stale local copy.
SRC_DIR  := ../Part1

all: test_protocol

test_protocol: test_protocol.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/protocol.h common_types.h
	$(CXX) $(CXXFLAGS) -I. -I$(SRC_DIR) test_protocol.cpp $(SRC_DIR)/protocol.cpp -o test_protocol

test: test_protocol
	./test_protocol

asan: test_protocol.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/protocol.h common_types.h
	$(CXX) -g -O1 -Wall -Wextra -Werror -pthread -std=c++17 \
	       -fsanitize=address,undefined -fno-omit-frame-pointer \
	       -I. -I$(SRC_DIR) test_protocol.cpp $(SRC_DIR)/protocol.cpp -o test_asan
	./test_asan

# The four submission Makefiles compile with -DPART_NUM and -DCASE_MODE.
# protocol.cpp must stay warning-free under every combination.
modes:
	@for cfg in "1 0" "2 1" "2 2" "2 3"; do \
	  set -- $$cfg; \
	  if $(CXX) $(CXXFLAGS) -I. -I$(SRC_DIR) -DPART_NUM=$$1 -DCASE_MODE=$$2 \
	       -c $(SRC_DIR)/protocol.cpp -o /tmp/pm_$$1_$$2.o 2>/dev/null; then \
	    echo "  [ OK ] -DPART_NUM=$$1 -DCASE_MODE=$$2"; \
	  else \
	    echo "  [FAIL] -DPART_NUM=$$1 -DCASE_MODE=$$2"; \
	  fi; \
	  rm -f /tmp/pm_$$1_$$2.o; \
	done

clean:
	rm -f test_protocol test_asan *.o

.PHONY: all test asan modes clean
