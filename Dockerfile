FROM ubuntu:24.04 AS builder

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        pkg-config \
        ca-certificates \
        libavcodec-dev \
        libavdevice-dev \
        libavformat-dev \
        libavutil-dev \
        libgnutls28-dev \
        libopencore-amrnb-dev \
        libopencore-amrwb-dev \
        libssl-dev \
        libsdl2-dev \
        libswscale-dev && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
    cmake --build build

FROM ubuntu:24.04

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        ca-certificates \
        libavcodec60 \
        libavdevice60 \
        libavformat60 \
        libavutil58 \
        libgnutls30 \
        libopencore-amrnb0 \
        libopencore-amrwb0 \
        libsdl2-2.0-0 \
        libssl3 \
        libswscale7 && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/build/sip_gateway /usr/local/bin/sip_gateway

ENV SIP_REST_API_PORT=8000
EXPOSE 8000 5060/udp 5060/tcp

CMD ["/usr/local/bin/sip_gateway"]
