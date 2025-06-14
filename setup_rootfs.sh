#!/bin/bash

# A script to create a minimal rootfs for our container

# Exit if any command fails
set -e

ROOTFS_DIR="my-container-rootfs"

echo "--> Creating root filesystem at ./${ROOTFS_DIR}"

# Create the directory if it doesn't exist
if [ -d "$ROOTFS_DIR" ]; then
    echo "--> Directory already exists. Clearing it."
    rm -rf "$ROOTOTS_DIR"
fi

mkdir -p "${ROOTFS_DIR}"/{bin,lib,lib64}

echo "--> Copying bash and its libraries"

# Copy bash
cp /bin/bash "${ROOTFS_DIR}/bin/"

# Copy the required libraries
# Use ldd to find the libraries required by bash and copy them over
for lib in $(ldd /bin/bash | grep -o '/lib.*\s'); do
    # Make sure the destination directory exists
    DEST_DIR="${ROOTFS_DIR}$(dirname ${lib})"
    mkdir -p "$DEST_DIR"
    # Copy the library
    cp "${lib}" "${DEST_DIR}/"
done

echo "--> Rootfs setup complete."