# Container Runtime System

## Description
This project is a container runtime system developed in C for the Linux operating system. It operates without a daemon, inspired by the architecture of modern tools like Podman. It leverages core Linux kernel features—including namespaces, control groups (cgroups v2), OverlayFS, and eBPF—to create, manage, and monitor isolated container environments directly from the command line.

## Features
- **Daemonless Architecture**: No central daemon, eliminating a single point of failure.
- **Comprehensive Namespace Isolation**: Utilizes PID, User, Network, Mount, UTS, and IPC namespaces.
- **Resource Management**: Enforces resource limits for CPU, Memory, and I/O using cgroups v2.
- **Copy-on-Write Filesystems**: Uses OverlayFS to create efficient, layered filesystems from a base image.
- **Full Container Lifecycle Management**: A complete CLI to `run`, `list`, `status`, `stop`, `start`, `freeze`, `thaw`, and `rm` containers.
- **Advanced Scheduling**: Supports pinning containers to specific CPU cores with a Round-Robin scheduling policy for performance tuning.
- **Inter-Container Communication**: Allows containers to share an IPC namespace for communication via shared memory.
- **Dynamic Mount Propagation**: Mounts on the host can be dynamically propagated into running containers.
- **eBPF-based Monitoring**: A dedicated tool to trace kernel-level events related to container creation.

## Prerequisites
Before you begin, ensure your system meets the following requirements:
- **Operating System**: A modern Linux distribution (e.g., Ubuntu 22.04 LTS or newer).
- **Kernel Version**: Linux kernel 4.15 or higher.
- **cgroups v2**: The system **must** be configured to use cgroups v2. The setup script will attempt to enable the necessary controllers.
- **Build Tools**: `gcc` and `make`.
- **BCC Tools for eBPF**: The `python3-bcc` package (or equivalent) is required to run the `monitor.py` script. You can typically install it with:
  ```bash
  sudo apt-get update
  sudo apt-get install -y bpfcc-tools python3-bpfcc
- **Test Utilities**: The `stress` utility is required for some tests. You can install it with:
  ```bash
  sudo apt-get update
  sudo apt-get install stress

## Installation and Setup

Follow these steps to get the project running.

**1. Clone the Repository**

```bash
git clone https://github.com/NikaGhaderi/Container-Runtime-System.git
cd Container-Runtime-System
```

**2. Run the Setup Script**
This script will create the base image (`ubuntu-base-image`) required to run containers. It also attempts to enable the necessary cgroup v2 controllers.

```bash
# Run the setup script
bash ./setup_rootfs.sh
```

> **Note:** If you ever want to rebuild the base image from scratch, first remove the old one with `sudo rm -rf ubuntu-base-image` and then run the script again.

**3. Compile the Runtime**
Compile the `main.c` source file to create the `my_runner` executable.

```bash
# The -w flag is used to suppress harmless warnings for a cleaner output
gcc -w -o my_runner main.c
```

You are now ready to run containers\!

## Usage

The container runtime is managed through the `my_runner` executable. All commands require `sudo` privileges due to their interaction with kernel namespaces and cgroups.

### Core Commands

#### `run`

Creates and runs a new container.

**Syntax:**
`sudo ./my_runner run [OPTIONS] <image_name> <command> [args...]`

**Example:**

```bash
# Run a simple container that prints a message
sudo ./my_runner run ubuntu-base-image /bin/echo "Hello from my container!"

# Run a detached container with a memory limit
sudo ./my_runner run --detach --mem 100M ubuntu-base-image /usr/bin/stress -c 1
```

**Options:**
| Flag | Short | Description | Example |
|---|---|---|---|
| `--mem <limit>` | `-m` | Sets a memory limit (e.g., `50M`, `1G`). | `--mem 512M` |
| `--cpu <quota>` | `-C` | Sets a CPU quota (e.g., `20000` for 20%). | `--cpu 50000` |
| `--io-write-bps <limit>` | `-w` | Limits disk write speed in bytes/sec. | `--io-write-bps 1000000` |
| `--io-read-bps <limit>` | `-r` | Limits disk read speed in bytes/sec. | `--io-read-bps 2000000` |
| `--detach` | `-d` | Runs the container in the background. | `--detach` |
| `--pin-cpu` | `-p` | Pins the container to a specific CPU core. | `--pin-cpu` |
| `--share-ipc` | `-i` | Shares the host's IPC namespace. | `--share-ipc` |
| `--propagate-mount <dir>`| `-M` | Propagates host mounts from `<dir>` into the container. | `--propagate-mount /mnt/shared` |

-----

#### `list`

Lists all containers (both running and stopped).

**Syntax:**
`sudo ./my_runner list`

-----

#### `status`

Displays detailed information and resource usage for a specific container.

**Syntax:**
`sudo ./my_runner status <container_pid>`

-----

#### `stop`

Stops a running container by terminating its main process. The container's state is preserved and it can be restarted.

**Syntax:**
`sudo ./my_runner stop <container_pid>`

-----

#### `start`

Restarts a stopped container. It will be assigned a new PID.

**Syntax:**
`sudo ./my_runner start <container_pid>`

-----

#### `rm`

Permanently removes a **stopped** container and all its associated resources (writable layer, state files).

**Syntax:**
`sudo ./my_runner rm <container_pid>`

-----

#### `freeze`

Suspends all processes within a running container without terminating them. The container's state is preserved in memory.

**Syntax:**
`sudo ./my_runner freeze <container_pid>`

-----

#### `thaw`

Resumes a frozen container, allowing it to continue execution from the exact point it was paused.

**Syntax:**
`sudo ./my_runner thaw <container_pid>`

## Monitoring with eBPF

The `monitor.py` script can be used to trace the system calls made by `my_runner` in real-time.

1.  **Start the monitor in one terminal:**
    ```bash
    sudo python3 monitor.py
    ```
2.  **Run container commands in another terminal:**
    ```bash
    sudo ./my_runner run ...
    ```

The first terminal will display the captured `clone` and `mkdir` events and save them to `ebpf_log.txt`.

## Cleanup

A helper script is provided to stop and remove all existing containers, which is useful for resetting the environment.

1.  **Make the script executable (only needs to be done once):**
    ```bash
    chmod +x cleanup.sh
    ```
2.  **Run the script:**
    ```bash
    ./cleanup.sh
    ```

---

## Get in Touch

This project was created by **Nika Ghaderi** as part of a university course on operating systems. Feel free to reach out with any questions, feedback, or suggestions!

**Email:** `nika_ghaderi@yahoo.com`
