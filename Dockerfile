# Reproducible development environment with every tool the project uses.
#
#   docker build -t linuxlab .
#   docker run --rm -it --cap-add=SYS_PTRACE linuxlab
#
# SYS_PTRACE is needed for strace and gdb inside the container.
FROM ubuntu:24.04

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential cmake clang clang-format clang-tidy cppcheck \
      valgrind strace gdb python3 procps less \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /linuxlab
COPY . .
RUN make -j"$(nproc)"

CMD ["bash"]
