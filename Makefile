CXXFLAGS ?= -O3 -march=native -std=c++20 -Wall -Wextra -Wno-missing-field-initializers
BIN ?= ./bench

$(BIN): src/bench.cpp $(wildcard include/*.hpp)
	$(CXX) $(CXXFLAGS) -Iinclude $< -o $@

run: $(BIN)
	$(BIN)

# Small dataset; exits non-zero if any structure disagrees with the naive scan.
test: $(BIN)
	$(BIN) 20000 300 2000 > /dev/null && echo "test: OK"

# No local compiler? Build and run inside a throwaway Alpine container.
docker:
	docker run --rm -v "$(CURDIR)":/src -w /src alpine:3 sh -c 'apk add -q g++ make && make run test BIN=/tmp/bench'

clean:
	rm -f bench

.PHONY: run test docker clean
