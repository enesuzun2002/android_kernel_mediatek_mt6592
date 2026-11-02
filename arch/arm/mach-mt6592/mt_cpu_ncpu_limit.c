/*
 * CPU count limit for thermal / low-battery protection.
 *
 * mt_cpufreq_cpu_power_protect() derives a (ncpu, freq) pair from the power
 * budget given by MTK thermal and the low-battery code. The frequency part is
 * applied by mt_cpufreq itself; the core-count part used to be applied only
 * by the "hotplug" cpufreq governor (hp_limited_cpu_num). With any other
 * governor (interactive) nobody enforced it, so this file does:
 *
 *  - take cores offline (highest number first, never cpu0) when the limit
 *    drops below the number of online cores,
 *  - refuse to bring a core online (cpu_up -> -EPERM) while that would
 *    exceed the limit, so userspace (power HAL) cannot online them again,
 *  - when the limit is raised again, online the cores it took offline,
 *    unless the screen went off meanwhile (userspace owns the core count
 *    then).
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/earlysuspend.h>

static unsigned int ncpu_limit = NR_CPUS;	/* >= possible means no limit */
static cpumask_t thermal_offlined;
static DEFINE_SPINLOCK(limit_lock);

static void ncpu_limit_work_fn(struct work_struct *work)
{
	unsigned int limit, cpu;
	cpumask_t restore;

	spin_lock(&limit_lock);
	limit = ncpu_limit;
	spin_unlock(&limit_lock);

	if (limit < 1)
		limit = 1;

	/* too many cores online: drop the highest numbered ones */
	while (num_online_cpus() > limit) {
		for (cpu = nr_cpu_ids; cpu-- > 1; )
			if (cpu_online(cpu))
				break;
		if (cpu < 1)
			break;
		if (cpu_down(cpu)) {
			pr_warn("ncpu_limit: cpu_down(%u) failed\n", cpu);
			break;
		}
		spin_lock(&limit_lock);
		cpumask_set_cpu(cpu, &thermal_offlined);
		spin_unlock(&limit_lock);
		pr_info("ncpu_limit: cpu%u offline (limit %u)\n", cpu, limit);
	}

	/* limit relaxed: bring back what we took away */
	spin_lock(&limit_lock);
	cpumask_copy(&restore, &thermal_offlined);
	spin_unlock(&limit_lock);

	for_each_cpu(cpu, &restore) {
		if (num_online_cpus() >= limit)
			break;
		if (!cpu_online(cpu) && !cpu_up(cpu))
			pr_info("ncpu_limit: cpu%u online again\n", cpu);
		spin_lock(&limit_lock);
		cpumask_clear_cpu(cpu, &thermal_offlined);
		spin_unlock(&limit_lock);
	}
}
static DECLARE_WORK(ncpu_limit_work, ncpu_limit_work_fn);

void mt_cpu_ncpu_limit_set(unsigned int ncpu)
{
	spin_lock(&limit_lock);
	ncpu_limit = ncpu;
	spin_unlock(&limit_lock);
	schedule_work(&ncpu_limit_work);
}
EXPORT_SYMBOL(mt_cpu_ncpu_limit_set);

static int ncpu_limit_cpu_callback(struct notifier_block *nb,
				   unsigned long action, void *hcpu)
{
	unsigned int limit;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_UP_PREPARE:
		spin_lock(&limit_lock);
		limit = ncpu_limit;
		spin_unlock(&limit_lock);
		/* the incoming core is not counted in num_online_cpus() yet */
		if (limit < num_possible_cpus() && num_online_cpus() >= limit)
			return NOTIFY_BAD;
		break;
	case CPU_ONLINE:
		spin_lock(&limit_lock);
		cpumask_clear_cpu((unsigned long)hcpu, &thermal_offlined);
		spin_unlock(&limit_lock);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block ncpu_limit_cpu_nb = {
	.notifier_call = ncpu_limit_cpu_callback,
	.priority = INT_MAX,
};

/* screen off: the power HAL decides the core count, forget thermal state */
static void ncpu_limit_early_suspend(struct early_suspend *h)
{
	spin_lock(&limit_lock);
	cpumask_clear(&thermal_offlined);
	spin_unlock(&limit_lock);
}

static void ncpu_limit_late_resume(struct early_suspend *h)
{
}

static struct early_suspend ncpu_limit_es = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1,
	.suspend = ncpu_limit_early_suspend,
	.resume = ncpu_limit_late_resume,
};

static int __init mt_cpu_ncpu_limit_init(void)
{
	register_cpu_notifier(&ncpu_limit_cpu_nb);
	register_early_suspend(&ncpu_limit_es);
	return 0;
}
early_initcall(mt_cpu_ncpu_limit_init);
