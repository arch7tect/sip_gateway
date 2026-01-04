FROM ubuntu:24.04 AS builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        ca-certificates \
        git \
        ninja-build \
        libasound2-dev \
        libavcodec-dev \
        libavdevice-dev \
        libavformat-dev \
        libavutil-dev \
        libgnutls28-dev \
        libopencore-amrnb-dev \
        libopencore-amrwb-dev \
        libssl-dev \
        libuuid1 \
        libsdl2-dev \
        libswscale-dev \
        wget && \
    rm -rf /var/lib/apt/lists/*

RUN arch="$(dpkg --print-architecture)" && \
    if [ "$arch" = "arm64" ]; then \
        cmake_arch="aarch64"; \
    else \
        cmake_arch="x86_64"; \
    fi && \
    wget -qO /tmp/cmake.sh "https://github.com/Kitware/CMake/releases/download/v4.1.0/cmake-4.1.0-linux-${cmake_arch}.sh" && \
    sh /tmp/cmake.sh --skip-license --prefix=/usr/local && \
    rm -f /tmp/cmake.sh

WORKDIR /app
COPY . /app

RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build --target pjproject-install && \
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build

FROM ubuntu:24.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        alsa-utils \
        libavcodec60 \
        libavdevice60 \
        libavformat60 \
        libavutil58 \
        libgnutls30 \
        libopencore-amrnb0 \
        libopencore-amrwb0 \
        libsdl2-2.0-0 \
        libssl3 \
        libuuid1 \
        libswscale7 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/build/sip_gateway /usr/local/bin/sip_gateway
COPY --from=builder /app/build/deps/onnxruntime/lib /app/build/deps/onnxruntime/lib

ENV LD_LIBRARY_PATH=/app/build/deps/onnxruntime/lib

ENV SIP_REST_API_PORT=8000
EXPOSE 8000 5060/udp 5060/tcp

CMD ["/usr/local/bin/sip_gateway"]
