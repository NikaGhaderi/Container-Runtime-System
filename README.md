# Container Runtime System

## Description
This project aims to implement a container runtime system that operates without a daemon, inspired by Podman. It leverages Linux features such as namespaces, cgroups, and eBPF to create isolated environments for running applications and services.

## Objectives
- Understand and implement various Linux namespaces (network, process, user, mount).
- Manage resources using cgroups (CPU, memory, I/O, network).
- Isolate environments using chroot.
- Design and implement a Union filesystem (e.g., overlayfs) for dynamic directory merging.
- Create a container runtime system that allows for the creation, management, and monitoring of containers.
- Interact directly with the Linux kernel and write code that interfaces with it.
- Utilize eBPF for monitoring system calls related to namespace and cgroup creation.

## Features
- Create and manage containers with unique mount, GID, UID, PID, and hostname.
- Support inter-container communication and shared mounts.
- Resource control for CPU, memory, and I/O.
- Monitor system calls related to namespaces and cgroups.
- Implement a simple user interface for managing containers (list, start, run, status).
- Support for freezing and resuming containers using cgroups freezer.
- Create Docker-like images using Union filesystem.

## Getting Started
1. **Clone the repository:**
   ```bash
   git clone https://github.com/NikaGhaderi/Container-Runtime-System.git
