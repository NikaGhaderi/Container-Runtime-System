#!/bin/bash


#how to use:
#chmod +x cleanup.sh
#sudo ./cleanup.sh

# A robust script to clean up all leftover overlayfs mounts and directories.
# This is useful if the main container program crashes and fails to clean up.

# The directory where all container layers are stored.
LAYERS_DIR="overlay_layers"

# Check if the overlay_layers directory exists.
if [ ! -d "$LAYERS_DIR" ]; then
    echo "No overlay_layers directory found. Nothing to clean up."
    exit 0
fi

echo "Scanning for stale container layers in '${LAYERS_DIR}'..."

# Find every subdirectory inside overlay_layers.
# The '*/' pattern ensures we only get directories.
for layer in ${LAYERS_DIR}/*/; do
    # Check if the item is actually a directory.
    if [ -d "$layer" ]; then
        # Remove the trailing slash to get the clean directory name, e.g., "overlay_layers/1234"
        layer_path=${layer%/}
        echo "--- Found stale layer: ${layer_path} ---"

        # Construct the paths to the nested mount points.
        merged_path="${layer_path}/merged"
        proc_path="${layer_path}/merged/proc"

        # 1. Aggressively unmount the inner /proc directory first.
        # We check if it's a mountpoint before trying to unmount.
        if mountpoint -q "$proc_path"; then
            echo "Unmounting nested procfs: ${proc_path}"
            umount -f -l "$proc_path"
        fi

        # 2. Aggressively unmount the main overlay 'merged' directory.
        if mountpoint -q "$merged_path"; then
            echo "Unmounting overlayfs: ${merged_path}"
            umount -f -l "$merged_path"
        fi

        # 3. Now that mounts are gone, safely remove the entire layer directory.
        echo "Removing directory: ${layer_path}"
        rm -rf "$layer_path"

        echo "Cleanup for ${layer_path} complete."
    fi
done

echo "All stale layers cleaned up."


