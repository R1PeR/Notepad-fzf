# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -DSUPPORT_TRACE_LOG -DLOG_LEVEL=LOG_LEVEL_INFO -DDEBUG -g -std=c11 -O2 -I. -Ilibs/raylib/include
LDFLAGS = -Llibs/raylib/lib -lraylib -lopengl32 -lgdi32 -lwinmm

# Directories
SRC_DIR = src
BUILD_DIR = build
TARGET = notepad.exe

# Source files - automatically find all .c files in src directory
SOURCES = $(wildcard $(SRC_DIR)/*.c) libs/engine/misc/Stopwatch.c libs/engine/misc/Logger.c
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(filter $(SRC_DIR)/%.c,$(SOURCES))) $(BUILD_DIR)/libs/engine/misc/Stopwatch.o $(BUILD_DIR)/libs/engine/misc/Logger.o


# Default target
all: $(TARGET)

# Build target
$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)
	@echo Build complete: $(TARGET)

# Compile source files from src directory
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Compile library files
$(BUILD_DIR)/libs/engine/misc/%.o: libs/engine/misc/%.c | $(BUILD_DIR)/libs/engine/misc
	$(CC) $(CFLAGS) -c $< -o $@

# Create build directories
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/libs/engine/misc:
	mkdir -p $(BUILD_DIR)/libs/engine/misc

# Clean build files
clean:
	rm -rf $(BUILD_DIR) $(TARGET)
	@echo Clean complete

# Rebuild
rebuild: clean all

# Run the application
run: $(TARGET)
	./$(TARGET)

.PHONY: all clean rebuild run
