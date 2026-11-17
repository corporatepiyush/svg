CC      ?= cc
LIBDIR  ?= libs
# Supported optimization knob.  Keep the language/warning flags separate so
# `make OPTFLAGS='-O3 -flto'` or a PGO flag set composes with the slab define.
OPTFLAGS ?= -O3
CFLAGS  ?= $(OPTFLAGS) -std=c17 -Wall -Wextra -pedantic
CPPFLAGS ?= -I$(LIBDIR)
DEPFLAGS ?= -MMD -MP
LDLIBS  ?= -lm -lpthread

# Small allocations use a thread-local slab by default.  `make ALLOC=libc`
# builds the plain libc allocator instead; sanitizer and fuzz builds use
# their own flags and are never slab-backed.
#
# DOC_ARENA=1 enables the experimental parse/build region allocator.  It was
# measured to be slower and to raise RSS on the corpus, so it is off by
# default and kept only for allocator experiments.
ALLOC ?= slab
DOC_ARENA ?= 0
ifeq ($(ALLOC),slab)
CFLAGS += -DSVG2IMG_SLAB
ifeq ($(DOC_ARENA),1)
CFLAGS += -DSVG2IMG_DOC_ARENA
endif
endif

SRCS = \
	$(LIBDIR)/xml_parse.c $(LIBDIR)/xml_dom.c \
	$(LIBDIR)/path_parse.c $(LIBDIR)/path_util.c \
	$(LIBDIR)/color.c $(LIBDIR)/length.c \
	$(LIBDIR)/svg_idmap.c $(LIBDIR)/svg_transform.c $(LIBDIR)/svg_geometry.c $(LIBDIR)/svg_gradient.c $(LIBDIR)/svg_pattern.c \
	$(LIBDIR)/svg_text.c $(LIBDIR)/svg_style.c $(LIBDIR)/svg_mask.c $(LIBDIR)/svg_marker.c $(LIBDIR)/svg_css.c $(LIBDIR)/svg_filter.c $(LIBDIR)/svg_build.c $(LIBDIR)/svg_refs.c $(LIBDIR)/svg_viewport.c $(LIBDIR)/svg_api.c $(LIBDIR)/svg_alloc.c \
	$(LIBDIR)/raster_util.c \
	$(LIBDIR)/raster_canvas.c \
	$(LIBDIR)/raster_blend.c \
	$(LIBDIR)/raster_mask.c \
	$(LIBDIR)/raster_gradient.c $(LIBDIR)/raster_pattern.c \
	$(LIBDIR)/raster_fill.c \
	$(LIBDIR)/raster_stroke.c \
	$(LIBDIR)/raster_shape.c $(LIBDIR)/image_png.c $(LIBDIR)/image_jpeg.c $(LIBDIR)/image_load.c \
	$(LIBDIR)/raster_clip.c \
	$(LIBDIR)/raster_text.c \
	$(LIBDIR)/raster_node.c $(LIBDIR)/raster_image.c $(LIBDIR)/raster_filter.c $(LIBDIR)/raster_filter_gen.c \
	$(LIBDIR)/png_deflate.c $(LIBDIR)/png_write.c $(LIBDIR)/svg_thread.c \
	$(LIBDIR)/font.c $(LIBDIR)/font_ttf.c $(LIBDIR)/cli.c $(LIBDIR)/main.c

OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)
TARGET = svg2img

.PHONY: all clean test check strict strict-alloc conformance conformance-list baseline diff-external verify-external bench bench-update docker-test

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

-include $(DEPS)

test: $(TARGET)
	./$(TARGET) -v -a 4 test/shapes.svg /tmp/svg2img_shapes.png
	./$(TARGET) -v -a 4 test/gradients.svg /tmp/svg2img_gradients.png
	./$(TARGET) -v -a 4 test/paths.svg /tmp/svg2img_paths.png
	./$(TARGET) -v -a 4 test/text.svg /tmp/svg2img_text.png
	./$(TARGET) -v -a 4 test/edge.svg /tmp/svg2img_edge.png
	./$(TARGET) -v -a 4 test/features.svg /tmp/svg2img_features.png
	./$(TARGET) -v -a 4 test/realworld.svg /tmp/svg2img_realworld.png
	@echo "test PNGs written to /tmp/svg2img_*.png"

# Build and smoke-test the x86_64 Linux tree in Docker.  On an x86_64 host
# this is native; on Apple Silicon it runs through the emulator.
docker-test:
	docker build --platform linux/amd64 --load -t svg2img-linux .
	docker run --rm --platform linux/amd64 svg2img-linux

check: $(TARGET)
	python3 test/check.py ./$(TARGET)
	python3 test/font_test.py ./$(TARGET)
	python3 test/security_test.py ./$(TARGET)

# Spec-derived conformance corpus: shapes, paths, transforms, viewBox, gradients,
# clipPath, opacity, use, text, inline style, units, colours, dashes and joins.
conformance: $(TARGET)
	python3 conformance/run.py --bin ./$(TARGET)

conformance-list:
	python3 conformance/run.py --list

# Visual baseline: capture, then diff a re-render against it.
baseline: $(TARGET)
	python3 tools/baseline.py capture
	python3 tools/baseline.py compare

# Differential test against whichever external renderer is installed locally
# (Chrome/Chromium, Inkscape, resvg, QuickLook).  Skips missing tools cleanly.
diff-external: $(TARGET)
	python3 fuzz/diff_external.py --bin ./$(TARGET)

# Verify every conformance assertion against an independent renderer.  This is
# the check that catches an expectation copied from our own output; it skips
# cleanly when no external renderer is installed.
verify-external: $(TARGET)
	python3 conformance/verify_external.py --backends chrome,inkscape,resvg

# Deterministic benchmark.  Informational by default; BENCH_CHECK=1 makes a
# regression over threshold fail the target.
bench: $(TARGET)
	python3 bench/gen.py
	python3 bench/run.py --bin ./$(TARGET) $(if $(BENCH_CHECK),--check)

bench-update: $(TARGET)
	python3 bench/gen.py
	python3 bench/run.py --bin ./$(TARGET) --update-baseline

STRICT_CFLAGS = -std=c17 -O2 -Wall -Wextra -pedantic -Werror -Wconversion \
	-Wsign-conversion -Wshadow -Wcast-qual -Wmissing-prototypes \
	-Wold-style-definition -Wredundant-decls -Wwrite-strings -I$(LIBDIR)

# Strict build including the optional allocation-hook file.
strict-alloc:
	$(CC) $(STRICT_CFLAGS) -DSVG2IMG_ALLOC_HOOKS -c -o /tmp/svg2img_strict_alloc.o $(LIBDIR)/svg_alloc.c
	@echo "strict alloc build ok"

strict:
	@for f in $(SRCS); do \
		b=$$(basename $$f .c); \
		$(CC) $(STRICT_CFLAGS) -c -o /tmp/svg2img_strict_$$b.o $$f || exit 1; \
	done
	@echo "strict build ok"

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) css.o path.o png.o raster.o svg.o xml.o
	rm -rf conformance/baseline bench/cases
