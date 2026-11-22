# RobCo CI image: QEMU 10.1.2 (arm, aarch64), the arm-none-eabi toolchain
# and a host gcc for the host unit tests.
# The image tag is derived from this file. See ci/README.md.

ARG UBUNTU=ubuntu:24.04

FROM ${UBUNTU} AS qemu-build
ARG QEMU_VERSION=10.1.2
ARG QEMU_SHA256=9d75f331c1a5cb9b6eb8fd9f64f563ec2eab346c822cb97f8b35cd82d3f11479
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential ca-certificates curl xz-utils pkg-config \
      ninja-build python3 python3-venv \
      libglib2.0-dev libpixman-1-dev libfdt-dev zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN curl -fsSLo qemu.tar.xz "https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz" \
 && echo "${QEMU_SHA256}  qemu.tar.xz" | sha256sum -c - \
 && tar xf qemu.tar.xz \
 && rm qemu.tar.xz
WORKDIR /src/qemu-${QEMU_VERSION}
RUN ./configure \
      --prefix=/usr/local \
      --target-list=arm-softmmu,aarch64-softmmu \
      --without-default-features \
      --enable-fdt=system \
      --enable-pixman \
      --disable-docs \
      --disable-tools \
      --disable-guest-agent \
      --disable-slirp \
      --disable-werror \
 && make -j"$(nproc)" \
 && make install DESTDIR=/out \
 && rm -rf /out/usr/local/share/applications /out/usr/local/share/icons

FROM ${UBUNTU}
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      libglib2.0-0t64 libpixman-1-0 libfdt1 zlib1g \
      gcc-arm-none-eabi binutils-arm-none-eabi \
      gcc libc6-dev \
      make git mtools dosfstools \
      python3 python3-venv python3-pip ca-certificates curl \
 && rm -rf /var/lib/apt/lists/*
COPY --from=qemu-build /out/usr/local/ /usr/local/
RUN qemu-system-arm --version \
 && qemu-system-arm -M help | grep raspi0 \
 && qemu-system-aarch64 -M help | grep raspi3b \
 && printf '%s\n' '{"execute":"qmp_capabilities"}' '{"execute":"query-commands"}' '{"execute":"quit"}' \
      | timeout 20 qemu-system-arm -M raspi0 -display none -S -monitor none -serial null -qmp stdio \
      | grep -q '"screendump"'
