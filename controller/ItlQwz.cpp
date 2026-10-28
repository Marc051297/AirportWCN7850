/*
 * ItlQwz - itlwm HAL for the Qualcomm WCN7850, driving OpenBSD's qwz(4).
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include "ItlQwz.hpp"

/* IOKit and net80211 come first: qwz_compat.h then defines malloc/free/... macros. */
#include "qwz_compat.h"
#include "qwzreg.h"
#include "qwzvar.h"
#undef malloc
#undef free
#undef wakeup

#define QWZLOG(fmt, ...)	qwz_log("qwz: " fmt "\n", ##__VA_ARGS__)

/* if_qwz_pci.c */
struct qwz_softc *qwz_pci_macos_alloc(void);
int	qwz_pci_macos_attach(struct qwz_softc *, struct pci_attach_args *);
void	qwz_pci_macos_detach(struct qwz_softc *);
void	qwz_pci_macos_free(struct qwz_softc *);
void	qwz_pci_macos_quiesced_detach(struct qwz_softc *);
/* qwz.c */
int	qwz_init(struct _ifnet *);

#define super ItlHalService
OSDefineMetaClassAndStructors(ItlQwz, ItlHalService)

/*
 * The controller calls if_start from the network stack's output thread and
 * if_watchdog from its own timer, neither holding the gate. qwz expects the
 * kernel lock there, so both are wrapped to run gated.
 */
static IOCommandGate *qwz_hal_gate;
static void (*qwz_if_start)(struct _ifnet *);
static void (*qwz_if_watchdog)(struct _ifnet *);

static IOReturn
qwz_start_gated(OSObject *, void *ifp, void *, void *, void *)
{
	qwz_if_start((struct _ifnet *)ifp);
	return kIOReturnSuccess;
}

static void
qwz_if_start_wrapper(struct _ifnet *ifp)
{
	qwz_hal_gate->runAction(qwz_start_gated, ifp);
}

static IOReturn
qwz_watchdog_gated(OSObject *, void *ifp, void *, void *, void *)
{
	qwz_if_watchdog((struct _ifnet *)ifp);
	return kIOReturnSuccess;
}

static void
qwz_if_watchdog_wrapper(struct _ifnet *ifp)
{
	qwz_hal_gate->runAction(qwz_watchdog_gated, ifp);
}

bool
ItlQwz::initWithController(IOEthernetController *controller, IOWorkLoop *workloop, IOCommandGate *commandGate)
{
	if (!super::initWithController(controller, workloop, commandGate))
		return false;
	qwz_compat_init();
	if (sc == NULL)
		sc = qwz_pci_macos_alloc();
	return sc != NULL;
}

bool
ItlQwz::qwz_match(IOPCIDevice *device)
{
	return device->configRead16(kIOPCIConfigVendorID) == PCI_VENDOR_QUALCOMM &&
	    device->configRead16(kIOPCIConfigDeviceID) == PCI_PRODUCT_QUALCOMM_WCN7850;
}

IOReturn
/*
 * IOCommandGate::runAction() passes the gate's OWNER (the itlwm controller)
 * as the first argument, not the HAL. The HAL is passed explicitly in arg1.
 * Casting owner to ItlQwz wrote sc into the controller object and left the
 * HAL's own sc NULL: the attach "succeeded" and the next line dereferenced
 * NULL, a panic that looks like a hard freeze with the NVIDIA driver.
 */
ItlQwz::attachGated(OSObject *, void *arg0, void *arg1, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg1;
	struct pci_attach_args *pa = (struct pci_attach_args *)arg0;
	int err;

	if (!qwz_stage_ok(3, "inside the qwz attach, holding the gate"))
		return kIOReturnError;

	if (that->sc == NULL && (that->sc = qwz_pci_macos_alloc()) == NULL)
		return kIOReturnNoMemory;
	err = qwz_pci_macos_attach(that->sc, pa);
	if (err == QWZ_EPARKED) {
		/* Parked at a stage >= 15: keep sc and the running firmware. */
		QWZLOG("parked: the driver stays idle with the firmware running");
		return kIOReturnError;
	}
	if (err) {
		QWZLOG("attach failed: %d", err);
		qwz_pci_macos_detach(that->sc);
		/* keep get80211Controller() valid: a fresh, empty softc */
		that->sc = qwz_pci_macos_alloc();
		return kIOReturnError;
	}
	that->attached = true;
	return kIOReturnSuccess;
}

