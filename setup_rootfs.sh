#!/bin/bash

# A script to create a more complete rootfs for our container
# Exit if any command fails
set -e

ROOTFS_DIR="my-container-rootfs"

# --- NEW & IMPROVED CLEANUP LOGIC ---
if [ -d "$ROOTFS_DIR" ]; then
    echo "--> Found existing rootfs. Cleaning up old mounts..."
    
    # Check if proc is mounted and perform a forced, lazy unmount.
    if mountpoint -q "${ROOTFS_DIR}/proc"; then
        echo "--> Aggressively unmounting old procfs."
        # The -f (force) and -l (lazy) flags are key here.
        # The '|| true' ensures the script doesn't exit if umount fails (e.g., not mounted).
        umount -f -l "${ROOTFS_DIR}/proc" || true
    fi
fi
# --- END OF NEW CLEANUP LOGIC ---


echo "--> Setting up root filesystem at ./${ROOTFS_DIR}"

# This rm command should now succeed
if [ -d "$ROOTFS_DIR" ]; then
    echo "--> Clearing old directory."
    rm -rf "$ROOTFS_DIR"
fi

# The rest of the script is the same as before.
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
    "/usr/bin/stress"
)

# Function to copy a binary and its library dependencies
copy_binary_with_deps() {
    # ... (The function is identical to the previous version) ...
    local binary_path="$1"
    if [ ! -f "$binary_path" ]; then
        echo "--> WARNING: Command not found on host system: $binary_path"
        return
    fi
    echo "--> Copying binary: $binary_path"
    local dest_binary="${ROOTFS_DIR}${binary_path}"
    mkdir -p "$(dirname "$dest_binary")"
    cp "$binary_path" "$dest_binary"
    for lib in $(ldd "$binary_path" | awk 'NF == 4 {print $3}; NF == 2 {print $1}'); do
        if [ ! -f "$lib" ]; then continue; fi
        local dest_lib="${ROOTFS_DIR}${lib}"
        if [ ! -f "$dest_lib" ]; then
            echo "    - Copying library: $lib"
            mkdir -p "$(dirname "$dest_lib")"
            cp "$lib" "$dest_lib"
        fi
    done
}


# --- Main script execution ---
mkdir -p "${ROOTFS_DIR}"/{bin,lib,lib64,usr/bin,proc}

for cmd in "${COMMANDS[@]}"; do
    copy_binary_with_deps "$cmd"
done

echo "--> Rootfs setup complete."