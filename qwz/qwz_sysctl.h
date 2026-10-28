/*
 * qwz_sysctl - the debug.qwz.* sysctls, registered by both kexts
 * (QwzMac.kext and AirportWCN7850.kext):
 *
 *   debug.qwz.publish=1   republish the log ring as QwzMacLog (scripts/qwz-log.sh)
 *   debug.qwz.logfile=1   keep the file sink (QWZ_LOG_FILE) open for every line
 *   debug.qwz.stage=<n>   stop the next bring-up before stage n (-1: boot-time limit)
 *   debug.qwz.start=1     rerun the bring-up; only where the product passes a callback
 *   debug.qwz.debug=<m>   qwz trace mask, debug builds (QWZ_D_* in qwzvar.h)
 *   debug.qwz.he=0        no 802.11ax for the next association (default 1; can only turn
 *                         HE off, it exists at all only with the itlwm_he=1 boot-arg)
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_SYSCTL_H
#define QWZ_SYSCTL_H

/* debug.qwz.start=1 calls this; false when a bring-up is already running. */
typedef bool (*qwz_sysctl_start_fn)(void *arg);

/* start may be NULL: debug.qwz.start then answers ENXIO. Registering twice is harmless. */
void	qwz_sysctl_register(qwz_sysctl_start_fn start, void *arg);
void	qwz_sysctl_unregister(void);
/* 1 while debug.qwz.logfile keeps the file sink open (so a bring-up must not close it). */
int	qwz_sysctl_logfile_on(void);

#endif /* QWZ_SYSCTL_H */
