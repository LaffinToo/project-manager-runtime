# Compiler settings
CC = tcc
CFLAGS = -Wall -O2

# Installation destination matching your Home directory
PREFIX = $(HOME)/.local
BINDIR = $(PREFIX)/bin

# Target binary name and system library requirements
TARGET = project
LIBS = -larchive

# Default compilation target rule
all: $(TARGET)

$(TARGET): src/project.c
	$(CC) $(CFLAGS) src/project.c -o $(TARGET) $(LIBS)

# Installation rule to copy the program straight to your home path (~/.local/bin)
install: all
	mkdir -p $(BINDIR)
	cp $(TARGET) $(BINDIR)/$(TARGET)
	chmod +x $(BINDIR)/$(TARGET)

# Clean rule to wipe temporary compilation workspace files
clean:
	rm -f $(TARGET)

.PHONY: all install clean
