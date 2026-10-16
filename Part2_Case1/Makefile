CXX = g++
CXXFLAGS = -O2 -Wall -Wextra -Werror -pthread -std=c++17 -MMD -MP

# Flags for Part 2 Case 1 (Stateless Re-request Recovery)
CASE_FLAGS = -DPART_NUM=2 -DCASE_MODE=1

# Shared utility files that belong to BOTH server and client
SHARED_SRC = $(wildcard protocol.cpp metrics_logger.cpp fault.cpp session.cpp checkpoint.cpp)
SHARED_OBJS = $(patsubst %.cpp,%.o,$(SHARED_SRC))

# Server-specific files
SERVER_SRC = $(wildcard server.cpp directory_mapper.cpp concurrency.cpp enhanced_epoll_server.cpp)
SERVER_OBJS = $(patsubst %.cpp,%.o,$(SERVER_SRC))

# Client-specific files
CLIENT_SRC = $(wildcard client.cpp enhanced_parallel_client.cpp)
CLIENT_OBJS = $(patsubst %.cpp,%.o,$(CLIENT_SRC))

DEPS = $(SHARED_OBJS:.o=.d) $(SERVER_OBJS:.o=.d) $(CLIENT_OBJS:.o=.d)

all: server client

server: $(SERVER_OBJS) $(SHARED_OBJS)
	$(CXX) $(CXXFLAGS) $(CASE_FLAGS) -o $@ $^

client: $(CLIENT_OBJS) $(SHARED_OBJS)
	$(CXX) $(CXXFLAGS) $(CASE_FLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(CASE_FLAGS) -c $< -o $@

-include $(DEPS)

clean:
	rm -f server client *.o *.d

.PHONY: all clean
