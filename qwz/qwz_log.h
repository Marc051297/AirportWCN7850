/*
 * qwz_log - the driver's log ring, published in the I/O Registry.
 *
 * Messages from an OpenCore-injected kext do not reach the unified log, and
 * the kernel message buffer wraps quickly on a verbose boot, so every line
 * also goes into a ring that is published on the PCI device together with
 * the current bring-up stage (the approach SimpleMtkWlan uses):
 *
 *   ioreg -l -w0 | grep -E '"QwzMac(Stage|Log|Version)"'
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_LOG_H
#define QWZ_LOG_H

#include <stdarg.h>

class IOService;

int	qwz_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* As qwz_log, but always on the console too (shutdown steps: the screen is the only log left). */
int	qwz_logc(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int	qwz_vlog(const char *fmt, va_list ap) __attribute__((format(printf, 1, 0)));

/* Where QwzMacLog/QwzMacStage are published: the WCN7850's IOPCIDevice. */
void	qwz_log_set_target(IOService *);
/* Records the current stage and republishes the log. */
void	qwz_log_stage(const char *stage);
int	qwz_stage_hit_limit(void);	/* qwz_compat.cpp: a checkpoint was refused by qwz_stage */
void	qwz_stage_reset(void);
/* Republishes the log ring as QwzMacLog. */
void	qwz_log_publish(void);

/*
 * Rate limit for a log site. *count is the site's running total (it always
 * counts); returns non-zero for the first `first` calls and then for every
 * `every`th one, so a site that fires in a loop cannot flood the ring.
 */
static inline int
qwz_log_ratelimit(unsigned int *count, unsigned int first, unsigned int every)
{
	unsigned int n = __atomic_add_fetch(count, 1u, __ATOMIC_RELAXED);

	return n <= first || (n % every) == 0;
}

/*
 * File sink for bring-ups started from the terminal: between begin and end,
 * lines logged by the calling thread are also appended to QWZ_LOG_FILE with
 * a synchronous write each, so the last line before a hard freeze survives.
 * Only the thread that called begin writes (never interrupt or workloop
 * context). Lines from other threads stay in the ring only.
 */
#define QWZ_LOG_FILE	"/private/var/log/qwzmac.log"
void	qwz_log_file_begin(void);
void	qwz_log_file_end(void);
/* Also let this thread (the driver workloop) write while the file is open. */
void	qwz_log_file_allow(struct thread *);

#endif /* QWZ_LOG_H */
