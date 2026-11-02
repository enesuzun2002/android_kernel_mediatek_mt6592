/* drivers/cpufreq/cpufreq_times.c
 *
 * Per-UID cpu time-in-state, concurrent active time and concurrent policy
 * time, as used by Android's BatteryStats.
 *
 * Backported to 3.10 from the Android common kernel (android-4.4 and
 * android-4.9 cpufreq_times.c, Copyright (C) 2018 Google, Inc.).
 *
 * Differences from upstream:
 *  - no per-task time_in_state (/proc/<pid>/time_in_state) and no
 *    /proc/uid/<uid>/time_in_state, only per-UID totals are kept;
 *  - the cpufreq policy and transition notifier chains are used instead
 *    of hooks in cpufreq.c;
 *  - the policy cpumask is cached so that nothing from the cpufreq core is
 *    touched from the scheduler tick;
 *  - the binary /proc/uid_cpupower/ files read by the Pie framework's
 *    KernelCpuProcReader are provided next to the text files.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/atomic.h>
#include <linux/cpufreq.h>
#include <linux/cpufreq_times.h>
#include <linux/cpumask.h>
#include <linux/hashtable.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/threads.h>

#define UID_HASH_BITS 10

static DEFINE_HASHTABLE(uid_hash_table, UID_HASH_BITS);

static DEFINE_SPINLOCK(uid_lock); /* uid_hash_table */

struct concurrent_times {
	atomic64_t active[NR_CPUS];
	atomic64_t policy[NR_CPUS];
};

struct uid_entry {
	uid_t uid;
	unsigned int max_state;
	struct hlist_node hash;
	struct rcu_head rcu;
	struct concurrent_times *concurrent_times;
	u64 time_in_state[0];
};

/**
 * struct cpu_freqs - per-policy frequency information
 * @offset: start of these freqs' stats in uid time_in_state array
 * @max_state: number of entries in freq_table
 * @last_index: index in freq_table of last frequency switched to
 * @cpus: cpus sharing this policy
 * @first_cpu: first cpu of @cpus
 * @freq_table: list of available frequencies
 */
struct cpu_freqs {
	unsigned int offset;
	unsigned int max_state;
	unsigned int last_index;
	struct cpumask cpus;
	unsigned int first_cpu;
	unsigned int freq_table[0];
};

static struct cpu_freqs *all_freqs[NR_CPUS];

static unsigned int next_offset;

static bool freq_entry_valid(unsigned int freq)
{
	return freq != CPUFREQ_ENTRY_INVALID;
}

/* Caller must hold rcu_read_lock() */
static struct uid_entry *find_uid_entry_rcu(uid_t uid)
{
	struct uid_entry *uid_entry;

	hash_for_each_possible_rcu(uid_hash_table, uid_entry, hash, uid) {
		if (uid_entry->uid == uid)
			return uid_entry;
	}
	return NULL;
}

/* Caller must hold uid lock */
static struct uid_entry *find_uid_entry_locked(uid_t uid)
{
	struct uid_entry *uid_entry;

	hash_for_each_possible(uid_hash_table, uid_entry, hash, uid) {
		if (uid_entry->uid == uid)
			return uid_entry;
	}
	return NULL;
}

static void uid_entry_reclaim(struct rcu_head *rcu)
{
	struct uid_entry *uid_entry = container_of(rcu, struct uid_entry, rcu);

	kfree(uid_entry->concurrent_times);
	kfree(uid_entry);
}

/* Caller must hold uid lock */
static struct uid_entry *find_or_register_uid_locked(uid_t uid)
{
	struct uid_entry *uid_entry, *temp;
	struct concurrent_times *times;
	unsigned int max_state = ACCESS_ONCE(next_offset);
	size_t alloc_size = sizeof(*uid_entry) + max_state *
		sizeof(uid_entry->time_in_state[0]);

	uid_entry = find_uid_entry_locked(uid);
	if (uid_entry) {
		if (uid_entry->max_state == max_state)
			return uid_entry;
		/* uid_entry->time_in_state is too small to track all freqs, so
		 * expand it.  A new entry replaces the old one, readers still
		 * traversing the old one under RCU are not affected.
		 */
		temp = kzalloc(alloc_size, GFP_ATOMIC);
		if (!temp)
			return uid_entry;
		memcpy(temp, uid_entry, sizeof(*uid_entry) +
		       uid_entry->max_state *
		       sizeof(uid_entry->time_in_state[0]));
		temp->max_state = max_state;
		hlist_replace_rcu(&uid_entry->hash, &temp->hash);
		/* concurrent_times is carried over to temp */
		kfree_rcu(uid_entry, rcu);
		return temp;
	}

