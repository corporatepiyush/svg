# Fuzzing harness for svg2img.
#
# Everything here is black-box: the renderers are linked into a batch runner
# that forks one child per input, so a crash cannot take down the driver.
#
#   make            build ./fuzz_run (sanitizers) and ./one (single file)
#   make seeds      generate the seed corpus
#   make gen        generate structured mutants (needs seeds)
#   make raw        generate raw-byte mutants
#   make feature    generate feature-targeted mutants
#   make grammar    generate XML/reference/numeric grammar mutants
#   make big        generate large-input stress documents and time them
#   make run        run the seed corpus under sanitizers
#   make regress    render the saved regression cases
#   make semantic   run the pixel-oracle semantic tests
#   make limits     run the configurable resource-limit tests
#   make alloc      build+run the allocation-failure injection harness
#   make resource   run the timeout / unbounded-work tests
#   make leak       run the leak checker on a plain-libc build
#   make campaign   generate + run mutants until a reproducible crash is found
#   make clean      remove binaries and generated corpora

CC      ?= cc
CFLAGS  ?= -std=c17 -g -O2
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=all
ROOT    := $(abspath ..)
LIB     := $(ROOT)/libs
SRC     := $(LIB)/xml_parse.c $(LIB)/xml_dom.c \
           $(LIB)/path_parse.c $(LIB)/path_util.c \
           $(LIB)/color.c $(LIB)/length.c \
           $(LIB)/svg_idmap.c $(LIB)/svg_transform.c $(LIB)/svg_geometry.c \
           $(LIB)/svg_gradient.c $(LIB)/svg_pattern.c $(LIB)/svg_text.c $(LIB)/svg_style.c \
           $(LIB)/svg_mask.c $(LIB)/svg_marker.c $(LIB)/svg_css.c \
           $(LIB)/svg_filter.c \
           $(LIB)/svg_build.c $(LIB)/svg_refs.c $(LIB)/svg_viewport.c \
           $(LIB)/svg_api.c $(LIB)/svg_alloc.c $(LIB)/svg_thread.c \
           $(LIB)/raster_util.c $(LIB)/raster_canvas.c $(LIB)/raster_blend.c $(LIB)/raster_mask.c $(LIB)/raster_gradient.c $(LIB)/raster_pattern.c $(LIB)/raster_fill.c $(LIB)/raster_stroke.c $(LIB)/raster_shape.c $(LIB)/raster_clip.c $(LIB)/raster_text.c $(LIB)/raster_node.c $(LIB)/raster_image.c $(LIB)/raster_filter.c $(LIB)/raster_filter_gen.c $(LIB)/image_png.c $(LIB)/image_jpeg.c $(LIB)/image_load.c \
           $(LIB)/png_deflate.c $(LIB)/png_write.c \
           $(LIB)/font.c $(LIB)/font_ttf.c
INC     := -I$(LIB)
LIBS    := -lm -lpthread
TMP     ?= $(or $(TMPDIR),/tmp)
WORK    ?= $(or $(SVG2IMG_FUZZ_WORK),$(TMP)/svg2img_fuzz)
PY      ?= python3
# 0 = auto (online CPUs)
JOBS    ?= 0

.PHONY: all seeds gen gen-run raw raw-run feature grammar big run regress semantic limits alloc alloc-run resource resource-quick leak refdiff refdiff_rand campaign clean

all: fuzz_run one

fuzz_run: fuzz_run.c $(SRC) $(LIB)/internal.h
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ fuzz_run.c $(SRC) $(LIBS)

one: $(LIB)/main.c $(LIB)/cli.c $(SRC) $(LIB)/internal.h
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ $(LIB)/main.c $(LIB)/cli.c $(SRC) $(LIBS)

fast: $(LIB)/main.c $(LIB)/cli.c $(SRC)
	$(CC) -std=c17 -O2 $(INC) -o one_fast $(LIB)/main.c $(LIB)/cli.c $(SRC) $(LIBS)

seeds:
	$(PY) gen_seeds.py

