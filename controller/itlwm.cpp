/*
* Copyright (C) 2020  钟先耀
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/
#include "itlwm.hpp"
#include <linux/types.h>
#include <linux/kernel.h>

#include <IOKit/IOInterruptController.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/network/IONetworkMedium.h>
#include <net/ethernet.h>
#include "qwz_log.h"
#include "qwz_sysctl.h"
#include <crypto/sha1.h>
#include <net80211/ieee80211_node.h>
#include <net80211/ieee80211_ioctl.h>

#define super IOEthernetController
OSDefineMetaClassAndStructors(itlwm, IOEthernetController)
OSDefineMetaClassAndStructors(CTimeout, OSObject)

IOWorkLoop *_fWorkloop;
IOCommandGate *_fCommandGate;

bool itlwm::init(OSDictionary *properties)
{
    fNetIf = NULL;
    fStartCall = NULL;
    fStopping = false;
    fBringUp = kBringUpIdle;
    return super::init(properties);
}

#define  PCI_MSI_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSI        0x05    /* Message Signalled Interrupts */
#define  PCI_MSIX_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSIX    0x11    /* MSI-X */
#define  PCI_MSIX_FLAGS_ENABLE    0x8000    /* MSI-X enable */
#define  PCI_MSI_FLAGS_ENABLE    0x0001    /* MSI feature enabled */

static void pciMsiSetEnable(IOPCIDevice *device, UInt8 msiCap, int enable)
{
    u16 control;
    
    control = device->configRead16(msiCap + PCI_MSI_FLAGS);
    control &= ~PCI_MSI_FLAGS_ENABLE;
    if (enable)
        control |= PCI_MSI_FLAGS_ENABLE;
    device->configWrite16(msiCap + PCI_MSI_FLAGS, control);
}

static void pciMsiXClearAndSet(IOPCIDevice *device, UInt8 msixCap, UInt16 clear, UInt16 set)
{
    u16 ctrl;
    
    ctrl = device->configRead16(msixCap + PCI_MSIX_FLAGS);
    ctrl &= ~clear;
    ctrl |= set;
    device->configWrite16(msixCap + PCI_MSIX_FLAGS, ctrl);
}

IOService* itlwm::probe(IOService *provider, SInt32 *score)
{
    bool isMatch = false;
    super::probe(provider, score);
    UInt8 msiCap;
    UInt8 msixCap;
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    if (!device) {
        return NULL;
    }
    qwz_log_set_target(device);
    qwz_log_stage("probe");
    int qwzoff = 0;
    if (PE_parse_boot_argn("-qwzoff", &qwzoff, sizeof(qwzoff))) {
        IOLog("qwz: disabled by the -qwzoff boot-arg\n");
        return NULL;
    }
    if (ItlQwz::qwz_match(device)) {
        isMatch = true;
        fHalService = new ItlQwz;
    }
    if (isMatch) {
        device->findPCICapability(PCI_CAP_ID_MSIX, &msixCap);
        if (msixCap) {
            pciMsiXClearAndSet(device, msixCap, PCI_MSIX_FLAGS_ENABLE, 0);
        }
        device->findPCICapability(PCI_CAP_ID_MSI, &msiCap);
        /*
         * QwzMac: leave MSI disabled here. IOPCIFamily programs the address
         * and enables MSI when the interrupt is registered; enabling it now
         * would let the card signal to an unprogrammed address.
         */
        if (!msiCap && !msixCap) {
            XYLog("%s No MSI cap\n", __FUNCTION__);
            fHalService->release();
            fHalService = NULL;
            return NULL;
        }
        return this;
    }
    return NULL;
}

