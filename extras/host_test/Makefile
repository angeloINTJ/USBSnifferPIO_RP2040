# Host-side regression test for src/USBPacketDecoder.cpp (no hardware needed).
#
#   make -C extras/host_test                 # builds against ../../src and runs
#   make -C extras/host_test SRC=/abs/src BUILD=/tmp/build
#
# Needs g++ with AddressSanitizer and UndefinedBehaviorSanitizer (stock on Ubuntu).
SRC      ?= ../../src
BUILD    ?= build
CXX      ?= g++
SANITIZE ?= -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
CXXFLAGS ?= -std=gnu++17 -O1 -g -Wall -Wextra -Werror $(SANITIZE)

TEST := $(BUILD)/decoder_test

.PHONY: test clean
test: $(TEST)
	$(TEST)

$(TEST): decoder_test.cpp $(SRC)/USBPacketDecoder.cpp $(SRC)/USBPacketDecoder.h $(SRC)/USBProtocol.h stubs/Arduino.h stubs/hardware/timer.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -Istubs -I$(SRC) decoder_test.cpp $(SRC)/USBPacketDecoder.cpp -o $@

clean:
	rm -rf $(BUILD)