	uid_entry = kzalloc(alloc_size, GFP_ATOMIC);
	if (!uid_entry)
		return NULL;

	times = kzalloc(sizeof(*times), GFP_ATOMIC);
	if (!times) {
		kfree(uid_entry);
		return NULL;
	}

	uid_entry->uid = uid;
	uid_entry->max_state = max_state;
	uid_entry->concurrent_times = times;

	hash_add_rcu(uid_hash_table, &uid_entry->hash, uid);

	return uid_entry;
}

static void *uid_seq_start(struct seq_file *seq, loff_t *pos)
{
	if (*pos >= HASH_SIZE(uid_hash_table))
		return NULL;

	return &uid_hash_table[*pos];
}

static void *uid_seq_next(struct seq_file *seq, void *v, loff_t *pos)
{
	do {
		(*pos)++;

		if (*pos >= HASH_SIZE(uid_hash_table))
			return NULL;
	} while (hlist_empty(&uid_hash_table[*pos]));

	return &uid_hash_table[*pos];
}

static void uid_seq_stop(struct seq_file *seq, void *v) { }

/* Number of valid frequencies over all policies, i.e. the number of columns
 * in uid_time_in_state.
 */
static unsigned int valid_freq_count(void)
{
	struct cpu_freqs *freqs, *last_freqs = NULL;
	unsigned int cpu, i, cnt = 0;

	for_each_possible_cpu(cpu) {
		freqs = all_freqs[cpu];
		if (!freqs || freqs == last_freqs)
			continue;
		last_freqs = freqs;
		for (i = 0; i < freqs->max_state; i++)
			if (freq_entry_valid(freqs->freq_table[i]))
				cnt++;
	}
	return cnt;
}

/* Calls @fn(time) for each valid frequency slot of @uid_entry, in the order
 * of the header of /proc/uid_time_in_state.
 */
static void uid_time_in_state_walk(struct uid_entry *uid_entry,
				   void (*fn)(struct seq_file *, u64),
				   struct seq_file *m)
{
	struct cpu_freqs *freqs, *last_freqs = NULL;
	unsigned int cpu, i, idx;
	u64 t;

	for_each_possible_cpu(cpu) {
		freqs = all_freqs[cpu];
		if (!freqs || freqs == last_freqs)
			continue;
		last_freqs = freqs;
		for (i = 0; i < freqs->max_state; i++) {
			if (!freq_entry_valid(freqs->freq_table[i]))
				continue;
			idx = freqs->offset + i;
			t = idx < uid_entry->max_state ?
				uid_entry->time_in_state[idx] : 0;
			fn(m, cputime_to_clock_t(t));
		}
	}
}

static void put_time_text(struct seq_file *m, u64 t)
{
	seq_printf(m, " %llu", t);
}

static void put_time_bin(struct seq_file *m, u64 t)
{
	u32 val = (u32)t;

	seq_write(m, &val, sizeof(val));
}

static int uid_time_in_state_seq_show(struct seq_file *m, void *v)
{
	struct uid_entry *uid_entry;
	struct cpu_freqs *freqs, *last_freqs = NULL;
	int i, cpu;

	if (v == uid_hash_table) {
		seq_puts(m, "uid:");
		for_each_possible_cpu(cpu) {
			freqs = all_freqs[cpu];
			if (!freqs || freqs == last_freqs)
				continue;
			last_freqs = freqs;
			for (i = 0; i < freqs->max_state; i++) {
				if (!freq_entry_valid(freqs->freq_table[i]))
					continue;
				seq_printf(m, " %u", freqs->freq_table[i]);
			}
		}
		seq_putc(m, '\n');
	}

	rcu_read_lock();

	hlist_for_each_entry_rcu(uid_entry, (struct hlist_head *)v, hash) {
		if (!uid_entry->max_state)
			continue;
		seq_printf(m, "%u:", uid_entry->uid);
		uid_time_in_state_walk(uid_entry, put_time_text, m);
		seq_putc(m, '\n');
	}

	rcu_read_unlock();
	return 0;
}

