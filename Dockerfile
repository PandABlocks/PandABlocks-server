# The devcontainer should use the developer target and run as root with podman
# or docker with user namespaces.
FROM ghcr.io/diamondlightsource/ubuntu-devcontainer:noble AS developer

# Add any system dependencies for the developer/build environment here.
# Candidates: an ARMv7-A cross-compiler toolchain for on-PandA builds; the
# native toolchain below is enough for the simulation server and docs.
RUN apt-get update -y && apt-get install -y --no-install-recommends \
    build-essential \
    python3 \
    && apt-get dist-clean