bool
ItlQwz::attach(IOPCIDevice *device)
{
	struct pci_attach_args pa;
	struct _ifnet *ifp;
	IOReturn ret;

	QWZLOG("QwzMac %s attaching to %04x:%04x subsystem %04x:%04x", QWZMAC_VERSION,
	    device->configRead16(kIOPCIConfigVendorID), device->configRead16(kIOPCIConfigDeviceID),
	    device->configRead16(kIOPCIConfigSubSystemVendorID),
	    device->configRead16(kIOPCIConfigSubSystemID));

#ifdef QWZ_DEBUG
	/* qwz_debug=<mask of QWZ_D_* from qwzvar.h> sets the trace level. */
	PE_parse_boot_argn("qwz_debug", &qwz_debug, sizeof(qwz_debug));
	QWZLOG("debug build, qwz_debug=0x%x", qwz_debug);
#endif
	if (qwz_stage_parked()) {
		QWZLOG("parked by an earlier attempt; reboot before starting again");
		return false;
	}
	qwz_compat_init();
	qwz_stage_reset();	/* each attempt starts with a clean limit flag */
	qwz_hal_gate = getMainCommandGate();
	qwz_compat_set_gate(qwz_hal_gate);
	qwz_compat_set_controller(getController());
	if (!qwz_stage_ok(1, "driver setup: task queue thread (taskq_init)"))
		return false;
	static bool taskq_ready;
	if (!taskq_ready) {	/* a retried bring-up must not start systq twice */
		taskq_init();
		taskq_ready = true;
	}

	bzero(&pa, sizeof(pa));
	pa.pa_tag = device;
	pa.pa_id = device->configRead16(kIOPCIConfigVendorID) |
	    ((pcireg_t)device->configRead16(kIOPCIConfigDeviceID) << 16);
	pa.pa_workloop = getMainWorkLoop();
	pa.pa_owner = this;
	pa.pa_controller = getController();

	/*
	 * Attach runs on the IOService start thread holding the gate, so the
	 * firmware boot can tsleep() while the workloop delivers interrupts.
	 */
	if (!qwz_stage_ok(2, "take the command gate (runAction)"))
		return false;
	ret = qwz_hal_gate->runAction(attachGated, &pa, this);
	if (ret != kIOReturnSuccess)
		return false;

	ifp = &sc->sc_ic.ic_if;
	qwz_if_start = ifp->if_start;
	qwz_if_watchdog = ifp->if_watchdog;
	ifp->if_start = qwz_if_start_wrapper;
	ifp->if_watchdog = qwz_if_watchdog_wrapper;

	snprintf(fwVersion, sizeof(fwVersion), "%s", sc->qmi_target.fw_build_id);
	QWZLOG("attached: MAC %s, firmware %s", ether_sprintf(sc->sc_ic.ic_myaddr), fwVersion);
	return true;
}

IOReturn
ItlQwz::detachGated(OSObject *, void *arg0, void *, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */

	QWZLOG("detach: attached %d shutdown %d", that->attached, that->shutdown);
	if (that->sc != NULL) {
		if (that->attached && that->shutdown)
			qwz_pci_macos_quiesced_detach(that->sc);	/* chip is off already */
		else if (that->attached)
			qwz_pci_macos_detach(that->sc);
		else
			qwz_pci_macos_free(that->sc);	/* never attached */
		that->sc = NULL;
	}
	that->attached = false;
	return kIOReturnSuccess;
}

/* qwz.c */
void	qwz_core_deinit(struct qwz_softc *);

IOReturn
ItlQwz::shutdownGated(OSObject *, void *arg0, void *, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;
	struct _ifnet *ifp;

	if (!that->attached || that->shutdown)
		return kIOReturnSuccess;
	ifp = &that->sc->sc_ic.ic_if;
	qwz_logc("qwz: shutdown: stopping the interface\n");
	rw_enter_write(&that->sc->ioctl_rwl);
	ifp->if_flags &= ~IFF_UP;
	if (ifp->if_flags & IFF_RUNNING)
		qwz_stop(ifp);
	qwz_logc("qwz: shutdown: interface stopped; firmware stop + power-down\n");
	if (that->sc->fw_initialized)
		qwz_core_deinit(that->sc);	/* firmware stop + chip power-down */
	rw_exit(&that->sc->ioctl_rwl);
	that->shutdown = true;
	qwz_logc("qwz: chip powered down for system shutdown\n");
	return kIOReturnSuccess;
}

/* Called from the controller's systemWillShutdown(), before IOKit stops us. */
void
ItlQwz::shutdownHardware()
{
	if (qwz_hal_gate != NULL)
		qwz_hal_gate->runAction(shutdownGated, this);
}

void
ItlQwz::detach(IOPCIDevice *device)
{
	if (qwz_hal_gate != NULL)
		qwz_hal_gate->runAction(detachGated, this);
	qwz_compat_fini();
}

void
ItlQwz::free()
{
	super::free();
}

IOReturn
ItlQwz::enableGated(OSObject *, void *arg0, void *, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */
	struct _ifnet *ifp = &that->sc->sc_ic.ic_if;
	int err = 0;

	/* What qwz_ioctl(SIOCSIFFLAGS) does when the interface goes up. */
	rw_enter_write(&that->sc->ioctl_rwl);
	ifp->if_flags |= IFF_UP;
	if (!(ifp->if_flags & IFF_RUNNING))
		err = qwz_init(ifp);
	rw_exit(&that->sc->ioctl_rwl);
	if (err)
		QWZLOG("init failed: %d", err);
	return err ? kIOReturnError : kIOReturnSuccess;
}

