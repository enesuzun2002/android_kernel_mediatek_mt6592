#ifndef _LINUX_DYN_SYNC_CNTRL_H
#define _LINUX_DYN_SYNC_CNTRL_H

#ifdef CONFIG_DYNAMIC_FSYNC
/*
 * dynamic fsync: while enabled and the screen is on, fsync()/fdatasync()
 * return success without touching the disk. Everything is flushed when the
 * screen goes off, on reboot/poweroff and on panic.
 */
extern bool dyn_fsync_active;
extern bool dyn_fsync_screen_on;

static inline bool dyn_fsync_skip(void)
{
	return dyn_fsync_active && dyn_fsync_screen_on;
}
#else
static inline bool dyn_fsync_skip(void)
{
	return false;
}
#endif

#endif
