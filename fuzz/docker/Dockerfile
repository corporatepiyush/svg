# Cross-build / cross-test image for svg2img on linux/amd64.
#
#   podman build --arch amd64 -t svg2img-test -f fuzz/docker/Dockerfile .
#   podman run --rm --arch amd64 svg2img-test
#
# The image is Debian + gcc + python3: enough for the strict build, the
# regression suite, the invariant tests and the sanitizer batch runner.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        gcc libc6-dev make python3 valgrind \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

CMD ["sh", "fuzz/docker/run.sh"]
