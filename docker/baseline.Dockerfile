# Reproducible baseline build: Ubuntu 24.04, GCC 13, CMake 3.28+, Ninja, Python 3.12.
# Assumes the repository is COPYed in — no clone step.
#
# Build:
#   docker build -f docker/baseline.Dockerfile -t retdec-baseline .
# Run:
#   docker run --rm retdec-baseline

FROM ubuntu:noble

ARG DEBIAN_FRONTEND=noninteractive

# Pin toolchain: noble ships GCC 13, CMake 3.28+, Python 3.12.
RUN apt-get -y update && \
	apt-get install -y --no-install-recommends \
	build-essential \
	g++-13 \
	gcc-13 \
	cmake \
	ninja-build \
	git \
	python3.12 \
	python3.12-venv \
	python-is-python3 \
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
	qt6-base-dev \
	qt6-base-dev-tools \
	libqt6svg6-dev \
	libgl1-mesa-dev \
	libxkbcommon-dev \
	libxcb-cursor0 \
	libxcb-icccm4 \
	libxcb-image0 \
	libxcb-keysyms1 \
	libxcb-render-util0 \
	libxcb-xinerama0 \
	libxcb-xfixes0 \
	&& rm -rf /var/lib/apt/lists/* && \
	update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 && \
	update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-13 100

WORKDIR /retdec
COPY . /retdec

RUN bash scripts/fetch-large-files.sh || \
	bash scripts/fetch-large-files.sh \
		--base-url "https://raw.githubusercontent.com/odin-loki/RetDec-Decompiler/main"

RUN cmake --preset full-linux-release \
		-DRETDEC_ENABLE_CUDA_ACCEL=OFF \
		-DRETDEC_ENABLE_NEURAL=OFF && \
	cmake --build build/linux --parallel "$(nproc)" --target retdec-decompiler && \
	ctest --test-dir build/linux -L unit -R "fileformat|utils|common|config" \
		--output-on-failure -j"$(nproc)" --timeout 300
