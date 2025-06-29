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
    "/bin/dd" "/usr/bin/free" "/usr/bin/head" "/usr/bin/tail"
    "/usr/bin/stress" "/bin/hostname" "/bin/whoami"
)

# Create the basic directory structure for the image
mkdir -p "${IMAGE_DIR}"/{bin,lib,lib64,usr/bin,proc,tmp,dev}

# Compile shm_writer and shm_reader if not already compiled
if [ ! -f "shm_writer" ] || [ ! -f "shm_reader" ]; then
    echo "--> Compiling shm_writer and shm_reader..."
    cat > shm_writer.c << 'EOF'
#include <sys/shm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#define SHM_KEY 1234
#define SHM_SIZE 1024
int main() {
    int shmid = shmget(SHM_KEY, SHM_SIZE, IPC_CREAT | 0666);
    if (shmid == -1) { perror("shmget"); return 1; }
    char *data = shmat(shmid, NULL, 0);
    if (data == (char *)-1) { perror("shmat"); return 1; }
    strcpy(data, "Hello from writer!");
    printf("Wrote to shared memory: %s\n", data);
    sleep(10); // Wait for reader
    shmdt(data);
    return 0;
}
EOF
    cat > shm_reader.c << 'EOF'
#include <sys/shm.h>
#include <stdio.h>
#include <unistd.h>
int main() {
    int shmid = shmget(1234, 1024, 0666);
    if (shmid == -1) { perror("shmget"); return 1; }
    char *data = shmat(shmid, NULL, 0);
    if (data == (char *)-1) { perror("shmat"); return 1; }
    printf("Read from shared memory: %s\n", data);
    shmdt(data);
    shmctl(shmid, IPC_RMID, NULL); // Clean up
    return 0;
}
EOF
    gcc -o shm_writer shm_writer.c
    gcc -o shm_reader shm_reader.c
fi

copy_binary_with_deps() {
    local binary_path="$1"
    local source_path="$binary_path"
    # If the binary is shm_writer or shm_reader, use the locally compiled version
    if [ "$binary_path" = "shm_writer" ] || [ "$binary_path" = "shm_reader" ]; then
        source_path="./$binary_path"
    fi
    if [ ! -f "$source_path" ]; then
        echo "--> WARNING: Command not found: $source_path"
        return
    fi
    local dest_binary="${IMAGE_DIR}/usr/bin/$binary_path"
    if [ "$binary_path" = "shm_writer" ] || [ "$binary_path" = "shm_reader" ]; then
        dest_binary="${IMAGE_DIR}/usr/bin/$binary_path"
    else
        dest_binary="${IMAGE_DIR}${binary_path}"
    fi
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
# Create /dev/zero (character device, major:1, minor:5)
mknod -m 666 "${IMAGE_DIR}/dev/zero" c 1 5

# Create /dev/sda (block device, major:8, minor:0)
# Note: This assumes /dev/sda is the target device for io.max limits
mknod -m 660 "${IMAGE_DIR}/dev/sda" b 8 0

echo "--> Base image setup complete."
