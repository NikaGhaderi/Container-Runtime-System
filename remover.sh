#!/bin/bash

# A script to stop and remove all containers managed by my_runner.

# The directory where container state is stored.
STATE_DIR="/run/my_runtime"
EXECUTABLE="./my_runner"

# Check if the state directory exists.
if [ ! -d "$STATE_DIR" ]; then
    echo "State directory ${STATE_DIR} not found. No containers to clean up."
    exit 0
fi

# Get a list of all container PIDs from the directory names.
# Use `ls -1` to get one entry per line.
PIDS=$(ls -1 "$STATE_DIR")

if [ -z "$PIDS" ]; then
    echo "No containers found to clean up."
    exit 0
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

echo "Cleanup complete."

