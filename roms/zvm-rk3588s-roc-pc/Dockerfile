FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
        bc \
        bison \
        build-essential \
        ca-certificates \
        device-tree-compiler \
        flex \
        gcc-aarch64-linux-gnu \
        git \
        libgnutls28-dev \
        libssl-dev \
        make \
        patch \
        python3 \
        python3-dev \
        python3-pyelftools \
        python3-setuptools \
        swig && \
    rm -rf /var/lib/apt/lists/*
