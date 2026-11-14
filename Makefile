CC      ?= cc
CFLAGS  ?= -O2 -std=c17 -Wall -Wextra -pedantic
DEPFLAGS ?= -MMD -MP
LDLIBS  ?= -lm

# Build the objects in parallel when the platform can report a core count.
NPROC := $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
MAKEFLAGS += -j$(NPROC)

SRCS = \
	xml_parse.c xml_dom.c \
	path_parse.c path_util.c \
	color.c length.c \
	svg_idmap.c svg_transform.c svg_geometry.c svg_gradient.c svg_pattern.c \
	svg_text.c svg_style.c svg_mask.c svg_marker.c svg_css.c svg_filter.c svg_build.c svg_refs.c svg_viewport.c svg_api.c svg_alloc.c \
	raster_util.c \
	raster_canvas.c \
	raster_blend.c \
	raster_mask.c \
	raster_gradient.c raster_pattern.c \
	raster_fill.c \
	raster_stroke.c \
	raster_shape.c image_png.c image_jpeg.c image_load.c \
	raster_clip.c \
	raster_text.c \
	raster_node.c raster_image.c raster_filter.c raster_filter_gen.c \
	png_deflate.c png_write.c \
	font.c cli.c main.c

OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)
TARGET = svg2img

.PHONY: all clean test check strict strict-alloc conformance conformance-list baseline diff-external verify-external bench bench-update

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

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

check: $(TARGET)
	python3 test/check.py ./$(TARGET)

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
	-Wold-style-definition -Wredundant-decls -Wwrite-strings

# Strict build including the optional allocation-hook file.
strict-alloc:
	$(CC) $(STRICT_CFLAGS) -DSVG2IMG_ALLOC_HOOKS -I. -c -o /tmp/svg2img_strict_alloc.o svg_alloc.c
	@echo "strict alloc build ok"

strict:
	@for f in $(SRCS); do \
		$(CC) $(STRICT_CFLAGS) -I. -c -o /tmp/svg2img_strict_$${f%.c}.o $$f || exit 1; \
	done
	@echo "strict build ok"

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) css.o path.o png.o raster.o svg.o xml.o
	rm -rf conformance/baseline bench/cases
