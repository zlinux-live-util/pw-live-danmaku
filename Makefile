# pw-live-danmaku -- single-process C++: live chat -> cairo rendering -> PipeWire video node
# All dependencies are distribution system libraries; no third-party package manager involved.

# gdk-pixbuf-2.0 is needed by the submodule's extras/assetcache (image decoding), which is compiled
# into this tree. The sibling project carries the same dependency for the same reason.
CXX      ?= g++
PKGS     := cairo pangocairo libpipewire-0.3 libcurl openssl libbrotlidec zlib gdk-pixbuf-2.0 glib-2.0
# -O3 -march=native pays off on the per-frame text compositing hot loop. The cost is a binary bound
# to the local instruction set; to run elsewhere or distribute it, use make PORTABLE=1.
# Expressed as one conditional assignment: empty when PORTABLE=1, otherwise -march=native.
ARCHFLAGS := $(if $(filter 1,$(PORTABLE)),,-march=native)

CXXFLAGS ?= -O3 -g -funroll-loops $(ARCHFLAGS)
CXXFLAGS += -std=c++20 -Wall -Wextra $(EXTRA_CXXFLAGS) $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))

TARGET := pw-live-danmaku
SRC    := $(wildcard src/*.cpp)
OBJ    := $(SRC:.cpp=.o)

# The video node output is a git submodule (see "Build from source" in the README). Its sources
# are compiled straight into this project's build tree: one set of compile flags, no ABI to track,
# and no .o files left behind in the submodule directory.
PWNODE_DIR  ?= lib/pw-video-simple-interface
PWNODE_SRC  := $(wildcard $(PWNODE_DIR)/src/*.cpp) $(wildcard $(PWNODE_DIR)/extras/*.cpp)
PWNODE_OBJ  := $(patsubst $(PWNODE_DIR)/%.cpp,build/pwvideo/%.o,$(PWNODE_SRC))
CXXFLAGS    += -I$(PWNODE_DIR)/src -I$(PWNODE_DIR)/extras
OBJ         += $(PWNODE_OBJ)
DEP         := $(OBJ:.o=.d)

ifeq ($(PWNODE_SRC),)
$(error Missing submodule $(PWNODE_DIR): run git submodule update --init --recursive first)
endif

# systemd user service: the unit is a template; @REPO@ / @ARGS@ are substituted at install time
UNIT         := pw-live-danmaku.service
UNIT_DIR     ?= $(HOME)/.config/systemd/user
SERVICE_ARGS ?= --room 1746707149 --node pw-live-danmaku --size 480x1080 --fps 30

.PHONY: all clean run install-service uninstall-service compile-commands test

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

build/pwvideo/%.o: $(PWNODE_DIR)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -c -o $@ $<

# Unit tests: the protocol helpers have no UI, so they are checked directly rather than through the
# picture. Not part of `all`, because they are a development aid rather than a runtime requirement.
# -Isrc because the tests live outside src/ and include its headers by name.
TESTFLAGS := $(CXXFLAGS) -Isrc
test: build/tests/json build/tests/bili
	@build/tests/json && build/tests/bili

build/tests/json: src/json.cpp tests/json_test.cpp
	@mkdir -p build/tests
	$(CXX) $(TESTFLAGS) -o $@ src/json.cpp tests/json_test.cpp

# bili.cpp uses the submodule's HttpClient and the Message model, so both are linked in, and pb.cpp
# because the entry and gift events are protobuf blobs rather than JSON.
build/tests/bili: src/bili.cpp src/json.cpp src/ws.cpp src/message.cpp src/notice.cpp src/pb.cpp \
                  tests/bili_test.cpp $(PWNODE_DIR)/extras/http.cpp \
                  $(PWNODE_DIR)/extras/text.cpp $(PWNODE_DIR)/extras/cairo_util.cpp
	@mkdir -p build/tests
	$(CXX) $(TESTFLAGS) -o $@ src/bili.cpp src/json.cpp src/ws.cpp src/message.cpp src/notice.cpp \
	    src/pb.cpp tests/bili_test.cpp $(PWNODE_DIR)/extras/http.cpp \
	    $(PWNODE_DIR)/extras/text.cpp $(PWNODE_DIR)/extras/cairo_util.cpp $(LDLIBS)

# Verify the video node end to end without OBS: attach as a consumer, land the frames on disk. The
# file is exactly width*height*4 bytes per frame in BGRA.
verify: $(TARGET)
	./$(TARGET) --room $(or $(ROOM),545068) --count 1 --seconds 90 --verbose &
	gst-launch-1.0 -q pipewiresrc target-object=pw-live-danmaku num-buffers=1 ! \
	    video/x-raw,format=BGRA ! filesink location=/tmp/pw-live-danmaku.raw
	ls -l /tmp/pw-live-danmaku.raw

run: $(TARGET)
	./$(TARGET)

# Render the unit and install it into the user systemd directory; does not enable/start it, so the
# machine's existing state is left unchanged
install-service: $(TARGET)
	@mkdir -p $(UNIT_DIR)
	sed -e 's|@REPO@|$(CURDIR)|g' -e 's|@ARGS@|$(SERVICE_ARGS)|g' \
	    $(UNIT) > $(UNIT_DIR)/$(UNIT)
	systemctl --user daemon-reload
	@echo "Installed $(UNIT_DIR)/$(UNIT)"
	@echo "To enable and start: systemctl --user enable --now pw-live-danmaku"
	@echo "After changing arguments: systemctl --user restart pw-live-danmaku"

uninstall-service:
	-systemctl --user disable --now pw-live-danmaku
	rm -f $(UNIT_DIR)/$(UNIT)
	systemctl --user daemon-reload
	@echo "Uninstalled $(UNIT_DIR)/$(UNIT)"

clean:
	rm -f $(OBJ) $(DEP) $(TARGET)
	rm -rf build

# Emit compile_commands.json for editor tooling (clangd, LSP servers). The submodule headers under
# lib/ are only reachable through the -I flags above, so without this database a language server
# cannot resolve "pwvideo.hpp" / "text.hpp" and reports a cascade of phantom errors.
compile-commands:
	@printf '[\n' > compile_commands.json
	@first=1; \
	for f in $(SRC) $(PWNODE_SRC); do \
	  [ $$first -eq 1 ] || printf ',\n' >> compile_commands.json; \
	  first=0; \
	  printf '  {\n    "directory": "%s",\n    "file": "%s",\n    "command": "%s %s -c %s"\n  }' \
	    "$(CURDIR)" "$$f" "$(CXX)" "$(CXXFLAGS)" "$$f" >> compile_commands.json; \
	done; \
	for f in $(wildcard tests/*.cpp); do \
	  printf ',\n' >> compile_commands.json; \
	  printf '  {\n    "directory": "%s",\n    "file": "%s",\n    "command": "%s %s -Isrc -c %s"\n  }' \
	    "$(CURDIR)" "$$f" "$(CXX)" "$(CXXFLAGS)" "$$f" >> compile_commands.json; \
	done; \
	printf '\n]\n' >> compile_commands.json
	@echo "Wrote compile_commands.json"

-include $(DEP)
