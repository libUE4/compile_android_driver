#include <linux/module.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/uio.h>
#include <net/sock.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/version.h>
#include <linux/list.h>
#include "server.h"
#include "touch.h"
#include "memory.h"

static int my_proto_release(struct socket *sock) {
	struct sock *sk = sock->sk;

	if (!sk) {
		return 0;
	}

	sock_orphan(sk);
	sock_put(sk);

	return 0;
}

static int my_proto_recvmsg(struct socket *sock, struct msghdr *msg, size_t len,
                            int flags)
{
    return -EOPNOTSUPP;
}

static int my_proto_sendmsg(struct socket *sock, struct msghdr *msg, size_t len)
{
	int ret = 0;
    int op_type;
	char *buf = NULL;
    struct sock_memory_args args;
    size_t total_len = iov_iter_count(&msg->msg_iter);  // 获取迭代器总长度

    // 分配缓冲区
    buf = kmalloc(total_len, GFP_KERNEL);
    if (!buf) {
        return -ENOMEM;
    }

    // 使用迭代器API复制数据
    if (copy_from_iter(buf, total_len, &msg->msg_iter) != total_len) {
        ret = -EFAULT;
        goto out;
    }

    // 检查最小长度要求
    if (total_len < sizeof(op_type) + sizeof(args)) {
        ret = -EINVAL;
        goto out;
    }

    // 解析操作类型和参数
    memcpy(&op_type, buf, sizeof(op_type));
    memcpy(&args, buf + sizeof(op_type), sizeof(args));

	// F U C K ACE
	switch (op_type) {
		case OP_READ: {
			ret = read_process_memory(args.pid, args.addr, args.buffer, args.size);
			break;
		}
		case OP_WRITE: {
			ret = write_process_memory(args.pid, args.addr, args.buffer, args.size);
			break;
		}
		default:
			ret = -EINVAL;
			break;
	}

out:
    kfree(buf);
    return ret;
}

int my_proto_ioctl(struct socket * sock, unsigned int cmd, unsigned long arg) {
	struct touch_event_base __user* event_user = (struct touch_event_base __user*) arg;
	struct touch_event_base event;
	int ret;

	if(!event_user) {
		return -EBADR;
	}

	if (copy_from_user(&event, event_user, sizeof(struct touch_event_base))) {
		return -EACCES;
	}

	if (cmd == CMD_TOUCH_CLICK_DOWN) {

        ret = mt_do_down_autoslot(&event);

        if (ret) return ret;

        if (copy_to_user((void __user*)arg, &event, sizeof(event))) return -EFAULT;

        return ret;
    }

	if (cmd == CMD_TOUCH_CLICK_UP) {
        ret = mt_do_up(&event);
        return ret;
    }

    if (cmd == CMD_TOUCH_MOVE) {
        ret = mt_do_move(&event);
        return ret;
    }

	return -ENOTTY;
}

static __poll_t my_proto_poll(struct file *file, struct socket *sock,
						 struct poll_table_struct *wait) {
	return 0;
}

#if(LINUX_VERSION_CODE >= KERNEL_VERSION(5, 6, 64))
static int my_proto_setsockopt(struct socket *sock, int level, int optname,
						  sockptr_t optval, unsigned int optlen)
{
	return -ENOPROTOOPT;
}
#else
static int my_proto_setsockopt(struct socket *sock, int level, int optname,
						  char __user* optval, unsigned int optlen)
{
	return -ENOPROTOOPT;
}
#endif

static int my_proto_getsockopt(struct socket *sock, int level, int optname,
						  char __user *optval, int __user *optlen)
{
	return -EOPNOTSUPP;
}

int my_proto_mmap(struct file *file, struct socket *sock,
				 struct vm_area_struct *vma) {
	return -EOPNOTSUPP;
}

static int free_family = AF_DECnet;

static const struct proto_ops my_proto_ops = {
    .family     = PF_DECnet,
    .owner      = THIS_MODULE,
	.release    = my_proto_release,
    .sendmsg    = my_proto_sendmsg,
    .recvmsg    = my_proto_recvmsg,
	#if(LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 64))
		.bind       = sock_no_bind,
		.connect    = sock_no_connect,
		.getname    = sock_no_getname,
	#endif
	.socketpair = sock_no_socketpair,
	.accept     = sock_no_accept,
	.poll		= my_proto_poll,
	.ioctl		= my_proto_ioctl,
	.shutdown	= sock_no_shutdown,
	.setsockopt	= my_proto_setsockopt,
	.getsockopt	= my_proto_getsockopt,
	.mmap		= my_proto_mmap
};

static struct proto my_proto = {
    .name    = "NET_MEMCPY",
    .owner   = THIS_MODULE,
    .obj_size = sizeof(struct sock),
};

static int my_proto_create(struct net *net, struct socket *sock, int protocol,
                          int kern)
{
	uid_t caller_uid;
    struct sock *sk;
    
	caller_uid = *((uid_t*) &current_cred()->uid);
	if (caller_uid != 0) {
		//pr_warn("[FlyBlue] Only root can create socket!\n");
		return -EAFNOSUPPORT;
	}

    if (sock->type != SOCK_DGRAM)
        return -2033;
    
    sock->ops = &my_proto_ops;
    
    sk = sk_alloc(net, PF_INET, GFP_KERNEL, &my_proto, kern);
    if (!sk)
        return -ENOMEM;
    
    sock_init_data(sock, sk);
    return 0;
}

static struct net_proto_family my_proto_family = {
    .family = PF_DECnet,
    .create = my_proto_create,
    .owner  = THIS_MODULE,
};

static int register_free_family(void) {
	int family;
	int err;
	for(family = free_family; family < NPROTO; family ++) {
		my_proto_family.family = family;
		err = sock_register(&my_proto_family);
		if (err)
			continue;
		else {
			free_family = family;
			//pr_info("[FlyBlue] Find free proto_family: %d\n", free_family);
			return 0;
		}
	}

	//pr_err("[FlyBlue] Can't find any free proto_family!\n");
	return err;
}

int init_server(void) {
	int err;

	err = proto_register(&my_proto, 1);
	if (err)
		goto out;

	err = register_free_family();
	if (err)
		goto out_proto;

	return 0;

	sock_unregister(free_family);
	out_proto:
	proto_unregister(&my_proto);
	out:
	return err;
}

void exit_server(void) {
	sock_unregister(free_family);
	proto_unregister(&my_proto);
}