gen: fuzz_run
	@test -d "$(WORK)/seeds" || SVG2IMG_FUZZ_WORK="$(WORK)" $(MAKE) seeds
	$(PY) mutate.py "$(WORK)/seeds" "$(WORK)/gen" 3000 1
	@ls "$(WORK)/gen"/*.svg > "$(WORK)/gen.txt"
	@wc -l < "$(WORK)/gen.txt"

raw: fuzz_run
	$(PY) rawfuzz.py "$(WORK)/raw" 4000 3
	@ls "$(WORK)/raw"/*.svg > "$(WORK)/raw.txt"
	@wc -l < "$(WORK)/raw.txt"

gen-run: gen
	./run_parallel.sh "$(WORK)/gen.txt" "$(JOBS)" 2

raw-run: raw
	./run_parallel.sh "$(WORK)/raw.txt" "$(JOBS)" 2

# Feature-targeted corpus: path grammar, presentation attributes, colours,
# transforms, dashes, id/use reference graphs.
feature: fuzz_run
	$(PY) featurfuzz.py "$(WORK)/feat" 6000 5
	@ls "$(WORK)/feat"/*.svg > "$(WORK)/feat.txt"
	@./run_parallel.sh "$(WORK)/feat.txt" "$(JOBS)" 2

# XML / reference-graph / numeric grammar corpus (SVG-only; CSS is ignored).
grammar: fuzz_run
	rm -rf "$(WORK)/grammar"
	$(PY) grammar_fuzz.py "$(WORK)/grammar" 600 3
	@ls "$(WORK)/grammar"/*.svg > "$(WORK)/grammar.txt"
	@./run_parallel.sh "$(WORK)/grammar.txt" "$(JOBS)" 2

# Pixel-oracle semantic tests (includes xfail markers for known gaps).
semantic: $(ROOT)/svg2img
	$(PY) semantic_cases.py $(ROOT)/svg2img

# Configurable resource-limit behaviour.
limits: $(ROOT)/svg2img
	$(PY) limit_test.py $(ROOT)/svg2img

# Allocation-failure injection: builds the library with
# -DSVG2IMG_ALLOC_HOOKS and fails the Nth allocation for every N.
alloc: alloc_run

alloc_run: alloc_run.c $(SRC) $(LIB)/internal.h
	$(CC) $(CFLAGS) $(SAN) -DSVG2IMG_ALLOC_HOOKS $(INC) -o $@ alloc_run.c $(SRC) $(LIBS)

alloc-run: alloc_run
	$(PY) alloc_fail.py $(CURDIR)/alloc_run

# Timeout / unbounded-work tests.
resource: $(ROOT)/svg2img
	$(PY) resource_fuzz.py $(ROOT)/svg2img
resource-quick: $(ROOT)/svg2img
	$(PY) resource_fuzz.py $(ROOT)/svg2img --quick

big: fast
	$(PY) bigfuzz.py "$(WORK)/big"
	SVG2IMG_FUZZ_WORK="$(WORK)" $(PY) bigrun.py ./one_fast 120 "$(JOBS)"

regress: $(ROOT)/svg2img
	$(PY) regress.py "$(ROOT)/svg2img"

# Leak regression tests (macOS leaks / Linux valgrind).
# The leak checker needs a plain-libc build: the default slab allocator
# serves small blocks from its own mmap'd arena, which `leaks` cannot see, so
# a leaked small object would pass silently.
leak/svg2img: $(SRC) $(LIB)/main.c $(LIB)/cli.c $(wildcard $(LIB)/*.h)
	@mkdir -p leak
	$(CC) $(CFLAGS) $(INC) -o $@ $(LIB)/main.c $(LIB)/cli.c $(SRC) $(LIBS)

leak: leak/svg2img
	$(PY) leak_check.py $(abspath leak/svg2img)

# Differential test against WebKit's SVG renderer.  macOS only (qlmanage).
refdiff: $(ROOT)/svg2img
	$(PY) refdiff.py "$(ROOT)/svg2img"

refdiff_rand: $(ROOT)/svg2img
	$(PY) refdiff_rand.py 120 21 "$(ROOT)/svg2img"

run: fuzz_run
	@test -d "$(WORK)/seeds" || SVG2IMG_FUZZ_WORK="$(WORK)" $(MAKE) seeds
	@ls "$(WORK)/seeds"/*.svg > "$(WORK)/seeds.txt"
	./run_parallel.sh "$(WORK)/seeds.txt" "$(JOBS)" 2

campaign: fuzz_run one
	@test -d "$(WORK)/seeds" || SVG2IMG_FUZZ_WORK="$(WORK)" $(MAKE) seeds
	SVG2IMG_FUZZ_WORK="$(WORK)" $(PY) campaign.py 40 400 7 "$(JOBS)"

clean:
	rm -f fuzz_run one one_fast
	rm -rf leak
	rm -rf *.dSYM
	@case "$(WORK)" in */svg2img_fuzz) rm -rf "$(WORK)";; *) echo "not removing $(WORK)";; esac