/*
 * Binary /proc/uid_cpupower/time_in_state, native endian u32 words:
 * [n, uid0, time0a, ..., time0n, uid1, ...] with n the number of
 * frequencies and the times in units of 10ms.
 */
static int uid_cpupower_time_in_state_seq_show(struct seq_file *m, void *v)
{
	struct uid_entry *uid_entry;
	u32 val;

	if (v == uid_hash_table) {
		val = valid_freq_count();
		seq_write(m, &val, sizeof(val));
	}

	rcu_read_lock();

	hlist_for_each_entry_rcu(uid_entry, (struct hlist_head *)v, hash) {
		if (!uid_entry->max_state)
			continue;
		val = uid_entry->uid;
		seq_write(m, &val, sizeof(val));
		uid_time_in_state_walk(uid_entry, put_time_bin, m);
	}

	rcu_read_unlock();
	return 0;
}

static atomic64_t *get_active_times(struct concurrent_times *times)
{
	return times->active;
}

static atomic64_t *get_policy_times(struct concurrent_times *times)
{
	return times->policy;
}

static int concurrent_time_seq_show(struct seq_file *m, void *v,
	atomic64_t *(*get_times)(struct concurrent_times *), bool binary)
{
	struct uid_entry *uid_entry;
	int i, num_possible_cpus = num_possible_cpus();

	rcu_read_lock();

	hlist_for_each_entry_rcu(uid_entry, (struct hlist_head *)v, hash) {
		atomic64_t *times = get_times(uid_entry->concurrent_times);

		if (binary) {
			u32 val = uid_entry->uid;

			seq_write(m, &val, sizeof(val));
		} else {
			seq_printf(m, "%u:", uid_entry->uid);
		}

		for (i = 0; i < num_possible_cpus; ++i) {
			u64 time = cputime_to_clock_t(atomic64_read(&times[i]));

			if (binary) {
				u32 val = (u32)time;

				seq_write(m, &val, sizeof(val));
			} else {
				seq_printf(m, " %llu", time);
			}
		}
		if (!binary)
			seq_putc(m, '\n');
	}

	rcu_read_unlock();

	return 0;
}

static int concurrent_active_time_seq_show(struct seq_file *m, void *v)
{
	if (v == uid_hash_table)
		seq_printf(m, "cpus: %u\n", num_possible_cpus());

	return concurrent_time_seq_show(m, v, get_active_times, false);
}

/* [n = num_possible_cpus, uid0, time0a, ..., time0n, uid1, ...] */
static int cpupower_concurrent_active_time_seq_show(struct seq_file *m, void *v)
{
	if (v == uid_hash_table) {
		u32 val = num_possible_cpus();

		seq_write(m, &val, sizeof(val));
	}

	return concurrent_time_seq_show(m, v, get_active_times, true);
}

static int concurrent_policy_time_seq_show(struct seq_file *m, void *v)
{
	int i;
	struct cpu_freqs *freqs, *last_freqs = NULL;

	if (v == uid_hash_table) {
		int cnt = 0;

		for_each_possible_cpu(i) {
			freqs = all_freqs[i];
			if (!freqs)
				continue;
			if (freqs != last_freqs) {
				if (last_freqs) {
					seq_printf(m, ": %d ", cnt);
					cnt = 0;
				}
				seq_printf(m, "policy%d", i);

				last_freqs = freqs;
			}
			cnt++;
		}
		if (last_freqs)
			seq_printf(m, ": %d\n", cnt);
	}

	return concurrent_time_seq_show(m, v, get_policy_times, false);
}

/* [n = number of policies, x0, ..., xn, uid0, time0a, ..., uid1, ...] where
 * xi is the number of cpus of policy i.  The per uid entries span
 * num_possible_cpus() words.
 */
static int cpupower_concurrent_policy_time_seq_show(struct seq_file *m, void *v)
{
	int i;
	u32 val;
	struct cpu_freqs *freqs, *last_freqs = NULL;

	if (v == uid_hash_table) {
		u32 policies = 0, cnt = 0;
		u32 per_policy[NR_CPUS];

		for_each_possible_cpu(i) {
			freqs = all_freqs[i];
			if (!freqs)
				continue;
			if (freqs != last_freqs) {
				if (last_freqs)
					per_policy[policies++] = cnt;
				cnt = 0;
				last_freqs = freqs;
			}
			cnt++;
		}
		if (last_freqs)
			per_policy[policies++] = cnt;

		seq_write(m, &policies, sizeof(policies));
		for (i = 0; i < policies; i++) {
			val = per_policy[i];
			seq_write(m, &val, sizeof(val));
		}
	}

	return concurrent_time_seq_show(m, v, get_policy_times, true);
}

