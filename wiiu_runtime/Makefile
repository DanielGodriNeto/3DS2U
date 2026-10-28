# Standalone runtime build; the master script installs src/translated_game.cpp.
ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is required)
endif
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC
WUT_ROOT ?= $(DEVKITPRO)/wut

# Use the shell shipped with Windows devkitPro, rather than a PATH-dependent cmd.
ifeq ($(OS),Windows_NT)
SHELL := $(DEVKITPRO)/msys2/usr/bin/sh.exe
.SHELLFLAGS := -c
endif

CXX := $(DEVKITPPC)/bin/powerpc-eabi-g++
ELF2RPL := $(DEVKITPRO)/tools/bin/elf2rpl
BUILD_DIR := ../build/runtime
TARGET := $(BUILD_DIR)/wiiu_runtime
SOURCES := $(wildcard src/*.cpp)
OBJECTS := $(patsubst src/%.cpp,$(BUILD_DIR)/%.o,$(SOURCES))
ARCH := -mcpu=750 -meabi -mhard-float
CPPFLAGS += -Iinclude -I../translator/include -I"$(WUT_ROOT)/include" -specs="$(WUT_ROOT)/share/wut.specs"
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++20 $(ARCH) -Wall -Wextra -Wpedantic -Wconversion -Werror -ffunction-sections -fdata-sections -frounding-math -ffp-contract=off
LDFLAGS += $(ARCH) -specs="$(WUT_ROOT)/share/wut.specs" -L"$(WUT_ROOT)/lib" -Wl,--gc-sections
LDLIBS += -lwut

.PHONY: all
.DELETE_ON_ERROR:
all: $(TARGET).rpx

$(TARGET).rpx: $(TARGET).elf
	"$(ELF2RPL)" "$<" "$@"

$(TARGET).elf: $(OBJECTS) Makefile
	"$(CXX)" $(OBJECTS) $(LDFLAGS) $(LDLIBS) -o "$@"

$(BUILD_DIR)/%.o: src/%.cpp Makefile | $(BUILD_DIR)
	"$(CXX)" $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c "$<" -o "$@"

$(BUILD_DIR):
	mkdir -p "$(BUILD_DIR)"

-include $(OBJECTS:.o=.d)

# Standalone dual-screen test, excluding the translated game's entry and dispatcher.
DISPLAY_OBJECTS := $(filter-out $(BUILD_DIR)/main.o $(BUILD_DIR)/translated_game.o,$(OBJECTS)) $(BUILD_DIR)/display_transfer_demo.o
.PHONY: display-demo
display-demo: ../build/display_transfer_demo.rpx

$(BUILD_DIR)/display_transfer_demo.o: tests/display_transfer_demo.cpp Makefile | $(BUILD_DIR)
	"$(CXX)" $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c "$<" -o "$@"

../build/display_transfer_demo.elf: $(DISPLAY_OBJECTS) Makefile
	"$(CXX)" $(DISPLAY_OBJECTS) $(LDFLAGS) $(LDLIBS) -o "$@"

../build/display_transfer_demo.rpx: ../build/display_transfer_demo.elf
	"$(ELF2RPL)" "$<" "$@"

-include $(BUILD_DIR)/display_transfer_demo.d
