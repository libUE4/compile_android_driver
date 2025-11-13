#include <linux/input.h>
#include <linux/slab.h>
#include <linux/input/mt.h>
#include <linux/uaccess.h>
#include <linux/input-event-codes.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include "touch.h"

static struct input_dev *g_touch_dev;
static struct input_handle *g_handle;

/* 专用工作队列，确保所有注入操作串行执行 */
static struct workqueue_struct *touch_wq;

/* 触摸操作工作项 */
struct touch_work {
    struct work_struct work;
    struct touch_event_base event;
    int operation; /* 0=down, 1=move, 2=up */
};

/* 跟踪注入使用的slots */
static DECLARE_BITMAP(inject_slots, 32);
static DEFINE_SPINLOCK(inject_slots_lock);

/* 统计注入的活跃触点数 */
static atomic_t inj_active = ATOMIC_INIT(0);
static int last_btn = -1;

/* 缓存上一次的触摸点 */
static int lastTouchPos_x = -1;
static int lastTouchPos_y = -1;

static inline struct input_mt *mt_checked(struct input_dev *dev)
{
    return (dev && dev->mt) ? dev->mt : NULL;
}

static inline bool mt_any_active(struct input_dev *dev)
{
    struct input_mt *mt = dev->mt;
    int i;
    if (!mt) return false;
    for (i = 0; i < mt->num_slots; i++) {
        if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) >= 0)
            return true;
    }
    return false;
}

static inline void maybe_emit_btntouch(struct input_dev *dev)
{
    int merged;
    if (!test_bit(BTN_TOUCH, dev->keybit))
        return;

    merged = (atomic_read(&inj_active) > 0) || mt_any_active(dev);

    if (last_btn < 0 || merged != last_btn) {
        input_event(dev, EV_KEY, BTN_TOUCH, merged);
        last_btn = merged;
    }
}

static inline int clamp_abs(struct input_dev *dev, unsigned int code, int v)
{
    if (!dev || !dev->absinfo) return v;
    if (v < dev->absinfo[code].minimum) v = dev->absinfo[code].minimum;
    if (v > dev->absinfo[code].maximum) v = dev->absinfo[code].maximum;
    return v;
}

static int mt_find_free_slot_for_inject(struct input_dev *dev)
{
    struct input_mt *mt = mt_checked(dev);
    unsigned long flags;
    int i;
    
    if (!mt) return -ENODEV;
    
    spin_lock_irqsave(&inject_slots_lock, flags);
    
    for (i = 0; i < mt->num_slots && i < 32; ++i) {
        if (test_bit(i, inject_slots))
            continue;
            
        if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) < 0) {
            set_bit(i, inject_slots);
            spin_unlock_irqrestore(&inject_slots_lock, flags);
            return i;
        }
    }
    
    spin_unlock_irqrestore(&inject_slots_lock, flags);
    return -EBUSY;
}

