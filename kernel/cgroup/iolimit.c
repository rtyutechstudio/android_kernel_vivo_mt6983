// SPDX-License-Identifier: GPL-2.0
/* X80 legacy controller recovered from the stock Image's iolimit paths. */
#include <linux/cgroup.h>
#include <linux/err.h>
#include <linux/jiffies.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/x80_iolimit.h>

struct iolimit_css {
	struct cgroup_subsys_state css;
	u64 switching;
	u64 write_limit;
	u64 write_budget;
	u64 write_used;
	struct timer_list write_timer;
	spinlock_t write_lock;
	wait_queue_head_t write_wait;
	u64 read_limit;
	u64 read_budget;
	u64 read_used;
	struct timer_list read_timer;
	spinlock_t read_lock;
	wait_queue_head_t read_wait;
};
static_assert(offsetof(struct iolimit_css, switching) == 0xe8);
static_assert(offsetof(struct iolimit_css, write_limit) == 0xf0);
static_assert(offsetof(struct iolimit_css, write_timer) == 0x108);
static_assert(offsetof(struct iolimit_css, read_limit) == 0x160);
static_assert(offsetof(struct iolimit_css, read_timer) == 0x178);
static_assert(sizeof(struct iolimit_css) == 0x1d0);
static_assert(CGROUP_SUBSYS_COUNT == 8);
static_assert(iolimit_cgrp_id == 7);
static_assert(offsetof(struct cgroup, root) == 504);

static struct iolimit_css *to_iolimit(struct cgroup_subsys_state *css)
{
	return container_of(css, struct iolimit_css, css);
}

static void write_timer_handler(struct timer_list *timer)
{
	struct iolimit_css *io = from_timer(io, timer, write_timer);

	spin_lock(&io->write_lock);
	io->write_used = 0;
	spin_unlock(&io->write_lock);
	wake_up_all(&io->write_wait);
	if (READ_ONCE(io->switching))
		mod_timer(timer, jiffies + 31);
}

static void read_timer_handler(struct timer_list *timer)
{
	struct iolimit_css *io = from_timer(io, timer, read_timer);

	spin_lock(&io->read_lock);
	io->read_used = 0;
	spin_unlock(&io->read_lock);
	wake_up_all(&io->read_wait);
	if (READ_ONCE(io->switching))
		mod_timer(timer, jiffies + 31);
}

static struct cgroup_subsys_state *iolimit_css_alloc(struct cgroup_subsys_state *parent)
{
	struct iolimit_css *io = kzalloc(sizeof(*io), GFP_KERNEL);

	if (!io)
		return ERR_PTR(-ENOMEM);
	timer_setup(&io->write_timer, write_timer_handler, 0);
	timer_setup(&io->read_timer, read_timer_handler, 0);
	spin_lock_init(&io->write_lock);
	spin_lock_init(&io->read_lock);
	init_waitqueue_head(&io->write_wait);
	init_waitqueue_head(&io->read_wait);
	return &io->css;
}

static void iolimit_css_free(struct cgroup_subsys_state *css)
{
	struct iolimit_css *io = to_iolimit(css);

	WRITE_ONCE(io->switching, 0);
	del_timer_sync(&io->write_timer);
	del_timer_sync(&io->read_timer);
	kfree(io);
}

static void iolimit_attach(struct cgroup_taskset *tset)
{
	struct cgroup_subsys_state *css;
	struct task_struct *task;

	cgroup_taskset_for_each(task, css, tset)
		wake_up_state(task, TASK_NORMAL);
}

static u64 switching_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	return READ_ONCE(to_iolimit(css)->switching);
}

static int switching_write(struct cgroup_subsys_state *css, struct cftype *cft, u64 value)
{
	struct iolimit_css *io = to_iolimit(css);

	if (value > 1)
		return -EINVAL;
	WRITE_ONCE(io->switching, value);
	if (value) {
		/* Stock begins with the current interval consumed. */
		spin_lock_bh(&io->write_lock);
		io->write_used = io->write_budget;
		spin_unlock_bh(&io->write_lock);
		spin_lock_bh(&io->read_lock);
		io->read_used = io->read_budget;
		spin_unlock_bh(&io->read_lock);
		mod_timer(&io->write_timer, jiffies + 31);
		mod_timer(&io->read_timer, jiffies + 31);
	} else {
		del_timer_sync(&io->write_timer);
		del_timer_sync(&io->read_timer);
		spin_lock_bh(&io->write_lock);
		io->write_used = 0;
		spin_unlock_bh(&io->write_lock);
		spin_lock_bh(&io->read_lock);
		io->read_used = 0;
		spin_unlock_bh(&io->read_lock);
		wake_up_all(&io->write_wait);
		wake_up_all(&io->read_wait);
	}
	return 0;
}

