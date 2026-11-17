# Portable Linux build/test image.
#
# On an x86_64 host this is a native build.  On Apple Silicon it can be
# exercised under emulation with:
#   docker build --platform linux/amd64 -t svg2img-linux .
#   docker run --rm --platform linux/amd64 svg2img-linux
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential make python3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# The Makefile selects -j$(nproc) itself; this build is only here to verify
# that the image contains a complete, runnable tree.
RUN make -j"$(nproc)"

CMD ["python3", "test/check.py", "./svg2img"]
