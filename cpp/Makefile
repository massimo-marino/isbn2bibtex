# Makefile for isbn2bibtex (C++23) - Ubuntu 24.04 or later.
#
# Build dependencies (see README-cpp.md):
#   sudo apt install build-essential pkg-config libcurl4-openssl-dev nlohmann-json3-dev

PROGRAM   := isbn2bibtex
SRC       := isbn2bibtex.cpp
BUILD_DIR := build
OBJ       := $(BUILD_DIR)/$(PROGRAM).o
BIN       := $(BUILD_DIR)/$(PROGRAM)
DEP       := $(OBJ:.o=.d)

# Installation (GNU conventions; /usr/local/bin is the default place on Ubuntu
# for software that is not installed by the package manager).
PREFIX    ?= /usr/local
BINDIR    ?= $(PREFIX)/bin
DESTDIR   ?=

CXX       ?= g++
STRIP     ?= strip
CXXSTD    := -std=c++23
CXXFLAGS  ?= -O2 -g0
WARNINGS  := -Wall -Wextra -Wpedantic
CURL_CFLAGS := $(shell pkg-config --cflags libcurl 2>/dev/null)
CURL_LIBS   := $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)
ALL_CXXFLAGS := $(CXXSTD) $(WARNINGS) $(CXXFLAGS) $(CURL_CFLAGS) -MMD -MP
LDLIBS    := $(CURL_LIBS)

.DEFAULT_GOAL := all
.PHONY: all strip clean install uninstall help

all: $(BIN)  ## Build the executable into build/

$(BIN): $(OBJ) | $(BUILD_DIR)
	$(CXX) $(LDFLAGS) $^ $(LDLIBS) -o $@

strip: $(BIN)  ## Strip symbols from the executable in build/
	$(STRIP) --strip-unneeded $(BIN)
	@echo "Stripped $(BIN)"

$(OBJ): $(SRC) | $(BUILD_DIR)
	$(CXX) $(ALL_CXXFLAGS) -c $< -o $@

$(BUILD_DIR):
	mkdir -p $@

install: $(BIN)  ## Install the executable into $(BINDIR)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(PROGRAM)
	@echo "Installed $(DESTDIR)$(BINDIR)/$(PROGRAM)"

uninstall:  ## Remove the executable from $(BINDIR)
	rm -f $(DESTDIR)$(BINDIR)/$(PROGRAM)
	@echo "Removed $(DESTDIR)$(BINDIR)/$(PROGRAM)"

clean:  ## Delete the build/ directory
	rm -rf $(BUILD_DIR)

help:  ## Show this help
	@echo "isbn2bibtex - C++23 build system"
	@echo
	@echo "Usage: make [target] [VARIABLE=value ...]"
	@echo
	@echo "Targets:"
	@echo "  all        Build $(BIN) (default target)"
	@echo "  strip      Build if needed, then strip symbols from $(BIN)"
	@echo "  clean      Remove the $(BUILD_DIR)/ directory (objects and binary)"
	@echo "  install    Build if needed, then install to $(BINDIR) (usually needs sudo)"
	@echo "  uninstall  Remove $(BINDIR)/$(PROGRAM) (usually needs sudo)"
	@echo "  help       Show this message"
	@echo
	@echo "Variables:"
	@echo "  PREFIX=$(PREFIX)		installation prefix (executable goes to PREFIX/bin)"
	@echo "  BINDIR=$(BINDIR)		executable directory (overrides PREFIX)"
	@echo "  DESTDIR=			staging root, for packaging (e.g. DESTDIR=/tmp/stage)"
	@echo "  CXX=$(CXX)			C++ compiler (needs C++23 support: g++ >= 13)"
	@echo "  CXXFLAGS=$(CXXFLAGS)		extra compiler flags"
	@echo "  STRIP=$(STRIP)			strip tool used by 'make strip'"
	@echo
	@echo "Examples:"
	@echo "  make && sudo make install"
	@echo "  make strip && sudo make install"
	@echo "  make install PREFIX=\$$HOME/.local"
	@echo "  sudo make uninstall"

-include $(DEP)
