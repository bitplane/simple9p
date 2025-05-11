ARG ARCH
FROM alpine:latest
ARG ARCH

# Install build dependencies
RUN apk add --no-cache \
    git \
    make \
    gcc \
    musl-dev \
    linux-headers

WORKDIR /build

# Copy our source files
COPY . ./

# Clone libixp
RUN git clone --depth 1 https://github.com/0intro/libixp.git && \
    make

# serve
ENTRYPOINT ["/build/build/simple9p"]