/* 在工作队列中执行的实际触摸操作 */
static void mt_do_down_work(struct touch_event_base *e)
{
    int slot = e->slot;
    int id;
    struct input_mt *mt;
    unsigned long flags;
    
    if (!g_touch_dev) return;
    
    mt = mt_checked(g_touch_dev);
    if (!mt) return;

    if (slot < 0) {
        slot = mt_find_free_slot_for_inject(g_touch_dev);
        if (slot < 0) return;
    } else if (slot >= mt->num_slots || slot >= 32) {
        return;
    } else {
        spin_lock_irqsave(&inject_slots_lock, flags);
        if (test_bit(slot, inject_slots) || 
            input_mt_get_value(&mt->slots[slot], ABS_MT_TRACKING_ID) >= 0) {
            spin_unlock_irqrestore(&inject_slots_lock, flags);
            return;
        }
        set_bit(slot, inject_slots);
        spin_unlock_irqrestore(&inject_slots_lock, flags);
    }

    /* 执行触摸操作 - 现在是串行的，不会有竞争 */
    id = input_mt_get_value(&mt->slots[slot], ABS_MT_TRACKING_ID);
    if (id < 0)
        id = input_mt_new_trkid(mt);

    input_event(g_touch_dev, EV_ABS, ABS_MT_TRACKING_ID, id);

    if (test_bit(ABS_MT_TOOL_TYPE, g_touch_dev->absbit))
        input_event(g_touch_dev, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);

    input_event(g_touch_dev, EV_ABS, ABS_MT_SLOT, e->slot);
    input_event(g_touch_dev, EV_ABS, ABS_MT_POSITION_X, clamp_abs(g_touch_dev, ABS_MT_POSITION_X, e->x));
    input_event(g_touch_dev, EV_ABS, ABS_MT_POSITION_Y, clamp_abs(g_touch_dev, ABS_MT_POSITION_Y, e->y));

    if (test_bit(ABS_MT_TOUCH_MAJOR, g_touch_dev->absbit))
        input_event(g_touch_dev, EV_ABS, ABS_MT_TOUCH_MAJOR, 15);
    if (test_bit(ABS_MT_TOUCH_MINOR, g_touch_dev->absbit))
        input_event(g_touch_dev, EV_ABS, ABS_MT_TOUCH_MINOR, 15);
    if (test_bit(ABS_MT_PRESSURE, g_touch_dev->absbit))
        input_event(g_touch_dev, EV_ABS, ABS_MT_PRESSURE, e->pressure > 0 ? e->pressure : 100);

    atomic_inc(&inj_active);

    maybe_emit_btntouch(g_touch_dev);
    input_mt_sync_frame(g_touch_dev);
    input_sync(g_touch_dev);

    e->slot = slot;

    lastTouchPos_x = e->x;
    lastTouchPos_y = e->y;
}

