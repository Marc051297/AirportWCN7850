/*
 * qwz_sysctl - the debug.qwz.* sysctls. See qwz_sysctl.h.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_sysctl.h"
#include "qwz_log.h"

#include <sys/types.h>
#include <sys/errno.h>
#include <sys/sysctl.h>

extern int qwz_stage_sysctl;	/* qwz_compat.cpp */

/*
 * debug.qwz.he: per-association kill switch for 802.11ax.  Read by
 * ieee80211_he_supported() (itl80211 ieee80211_proto.c).  It can only turn HE
 * off: HE exists at all only when the itlwm_he=1 boot-arg set HEON before the
 * firmware's capabilities were read (qwz_mac_setup_he_cap).
 */
int qwz_he_sysctl = 1;

static qwz_sysctl_start_fn qwz_sysctl_start_cb;
static void *qwz_sysctl_start_arg;
static int qwz_logfile_on;	/* debug.qwz.logfile: file sink kept open */
static bool qwz_sysctl_registered;

static int
qwz_sysctl_start(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = 0;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v != 1)
		return EINVAL;
	if (qwz_sysctl_start_cb == NULL)
		return ENXIO;	/* this product's bring-up cannot be rerun */
	qwz_log_file_begin();
	qwz_log("qwz: sysctl debug.qwz.start=1 received\n");
	qwz_log_file_end();
	return qwz_sysctl_start_cb(qwz_sysctl_start_arg) ? 0 : EBUSY;
}

static int
qwz_sysctl_publish(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = 0;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	qwz_log_publish();	/* refresh QwzMacLog in the I/O Registry */
	return 0;
}

static int
qwz_sysctl_logfile(struct sysctl_oid *oidp, void *arg1, int arg2, struct sysctl_req *req)
{
	int v = qwz_logfile_on;
	int err = sysctl_handle_int(oidp, &v, 0, req);

	if (err || req->newptr == USER_ADDR_NULL)
		return err;
	if (v && !qwz_logfile_on) {
		qwz_log_file_begin();
		qwz_log("qwz: ==== file log on (sysctl) ====\n");
	} else if (!v && qwz_logfile_on) {
		qwz_log("qwz: ==== file log off (sysctl) ====\n");
		qwz_log_file_end();
	}
	qwz_logfile_on = v ? 1 : 0;
	return 0;
}

SYSCTL_NODE(_debug, OID_AUTO, qwz, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "QwzMac");
SYSCTL_PROC(_debug_qwz, OID_AUTO, start, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_start, "I", "write 1 to run the bring-up");
SYSCTL_INT(_debug_qwz, OID_AUTO, stage, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_stage_sysctl, 0, "stop the bring-up before this stage (-1: boot-time limit)");
SYSCTL_PROC(_debug_qwz, OID_AUTO, publish, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_publish, "I", "write 1 to republish the log ring as QwzMacLog");
SYSCTL_PROC(_debug_qwz, OID_AUTO, logfile, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, qwz_sysctl_logfile, "I", "1: every log line also goes to " QWZ_LOG_FILE);
SYSCTL_INT(_debug_qwz, OID_AUTO, he, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_he_sysctl, 0, "0: no 802.11ax for the next association (needs itlwm_he=1 to be on at all)");
#ifdef QWZ_DEBUG
extern uint32_t qwz_debug;	/* qwz.c: QWZ_D_* mask from qwzvar.h */
SYSCTL_UINT(_debug_qwz, OID_AUTO, debug, CTLFLAG_RW | CTLFLAG_LOCKED,
    &qwz_debug, 0, "qwz debug mask (QWZ_D_* in qwzvar.h)");
#endif

void
qwz_sysctl_register(qwz_sysctl_start_fn start, void *arg)
{
	qwz_sysctl_start_cb = start;
	qwz_sysctl_start_arg = arg;
	if (qwz_sysctl_registered)
		return;
	sysctl_register_oid(&sysctl__debug_qwz);
	sysctl_register_oid(&sysctl__debug_qwz_start);
	sysctl_register_oid(&sysctl__debug_qwz_stage);
	sysctl_register_oid(&sysctl__debug_qwz_publish);
	sysctl_register_oid(&sysctl__debug_qwz_logfile);
	sysctl_register_oid(&sysctl__debug_qwz_he);
#ifdef QWZ_DEBUG
	sysctl_register_oid(&sysctl__debug_qwz_debug);
#endif
	qwz_sysctl_registered = true;
}

void
qwz_sysctl_unregister(void)
{
	qwz_sysctl_start_cb = NULL;
	qwz_sysctl_start_arg = NULL;
	if (!qwz_sysctl_registered)
		return;
#ifdef QWZ_DEBUG
	sysctl_unregister_oid(&sysctl__debug_qwz_debug);
#endif
	sysctl_unregister_oid(&sysctl__debug_qwz_he);
	sysctl_unregister_oid(&sysctl__debug_qwz_logfile);
	sysctl_unregister_oid(&sysctl__debug_qwz_publish);
	sysctl_unregister_oid(&sysctl__debug_qwz_stage);
	sysctl_unregister_oid(&sysctl__debug_qwz_start);
	sysctl_unregister_oid(&sysctl__debug_qwz);
	qwz_sysctl_registered = false;
}

int
qwz_sysctl_logfile_on(void)
{
	return qwz_logfile_on;
}
