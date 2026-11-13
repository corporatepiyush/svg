#!/bin/sh
# Full verification pass inside the container.
set -e
cd /src

echo "== uname =="
uname -m
gcc --version | head -1

echo "== strict -O2 build (warnings are errors) =="
STRICT="-std=c17 -O2 -Wall -Wextra -pedantic -Werror -Wconversion -Wsign-conversion \
-Wshadow -Wcast-qual -Wmissing-prototypes -Wold-style-definition -Wredundant-decls \
-Wwrite-strings"
for f in xml_parse xml_dom path_parse path_util color length \
         svg_idmap svg_transform svg_geometry svg_gradient svg_text svg_style \
         svg_build svg_refs svg_viewport svg_api \
         raster_util raster_canvas raster_blend raster_mask raster_gradient raster_fill raster_stroke raster_shape raster_clip raster_text raster_node \
         png_deflate png_write font cli main; do
    gcc $STRICT -I. -c -o /tmp/$f.o $f.c
done
echo "strict build ok"

echo "== normal build =="
make clean >/dev/null
make -j"$(nproc 2>/dev/null || echo 4)"

echo "== pixel regression suite =="
python3 test/check.py ./svg2img

echo "== saved fuzz regression cases =="
python3 fuzz/regress.py ./svg2img

echo "== invariant / differential tests =="
python3 fuzz/difftest.py ./svg2img

echo "== pixel-oracle semantic tests =="
python3 fuzz/semantic_cases.py ./svg2img

echo "== timeout / unbounded-work tests (quick) =="
python3 fuzz/resource_fuzz.py ./svg2img --quick

echo "== leak regression tests =="
python3 fuzz/leak_check.py ./svg2img

echo "== sanitizer build + seed corpus =="
SAN="-fsanitize=address,undefined -fno-sanitize-recover=all"
SRCS="xml_parse.c xml_dom.c path_parse.c path_util.c color.c length.c \
svg_idmap.c svg_transform.c svg_geometry.c svg_gradient.c svg_text.c svg_style.c \
svg_build.c svg_refs.c svg_viewport.c svg_api.c \
raster_util.c raster_canvas.c raster_blend.c raster_mask.c raster_gradient.c raster_fill.c raster_stroke.c raster_shape.c raster_clip.c raster_text.c raster_node.c \
png_deflate.c png_write.c font.c"
gcc -std=c17 -g -O2 $SAN -I. -o /tmp/fuzz_run fuzz/fuzz_run.c $SRCS -lm
gcc -std=c17 -g -O2 $SAN -I. -o /tmp/one $SRCS cli.c main.c -lm

export SVG2IMG_FUZZ_WORK=/tmp/fuzzwork
JOBS=$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
BIN=/tmp/fuzz_run

python3 fuzz/gen_seeds.py
ls "$SVG2IMG_FUZZ_WORK/seeds"/*.svg > /tmp/seeds.txt
bash fuzz/run_parallel.sh /tmp/seeds.txt "$JOBS" 2

echo "== sanitizer run over structured mutants =="
python3 fuzz/mutate.py "$SVG2IMG_FUZZ_WORK/seeds" "$SVG2IMG_FUZZ_WORK/gen" 300 1
ls "$SVG2IMG_FUZZ_WORK/gen"/*.svg > /tmp/gen.txt
bash fuzz/run_parallel.sh /tmp/gen.txt "$JOBS" 2

echo "== sanitizer run over SVG grammar mutants =="
python3 fuzz/grammar_fuzz.py "$SVG2IMG_FUZZ_WORK/grammar" 300 3
ls "$SVG2IMG_FUZZ_WORK/grammar"/*.svg > /tmp/grammar.txt
bash fuzz/run_parallel.sh /tmp/grammar.txt "$JOBS" 2

echo "ALL CROSS-TESTS PASSED"