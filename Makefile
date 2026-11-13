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
	svg_idmap.c svg_transform.c svg_geometry.c svg_gradient.c \
	svg_text.c svg_style.c svg_build.c svg_refs.c svg_viewport.c svg_api.c \
	raster_util.c \
	raster_canvas.c \
	raster_blend.c \
	raster_mask.c \
	raster_gradient.c \
	raster_fill.c \
	raster_stroke.c \
	raster_shape.c \
	raster_clip.c \
	raster_text.c \
	raster_node.c \
	png_deflate.c png_write.c \
	font.c cli.c main.c

OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)
TARGET = svg2img

.PHONY: all clean test check strict

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

STRICT_CFLAGS = -std=c17 -O2 -Wall -Wextra -pedantic -Werror -Wconversion \
	-Wsign-conversion -Wshadow -Wcast-qual -Wmissing-prototypes \
	-Wold-style-definition -Wredundant-decls -Wwrite-strings

strict:
	@for f in $(SRCS); do \
		$(CC) $(STRICT_CFLAGS) -I. -c -o /tmp/svg2img_strict_$${f%.c}.o $$f || exit 1; \
	done
	@echo "strict build ok"

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET) css.o path.o png.o raster.o svg.o xml.o