void cpufreq_acct_update_power(struct task_struct *p, cputime_t cputime)
{
	unsigned long flags;
	unsigned int state;
	unsigned int active_cpu_cnt = 0;
	unsigned int policy_cpu_cnt = 0;
	struct uid_entry *uid_entry;
	struct cpu_freqs *freqs = all_freqs[task_cpu(p)];
	uid_t uid = from_kuid_munged(&init_user_ns, task_uid(p));
	int cpu;

	if (!freqs || is_idle_task(p) || p->flags & PF_EXITING)
		return;

	state = freqs->offset + ACCESS_ONCE(freqs->last_index);

	spin_lock_irqsave(&uid_lock, flags);
	uid_entry = find_or_register_uid_locked(uid);
	if (uid_entry && state < uid_entry->max_state)
		uid_entry->time_in_state[state] += cputime;
	spin_unlock_irqrestore(&uid_lock, flags);

	rcu_read_lock();
	uid_entry = find_uid_entry_rcu(uid);
	if (!uid_entry) {
		rcu_read_unlock();
		return;
	}

	/* cpus that are offline (hotplug) are idle by definition */
	for_each_possible_cpu(cpu)
		if (!idle_cpu(cpu))
			++active_cpu_cnt;
	if (!active_cpu_cnt)
		active_cpu_cnt = 1;

	atomic64_add(cputime,
		     &uid_entry->concurrent_times->active[active_cpu_cnt - 1]);

	for_each_cpu(cpu, &freqs->cpus)
		if (!idle_cpu(cpu))
			++policy_cpu_cnt;
	if (!policy_cpu_cnt)
		policy_cpu_cnt = 1;

	atomic64_add(cputime,
		     &uid_entry->concurrent_times->policy[freqs->first_cpu +
							  policy_cpu_cnt - 1]);
	rcu_read_unlock();
}

static int cpufreq_times_get_index(struct cpu_freqs *freqs, unsigned int freq)
{
	int index;

	for (index = 0; index < freqs->max_state; ++index) {
		if (freqs->freq_table[index] == freq)
			return index;
	}
	return -1;
}

static void cpufreq_times_create_policy(struct cpufreq_policy *policy)
{
	int cpu, index;
	unsigned int count = 0;
	struct cpufreq_frequency_table *pos, *table;
	struct cpu_freqs *freqs;
	const struct cpumask *cpus;

	if (all_freqs[policy->cpu])
		return;

	table = cpufreq_frequency_get_table(policy->cpu);
	if (!table)
		return;

	for (pos = table; pos->frequency != CPUFREQ_TABLE_END; pos++)
		count++;

	freqs = kzalloc(sizeof(*freqs) + sizeof(freqs->freq_table[0]) * count,
			GFP_KERNEL);
	if (!freqs)
		return;

	freqs->max_state = count;

	for (pos = table; pos->frequency != CPUFREQ_TABLE_END; pos++)
		freqs->freq_table[pos - table] = pos->frequency;

	index = cpufreq_times_get_index(freqs, policy->cur);
	if (index >= 0)
		ACCESS_ONCE(freqs->last_index) = index;

	cpus = cpumask_empty(policy->related_cpus) ? policy->cpus :
						     policy->related_cpus;
	cpumask_copy(&freqs->cpus, cpus);
	freqs->first_cpu = cpumask_first(cpus);

	freqs->offset = next_offset;
	ACCESS_ONCE(next_offset) = freqs->offset + count;
	for_each_cpu(cpu, cpus)
		all_freqs[cpu] = freqs;
}

static void uid_entry_remove_locked(struct uid_entry *uid_entry)
{
	hash_del_rcu(&uid_entry->hash);
	call_rcu(&uid_entry->rcu, uid_entry_reclaim);
}

void cpufreq_task_times_remove_uids(uid_t uid_start, uid_t uid_end)
{
	struct uid_entry *uid_entry;
	struct hlist_node *tmp;
	unsigned long flags;

	spin_lock_irqsave(&uid_lock, flags);

	for (; uid_start <= uid_end; uid_start++) {
		hash_for_each_possible_safe(uid_hash_table, uid_entry, tmp,
			hash, uid_start) {
			if (uid_start == uid_entry->uid)
				uid_entry_remove_locked(uid_entry);
		}
		if (uid_start == (uid_t)-1)
			break;
	}

	spin_unlock_irqrestore(&uid_lock, flags);
}

