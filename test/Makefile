# Makefile - Unit test harness for Member 1 & Member 2 modules
#
#   make        - build test binaries
#   make test   - run both unit test suites
#   make clean  - remove binaries

CXX      := g++
CXXFLAGS := -O2 -Wall -Wextra -Werror -pthread -std=c++17

SRC_DIR  := ../Part1

all: test_protocol test_mapper

test_protocol: test_protocol.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/protocol.h common_types.h
	$(CXX) $(CXXFLAGS) -I. -I$(SRC_DIR) test_protocol.cpp $(SRC_DIR)/protocol.cpp -o test_protocol

test_mapper: test_mapper.cpp $(SRC_DIR)/directory_mapper.cpp $(SRC_DIR)/concurrency.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/metrics_logger.cpp
	$(CXX) $(CXXFLAGS) -I. -I$(SRC_DIR) test_mapper.cpp $(SRC_DIR)/directory_mapper.cpp $(SRC_DIR)/concurrency.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/metrics_logger.cpp -o test_mapper

test: test_protocol test_mapper
	./test_protocol
	./test_mapper

clean:
	rm -f test_protocol test_mapper test_asan *.o

.PHONY: all test clean
