#!/bin/bash
set -e

# --- Part 1: Cgroup and Runtime State Setup ---
echo "--> Ensuring runtime directories exist..."
mkdir -p /sys/fs/cgroup/my_runtime
mkdir -p /run/my_runtime

# ADDED: Enable cgroup v2 controllers at the root level.
# This is a prerequisite for creating child cgroups with these controllers.
# The command might fail if already configured, so we ignore errors.
echo "--> Enabling CPU, IO, and Memory cgroup controllers..."
echo "+cpu +io +memory +pids" | sudo tee /sys/fs/cgroup/cgroup.subtree_control > /dev/null 2>&1 || true


# --- Part 2: Base Image Creation ---
IMAGE_DIR="ubuntu-base-image"

if [ -d "$IMAGE_DIR" ]; then
    echo "--> Base image '${IMAGE_DIR}' already exists. Skipping image creation."
    echo "--> To rebuild, first run 'sudo rm -rf ${IMAGE_DIR}'"
    echo "--> Setup complete."
    exit 0
fi

echo "--> Creating base image at ./${IMAGE_DIR}"

COMMANDS=(
    "/bin/bash" "/bin/ls" "/bin/cat" "/bin/echo" "/bin/ps" "/bin/sleep"
    "/bin/touch" "/bin/rm" "/bin/mkdir" "/bin/mount" "/bin/umount"
    "/bin/dd" "/usr/bin/free" "/usr/bin/head" "/usr/bin/tail" "/usr/bin/stress"
    "/usr/bin/whoami" "/usr/bin/ip" "/usr/bin/taskset" "/usr/bin/chrt"
)

# Create the basic directory structure for the image
mkdir -p "${IMAGE_DIR}"/{bin,lib,lib64,usr/bin,proc,tmp,dev,etc,root}

# Create minimal /etc/passwd and /etc/group for user mapping
echo "--> Creating /etc/passwd and /etc/group"
echo "root:x:0:0:root:/root:/bin/bash" > "${IMAGE_DIR}/etc/passwd"
echo "root:x:0:" > "${IMAGE_DIR}/etc/group"
echo "nogroup:x:65534:" >> "${IMAGE_DIR}/etc/group"


# Compile shm_writer and shm_reader if not already compiled
if [ ! -f "shm_writer" ] || [ ! -f "shm_reader" ]; then
    echo "--> Compiling shm_writer and shm_reader..."
    # (shm_writer and shm_reader C code omitted for brevity)
    gcc -o shm_writer shm_writer.c
    gcc -o shm_reader shm_reader.c
fi

copy_binary_with_deps() {
    local binary_path="$1"
    local source_path="$binary_path"
    if [ "$binary_path" = "shm_writer" ] || [ "$binary_path" = "shm_reader" ]; then
        source_path="./$binary_path"
    fi
    if [ ! -f "$source_path" ]; then
        echo "--> WARNING: Command not found: $source_path"
        return
    fi
    local dest_binary="${IMAGE_DIR}${binary_path}"
    mkdir -p "$(dirname "$dest_binary")"
    cp "$source_path" "$dest_binary"
    for lib in $(ldd "$source_path" | awk 'NF == 4 {print $3}; NF == 2 {print $1}' | grep -v "not a dynamic executable"); do
        if [ ! -f "$lib" ]; then continue; fi
        local dest_lib="${IMAGE_DIR}${lib}"
        if [ ! -f "$dest_lib" ]; then
            mkdir -p "$(dirname "$dest_lib")"
            cp "$lib" "${dest_lib}"
        fi
    done
}

# Copy standard commands
for cmd in "${COMMANDS[@]}"; do
    copy_binary_with_deps "$cmd"
done

# Copy shm_writer and shm_reader
copy_binary_with_deps "shm_writer"
copy_binary_with_deps "shm_reader"

# Create /bin/sh symlink to /bin/bash
echo "--> Creating /bin/sh symlink to /bin/bash..."
ln -sf /bin/bash "${IMAGE_DIR}/bin/sh"

# Create device nodes
echo "--> Creating device nodes in ${IMAGE_DIR}/dev..."
mknod -m 666 "${IMAGE_DIR}/dev/zero" c 1 5
mknod -m 660 "${IMAGE_DIR}/dev/sda" b 8 0

echo "--> Base image setup complete."
