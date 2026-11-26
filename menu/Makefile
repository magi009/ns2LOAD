# SPDX-License-Identifier: GPL-2.0-or-later
#
# nx2-flow, the menu: a shelf of the games found as NUP files on the card.
#
#   make                 the program for the console (ounce-devkit): out/nx2-flow.nro
#   make preview         a PC program that renders the menu's screens from a pretend card, and
#                        the pictures themselves in out/preview/ (needs a PC g++ and Python + Pillow)
#   make nettest         a PC program that runs the file receiver with a folder for a card, to
#                        try it against a sender with no console
#   make font            source/font_baked.h again (tools/bake_font.py)
#   make clean

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TARGET   := nx2-flow
VERSION  := 0.2.2
OUT      := out
SOURCES  := app covers images library nup transfer main_switch
HEADERS  := $(wildcard source/*.h)

include $(DEVKITPRO)/libounce/switch2_rules
ARCH     := $(SWITCH2_CPU) -mtp=soft -fPIE
DEFINES  := -D__SWITCH__ -D__SWITCH2__
LIBDIRS  := $(PORTLIBS) $(LIBNX) $(DEVKITPRO)/portlibs/switch
LIBS     := -lnx

INCLUDES := $(foreach dir,$(LIBDIRS),-I$(dir)/include) -Isource
LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)
CXXFLAGS := -g -O2 -Wall -ffunction-sections $(ARCH) $(DEFINES) $(INCLUDES) -std=gnu++20 -fno-rtti -fno-exceptions
LDFLAGS  := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,-Map,$(OUT)/$(TARGET).map
TOOLS    := $(DEVKITPRO)/tools/bin
OBJECTS  := $(foreach s,$(SOURCES),$(OUT)/$(s).o)

# The preview is built by the PC's own compiler, which is not the one the rules above set CXX to.
HOSTCXX  ?= g++
PYTHON   ?= python

.PHONY: all clean preview nettest font
all: $(OUT)/$(TARGET).nro

$(OUT):
	mkdir -p $@

$(OUT)/%.o: source/%.cpp $(HEADERS) | $(OUT)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(OUT)/$(TARGET).elf: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LIBPATHS) $(LIBS) -o $@

$(OUT)/$(TARGET).nacp: | $(OUT)
	$(TOOLS)/nacptool --create "nx2-flow" "Olmectron" "$(VERSION)" $@

$(OUT)/$(TARGET).nro: $(OUT)/$(TARGET).elf $(OUT)/$(TARGET).nacp icon.jpg
	$(TOOLS)/elf2nro $< $@ --nacp=$(OUT)/$(TARGET).nacp --icon=icon.jpg

PREVIEW_SOURCES := preview/main.cpp source/app.cpp source/covers.cpp source/images.cpp source/library.cpp source/nup.cpp

$(OUT)/preview/preview$(EXE): $(PREVIEW_SOURCES) $(HEADERS)
	mkdir -p $(OUT)/preview
	$(HOSTCXX) -std=gnu++20 -O2 -Wall -Isource $(PREVIEW_SOURCES) -o $@

preview: $(OUT)/preview/preview$(EXE)
	rm -rf $(OUT)/preview/card $(OUT)/preview/*.ppm $(OUT)/preview/*.png
	$(PYTHON) tools/make_fake_card.py $(OUT)/preview/card
	$(OUT)/preview/preview$(EXE) $(OUT)/preview/card $(OUT)/preview
	$(PYTHON) tools/ppm_to_png.py $(OUT)/preview

# The file receiver itself, as it runs on the console, with a folder for a card.
nettest:
	mkdir -p $(OUT)/preview
	$(HOSTCXX) -std=gnu++20 -O2 -Wall -Isource preview/transfer_test.cpp source/transfer.cpp source/library.cpp source/nup.cpp source/images.cpp -o $(OUT)/preview/transfer_test$(EXE)

font:
	$(PYTHON) tools/bake_font.py

clean:
	rm -rf $(OUT)
