#!/bin/bash

# A script to create a more complete rootfs for our container
# Exit if any command fails
set -e

ROOTFS_DIR="my-container-rootfs"

# --- List of essential commands to include in the container ---
# This is the main part you might want to edit in the future
# to add more commands.
COMMANDS=(
    "/bin/bash"
    "/bin/ls"
    "/bin/cat"
    "/bin/echo"
    "/bin/ps"
    "/bin/touch"
    "/bin/rm"
    "/bin/mkdir"
    "/bin/mount"
    "/bin/umount"
    "/usr/bin/free"
    "/usr/bin/head"
    "/usr/bin/tail"
)

# --- End of commands list ---


# Function to copy a binary and its library dependencies
copy_binary_with_deps() {
    local binary_path="$1"
    
    if [ ! -f "$binary_path" ]; then
        echo "--> WARNING: Command not found on host system: $binary_path"
        return
    fi
    
    echo "--> Copying binary: $binary_path"
    
    # Copy the binary itself
    local dest_binary="${ROOTFS_DIR}${binary_path}"
    mkdir -p "$(dirname "$dest_binary")"
    cp "$binary_path" "$dest_binary"

    # Copy the required libraries
    for lib in $(ldd "$binary_path" | awk 'NF == 4 {print $3}; NF == 2 {print $1}'); do
        if [ ! -f "$lib" ]; then
            continue # Skip non-existent files like linux-vdso.so
        fi
        
        local dest_lib="${ROOTFS_DIR}${lib}"
        if [ ! -f "$dest_lib" ]; then
            echo "    - Copying library: $lib"
            mkdir -p "$(dirname "$dest_lib")"
            cp "$lib" "$dest_lib"
        fi
    done
}


# --- Main script execution ---

echo "--> Setting up root filesystem at ./${ROOTFS_DIR}"

if [ -d "$ROOTFS_DIR" ]; then
    echo "--> Directory already exists. Recreating it."
    rm -rf "$ROOTFS_DIR"
fi

# Create a basic directory structure
mkdir -p "${ROOTFS_DIR}"/{bin,lib,lib64,usr/bin,proc}
# /proc is needed for mount and ps

# Loop through our list of commands and copy each one
for cmd in "${COMMANDS[@]}"; do
    copy_binary_with_deps "$cmd"
done

echo "--> Rootfs setup complete."
echo "--> You can now run your container."