# Stage 1: Build Environment
FROM registry.access.redhat.com/ubi9/ubi:latest AS builder
WORKDIR /build

# Install the GNU C++ compiler
RUN dnf install -y gcc-c++

# Copy the source code and compile
COPY poc.cpp .
# -O3 enables compiler optimizations for mathematical workloads
RUN g++ -std=c++11 -O3 poc.cpp -o hpc_app

# Copy the source code and compile
COPY pocadv.cpp .
# -O3 enables compiler optimizations for mathematical workloads
# -ldl explicitly tell the GNU compiler to link the dynamic linker library
RUN g++ -std=c++11 -O3 pocadv.cpp -o hpc_adv_app -ldl -pthread

# Stage 2: Minimal Runtime Environment
FROM registry.access.redhat.com/ubi9/ubi:latest

COPY epel-release-latest-9.noarch.rpm /tmp
COPY stress-ng-0.14.02-1.fc36.x86_64.rpm /tmp
COPY Judy-1.0.5-28.fc36.x86_64.rpm /tmp
RUN ls -la /tmp/*.rpm

# Install the EPEL 9 repository directly via URL (No subscription required)
RUN dnf --disableplugin=subscription-manager install -y /tmp/epel-release-latest-9.noarch.rpm && \
    /usr/bin/crb enable && \
    dnf install -y glibc && \
    dnf install -y glibc-devel && \
    dnf --disableplugin=subscription-manager install -y /tmp/Judy-1.0.5-28.fc36.x86_64.rpm && \
    dnf --disableplugin=subscription-manager install -y /tmp/stress-ng-0.14.02-1.fc36.x86_64.rpm && \
    dnf clean all

WORKDIR /app

# Copy only the compiled binaries from the builder stage
COPY --from=builder /build/hpc_app /app/hpc_app
COPY --from=builder /build/hpc_adv_app /app/hpc_adv_app

# OpenShift enforces strict security and runs containers as arbitrary UIDs by default.
# Setting a non-root user (1001) ensures the container starts without permissions errors.
USER 1001

# Command to execute when the container starts
CMD ["./hpc_app"]

