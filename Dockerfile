# The devcontainer should use the developer target and run as root with podman
# or docker with user namespaces.
FROM ghcr.io/diamondlightsource/ubuntu-devcontainer:resolute AS developer

# Add any system dependencies for the developer/build environment here.
# Candidates: an ARMv7-A cross-compiler toolchain for on-PandA builds; the
# native toolchain below is enough for the simulation server and docs.
#   npm:      drives npx mystmd for the docs build
#   valgrind: the test_configs regression tests run sim_server under valgrind
#             (mirrors the apt install in .github/workflows/_test.yml so `make
#             tests` works the same locally in this container as it does in CI)
RUN apt-get update -y && apt-get install -y --no-install-recommends \
    npm \
    valgrind \
    && apt-get dist-clean
