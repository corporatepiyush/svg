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
#   make campaign   generate + run mutants until a reproducible crash is found
#   make clean      remove binaries and generated corpora

CC      ?= cc
CFLAGS  ?= -std=c17 -g -O2
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=all
ROOT    := $(abspath ..)
SRC     := $(ROOT)/xml_parse.c $(ROOT)/xml_dom.c \
           $(ROOT)/path_parse.c $(ROOT)/path_util.c \
           $(ROOT)/color.c $(ROOT)/length.c \
           $(ROOT)/svg_idmap.c $(ROOT)/svg_transform.c $(ROOT)/svg_geometry.c \
           $(ROOT)/svg_gradient.c $(ROOT)/svg_pattern.c $(ROOT)/svg_text.c $(ROOT)/svg_style.c \
           $(ROOT)/svg_mask.c $(ROOT)/svg_marker.c $(ROOT)/svg_css.c \
           $(ROOT)/svg_build.c $(ROOT)/svg_refs.c $(ROOT)/svg_viewport.c \
           $(ROOT)/svg_api.c $(ROOT)/svg_alloc.c \
           $(ROOT)/raster_util.c $(ROOT)/raster_canvas.c $(ROOT)/raster_blend.c $(ROOT)/raster_mask.c $(ROOT)/raster_gradient.c $(ROOT)/raster_pattern.c $(ROOT)/raster_fill.c $(ROOT)/raster_stroke.c $(ROOT)/raster_shape.c $(ROOT)/raster_clip.c $(ROOT)/raster_text.c $(ROOT)/raster_node.c $(ROOT)/raster_image.c $(ROOT)/image_png.c \
           $(ROOT)/png_deflate.c $(ROOT)/png_write.c $(ROOT)/font.c
INC     := -I$(ROOT)
LIBS    := -lm
TMP     ?= $(or $(TMPDIR),/tmp)
WORK    ?= $(or $(SVG2IMG_FUZZ_WORK),$(TMP)/svg2img_fuzz)
PY      ?= python3
# 0 = auto (online CPUs)
JOBS    ?= 0

.PHONY: all seeds gen gen-run raw raw-run feature grammar big run regress semantic limits alloc alloc-run resource resource-quick leak refdiff refdiff_rand campaign clean

all: fuzz_run one

fuzz_run: fuzz_run.c $(SRC) $(ROOT)/internal.h
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ fuzz_run.c $(SRC) $(LIBS)

one: $(ROOT)/main.c $(ROOT)/cli.c $(SRC) $(ROOT)/internal.h
	$(CC) $(CFLAGS) $(SAN) $(INC) -o $@ $(ROOT)/main.c $(ROOT)/cli.c $(SRC) $(LIBS)

fast: $(ROOT)/main.c $(ROOT)/cli.c $(SRC)
	$(CC) -std=c17 -O2 $(INC) -o one_fast $(ROOT)/main.c $(ROOT)/cli.c $(SRC) $(LIBS)

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

alloc_run: alloc_run.c $(SRC) $(ROOT)/internal.h
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
leak: $(ROOT)/svg2img
	$(PY) leak_check.py $(ROOT)/svg2img

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
	rm -rf *.dSYM
	@case "$(WORK)" in */svg2img_fuzz) rm -rf "$(WORK)";; *) echo "not removing $(WORK)";; esac
