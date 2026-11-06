CXX      := i686-w64-mingw32-g++
TARGET   := build/neufoxWidescreen.asi
VERSION  ?= dev

CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -m32 -msse2 -mfpmath=sse -mstackrealign \
            -fno-exceptions -fno-rtti -fno-asynchronous-unwind-tables \
            -DNWS_VERSION=\"$(VERSION)\"
LDFLAGS  := -shared -static -s -Wl,--enable-stdcall-fixup

SRCS := $(wildcard src/*.cpp) src/thunks.S src/defaults.S
OBJS := $(patsubst src/%,build/obj/%.o,$(SRCS))

all: $(TARGET) build/neufoxWidescreen.ini

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

build/obj/%.cpp.o: src/%.cpp $(wildcard src/*.hpp) | build/obj
	$(CXX) $(CXXFLAGS) -c -o $@ $<

build/obj/%.S.o: src/%.S | build/obj
	$(CXX) -m32 -c -o $@ $<

# The default ini is embedded in the .asi.
build/obj/defaults.S.o: neufoxWidescreen.ini

# Rebuild what shows the version when VERSION changes.
build/obj/version: FORCE | build/obj
	@echo '$(VERSION)' | cmp -s - $@ || echo '$(VERSION)' > $@
build/obj/main.cpp.o: build/obj/version

build/neufoxWidescreen.ini: neufoxWidescreen.ini | build/obj
	cp $< $@

build/obj:
	mkdir -p $@

dist: all
	rm -f build/neufoxWidescreen.zip
	cd build && cp ../README.md ../LICENSE . && zip -q neufoxWidescreen.zip neufoxWidescreen.asi neufoxWidescreen.ini README.md LICENSE && rm README.md LICENSE

clean:
	rm -rf build

.PHONY: all clean dist FORCE
