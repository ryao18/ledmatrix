CFLAGS=-Wall -O3 -Wextra -Wno-unused-parameter -march=native -flto
CXXFLAGS=$(CFLAGS)
OBJECTS=demo-main.o minimal-example.o c-example.o text-example.o scrolling-text-example.o clock.o ledcat.o input-example.o pixel-mover.o image-cats.o cats.o
BINARIES=demo minimal-example c-example text-example scrolling-text-example clock ledcat input-example pixel-mover image-cats cats

# Where our library resides. You mostly only need to change the
# RGB_LIB_DISTRIBUTION, this is where the library is checked out.
RGB_LIB_DISTRIBUTION=..
RGB_INCDIR=$(RGB_LIB_DISTRIBUTION)/include
RGB_LIBDIR=$(RGB_LIB_DISTRIBUTION)/lib
RGB_LIBRARY_NAME=rgbmatrix
RGB_LIBRARY=$(RGB_LIBDIR)/lib$(RGB_LIBRARY_NAME).a
LDFLAGS+=-L$(RGB_LIBDIR) -l$(RGB_LIBRARY_NAME) -lrt -lm -lpthread -s -flto

# To compile image-example
MAGICK_CXXFLAGS?=$(shell GraphicsMagick++-config --cppflags --cxxflags)
MAGICK_LDFLAGS?=$(shell GraphicsMagick++-config --ldflags --libs)

all : $(BINARIES)

$(RGB_LIBRARY): FORCE
	$(MAKE) -C $(RGB_LIBDIR)

demo : demo-main.o $(RGB_LIBRARY)
	$(CXX) $< -o $@ $(LDFLAGS)

minimal-example : minimal-example.o
input-example : input-example.o
text-example: text-example.o
scrolling-text-example : scrolling-text-example.o
clock : clock.o
ledcat : ledcat.o
pixel-mover : pixel-mover.o

# All the binaries that have the same name as the object file.q
% : %.o $(RGB_LIBRARY)
	$(CXX) $< -o $@ $(LDFLAGS)

image-example.o : image-example.cc
	$(CXX) -I$(RGB_INCDIR) $(CXXFLAGS) $(MAGICK_CXXFLAGS) -c -o $@ $<

image-example: image-example.o $(RGB_LIBRARY)
	$(CXX) $< -o $@ $(LDFLAGS) $(MAGICK_LDFLAGS)

image-cats.o : image-cats.cc
	$(CXX) -I$(RGB_INCDIR) $(CXXFLAGS) $(MAGICK_CXXFLAGS) -c -o $@ $<

image-cats: image-cats.o $(RGB_LIBRARY)
	$(CXX) $< -o $@ $(LDFLAGS) $(MAGICK_LDFLAGS)

cats.o : cats.cc
	$(CXX) -I$(RGB_INCDIR) $(CXXFLAGS) -std=c++17 $(MAGICK_CXXFLAGS) -c -o $@ $<

cats: cats.o $(RGB_LIBRARY)
	$(CXX) $< -o $@ $(LDFLAGS) $(MAGICK_LDFLAGS) -lcurl -ljsoncpp

# Build the same cats application against an SDL window for desktop development.
PREVIEW_DEPS ?= $(abspath .cats-preview-deps)
PREVIEW_MAGICK_CONFIG = $(or $(wildcard $(PREVIEW_DEPS)/bin/GraphicsMagick++-config),GraphicsMagick++-config)
PREVIEW_JSON_PREFIX = $(if $(wildcard $(PREVIEW_DEPS)/include/json/json.h),$(PREVIEW_DEPS),$(shell brew --prefix jsoncpp))
PREVIEW_SOURCES = cats.cc cats-preview.cc ../lib/bdf-font.cc ../lib/graphics.cc

cats-preview: $(PREVIEW_SOURCES) cats-preview.h ../include/canvas.h ../include/graphics.h ../lib/utf8-internal.h
	$(CXX) -std=c++17 -O2 -Wall -I$(RGB_INCDIR) -I"$(PREVIEW_JSON_PREFIX)/include" \
		-DCATS_PREVIEW -DCATS_FONT_DIR='"$(abspath ../fonts)"' \
		$(shell sdl2-config --cflags) $(shell $(PREVIEW_MAGICK_CONFIG) --cppflags --cxxflags) \
		$(PREVIEW_SOURCES) -o $@ $(shell sdl2-config --libs) \
		$(shell $(PREVIEW_MAGICK_CONFIG) --ldflags --libs) -L"$(PREVIEW_JSON_PREFIX)/lib" -ljsoncpp -lcurl

preview: cats-preview
	./cats-preview ../img/ocean_portrait_blue.png ../img/fungus_small.png

# Since the C example uses the C++ library underneath, which depends on C++
# runtime stuff, you still have to also link -lstdc++
c-example : c-example.o $(RGB_LIBRARY)
	$(CC) $< -o $@ $(LDFLAGS) -lstdc++

%.o : %.cc
	$(CXX) -I$(RGB_INCDIR) $(CXXFLAGS) -c -o $@ $<

%.o : %.c
	$(CC) -I$(RGB_INCDIR) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJECTS) $(BINARIES) cats-preview

FORCE:
.PHONY: FORCE preview
