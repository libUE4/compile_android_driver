#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/tty.h>
#include <linux/pid.h>
#include <linux/kprobes.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>

#include "server.h"
#include "touch.h"

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 3, 0))
	MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver); 
#endif

static int __init flkernel_init(void) {
    int ret;

    ret = 0;

    ret = init_server();
	if (ret) {
		return ret;
	}
	//pr_info("[FlyBlue] Init server ok\n");

    init_input_dev();

	list_del_init(&__this_module.list); // 摘除链表，/proc/modules 中不可见。
	kobject_del(&THIS_MODULE->mkobj.kobj); // 摘除kobj，/sys/modules/中不可见。

    return ret;
}

static void __exit flkernel_exit(void) {
    exit_server();
    exit_input_dev();
}

module_init(flkernel_init);
module_exit(flkernel_exit);

MODULE_AUTHOR("FlyBlue");
MODULE_VERSION("0.6.0");
MODULE_LICENSE("GPL");