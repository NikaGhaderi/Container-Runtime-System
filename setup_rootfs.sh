#!/bin/bash

# A script to create a more complete rootfs for our container
# Exit if any command fails
set -e

ROOTFS_DIR="my-container-rootfs"

# --- THE FINAL, ROBUST CLEANUP LOGIC ---
if [ -d "$ROOTFS_DIR" ]; then
    echo "--> Found existing rootfs. Cleaning up any old mounts..."

    # This loop will run as many times as needed to unmount all stacked layers.
    while mountpoint -q "${ROOTFS_DIR}/proc"; do
        echo "--> Unmounting a /proc layer..."
        # Use a forceful, lazy unmount just in case.
        # The '|| true' prevents the script from exiting if umount has a temporary error.
        umount -f -l "${ROOTFS_DIR}/proc" || true
        # Give the kernel a fraction of a second to process the unmount
        sleep 0.1
    done
    echo "--> All /proc layers have been unmounted."
fi
# --- END OF CLEANUP LOGIC ---


echo "--> Setting up fresh root filesystem at ./${ROOTFS_DIR}"

# This rm command should now succeed every time.
if [ -d "$ROOTFS_DIR" ]; then
    echo "--> Clearing old directory."
    rm -rf "$ROOTFS_DIR"
fi

# The list of commands to install in the container.
# Make sure /bin/sleep is in the list.
COMMANDS=(
    "/bin/bash"
    "/bin/ls"
    "/bin/cat"
    "/bin/echo"
    "/bin/ps"
    "/bin/sleep"
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
    local binary_path="$1"
    if [ ! -f "$binary_path" ]; then
        echo "--> WARNING: Command not found on host system: $binary_path"
        return
    fi
    # This function is the same as before, no changes needed here.
    echo "--> Copying binary: $binary_path"
    local dest_binary="${ROOTFS_DIR}${binary_path}"
    mkdir -p "$(dirname "$dest_binary")"
    cp "$binary_path" "$dest_binary"
    for lib in $(ldd "$binary_path" | awk 'NF == 4 {print $3}; NF == 2 {print $1}'); do
        if [ ! -f "$lib" ]; then continue; fi
        local dest_lib="${ROOTFS_DIR}${lib}"
        if [ ! -f "$dest_lib" ]; then
            mkdir -p "$(dirname "$dest_lib")"
            cp "$lib" "${dest_lib}"
        fi
    done
}


# --- Main script execution ---
mkdir -p "${ROOTFS_DIR}"/{bin,lib,lib64,usr/bin,proc}

for cmd in "${COMMANDS[@]}"; do
    copy_binary_with_deps "$cmd"
done

echo "--> Rootfs setup complete."