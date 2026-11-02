/* include/linux/cpufreq_times.h
 *
 * Per-UID cpu time-in-state accounting (backport of the Android common
 * kernel cpufreq_times for 3.10).
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

#ifndef _LINUX_CPUFREQ_TIMES_H
#define _LINUX_CPUFREQ_TIMES_H

#include <linux/sched.h>

#ifdef CONFIG_CPU_FREQ_TIMES
void cpufreq_acct_update_power(struct task_struct *p, cputime_t cputime);
void cpufreq_task_times_remove_uids(uid_t uid_start, uid_t uid_end);
#else
static inline void cpufreq_acct_update_power(struct task_struct *p,
					     cputime_t cputime) {}
static inline void cpufreq_task_times_remove_uids(uid_t uid_start,
						  uid_t uid_end) {}
#endif /* CONFIG_CPU_FREQ_TIMES */
#endif /* _LINUX_CPUFREQ_TIMES_H */
