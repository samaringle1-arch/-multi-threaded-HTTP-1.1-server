# Makefile — Multi-threaded HTTP/1.1 server (Week 5 — complete)

CC     = gcc
CFLAGS = -Wall -Wextra -g -fsanitize=address,undefined
LFLAGS = -lpthread

# ---- All source files -----------------------------------------------
SERVER_SRC = server.c http_parser.c http_response.c \
             file_server.c thread_pool.c \
             keep_alive.c chunked.c config.c

HEADERS    = server.h http_parser.h http_response.h \
             file_server.h thread_pool.h \
             keep_alive.h chunked.h config.h

# ---- Test suites ----------------------------------------------------
TEST2_SRC  = test_parser.c http_parser.c
TEST3_SRC  = test_thread_pool.c thread_pool.c http_parser.c
TEST4_SRC  = test_file_server.c file_server.c http_response.c http_parser.c
TEST5_SRC  = test_keep_alive.c keep_alive.c chunked.c \
             http_parser.c http_response.c file_server.c

.PHONY: all test test_parser test_pool test_files test_ka \
        run clean helgrind fuzz

# ---- Main targets ---------------------------------------------------
all: server

server: $(SERVER_SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(SERVER_SRC) -o server $(LFLAGS)
	@echo ""
	@echo "  Built → ./server"
	@echo "  Run  : make run"
	@echo "  Test : make test"
	@echo "  Help : ./server --help"
	@echo ""

# ---- Test targets ---------------------------------------------------
test_parser: $(TEST2_SRC) http_parser.h
	$(CC) $(CFLAGS) $(TEST2_SRC) -o test_parser
	./test_parser

test_pool: $(TEST3_SRC) thread_pool.h server.h http_parser.h
	$(CC) $(CFLAGS) $(TEST3_SRC) -o test_pool $(LFLAGS)
	./test_pool

test_files: $(TEST4_SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(TEST4_SRC) -o test_files $(LFLAGS)
	./test_files

test_ka: $(TEST5_SRC) $(HEADERS)
	$(CC) $(CFLAGS) $(TEST5_SRC) -o test_ka $(LFLAGS)
	./test_ka

# Run all test suites in sequence
test: test_parser test_pool test_files test_ka
	@echo ""
	@echo "  All test suites passed ✓"
	@echo ""

# ---- Run ------------------------------------------------------------
run: server
	./server --port 8080 --root ./www --threads 4 --timeout 30

# ---- Debug / analysis -----------------------------------------------
# Race condition check — most important for Week 3
helgrind: CFLAGS = -Wall -Wextra -g
helgrind: $(TEST3_SRC) thread_pool.h server.h
	$(CC) $(CFLAGS) $(TEST3_SRC) -o test_pool $(LFLAGS)
	valgrind --tool=helgrind ./test_pool

# AFL fuzzer for the HTTP parser
fuzz: CFLAGS = -Wall -Wextra -g
fuzz: fuzz_parser.c http_parser.c http_parser.h
	afl-gcc $(CFLAGS) fuzz_parser.c http_parser.c -o fuzz_parser
	@echo "Run: mkdir -p fuzz_in && echo 'GET / HTTP/1.1\r\n\r\n' > fuzz_in/seed"
	@echo "     afl-fuzz -i fuzz_in -o fuzz_out -- ./fuzz_parser"

# ---- Clean ----------------------------------------------------------
clean:
	rm -f server test_parser test_pool test_files test_ka fuzz_parser