static u64 limit_read(struct cgroup_subsys_state *css, struct cftype *cft)
{
	struct iolimit_css *io = to_iolimit(css);

	return cft->private ? READ_ONCE(io->read_limit) : READ_ONCE(io->write_limit);
}

static int limit_write(struct cgroup_subsys_state *css, struct cftype *cft, u64 value)
{
	struct iolimit_css *io = to_iolimit(css);

	if (value > S64_MAX || (value && value < 32768))
		return -EINVAL;
	if (cft->private) {
		spin_lock_bh(&io->read_lock);
		io->read_limit = value;
		io->read_budget = value >> 3;
		spin_unlock_bh(&io->read_lock);
		wake_up_all(&io->read_wait);
	} else {
		spin_lock_bh(&io->write_lock);
		io->write_limit = value;
		io->write_budget = value >> 3;
		spin_unlock_bh(&io->write_lock);
		wake_up_all(&io->write_wait);
	}
	return 0;
}

static struct cftype iolimit_files[] = {
	{ .name = "switching", .flags = CFTYPE_NOT_ON_ROOT,
	  .read_u64 = switching_read, .write_u64 = switching_write },
	{ .name = "write_limit", .flags = CFTYPE_NOT_ON_ROOT,
	  .read_u64 = limit_read, .write_u64 = limit_write },
	{ .name = "read_limit", .flags = CFTYPE_NOT_ON_ROOT, .private = 1,
	  .read_u64 = limit_read, .write_u64 = limit_write },
	{ }
};

struct cgroup_subsys iolimit_cgrp_subsys = {
	.css_alloc = iolimit_css_alloc,
	.css_free = iolimit_css_free,
	.attach = iolimit_attach,
	.legacy_cftypes = iolimit_files,
};

static bool iolimit_ready(struct iolimit_css *io, bool read, size_t bytes)
{
	u64 budget = read ? READ_ONCE(io->read_budget) : READ_ONCE(io->write_budget);
	u64 used = read ? READ_ONCE(io->read_used) : READ_ONCE(io->write_used);

	bool migrated;

	rcu_read_lock();
	migrated = task_css(current, iolimit_cgrp_id) != &io->css;
	rcu_read_unlock();
	return !READ_ONCE(io->switching) || !budget ||
	       (used <= budget && bytes <= budget - used) || migrated;
}

/* Stock callers charge exactly PAGE_SIZE. The minimum enabled limit makes
 * budget >= PAGE_SIZE; do not use this interface for unsplit larger requests.
 */
static void do_io_bandwidth_control(size_t bytes, bool read)
{
	struct cgroup_subsys_state *css;
	struct iolimit_css *io;
	spinlock_t *lock;
	wait_queue_head_t *wait;
	u64 budget, used;

	if (!bytes || (read && (current->flags & PF_KTHREAD)))
		return;
	for (;;) {
		if (test_thread_flag(TIF_MEMDIE) || signal_pending(current))
			return;
		rcu_read_lock();
		css = task_css(current, iolimit_cgrp_id);
		io = to_iolimit(css);
		if (!css->parent || !READ_ONCE(io->switching) ||
		    !(read ? READ_ONCE(io->read_limit) : READ_ONCE(io->write_limit))) {
			rcu_read_unlock();
			return;
		}
		lock = read ? &io->read_lock : &io->write_lock;
		wait = read ? &io->read_wait : &io->write_wait;
		spin_lock_bh(lock);
		budget = read ? io->read_budget : io->write_budget;
		used = read ? io->read_used : io->write_used;
		if (used <= budget && bytes <= budget - used) {
			if (read)
				io->read_used += bytes;
			else
				io->write_used += bytes;
			spin_unlock_bh(lock);
			rcu_read_unlock();
			return;
		}
		spin_unlock_bh(lock);
		if (!css_tryget_online(css)) {
			rcu_read_unlock();
			return;
		}
		rcu_read_unlock();
		wait_event_interruptible_timeout(*wait, iolimit_ready(io, read, bytes), 32);
		css_put(css);
	}
}

void do_io_read_bandwidth_control(size_t bytes)
{
	do_io_bandwidth_control(bytes, true);
}

void do_io_write_bandwidth_control(size_t bytes)
{
	do_io_bandwidth_control(bytes, false);
}