bool itlwm::configureInterface(IONetworkInterface *netif)
{
    IONetworkData *nd;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    
    if (super::configureInterface(netif) == false) {
        XYLog("super failed\n");
        return false;
    }
    
    nd = netif->getParameter(kIONetworkStatsKey);
    if (!nd || !(fpNetStats = (IONetworkStats *)nd->getBuffer())) {
        XYLog("network statistics buffer unavailable?\n");
        return false;
    }
    ifp->netStat = fpNetStats;
    ether_ifattach(ifp, OSDynamicCast(IOEthernetInterface, netif));
    fpNetStats->collisions = 0;
#ifdef __PRIVATE_SPI__
    netif->configureOutputPullModel(fHalService->getDriverInfo()->getTxQueueSize(), 0, 0, IOEthernetInterface::kOutputPacketSchedulingModelNormal, 0);
#endif
    
    return true;
}

struct _ifnet *itlwm::getIfp()
{
    return &fHalService->get80211Controller()->ic_ac.ac_if;
}

IOEthernetInterface *itlwm::getNetworkInterface()
{
    return getIfp()->iface;
}

bool itlwm::createMediumTables(const IONetworkMedium **primary)
{
    IONetworkMedium    *medium;
    
    OSDictionary *mediumDict = OSDictionary::withCapacity(1);
    if (mediumDict == NULL) {
        XYLog("Cannot allocate OSDictionary\n");
        return false;
    }
    
    medium = IONetworkMedium::medium(kIOMediumEthernetAuto, 1000 * 1000000ULL);  /* 1 Gbps for VHT80 */
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();
    if (primary) {
        *primary = medium;
    }
    
    bool result = publishMediumDictionary(mediumDict);
    if (!result) {
        XYLog("Cannot publish medium dictionary!\n");
    }

    mediumDict->release();
    return result;
}

void itlwm::joinSSID(const char *ssid_name, const char *ssid_pwd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    
    if (strlen(ssid_pwd) == 0) {
        memset(&nwkey, 0, sizeof(ieee80211_nwkey));
        nwkey.i_wepon = IEEE80211_NWKEY_OPEN;
        nwkey.i_defkid = 0;
        memcpy(join.i_nwid, ssid_name, strlen(ssid_name));
        join.i_len = strlen(ssid_name);
        join.i_flags = IEEE80211_JOIN_NWKEY;
    } else {
        memset(&wpa, 0, sizeof(ieee80211_wpaparams));
        wpa.i_enabled = 1;
        wpa.i_ciphers = 0;
        wpa.i_groupcipher = 0;
        wpa.i_protos = IEEE80211_WPA_PROTO_WPA1 | IEEE80211_WPA_PROTO_WPA2;
        wpa.i_akms = IEEE80211_WPA_AKM_PSK | IEEE80211_WPA_AKM_8021X | IEEE80211_WPA_AKM_SHA256_PSK | IEEE80211_WPA_AKM_SHA256_8021X;
        memcpy(wpa.i_name, "zxy", strlen("zxy"));
        memset(&psk, 0, sizeof(ieee80211_wpapsk));
        memcpy(psk.i_name, "zxy", strlen("zxy"));
        psk.i_enabled = 1;
        pbkdf2_sha1(ssid_pwd, (const uint8_t*)ssid_name, strlen(ssid_name),
                    4096, psk.i_psk , 32);
        memset(&nwkey, 0, sizeof(ieee80211_nwkey));
        nwkey.i_wepon = 0;
        nwkey.i_defkid = 0;
        memset(&join, 0, sizeof(ieee80211_join));
        join.i_wpaparams = wpa;
        join.i_wpapsk = psk;
        join.i_flags = IEEE80211_JOIN_WPAPSK | IEEE80211_JOIN_ANY | IEEE80211_JOIN_WPA | IEEE80211_JOIN_8021X;
        join.i_nwkey = nwkey;
        join.i_len = strlen(ssid_name);
        memcpy(join.i_nwid, ssid_name, join.i_len);
    }
    if (ieee80211_add_ess(ic, &join) == 0)
        ic->ic_flags |= IEEE80211_F_AUTO_JOIN;
}

