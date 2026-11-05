FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
       build-essential cmake ninja-build gdb git ca-certificates libboost-dev \
    && rm -rf /var/lib/apt/lists/*
