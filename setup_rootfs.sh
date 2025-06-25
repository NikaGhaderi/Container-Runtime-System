#!/bin/bash
set -e

# The name of our read-only base image directory
IMAGE_DIR="ubuntu-base-image"

# Only build the image if it doesn't already exist
if [ -d "$IMAGE_DIR" ]; then
    echo "--> Base image '${IMAGE_DIR}' already exists. Skipping setup."
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

# Function to copy a binary and its dependencies into the image
copy_binary_with_deps() {
    local binary_path="$1"
    if [ ! -f "$binary_path" ]; then
        echo "--> WARNING: Command not found on host: $binary_path"
        return
    fi
    echo "--> Copying binary: $binary_path"
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

# Loop through our list of commands and copy each one into the image
for cmd in "${COMMANDS[@]}"; do
    copy_binary_with_deps "$cmd"
done

echo "--> Base image setup complete."