static void mt_do_move_work(const struct touch_event_base *e)
{
    struct input_dev *dev = g_touch_dev;
    struct input_mt *mt;
    unsigned long flags;
    int id;
    
    if (!dev) return;
    
    mt = mt_checked(dev);
    if (!mt) return;
    
    if (e->slot < 0 || e->slot >= mt->num_slots || e->slot >= 32)
        return;
    
    /* 验证这是一个注入的slot */
    spin_lock_irqsave(&inject_slots_lock, flags);
    if (!test_bit(e->slot, inject_slots)) {
        spin_unlock_irqrestore(&inject_slots_lock, flags);
        return;
    }
    spin_unlock_irqrestore(&inject_slots_lock, flags);

    id = input_mt_get_value(&mt->slots[e->slot], ABS_MT_TRACKING_ID);
    if (id < 0 && lastTouchPos_x != -1 && lastTouchPos_y != -1) {
        /* 隐式 Down */
        input_event(dev, EV_ABS, ABS_MT_SLOT, e->slot);
        id = input_mt_new_trkid(mt);
        input_event(dev, EV_ABS, ABS_MT_TRACKING_ID, id);
        atomic_inc(&inj_active);

        if (test_bit(ABS_MT_TOOL_TYPE, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);

        input_event(dev, EV_ABS, ABS_MT_SLOT, e->slot);
        input_event(dev, EV_ABS, ABS_MT_POSITION_X, clamp_abs(dev, ABS_MT_POSITION_X, lastTouchPos_x));
        input_event(dev, EV_ABS, ABS_MT_POSITION_Y, clamp_abs(dev, ABS_MT_POSITION_Y, lastTouchPos_y));

        if (test_bit(ABS_MT_TOUCH_MAJOR, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, 15);
        if (test_bit(ABS_MT_TOUCH_MINOR, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_TOUCH_MINOR, 15);
        if (test_bit(ABS_MT_PRESSURE, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_PRESSURE, e->pressure > 0 ? e->pressure : 100);

        maybe_emit_btntouch(dev);
        input_mt_sync_frame(dev);
        input_sync(dev);

        input_event(dev, EV_ABS, ABS_MT_TRACKING_ID, id);
        if (test_bit(ABS_MT_TOOL_TYPE, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);

        input_event(dev, EV_ABS, ABS_MT_SLOT, e->slot);
        input_event(dev, EV_ABS, ABS_MT_POSITION_X, clamp_abs(dev, ABS_MT_POSITION_X, e->x));
        input_event(dev, EV_ABS, ABS_MT_POSITION_Y, clamp_abs(dev, ABS_MT_POSITION_Y, e->y));

        if (test_bit(ABS_MT_TOUCH_MAJOR, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, 15);
        if (test_bit(ABS_MT_TOUCH_MINOR, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_TOUCH_MINOR, 15);
        if (test_bit(ABS_MT_PRESSURE, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_PRESSURE, e->pressure > 0 ? e->pressure : 100);

        maybe_emit_btntouch(dev);
        input_mt_sync_frame(dev);
        input_sync(dev);
    } else {
        if (test_bit(ABS_MT_TOOL_TYPE, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);

        input_event(dev, EV_ABS, ABS_MT_SLOT, e->slot);
        input_event(dev, EV_ABS, ABS_MT_POSITION_X, clamp_abs(dev, ABS_MT_POSITION_X, e->x));
        input_event(dev, EV_ABS, ABS_MT_POSITION_Y, clamp_abs(dev, ABS_MT_POSITION_Y, e->y));

        if (test_bit(ABS_MT_TOUCH_MAJOR, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, 15);
        if (test_bit(ABS_MT_TOUCH_MINOR, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_TOUCH_MINOR, 15);
        if (test_bit(ABS_MT_PRESSURE, dev->absbit))
            input_event(dev, EV_ABS, ABS_MT_PRESSURE, e->pressure > 0 ? e->pressure : 100);

        maybe_emit_btntouch(dev);
        input_mt_sync_frame(dev);
        input_sync(dev);
    }

    lastTouchPos_x = e->x;
    lastTouchPos_y = e->y;
}

static void mt_do_up_work(const struct touch_event_base *e)
{
    struct input_dev *dev = g_touch_dev;
    struct input_mt *mt;
    unsigned long flags;
    int id;
    
    if (!dev) return;
    
    mt = mt_checked(dev);
    if (!mt) return;
    
    if (e->slot < 0 || e->slot >= mt->num_slots || e->slot >= 32)
        return;
    
    /* 验证这是一个注入的slot */
    spin_lock_irqsave(&inject_slots_lock, flags);
    if (!test_bit(e->slot, inject_slots)) {
        spin_unlock_irqrestore(&inject_slots_lock, flags);
        return;
    }
    spin_unlock_irqrestore(&inject_slots_lock, flags);

    id = input_mt_get_value(&mt->slots[e->slot], ABS_MT_TRACKING_ID);
    if (id < 0) {
        /* 已经释放 */
        spin_lock_irqsave(&inject_slots_lock, flags);
        clear_bit(e->slot, inject_slots);
        spin_unlock_irqrestore(&inject_slots_lock, flags);
        return;
    }

    if (test_bit(ABS_MT_TOOL_TYPE, dev->absbit))
        input_event(dev, EV_ABS, ABS_MT_TOOL_TYPE, MT_TOOL_FINGER);
    input_event(dev, EV_ABS, ABS_MT_SLOT, e->slot);
    input_event(dev, EV_ABS, ABS_MT_TRACKING_ID, -1);
    
    /* 清除注入slot标记 */
    spin_lock_irqsave(&inject_slots_lock, flags);
    clear_bit(e->slot, inject_slots);
    spin_unlock_irqrestore(&inject_slots_lock, flags);
    
    if (atomic_read(&inj_active) > 0) {
        atomic_dec(&inj_active);
    }
    
    maybe_emit_btntouch(dev);
    input_mt_sync_frame(dev);
    input_sync(dev);

    lastTouchPos_x = -1;
    lastTouchPos_y = -1;
}

/* 工作队列处理函数 */
static void touch_work_handler(struct work_struct *work)
{
    struct touch_work *tw = container_of(work, struct touch_work, work);
    
    /* 确保与真实触摸事件不会同时执行 */
    switch (tw->operation) {
    case 0: /* down */
        mt_do_down_work(&tw->event);
        break;
    case 1: /* move */
        mt_do_move_work(&tw->event);
        break;
    case 2: /* up */
        mt_do_up_work(&tw->event);
        break;
    }
    
    kfree(tw);
}

/* 公共接口 - 提交到工作队列 */
int mt_do_down_autoslot(struct touch_event_base *e)
{
    struct touch_work *tw;
    
    if (!touch_wq || !g_touch_dev)
        return -ENODEV;
    
    tw = kmalloc(sizeof(*tw), GFP_ATOMIC);
    if (!tw)
        return -ENOMEM;
    
    INIT_WORK(&tw->work, touch_work_handler);
    tw->event = *e;
    tw->operation = 0;
    
    queue_work(touch_wq, &tw->work);
    
    /* 等待slot分配完成 */
    flush_work(&tw->work);
    
    /* 复制回slot值 */
    *e = tw->event;
    
    return 0;
}

int mt_do_move(const struct touch_event_base *e)
{
    struct touch_work *tw;
    
    if (!touch_wq || !g_touch_dev)
        return -ENODEV;
    
    tw = kmalloc(sizeof(*tw), GFP_ATOMIC);
    if (!tw)
        return -ENOMEM;
    
    INIT_WORK(&tw->work, touch_work_handler);
    tw->event = *e;
    tw->operation = 1;
    
    queue_work(touch_wq, &tw->work);
    
    return 0;
}

int mt_do_up(const struct touch_event_base *e)
{
    struct touch_work *tw;
    
    if (!touch_wq || !g_touch_dev)
        return -ENODEV;
    
    tw = kmalloc(sizeof(*tw), GFP_ATOMIC);
    if (!tw)
        return -ENOMEM;
    
    INIT_WORK(&tw->work, touch_work_handler);
    tw->event = *e;
    tw->operation = 2;
    
    queue_work(touch_wq, &tw->work);
    
    return 0;
}

static int mt_connect(struct input_handler *handler, struct input_dev *dev,
                      const struct input_device_id *id)
{
    struct input_handle *h;
    int ret;
    
    h = kzalloc(sizeof(*h), GFP_KERNEL);
    if (!h)
        return -ENOMEM;

    h->dev     = dev;
    h->handler = handler;
    h->name    = "snd-soc-*";

    ret = input_register_handle(h);
    if (ret) {
        kfree(h);
        return ret;
    }

    ret = input_open_device(h);
    if (ret) {
        input_unregister_handle(h);
        kfree(h);
        return ret;
    }

    if (dev->name) {
        if (test_bit(EV_ABS, dev->evbit) && 
                test_bit(ABS_MT_POSITION_X, dev->absbit) &&
                test_bit(ABS_MT_POSITION_Y, dev->absbit)) {
            g_touch_dev = dev;
            g_handle = h;
            bitmap_zero(inject_slots, 32);
        }
    }

    return 0;
}

static void mt_disconnect(struct input_handle *h)
{
    if (h == g_handle) {
        g_handle = NULL;
        g_touch_dev = NULL;
    }
    input_close_device(h);
    input_unregister_handle(h);
    kfree(h);
}

static const struct input_device_id mt_ids[] = {
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT | 
                 INPUT_DEVICE_ID_MATCH_ABSBIT,
        .evbit = { BIT_MASK(EV_ABS) },
        .absbit = { 
            BIT_MASK(ABS_MT_POSITION_X) | BIT_MASK(ABS_MT_POSITION_Y) |
            BIT_MASK(ABS_MT_SLOT) | BIT_MASK(ABS_MT_TRACKING_ID)
        },
    },
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT | 
                 INPUT_DEVICE_ID_MATCH_ABSBIT,
        .evbit = { BIT_MASK(EV_ABS) },
        .absbit = { BIT_MASK(ABS_MT_POSITION_X) | BIT_MASK(ABS_MT_POSITION_Y) },
    },
    { }
};
MODULE_DEVICE_TABLE(input, mt_ids);

static struct input_handler mt_handler = {
    .connect    = mt_connect,
    .disconnect = mt_disconnect,
    .name       = "snd-soc-*",
    .id_table   = mt_ids,
};

int init_input_dev(void) {
    int ret;
    
    /* 创建单线程工作队列，确保串行执行 */
    touch_wq = alloc_ordered_workqueue("snd-soc-work", WQ_HIGHPRI);
    if (!touch_wq)
        return -ENOMEM;
    
    ret = input_register_handler(&mt_handler);
    if (ret) {
        destroy_workqueue(touch_wq);
        touch_wq = NULL;
    } else {
        list_del_init(&mt_handler.node); // 删除全局handler
    }
    
    return ret;
}

void exit_input_dev(void) {
    input_unregister_handler(&mt_handler);
    
    if (touch_wq) {
        flush_workqueue(touch_wq);
        destroy_workqueue(touch_wq);
        touch_wq = NULL;
    }
}