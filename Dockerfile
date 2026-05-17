# Multi-stage image: build RetDec with root CMake presets (see docs/BUILD_REFERENCE.md).
# Requires CMake 3.26+ (Ubuntu 24.04 base). Binary dir: build/linux (Ninja).

FROM ubuntu:noble AS builder

RUN useradd -m retdec

RUN apt-get -y update && \
	DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
	build-essential \
	cmake \
	ninja-build \
	git \
	python3 \
	doxygen \
	graphviz \
	upx \
	openssl \
	libssl-dev \
	zlib1g-dev \
	autoconf \
	automake \
	pkg-config \
	m4 \
	libtool \
	perl \
	make \
	python-is-python3 \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /home/retdec
COPY . /home/retdec/retdec
RUN chown -R retdec:retdec /home/retdec/retdec

USER retdec
ENV HOME=/home/retdec
WORKDIR /home/retdec/retdec

RUN cmake --preset core-release \
		-DCMAKE_INSTALL_PREFIX=/home/retdec/retdec-install && \
	cmake --build build/linux -j"$(nproc)" && \
	cmake --install build/linux

FROM ubuntu:noble

RUN useradd -m retdec
WORKDIR /home/retdec
ENV HOME=/home/retdec

RUN apt-get update -y && \
	DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
	openssl graphviz upx python3 \
	&& rm -rf /var/lib/apt/lists/*

USER retdec

COPY --from=builder /home/retdec/retdec-install /retdec-install

ENV PATH=/retdec-install/bin:$PATH
