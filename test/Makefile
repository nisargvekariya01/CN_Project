# Makefile - Unit test harness for Member 1, 2, 3, 4 & 5 modules
#
#   make        - build test binaries
#   make test   - run all unit test suites
#   make clean  - remove binaries

CXX       := g++
CXXFLAGS  := -O2 -Wall -Wextra -Werror -pthread -std=c++17

SRC_DIR   := ../Part1
CASE2_DIR := ../Part2_Case2
ENH_DIR   := ../Part2_Case2_Enhanced

all: test_protocol test_mapper test_fault test_session_ckpt test_enhanced

test_protocol: test_protocol.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/protocol.h common_types.h
	$(CXX) $(CXXFLAGS) -I. -I$(SRC_DIR) test_protocol.cpp $(SRC_DIR)/protocol.cpp -o test_protocol

test_mapper: test_mapper.cpp $(SRC_DIR)/directory_mapper.cpp $(SRC_DIR)/concurrency.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/metrics_logger.cpp
	$(CXX) $(CXXFLAGS) -I. -I$(SRC_DIR) test_mapper.cpp $(SRC_DIR)/directory_mapper.cpp $(SRC_DIR)/concurrency.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/metrics_logger.cpp -o test_mapper

test_fault: test_fault.cpp $(SRC_DIR)/fault.cpp $(SRC_DIR)/client.cpp $(SRC_DIR)/directory_mapper.cpp $(SRC_DIR)/concurrency.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/metrics_logger.cpp
	$(CXX) $(CXXFLAGS) -DUNIT_TEST -I. -I$(SRC_DIR) test_fault.cpp $(SRC_DIR)/fault.cpp $(SRC_DIR)/client.cpp $(SRC_DIR)/directory_mapper.cpp $(SRC_DIR)/concurrency.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/metrics_logger.cpp -o test_fault

test_session_ckpt: test_session_ckpt.cpp $(CASE2_DIR)/session.cpp $(CASE2_DIR)/checkpoint.cpp $(SRC_DIR)/protocol.cpp $(SRC_DIR)/metrics_logger.cpp $(SRC_DIR)/directory_mapper.cpp
	$(CXX) $(CXXFLAGS) -I. -I$(SRC_DIR) -I$(CASE2_DIR) $^ -o $@

test_enhanced: test_enhanced.cpp $(ENH_DIR)/enhanced_epoll_server.cpp $(ENH_DIR)/enhanced_parallel_client.cpp $(ENH_DIR)/concurrency.cpp $(ENH_DIR)/protocol.cpp $(ENH_DIR)/fault.cpp $(ENH_DIR)/metrics_logger.cpp $(ENH_DIR)/directory_mapper.cpp $(ENH_DIR)/session.cpp $(ENH_DIR)/checkpoint.cpp
	$(CXX) $(CXXFLAGS) -I. -I$(SRC_DIR) -I$(ENH_DIR) $^ -o $@

test: test_protocol test_mapper test_fault test_session_ckpt test_enhanced
	./test_protocol
	./test_mapper
	./test_fault
	./test_session_ckpt
	./test_enhanced

clean:
	rm -f test_protocol test_mapper test_fault test_session_ckpt test_enhanced test_asan *.o it305_runtime.log
	rm -rf test_ckpt_dir test_seek_file.bin test_scratch

.PHONY: all test clean