void itlwm::associateSSID(const char *ssid, const char *pwd)
{
    struct ieee80211com *ic = fHalService->get80211Controller();
    if (strlen(pwd) == 0) {
        memcpy(nwid.i_nwid, ssid, 32);
        nwid.i_len = strlen((char *)nwid.i_nwid);
        memset(ic->ic_des_essid, 0, IEEE80211_NWID_LEN);
        ic->ic_des_esslen = nwid.i_len;
        memcpy(ic->ic_des_essid, nwid.i_nwid, nwid.i_len);
        if (ic->ic_des_esslen > 0) {
            /* 'nwid' disables auto-join magic */
            ic->ic_flags &= ~IEEE80211_F_AUTO_JOIN;
        } else if (!TAILQ_EMPTY(&ic->ic_ess)) {
            /* '-nwid' re-enables auto-join */
            ic->ic_flags |= IEEE80211_F_AUTO_JOIN;
        }
        /* disable WPA/WEP */
        ieee80211_disable_rsn(ic);
        ieee80211_disable_wep(ic);
    } else {
        memset(&psk, 0, sizeof(psk));
        memcpy(nwid.i_nwid, ssid, 32);
        nwid.i_len = strlen((char *)nwid.i_nwid);
        memset(ic->ic_des_essid, 0, IEEE80211_NWID_LEN);
        ic->ic_des_esslen = nwid.i_len;
        memcpy(ic->ic_des_essid, nwid.i_nwid, nwid.i_len);
        if (ic->ic_des_esslen > 0) {
            /* 'nwid' disables auto-join magic */
            ic->ic_flags &= ~IEEE80211_F_AUTO_JOIN;
        } else if (!TAILQ_EMPTY(&ic->ic_ess)) {
            /* '-nwid' re-enables auto-join */
            ic->ic_flags |= IEEE80211_F_AUTO_JOIN;
        }
        /* disable WPA/WEP */
        ieee80211_disable_rsn(ic);
        ieee80211_disable_wep(ic);
        size_t passlen = strlen(pwd);
        /* Parse a WPA passphrase */
        if (passlen < 8 || passlen > 63)
            XYLog("wpakey: passphrase must be between "
                  "8 and 63 characters");
        if (nwid.i_len == 0)
            XYLog("wpakey: nwid not set");
        pbkdf2_sha1(pwd, (const uint8_t*)ssid, nwid.i_len, 4096,
                    psk.i_psk, 32);
        psk.i_enabled = 1;
        if (psk.i_enabled) {
            ic->ic_flags |= IEEE80211_F_PSK;
            memcpy(ic->ic_psk, psk.i_psk, sizeof(ic->ic_psk));
            if (ic->ic_flags & IEEE80211_F_WEPON)
                ieee80211_disable_wep(ic);
        } else {
            ic->ic_flags &= ~IEEE80211_F_PSK;
            memset(ic->ic_psk, 0, sizeof(ic->ic_psk));
        }
        memset(&wpa, 0, sizeof(wpa));
        ieee80211_ioctl_getwpaparms(ic, &wpa);
        wpa.i_enabled = psk.i_enabled;
        wpa.i_ciphers = 0;
        wpa.i_groupcipher = 0;
        wpa.i_protos = IEEE80211_WPA_PROTO_WPA1 | IEEE80211_WPA_PROTO_WPA2;
        wpa.i_akms = IEEE80211_WPA_AKM_PSK | IEEE80211_WPA_AKM_8021X | IEEE80211_WPA_AKM_SHA256_PSK | IEEE80211_WPA_AKM_SHA256_8021X;
        ieee80211_ioctl_setwpaparms(ic, &wpa);
    }
    if (ic->ic_state > IEEE80211_S_AUTH && ic->ic_bss != NULL)
        IEEE80211_SEND_MGMT(ic, ic->ic_bss, IEEE80211_FC0_SUBTYPE_DEAUTH, IEEE80211_REASON_AUTH_LEAVE);
    ieee80211_del_ess(ic, NULL, 0, 1);
    struct ieee80211_node *selbs = ieee80211_node_choose_bss(ic, 0, NULL);
    if (selbs == NULL) {
        if (ic->ic_state != IEEE80211_S_SCAN) {
            ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
        }
    } else {
        if (ic->ic_state > IEEE80211_S_AUTH) {
            ieee80211_node_join_bss(ic, selbs, 1);
            fHalService->getDriverController()->clearScanningFlags();
        } else {
            if (ic->ic_state != IEEE80211_S_SCAN) {
                ieee80211_new_state(ic, IEEE80211_S_SCAN, -1);
            }
        }
    }
}

