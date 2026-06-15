# Makefile for amicode - native agentic Claude client for the Amiga 1200.
#
# Toolchain: vbcc (m68k-amigaos), NDK3.2 + Roadshow, AmiSSL v5 SDK.
# Build host: Linux/macOS cross-compile; deploy the resulting m68k binary to
# the Amiga (PCMCIA CF or network).
#
# Override any of these on the command line, e.g.:
#   make VBCC=/path/to/vbcc AMISSL_SDK=/path/to/AmiSSL/Developer
# ---------------------------------------------------------------------------

# vbcc installation (expects $(VBCC)/bin/vc and config/ with the a1200 target).
VBCC       ?= $(HOME)/opt/vbcc

# AmiSSL v5 SDK "Developer" dir (contains include/, lib/, Examples/).
AMISSL_SDK ?= $(HOME)/opt/amissl-sdk/AmiSSL/Developer

# Roadshow / bsdsocket headers shipped with NDK3.2.
NET_INC    ?= $(VBCC)/NDK3.2/SANA+RoadshowTCP-IP/netinclude

VC          = $(VBCC)/bin/vc
TARGET_CFG  = +a1200

# AmiSSL SDK include must precede the partial copy in vbcc/targets (the a1200
# config adds that automatically), so list it first via -I.
INCLUDES    = -I$(AMISSL_SDK)/include -I$(NET_INC) -Isrc -Isrc/json

# -c99 for modern C; warnings on. Link amiga.lib for RangeRand(); mieee.lib for
# the IEEE double math (strtod/fabs/MathIeeeDoubBas) that cJSON's numbers need.
CFLAGS      = $(TARGET_CFG) -c99 -O1 $(INCLUDES)
LDFLAGS     = $(TARGET_CFG) -lmieee -lamiga

BIN         = amicode
SRCDIR      = src
OBJDIR      = build

SOURCES     = $(SRCDIR)/main.c \
              $(SRCDIR)/net.c \
              $(SRCDIR)/api.c \
              $(SRCDIR)/agent.c \
              $(SRCDIR)/tools.c \
              $(SRCDIR)/json/cJSON.c

OBJECTS     = $(patsubst $(SRCDIR)/%.c,$(OBJDIR)/%.o,$(SOURCES))

# Make the vbcc env + tools visible to the vc driver (it execs vbccm68k,
# vasmm68k_mot and vlink by bare name, so they must be on PATH).
export VBCC
export PATH := $(VBCC)/bin:$(PATH)

.PHONY: all clean dirs

all: dirs $(BIN)

dirs:
	@mkdir -p $(OBJDIR)/json

$(OBJDIR)/%.o: $(SRCDIR)/%.c
	$(VC) $(CFLAGS) -c $< -o $@

$(BIN): $(OBJECTS)
	$(VC) $(OBJECTS) $(LDFLAGS) -o $(BIN)

clean:
	rm -rf $(OBJDIR) $(BIN)
