# .docker/nsjail.Dockerfile

# --- Build Stage ---
# Use the same base image as the final application for GLIBC compatibility
ARG BASE_IMAGE=openjdk:25-jdk-slim
FROM ${BASE_IMAGE} AS builder

# Argument for git repository and branch/tag/commit
ARG NSJAIL_REPO=https://github.com/SageSeekerSociety/nsjail.git
ARG NSJAIL_REF=master # Default to master branch, can be overridden

# Set user to root for installations
USER root

# Install necessary build dependencies for nsjail on this base image
# Combine RUN commands to reduce layers
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    build-essential \
    git \
    pkg-config \
    protobuf-compiler \
    libprotobuf-dev \
    libnl-route-3-dev \
    bison \
    flex \
    libtool \
    autoconf \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Clone the specified reference (branch, tag, or commit)
WORKDIR /usr/local/src
RUN echo "Cloning nsjail from ${NSJAIL_REPO} at ref ${NSJAIL_REF}..." && \
    git clone --depth 1 --branch ${NSJAIL_REF} ${NSJAIL_REPO} nsjail

# Build nsjail
WORKDIR /usr/local/src/nsjail
RUN echo "Building nsjail..." && \
    make -j$(nproc) && \
    echo "Nsjail build completed." && \
    # Optional: Strip the binary to reduce size
    strip nsjail || echo "Strip failed, continuing..."


# --- Final Stage ---
# Use the same base image again for the final image
FROM ${BASE_IMAGE} as final

# Set user to root for installations
USER root

# Install only the essential runtime dependencies for nsjail
# Ensure these match the linked libraries (check with ldd on the compiled binary if unsure)
RUN apt-get update && \
    apt-get install -y --no-install-recommends \
    libc6 \
    libstdc++6 \
    libprotobuf32 \
    libnl-route-3-200 \
    && rm -rf /var/lib/apt/lists/*

# Copy the compiled nsjail binary from the builder stage
COPY --from=builder /usr/local/src/nsjail/nsjail /usr/local/bin/nsjail

# Ensure the binary is executable
RUN chmod +x /usr/local/bin/nsjail

# Set WORKDIR (optional, depends on how image is used later)
WORKDIR /

# Default command (optional, e.g., show version)
# CMD ["nsjail", "--version"]