static void cpufreq_times_record_transition(struct cpufreq_freqs *freq)
{
	int index;
	struct cpu_freqs *freqs;

	if (freq->cpu >= nr_cpu_ids)
		return;

	freqs = all_freqs[freq->cpu];
	if (!freqs)
		return;

	index = cpufreq_times_get_index(freqs, freq->new);
	if (index >= 0)
		ACCESS_ONCE(freqs->last_index) = index;
}

static int cpufreq_times_policy_notifier(struct notifier_block *nb,
					 unsigned long val, void *data)
{
	if (val == CPUFREQ_NOTIFY)
		cpufreq_times_create_policy(data);
	return NOTIFY_OK;
}

static int cpufreq_times_trans_notifier(struct notifier_block *nb,
					unsigned long val, void *data)
{
	if (val == CPUFREQ_POSTCHANGE)
		cpufreq_times_record_transition(data);
	return NOTIFY_OK;
}

static struct notifier_block cpufreq_times_policy_nb = {
	.notifier_call = cpufreq_times_policy_notifier,
};

static struct notifier_block cpufreq_times_trans_nb = {
	.notifier_call = cpufreq_times_trans_notifier,
};

#define UID_SEQ_OPS(name, show_fn)					\
static const struct seq_operations name##_seq_ops = {			\
	.start = uid_seq_start,						\
	.next = uid_seq_next,						\
	.stop = uid_seq_stop,						\
	.show = show_fn,						\
};									\
static int name##_open(struct inode *inode, struct file *file)		\
{									\
	return seq_open(file, &name##_seq_ops);				\
}									\
static const struct file_operations name##_fops = {			\
	.open		= name##_open,					\
	.read		= seq_read,					\
	.llseek		= seq_lseek,					\
	.release	= seq_release,					\
}

UID_SEQ_OPS(uid_time_in_state, uid_time_in_state_seq_show);
UID_SEQ_OPS(concurrent_active_time, concurrent_active_time_seq_show);
UID_SEQ_OPS(concurrent_policy_time, concurrent_policy_time_seq_show);
UID_SEQ_OPS(cpupower_time_in_state, uid_cpupower_time_in_state_seq_show);
UID_SEQ_OPS(cpupower_active_time, cpupower_concurrent_active_time_seq_show);
UID_SEQ_OPS(cpupower_policy_time, cpupower_concurrent_policy_time_seq_show);

static int __init cpufreq_times_init(void)
{
	struct proc_dir_entry *dir;
	struct cpufreq_policy *policy;
	unsigned int cpu;
	int ret;

	ret = cpufreq_register_notifier(&cpufreq_times_policy_nb,
					CPUFREQ_POLICY_NOTIFIER);
	if (ret)
		return ret;
	ret = cpufreq_register_notifier(&cpufreq_times_trans_nb,
					CPUFREQ_TRANSITION_NOTIFIER);
	if (ret) {
		cpufreq_unregister_notifier(&cpufreq_times_policy_nb,
					    CPUFREQ_POLICY_NOTIFIER);
		return ret;
	}

	proc_create_data("uid_time_in_state", 0444, NULL,
			 &uid_time_in_state_fops, NULL);
	proc_create_data("uid_concurrent_active_time", 0444, NULL,
			 &concurrent_active_time_fops, NULL);
	proc_create_data("uid_concurrent_policy_time", 0444, NULL,
			 &concurrent_policy_time_fops, NULL);

	/* policies created before the notifiers were registered */
	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		cpufreq_times_create_policy(policy);
		cpufreq_cpu_put(policy);
	}

	dir = proc_mkdir("uid_cpupower", NULL);
	if (dir) {
		proc_create_data("time_in_state", 0444, dir,
				 &cpupower_time_in_state_fops, NULL);
		proc_create_data("concurrent_active_time", 0444, dir,
				 &cpupower_active_time_fops, NULL);
		proc_create_data("concurrent_policy_time", 0444, dir,
				 &cpupower_policy_time_fops, NULL);
	}

	return 0;
}

/*
 * The transition notifier list is only usable after the pure initcalls and
 * the cpufreq drivers register their policies in device initcalls.
 */
late_initcall(cpufreq_times_init);
