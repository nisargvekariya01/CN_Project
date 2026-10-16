# Makefile - Unit test harness for Member 1, 2 & 3 modules
#
#   make        - build test binaries
#   make test   - run all unit test suites
#   make clean  - remove binaries

CXX      := g++
CXXFLAGS := -O2 -Wall -Wextra -Werror -pthread -std=c++17

SRC_DIR  := ../Part1

all: test_protocol test_mapper test_fault

test_protocol: test_protocol.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/protocol.h common_types.h
	$(CXX) $(CXXFLAGS) -I. -I$(SRC_DIR) test_protocol.cpp $(SRC_DIR)/protocol.cpp -o test_protocol

test_mapper: test_mapper.cpp $(SRC_DIR)/directory_mapper.cpp $(SRC_DIR)/concurrency.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/metrics_logger.cpp
	$(CXX) $(CXXFLAGS) -I. -I$(SRC_DIR) test_mapper.cpp $(SRC_DIR)/directory_mapper.cpp $(SRC_DIR)/concurrency.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/metrics_logger.cpp -o test_mapper

test_fault: test_fault.cpp $(SRC_DIR)/fault.cpp $(SRC_DIR)/client.cpp $(SRC_DIR)/directory_mapper.cpp $(SRC_DIR)/concurrency.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/metrics_logger.cpp
	$(CXX) $(CXXFLAGS) -DUNIT_TEST -I. -I$(SRC_DIR) test_fault.cpp $(SRC_DIR)/fault.cpp $(SRC_DIR)/client.cpp $(SRC_DIR)/directory_mapper.cpp $(SRC_DIR)/concurrency.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/metrics_logger.cpp -o test_fault

test: test_protocol test_mapper test_fault
	./test_protocol
	./test_mapper
	./test_fault

clean:
	rm -f test_protocol test_mapper test_fault test_asan *.o it305_runtime.log

.PHONY: all test clean
