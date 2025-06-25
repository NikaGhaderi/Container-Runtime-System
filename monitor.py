#!/usr/bin/python3
from bcc import BPF
import ctypes as ct
import time

# This is our eBPF program written in C.
# It will be compiled on the fly by BCC.
bpf_text = """
#include <linux/sched.h>
#include <uapi/linux/bpf.h>
#include <asm/unistd_64.h>

// The data structure that our BPF program will send to user-space
struct data_t {
    u32 pid;
    char comm[TASK_COMM_LEN];
    char syscall_name[32];
};

// This is the "channel" to send data out on, called "events"
BPF_PERF_OUTPUT(events);

// This function is the core of our BPF program.
// It runs in the kernel every time a syscall is made.
TRACEPOINT_PROBE(raw_syscalls, sys_enter) {
    // Get the ID of the syscall being made
    u64 syscall_id = args->id;

    // A struct to hold our data
    struct data_t data = {};

    // We only care about a few specific syscalls.
    // We check the ID to see if it's one we want to log.
    if (syscall_id == __NR_clone) {
        // The clone() syscall is used to create namespaces
        bpf_probe_read_kernel_str(&data.syscall_name, sizeof(data.syscall_name), "clone");
    } else if (syscall_id == __NR_unshare) {
        // The unshare() syscall is also used to create namespaces
        bpf_probe_read_kernel_str(&data.syscall_name, sizeof(data.syscall_name), "unshare");
    } else if (syscall_id == __NR_mkdir) {
        // The mkdir() syscall is used to create cgroup directories
        bpf_probe_read_kernel_str(&data.syscall_name, sizeof(data.syscall_name), "mkdir");
    } else {
        // If it's not a syscall we care about, exit immediately.
        return 0;
    }

    // If it was one of our target syscalls, populate the rest of the data.
    data.pid = bpf_get_current_pid_tgid() >> 32;
    bpf_get_current_comm(&data.comm, sizeof(data.comm));

    // Submit the data to be sent to our Python program
    events.perf_submit(args, &data, sizeof(data));

    return 0;
}
"""

# --- The User-Space Python Program ---

# Define the C struct in Python to help with decoding the data
class Data(ct.Structure):
    _fields_ = [
        ("pid", ct.c_uint),
        ("comm", ct.c_char * 16), # TASK_COMM_LEN
        ("syscall_name", ct.c_char * 32),
    ]

# The function that will be called for each event from the kernel
def print_event(cpu, data, size):
    # Cast the raw data from the kernel into our Data structure
    event = ct.cast(data, ct.POINTER(Data)).contents
    
    # Get the current time
    current_time = time.strftime("%Y-%m-%d %H:%M:%S")

    # Decode the strings from bytes to a Python string
    comm = event.comm.decode('utf-8', 'replace')
    syscall = event.syscall_name.decode('utf-8', 'replace')

    log_line = f"{current_time} | PID: {event.pid:<7} | COMM: {comm:<15} | SYSCALL: {syscall}\n"

    # Print to the screen and write to a log file
    print(log_line, end="")
    with open("ebpf_log.txt", "a") as f:
        f.write(log_line)


# --- Main program execution ---
print("Starting eBPF monitoring... Press Ctrl+C to exit.")

# 1. Create the BPF object by passing it our C code
b = BPF(text=bpf_text)

# 2. Open the "events" channel (the BPF_PERF_OUTPUT buffer)
#    and tell it to call our `print_event` function for each piece of data.
b["events"].open_perf_buffer(print_event)

# 3. Start polling the buffer for events. This loop runs forever.
while True:
    try:
        b.perf_buffer_poll()
    except KeyboardInterrupt:
        exit()