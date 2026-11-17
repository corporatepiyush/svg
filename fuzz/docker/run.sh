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
for src in libs/*.c; do
    f=$(basename "$src" .c)
    gcc $STRICT -Ilibs -c -o /tmp/$f.o "$src"
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
SRCS=$(find libs -maxdepth 1 -name '*.c' ! -name cli.c ! -name main.c | sort)
gcc -std=c17 -g -O2 $SAN -Ilibs -o /tmp/fuzz_run fuzz/fuzz_run.c $SRCS -lm
gcc -std=c17 -g -O2 $SAN -Ilibs -o /tmp/one $SRCS libs/cli.c libs/main.c -lm

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