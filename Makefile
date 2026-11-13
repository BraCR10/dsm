CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -Werror -pthread
CPPFLAGS += -Icore/include -Icore/src -isystem core/third_party -MMD -MP
LDLIBS += -lcrypto -pthread

BUILD_DIR := build
LIBRARY := $(BUILD_DIR)/libdsm.a

LIBRARY_SOURCES := $(shell find core/src -name '*.cpp')
LIBRARY_OBJECTS := $(LIBRARY_SOURCES:%.cpp=$(BUILD_DIR)/%.o)

TEST_SOURCES := $(wildcard core/tests/*.cpp)
TEST_OBJECTS := $(TEST_SOURCES:%.cpp=$(BUILD_DIR)/%.o)
TEST_BINARY := $(BUILD_DIR)/dsm_tests

NODE_BINARY := $(BUILD_DIR)/dsm_node
NODE_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(wildcard tools/dsm_node/*.cpp))

CLI_BINARY := $(BUILD_DIR)/dsm_cli
CLI_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(wildcard tools/dsm_cli/*.cpp))

DEMO_BINARY := $(BUILD_DIR)/demo
DEMO_OBJECTS := $(patsubst %.cpp,$(BUILD_DIR)/%.o,$(wildcard apps/demo/*.cpp))

.PHONY: all test clean

all: $(LIBRARY) $(NODE_BINARY) $(CLI_BINARY) $(DEMO_BINARY)

$(LIBRARY): $(LIBRARY_OBJECTS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(TEST_BINARY): $(TEST_OBJECTS) $(LIBRARY)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(NODE_BINARY): $(NODE_OBJECTS) $(LIBRARY)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(CLI_BINARY): $(CLI_OBJECTS) $(LIBRARY)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(DEMO_BINARY): $(DEMO_OBJECTS) $(LIBRARY)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

test: $(TEST_BINARY)
	./$(TEST_BINARY)

clean:
	rm -rf $(BUILD_DIR)

-include $(LIBRARY_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d) $(NODE_OBJECTS:.o=.d) $(CLI_OBJECTS:.o=.d) $(DEMO_OBJECTS:.o=.d)