IOReturn
ItlQwz::enable(IONetworkInterface *netif)
{
	return (attached && !shutdown) ? qwz_hal_gate->runAction(enableGated, this) : kIOReturnNotReady;
}

IOReturn
ItlQwz::disableGated(OSObject *, void *arg0, void *, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */
	struct _ifnet *ifp = &that->sc->sc_ic.ic_if;

	rw_enter_write(&that->sc->ioctl_rwl);
	ifp->if_flags &= ~IFF_UP;
	if (ifp->if_flags & IFF_RUNNING)
		qwz_stop(ifp);
	rw_exit(&that->sc->ioctl_rwl);
	return kIOReturnSuccess;
}

IOReturn
ItlQwz::disable(IONetworkInterface *netif)
{
	if (shutdown)
		return kIOReturnSuccess;	/* the chip is already off */
	return attached ? qwz_hal_gate->runAction(disableGated, this) : kIOReturnNotReady;
}

struct ieee80211com *
ItlQwz::get80211Controller()
{
	return sc ? &sc->sc_ic : NULL;
}

ItlDriverInfo *
ItlQwz::getDriverInfo()
{
	return this;
}

ItlDriverController *
ItlQwz::getDriverController()
{
	return this;
}

const char *
ItlQwz::getFirmwareVersion()
{
	return fwVersion;
}

int16_t
ItlQwz::getBSSNoise()
{
	return -95;	/* dBm; getNOISE() and scan results must not negate */
}

bool
ItlQwz::is5GBandSupport()
{
	return true;
}

int
ItlQwz::getTxNSS()
{
	struct ieee80211com *ic = sc ? &sc->sc_ic : NULL;
	int nss = (sc && sc->num_tx_chains > 0) ? sc->num_tx_chains : 2;

	/* Spatial streams usable towards the BSS: our chains, capped by the AP. */
	if (ic != NULL && ic->ic_bss != NULL && ic->ic_bss->ni_rx_nss > 0 &&
	    ic->ic_bss->ni_rx_nss < nss)
		nss = ic->ic_bss->ni_rx_nss;
	return nss;
}

/* The last unicast data frame received: qwz_dp_rx_h_rate in qwz.c. */
uint32_t
ItlQwz::getRxRateKbps()
{
	return sc ? sc->sc_rx_rate_kbps : 0;
}

int
ItlQwz::getRxRateMcs()
{
	return sc ? sc->sc_rx_rate_mcs : -1;
}

int
ItlQwz::getRxRateNss()
{
	return sc ? sc->sc_rx_rate_nss : 0;
}

int
ItlQwz::getRxRateSgi()
{
	return (sc && sc->sc_rx_rate_kbps != 0) ? sc->sc_rx_rate_sgi : -1;
}

/*
 * The smoothed RSSI of the associated BSS (qwz_bss_rssi_sample in qwz.c), in
 * dBm. Falls back to the caller's raw value when the average does not belong
 * to the BSS we are in now, so a stale value is never reported.
 */
int8_t
ItlQwz::getBSSRssi(int8_t raw)
{
	struct ieee80211com *ic = sc ? &sc->sc_ic : NULL;
	int x, dbm;

	if (ic == NULL || ic->ic_state != IEEE80211_S_RUN || ic->ic_bss == NULL ||
	    !sc->sc_bss_rssi_valid ||
	    !IEEE80211_ADDR_EQ(sc->sc_bss_rssi_bssid, ic->ic_bss->ni_bssid))
		return raw;
	x = sc->sc_bss_rssi_fx;		/* 1/16 dBm */
	dbm = x >= 0 ? (x + 8) / 16 : -((-x + 8) / 16);
	if (dbm >= 0 || dbm < -127)
		return raw;
	return (int8_t)dbm;
}

const char *
ItlQwz::getFirmwareName()
{
	return "qwz-wcn7850-hw2.0";
}

UInt32
ItlQwz::supportedFeatures()
{
	return kIONetworkFeatureMultiPages;
}

const char *
ItlQwz::getFirmwareCountryCode()
{
	return "ZZ";
}

uint32_t
ItlQwz::getTxQueueSize()
{
	return 256;
}

void
ItlQwz::clearScanningFlags()
{
}

IOReturn
ItlQwz::setMulticastList(IOEthernetAddress *addr, int count)
{
	return kIOReturnSuccess;
}

#ifdef AIRPORT_WCL
IOReturn
ItlQwz::setScanHomeAwayTime(uint32_t ms)
{
	if (sc == NULL)
		return kIOReturnNotReady;
	sc->sc_ic.ic_wcl_scan_away_time_ms = ms;
	return kIOReturnSuccess;
}
#endif