/* debug.qwz.start=1 (qwz/qwz_sysctl.cpp) reruns the bring-up of this product. */
static bool
qwz_sysctl_bring_up(void *owner)
{
    return ((itlwm *)owner)->requestBringUp();
}

bool itlwm::start(IOService *provider)
{
    int boot_value = 0;
    if (!super::start(provider)) {
        return false;
    }
    pciNub = OSDynamicCast(IOPCIDevice, provider);
    if (!pciNub) {
        return false;
    }
    pciNub->setBusMasterEnable(true);
    pciNub->setIOEnable(true);
    pciNub->setMemoryEnable(true);
    /* QwzMac: MSI only; never let the card fall back to its INTx line. */
    pciNub->configWrite16(kIOPCIConfigCommand,
        pciNub->configRead16(kIOPCIConfigCommand) | 0x0400);
    if (pciNub->requestPowerDomainState(kIOPMPowerOn,
                                        (IOPowerConnection *) getParentEntry(gIOPowerPlane), IOPMLowestState) != IOPMNoErr) {
        return false;
    }
    if (initPCIPowerManagment(pciNub) == false) {
        super::stop(pciNub);
        return false;
    }
    if (_fWorkloop == NULL) {
        XYLog("No _fWorkloop!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)itlwm::tsleepHandler);
    if (_fCommandGate == 0) {
        XYLog("No command gate!!\n");
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    _fWorkloop->addEventSource(_fCommandGate);
    const IONetworkMedium *primaryMedium;
    if (!createMediumTables(&primaryMedium) ||
        !setCurrentMedium(primaryMedium) || !setSelectedMedium(primaryMedium)) {
        XYLog("setup medium fail\n");
        releaseAll();
        return false;
    }
    fHalService->initWithController(this, _fWorkloop, _fCommandGate);
    
    /*
     * QwzMac: the bring-up runs here in start(), as in itlwm and
     * SimpleMtkWlan. qwz_stage=<n> stops it before stage n; the driver then
     * stays loaded and idle, and `sudo sysctl debug.qwz.stage=M` followed by
     * `sudo sysctl debug.qwz.start=1` reruns it up to stage M on a thread
     * call, so the stages can be stepped through without rebooting.
     */
    fStartCall = thread_call_allocate(&itlwm::startThreadCall, this);
    qwz_sysctl_register(qwz_sysctl_bring_up, this);
    fBringUp = kBringUpRunning;
    if (!finishStart()) {
        fBringUp = kBringUpIdle;
        if (qwz_stage_hit_limit()) {
            qwz_log_stage("idle at the qwz_stage limit (no interface); sysctl debug.qwz.start=1 continues");
            return true;
        }
        qwz_log_stage("bring-up failed; the driver is unloaded");
        qwz_sysctl_unregister();
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    fBringUp = kBringUpDone;
    qwz_log_stage("running");
    return true;
}

extern int qwz_stage_sysctl;	/* qwz/qwz_compat.cpp */

bool itlwm::requestBringUp()
{
    if (fStopping || fStartCall == NULL ||
        !OSCompareAndSwap(kBringUpIdle, kBringUpRunning, &fBringUp))
        return false;
    qwz_log_file_begin();
    qwz_log("qwz: ==== bring-up requested (sysctl), stage limit %d ====\n", qwz_stage_sysctl);
    qwz_log("qwz: entering the thread call\n");
    qwz_log_file_end();
    retain();
    if (thread_call_enter(fStartCall))
        release();
    return true;
}

void itlwm::startThreadCall(thread_call_param_t owner, thread_call_param_t)
{
    itlwm *that = (itlwm *)owner;

    qwz_log_file_begin();
    if (_fWorkloop != NULL)
        qwz_log_file_allow(_fWorkloop->getThread());
    qwz_log("qwz: thread call running\n");
    if (!that->fStopping && that->finishStart()) {
        that->fBringUp = kBringUpDone;
        qwz_log_stage("running");
    } else {
        that->fBringUp = kBringUpIdle;
        qwz_log_stage(qwz_stage_hit_limit() ?
            "idle at the debug.qwz.stage limit; raise it and start again" :
            "bring-up failed; the driver stays idle, start again to retry");
    }
    qwz_log("qwz: thread call done\n");
    if (!qwz_sysctl_logfile_on())
        qwz_log_file_end();
    that->release();
}

bool itlwm::finishStart()
{
    int boot_value = 0;

    if (!fHalService->attach(pciNub)) {
        XYLog("attach fail\n");
        return false;
    }
    if (PE_parse_boot_argn("-novht", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOVHT;
    if (PE_parse_boot_argn("-noht40", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOHT40;
    /*
     * QwzMac: the watchdog must exist before the interface does. Attaching
     * the interface registers it, and the network stack may call enable()
     * at once, on another thread; enable() uses watchdogTimer. The bring-up
     * froze (NULL dereference) when enable() won that race.
     */
    fWatchdogWorkLoop = IOWorkLoop::workLoop();
    watchdogTimer = fWatchdogWorkLoop == NULL ? NULL :
        IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &itlwm::watchdogAction));
    if (watchdogTimer == NULL) {
        XYLog("init watchdog fail\n");
        fHalService->detach(pciNub);
        return false;
    }
    fWatchdogWorkLoop->addEventSource(watchdogTimer);
    setLinkStatus(kIONetworkLinkValid);
    if (!attachInterface((IONetworkInterface **)&fNetIf, true)) {
        XYLog("attach to interface fail\n");
        fHalService->detach(pciNub);
        fNetIf = NULL;
        return false;
    }
    OSObject *wifiEntryObject = NULL;
    OSDictionary *wifiEntry = NULL;
    OSString *entryKey = NULL;
    OSDictionary *wifiDict = OSDynamicCast(OSDictionary, getProperty("WiFiConfig"));
    if (wifiDict != NULL) {
        OSCollectionIterator *iterator = OSCollectionIterator::withCollection(wifiDict);
        while ((wifiEntryObject = iterator->getNextObject())) {
            entryKey = OSDynamicCast(OSString, wifiEntryObject);
            if (entryKey == NULL) {
                continue;
            }
            wifiEntry = OSDynamicCast(OSDictionary, wifiDict->getObject(entryKey));
            if (wifiEntry == NULL) {
                continue;
            }
            OSString *ssidObj = OSDynamicCast(OSString, wifiEntry->getObject("ssid"));
            OSString *pwdObj = OSDynamicCast(OSString, wifiEntry->getObject("password"));
            if (ssidObj == NULL || pwdObj == NULL || ssidObj->isEqualTo("")) {
                continue;
            }
            
            joinSSID(ssidObj->getCStringNoCopy(), pwdObj->getCStringNoCopy());
        }
        iterator->release();
    }
    if (TAILQ_EMPTY(&fHalService->get80211Controller()->ic_ess)) {
        fHalService->get80211Controller()->ic_flags |= IEEE80211_F_AUTO_JOIN;
    }
    registerService();
    fNetIf->registerService();
    return true;
}

void itlwm::watchdogAction(IOTimerEventSource *timer)
{
    struct _ifnet *ifp = getIfp();
    (*ifp->if_watchdog)(ifp);
    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
}

const OSString * itlwm::newVendorString() const
{
    return OSString::withCString("Apple");
}

const OSString * itlwm::newModelString() const
{
    return OSString::withCString("Qualcomm WCN7850 Wireless Card");
}

bool itlwm::initPCIPowerManagment(IOPCIDevice *provider)
{
    UInt16 reg16;

    reg16 = provider->configRead16(kIOPCIConfigCommand);

    reg16 |= ( kIOPCICommandBusMaster       |
               kIOPCICommandMemorySpace     |
               kIOPCICommandMemWrInvalidate );

    reg16 &= ~kIOPCICommandIOSpace;  // disable I/O space

    provider->configWrite16( kIOPCIConfigCommand, reg16 );
    provider->findPCICapability(kIOPCIPowerManagementCapability,
                                &pmPCICapPtr);
    if (pmPCICapPtr) {
        UInt16 pciPMCReg = provider->configRead32( pmPCICapPtr ) >> 16;
        if (pciPMCReg & kPCIPMCPMESupportFromD3Cold) {
            magicPacketSupported = true;
        }
        provider->configWrite16((pmPCICapPtr + 4), 0x8000 );
        IOSleep(10);
    }
    return true;
}

bool itlwm::createWorkLoop()
{
    _fWorkloop = IOWorkLoop::workLoop();
    return _fWorkloop != 0;
}

IOWorkLoop *itlwm::getWorkLoop() const
{
    return _fWorkloop;
}

IOReturn itlwm::selectMedium(const IONetworkMedium *medium) {
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

void itlwm::systemWillShutdown(IOOptionBits specifier)
{
    if ((specifier == kIOMessageSystemWillPowerOff ||
         specifier == kIOMessageSystemWillRestart) && fHalService != NULL) {
        qwz_log("qwz: system shutdown: turning the radio off\n");
        fHalService->shutdownHardware();
    }
    super::systemWillShutdown(specifier);
}

void itlwm::stop(IOService *provider)
{
    XYLog("%s\n", __FUNCTION__);
    fStopping = true;
    qwz_sysctl_unregister();
    if (fStartCall != NULL) {
        if (thread_call_cancel(fStartCall))
            release();
        thread_call_free(fStartCall);
        fStartCall = NULL;
    }
    super::stop(provider);
    if (fNetIf != NULL) {
        struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
        setLinkStatus(kIONetworkLinkValid);
        fHalService->detach(pciNub);
        ether_ifdetach(ifp);
        detachInterface(fNetIf, true);
        OSSafeReleaseNULL(fNetIf);
    } else if (fHalService != NULL) {
        fHalService->detach(pciNub);
    }
    releaseAll();
}

void itlwm::releaseAll()
{
    if (fHalService) {
        fHalService->release();
        fHalService = NULL;
    }
    if (_fWorkloop) {
        if (_fCommandGate) {
            IOCommandGate *gate = _fCommandGate;
            _fCommandGate = NULL;   /* taskq threads test this before runAction */
            _fWorkloop->removeEventSource(gate);
            gate->release();
        }
        if (fWatchdogWorkLoop && watchdogTimer) {
            watchdogTimer->cancelTimeout();
            fWatchdogWorkLoop->removeEventSource(watchdogTimer);
            watchdogTimer->release();
            watchdogTimer = NULL;
            fWatchdogWorkLoop->release();
            fWatchdogWorkLoop = NULL;
        }
        _fWorkloop->release();
        _fWorkloop = NULL;
    }
    unregistPM();
}

void itlwm::free()
{
    XYLog("%s\n", __FUNCTION__);
    if (fHalService != NULL) {
        fHalService->release();
        fHalService = NULL;
    }
    super::free();
}

IOReturn itlwm::enable(IONetworkInterface *netif)
{
    IOReturn ret;

    super::enable(netif);
    _fCommandGate->enable();
    ret = fHalService->enable(netif);
    if (watchdogTimer != NULL) {
        watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
        watchdogTimer->enable();
    }
    return kIOReturnSuccess;
}

IOReturn itlwm::disable(IONetworkInterface *netif)
{
    super::disable(netif);
    if (watchdogTimer != NULL) {
        watchdogTimer->cancelTimeout();
        watchdogTimer->disable();
    }
    fHalService->disable(netif);
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

bool itlwm::
setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium, UInt64 speed, OSData * data)
{
    _ifnet *ifq = &fHalService->get80211Controller()->ic_ac.ac_if;
    bool ret = super::setLinkStatus(status, activeMedium, speed, data);
    if (fNetIf) {
        if (status & kIONetworkLinkActive) {
#ifdef __PRIVATE_SPI__
            fNetIf->startOutputThread();
#endif
        } else if (!(status & kIONetworkLinkNoNetworkChange)) {
#ifdef __PRIVATE_SPI__
            fNetIf->stopOutputThread();
            fNetIf->flushOutputQueue();
#endif
            ifq_flush(&ifq->if_snd);
            mq_purge(&fHalService->get80211Controller()->ic_mgtq);
        }
    }
    return ret;
}

IOReturn itlwm::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (IEEE80211_ADDR_EQ(etheranyaddr, fHalService->get80211Controller()->ic_myaddr)) {
        return kIOReturnError;
    } else {
        IEEE80211_ADDR_COPY(addrP, fHalService->get80211Controller()->ic_myaddr);
        return kIOReturnSuccess;
    }
}

IOReturn itlwm::setHardwareAddress(const IOEthernetAddress *addrP)
{
    if (!fNetIf || !addrP)
        return kIOReturnError;
    if_setlladdr(&fHalService->get80211Controller()->ic_ac.ac_if, addrP->bytes);
    if (fHalService->get80211Controller()->ic_state > IEEE80211_S_INIT) {
        fHalService->disable(fNetIf);
        fHalService->enable(fNetIf);
    }
    return kIOReturnSuccess;
}

#ifdef __PRIVATE_SPI__
IOReturn itlwm::outputStart(IONetworkInterface *interface, IOOptionBits options)
{
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    mbuf_t m = NULL;
    if (ifq_is_oactive(&ifp->if_snd))
        return kIOReturnNoResources;
    while (kIOReturnSuccess == interface->dequeueOutputPackets(1, &m)) {
        if (outputPacket(m, NULL)!= kIOReturnOutputSuccess ||
            ifq_is_oactive(&ifp->if_snd))
            return kIOReturnNoResources;
    }
    return kIOReturnSuccess;
}
#endif

UInt32 itlwm::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;

    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE) {
            freePacket(m);
        }
        return kIOReturnOutputDropped;
    }
    if (m == NULL) {
        XYLog("%s m==NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR) ){
        XYLog("%s pkthdr is NULL!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    if (mbuf_type(m) == MBUF_TYPE_FREE) {
        XYLog("%s mbuf is FREE!!\n", __FUNCTION__);
        ifp->netStat->outputErrors++;
        ret = kIOReturnOutputDropped;
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return ret;
}

UInt32 itlwm::getFeatures() const
{
    return fHalService->getDriverInfo()->supportedFeatures();
}

IOReturn itlwm::setPromiscuousMode(IOEnetPromiscuousMode mode)
{
    return kIOReturnSuccess;
}

IOReturn itlwm::setMulticastMode(IOEnetMulticastMode mode)
{
    return kIOReturnSuccess;
}

IOReturn itlwm::setMulticastList(IOEthernetAddress* addr, UInt32 len)
{
    return fHalService->getDriverController()->setMulticastList(addr, len);
}

IOReturn itlwm::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn    rtn = kIOReturnSuccess;
    if (group == gIOEthernetWakeOnLANFilterGroup && magicPacketSupported) {
        *filters = kIOEthernetWakeOnMagicPacket;
    } else if (group == gIONetworkFilterGroup) {
        *filters = kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
    } else {
        rtn = IOEthernetController::getPacketFilters(group, filters);
    }
    return rtn;
}

IOReturn itlwm::
tsleepHandler(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
    itlwm* dev = OSDynamicCast(itlwm, owner);
    if (dev == 0)
        return kIOReturnError;
    
    if (arg1 == 0) {
        if (_fCommandGate->commandSleep(arg0, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    } else {
        AbsoluteTime deadline;
        clock_interval_to_deadline((*(int*)arg1), kNanosecondScale, reinterpret_cast<uint64_t*> (&deadline));
        if (_fCommandGate->commandSleep(arg0, deadline, THREAD_INTERRUPTIBLE) == THREAD_AWAKENED)
            return kIOReturnSuccess;
        else
            return kIOReturnTimeout;
    }
}
