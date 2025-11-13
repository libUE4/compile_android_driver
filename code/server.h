//
// Created by fuqiuluo on 25-2-3.
//

#ifndef FLYBLUE_SERVER_H
#define FLYBLUE_SERVER_H

#include <linux/completion.h>
#include <linux/bpf.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/mm.h>
#include <net/sock.h>

enum {
    OP_READ = 1,
    OP_WRITE = 2,
    CMD_TOUCH_CLICK_DOWN = 1000,
    CMD_TOUCH_CLICK_UP = 1001,
    CMD_TOUCH_MOVE = 1006
};

struct touch_event_base {
    int slot;
    int x;
    int y;
    int pressure;
};

struct sock_memory_args {
    pid_t pid;
    uintptr_t addr;
    void *buffer;
    size_t size;
};

int init_server(void);

void exit_server(void);

#endif //FLYBLUE_SERVER_H
