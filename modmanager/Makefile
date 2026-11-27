# nx2-mods: the mods of a card's ports, put on and taken off on the console itself.
#
#   make             the program for the console (ounce-devkit): ../out/nx2-mods.nro
#   make test        the logic core checked on this PC (a PC g++; with Python, also against tools/mods.py)
#   make preview     the screens as pictures in preview/ (a PC g++, and Python + Pillow)
#   make font        source/font_data.h again, from the pictures in tools/make_font.py
#   make icon        icon.jpg again (Python + Pillow)
#   make clean
#
# Run in devkitPro's own shell, where DEVKITPRO is set:
#   /c/devkitPro/msys2/usr/bin/bash -lc "cd <this folder> && make"

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TARGET   := nx2-mods
TITLE    := nx2-mods
AUTHOR   := Olmectron
VERSION  := 0.1.0
BUILD    := build
NRO      := ../out/$(TARGET).nro

CORE     := json modcore
SCREENS  := gfx ui
SOURCES  := $(CORE) $(SCREENS) main_switch
HEADERS  := $(wildcard source/*.h)

include $(DEVKITPRO)/libounce/switch2_rules

ARCH     := $(SWITCH2_CPU) -mtp=soft -fPIE
INCLUDES := -I$(LIBNX)/include -Isource
CXXFLAGS := -g -O2 -Wall -Wextra -ffunction-sections $(ARCH) -D__SWITCH__ -D__SWITCH2__ $(INCLUDES) -std=gnu++17 -fno-rtti -fno-exceptions
LDFLAGS  := -specs=$(LIBNX)/switch.specs -g $(ARCH) -Wl,-Map,$(BUILD)/$(TARGET).map
LIBS     := -L$(LIBNX)/lib -lnx
TOOLS    := $(DEVKITPRO)/tools/bin
OBJECTS  := $(foreach s,$(SOURCES),$(BUILD)/$(s).o)

# The tests and the previews are built by the PC's own compiler, not the one the rules above set CXX to.
HOSTCXX      ?= g++
HOSTFLAGS    := -std=gnu++17 -O2 -g -Wall -Wextra -Isource
# (devkitPro's shell doesn't have Windows' Python on its path: the usual places are tried)
PYTHON       ?= $(firstword $(wildcard /c/Python313/python.exe /c/Python312/python.exe /usr/bin/python3 /usr/bin/python) python)
REPO         := $(abspath ..)

.PHONY: all clean test preview font icon
all: $(NRO)

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: source/%.cpp $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS)
	$(CXX) $(LDFLAGS) $^ $(LIBS) -o $@

$(BUILD)/$(TARGET).nacp: Makefile | $(BUILD)
	$(TOOLS)/nacptool --create "$(TITLE)" "$(AUTHOR)" "$(VERSION)" $@

$(NRO): $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).nacp icon.jpg
	mkdir -p $(dir $@)
	$(TOOLS)/elf2nro $< $@ --nacp=$(BUILD)/$(TARGET).nacp --icon=icon.jpg

$(BUILD)/host/test_core$(EXE): test/test_core.cpp $(foreach s,$(CORE),source/$(s).cpp) $(HEADERS)
	mkdir -p $(BUILD)/host
	$(HOSTCXX) $(HOSTFLAGS) test/test_core.cpp $(foreach s,$(CORE),source/$(s).cpp) -o $@

test: $(BUILD)/host/test_core$(EXE)
	$< $(BUILD)/host/work --repo "$(REPO)" --python "$(PYTHON)"

$(BUILD)/host/preview$(EXE): preview/preview_main.cpp $(foreach s,$(CORE) $(SCREENS),source/$(s).cpp) $(HEADERS)
	mkdir -p $(BUILD)/host
	$(HOSTCXX) $(HOSTFLAGS) preview/preview_main.cpp $(foreach s,$(CORE) $(SCREENS),source/$(s).cpp) -o $@

preview: $(BUILD)/host/preview$(EXE)
	$< $(BUILD)/host/preview-work $(BUILD)/host/pictures
	$(PYTHON) tools/ppm_to_png.py $(BUILD)/host/pictures preview

font:
	$(PYTHON) tools/make_font.py source/font_data.h

icon:
	$(PYTHON) tools/make_icon.py icon.jpg

clean:
	rm -rf $(BUILD)
