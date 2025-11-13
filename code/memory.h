#include <linux/types.h>
#include <linux/pid.h>
#include <linux/sched.h>

int is_pid_alive(pid_t pid);

int read_process_memory(pid_t pid, uintptr_t addr, void* buffer, size_t size);

int write_process_memory(pid_t pid, uintptr_t addr, void* buffer, size_t size);