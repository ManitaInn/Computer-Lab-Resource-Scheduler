# Computer Lab Resource Scheduler
#   make          build the web server  ->  ./lab_scheduler
#   make test     build and run the engine tests
#   make run      build and start the server on http://127.0.0.1:8080
#
# Windows (MinGW-w64 / MSYS2) users: build with
#   g++ -std=c++17 -O2 -pthread src/main.cpp -o lab_scheduler.exe -lws2_32 -lwsock32

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
SERVER_SRC = src/main.cpp
HEADERS    = src/scheduler.hpp src/storage.hpp

lab_scheduler: $(SERVER_SRC) $(HEADERS) third_party/httplib.h third_party/json.hpp
	$(CXX) $(CXXFLAGS) -Ithird_party $(SERVER_SRC) -o $@

test_scheduler: tests/test_scheduler.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) tests/test_scheduler.cpp -o $@

test: test_scheduler
	./test_scheduler

run: lab_scheduler
	./lab_scheduler

clean:
	rm -f lab_scheduler test_scheduler lab_scheduler.exe

.PHONY: test run clean
