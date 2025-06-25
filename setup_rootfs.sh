#!/bin/bash
set -e

# --- Part 1: Cgroup and Runtime State Setup ---
# This part will now run every time to ensure the necessary
# cgroup and state directories exist.

echo "--> Ensuring runtime directories exist..."
mkdir -p /sys/fs/cgroup/my_runtime
mkdir -p /run/my_runtime

# We can also add the robust cleanup logic here for any leftover container rootfs
# that might be using the old name, just in case.
LEGACY_ROOTFS="my-container-rootfs"
if [ -d "$LEGACY_ROOTFS" ]; then
    echo "--> Found old rootfs. Cleaning up old mounts..."
    while mountpoint -q "${LEGACY_ROOTFS}/proc" 2>/dev/null; do
        echo "--> Unmounting a /proc layer from old rootfs..."
        umount -f -l "${LEGACY_ROOTFS}/proc" || true
        sleep 0.1
    done
fi


# --- Part 2: Base Image Creation ---
# This part will still only run if the image is missing.

IMAGE_DIR="ubuntu-base-image"

if [ -d "$IMAGE_DIR" ]; then
    echo "--> Base image '${IMAGE_DIR}' already exists. Skipping image creation."
    echo "--> Setup complete."
    exit 0
fi

echo "--> Creating base image at ./${IMAGE_DIR}"

COMMANDS=(
    "/bin/bash" "/bin/ls" "/bin/cat" "/bin/echo" "/bin/ps" "/bin/sleep"
    "/bin/touch" "/bin/rm" "/bin/mkdir" "/bin/mount" "/bin/umount"
    "/usr/bin/free" "/usr/bin/head" "/usr/bin/tail" "/usr/bin/stress"
)

# Create the basic directory structure for the image
mkdir -p "${IMAGE_DIR}"/{bin,lib,lib64,usr/bin,proc,tmp}

copy_binary_with_deps() {
    local binary_path="$1"
    if [ ! -f "$binary_path" ]; then
        echo "--> WARNING: Command not found on host: $binary_path"
        return
    fi
    local dest_binary="${IMAGE_DIR}${binary_path}"
    mkdir -p "$(dirname "$dest_binary")"
    cp "$binary_path" "$dest_binary"
    for lib in $(ldd "$binary_path" | awk 'NF == 4 {print $3}; NF == 2 {print $1}'); do
        if [ ! -f "$lib" ]; then continue; fi
        local dest_lib="${IMAGE_DIR}${lib}"
        if [ ! -f "$dest_lib" ]; then
            mkdir -p "$(dirname "$dest_lib")"
            cp "$lib" "${dest_lib}"
        fi
    done
}

for cmd in "${COMMANDS[@]}"; do
    copy_binary_with_deps "$cmd"
done

echo "--> Base image setup complete."