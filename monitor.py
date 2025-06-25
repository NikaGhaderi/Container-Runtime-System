#!/usr/bin/python3
from bcc import BPF
import ctypes as ct
import time

# This is our eBPF program written in C.
bpf_text = """
#include <linux/sched.h>
#include <uapi/linux/bpf.h>
// --- THE FIX IS HERE: Using the direct kernel architecture header ---
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
TRACEPOINT_PROBE(raw_syscalls, sys_enter) {
    u64 syscall_id = args->id;
    struct data_t data = {};

    // Now the compiler will know what these __NR_ names mean.
    if (syscall_id == __NR_clone) {
        bpf_probe_read_kernel_str(&data.syscall_name, sizeof(data.syscall_name), "clone");
    } else if (syscall_id == __NR_unshare) {
        bpf_probe_read_kernel_str(&data.syscall_name, sizeof(data.syscall_name), "unshare");
    } else if (syscall_id == __NR_mkdir) {
        bpf_probe_read_kernel_str(&data.syscall_name, sizeof(data.syscall_name), "mkdir");
    } else {
        return 0;
    }

    data.pid = bpf_get_current_pid_tgid() >> 32;
    bpf_get_current_comm(&data.comm, sizeof(data.comm));

    events.perf_submit(args, &data, sizeof(data));
    return 0;
}
"""

# --- The User-Space Python Program ---
# (This part is unchanged)

class Data(ct.Structure):
    _fields_ = [
        ("pid", ct.c_uint),
        ("comm", ct.c_char * 16),
        ("syscall_name", ct.c_char * 32),
    ]

def print_event(cpu, data, size):
    event = ct.cast(data, ct.POINTER(Data)).contents
    current_time = time.strftime("%Y-%m-%d %H:%M:%S")
    comm = event.comm.decode('utf-8', 'replace')
    syscall = event.syscall_name.decode('utf-8', 'replace')
    log_line = f"{current_time} | PID: {event.pid:<7} | COMM: {comm:<15} | SYSCALL: {syscall}\n"
    print(log_line, end="")
    with open("ebpf_log.txt", "a") as f:
        f.write(log_line)

# --- Main program execution ---
print("Starting eBPF monitoring... Press Ctrl+C to exit.")
try:
    b = BPF(text=bpf_text)
    b["events"].open_perf_buffer(print_event)
    while True:
        try:
            b.perf_buffer_poll()
        except KeyboardInterrupt:
            exit()
except Exception as e:
    print(f"Error: {e}")