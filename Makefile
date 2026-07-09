CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Iinclude -pthread
SRC      := src/OrderBook.cpp src/MatchingEngine.cpp
BUILD    := build

.PHONY: all clean test run

all: $(BUILD)/matching_engine

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/matching_engine: $(SRC) src/main.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SRC) src/main.cpp -o $@

$(BUILD)/tests: $(SRC) tests/test_main.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(SRC) tests/test_main.cpp -o $@

test: $(BUILD)/tests
	./$(BUILD)/tests

run: $(BUILD)/matching_engine
	./$(BUILD)/matching_engine

clean:
	rm -rf $(BUILD)
