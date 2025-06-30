#!/bin/bash

# A complete cleanup script to stop and remove all containers managed by my_runner
# and clean up all leftover overlayfs mounts and directories.

# The directory where container state is stored.
STATE_DIR="/run/my_runtime"
EXECUTABLE="./my_runner"

# The directory where all container layers are stored.
LAYERS_DIR="overlay_layers"

# Function to clean up containers
cleanup_containers() {
    # Check if the state directory exists.
    if [ ! -d "$STATE_DIR" ]; then
        echo "State directory ${STATE_DIR} not found. No containers to clean up."
        return
    fi

    # Get a list of all container PIDs from the directory names.
    PIDS=$(ls -1 "$STATE_DIR")

    if [ -z "$PIDS" ]; then
        echo "No containers found to clean up."
        return
    fi

    echo "Found containers with PIDs: $PIDS"
    echo "Stopping and removing all containers..."

    for pid in $PIDS; do
        # Check if the PID is a number to avoid errors with non-directory files.
        if ! [[ "$pid" =~ ^[0-9]+$ ]]; then
            echo "Skipping non-numeric entry: $pid"
            continue
        fi

        echo "--- Cleaning up container $pid ---"

        # Check if the container is running before trying to stop it.
        if [ -d "/proc/$pid" ]; then
            echo "Stopping container $pid..."
            sudo "$EXECUTABLE" stop "$pid"
        else
            echo "Container $pid is already stopped."
        fi

        # Remove the container's resources.
        echo "Removing container $pid..."
        sudo "$EXECUTABLE" rm "$pid"
        echo "---------------------------------"
    done

    echo "Container cleanup complete."
}

# Function to clean up overlay layers
cleanup_overlay_layers() {
    # Check if the overlay_layers directory exists.
    if [ ! -d "$LAYERS_DIR" ]; then
        echo "No overlay_layers directory found. Nothing to clean up."
        return
    fi

    echo "Scanning for stale container layers in '${LAYERS_DIR}'..."

    # Find every subdirectory inside overlay_layers.
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
}

# Execute cleanup functions
cleanup_containers
cleanup_overlay_layers

echo "Complete cleanup process finished."
