//
//  AirportItlwmV2.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "AirportItlwmV2.hpp"
#include <sys/_netstat.h>
#include <crypto/sha1.h>
#include <net80211/ieee80211_priv.h>
#include <net80211/ieee80211_var.h>

#include "AirportItlwmSkywalkInterface.hpp"
#include "qwz_log.h"
#include <kern/clock.h>
#include "IOPCIEDeviceWrapper.hpp"

#define super IO80211Controller
OSDefineMetaClassAndStructors(AirportItlwm, IO80211Controller);
OSDefineMetaClassAndStructors(CTimeout, OSObject)

IO80211WorkQueue *_fWorkloop;
IOCommandGate *_fCommandGate;

void AirportItlwm::checkInterfaceRole(const char *when, bool restore)
{
    if (!fNetIf)
        return;
    OSString *role = OSDynamicCast(OSString, fNetIf->getProperty("IO80211InterfaceRole"));
    XYLog("interface role property %s: %s\n", when, role ? role->getCStringNoCopy() : "missing");
    if (!role && restore && fNetIf->getInterfaceRole() == 1) {
        fNetIf->setProperty("IO80211InterfaceRole", "Infrastructure");
        XYLog("interface role property put back\n");
    }
}

void AirportItlwm::roleTimerFired(OSObject *owner, IOTimerEventSource *sender)
{
    static const uint32_t delays[] = {2000, 7000, 20000};
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);
    if (!self)
        return;
    self->checkInterfaceRole("timer", true);
    if (self->roleChecks < sizeof(delays) / sizeof(delays[0]))
        sender->setTimeoutMS(delays[self->roleChecks++]);
}

void AirportItlwm::releaseAll()
{
    quiescePM();
#ifdef AIRPORT_WCL
    if (fNetIf)
        static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->stopWCL();
    OSSafeReleaseNULL(driverFaultReporter);
    OSSafeReleaseNULL(driverCoreFaultReporter);
    OSSafeReleaseNULL(driverFaultStream);
    OSSafeReleaseNULL(driverLogger);
    // Stop only successfully started pipes, while their owner workloop exists.
    if (driverSnapshotsPipeStarted) {
        driverSnapshotsPipe->CCPipe::stopPipe();
        driverSnapshotsPipeStarted = false;
    }
    if (driverDataPathPipeStarted) {
        driverDataPathPipe->CCPipe::stopPipe();
        driverDataPathPipeStarted = false;
    }
    if (driverLogPipeStarted) {
        driverLogPipe->CCPipe::stopPipe();
        driverLogPipeStarted = false;
    }
#endif
    OSSafeReleaseNULL(driverLogPipe);
    OSSafeReleaseNULL(driverDataPathPipe);
    OSSafeReleaseNULL(driverSnapshotsPipe);
#ifndef AIRPORT_WCL
    OSSafeReleaseNULL(driverFaultReporter);
#endif
    if (fHalService) {
        fHalService->release();
        fHalService = NULL;
    }
    if (_fWorkloop) {
        if (_fCommandGate) {
//            _fCommandGate->disable();
            _fWorkloop->removeEventSource(_fCommandGate);
            _fCommandGate->release();
            _fCommandGate = NULL;
        }
        if (scanSource) {
            scanSource->cancelTimeout();
            scanSource->disable();
            _fWorkloop->removeEventSource(scanSource);
            scanSource->release();
            scanSource = NULL;
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

void AirportItlwm::
eventHandler(struct ieee80211com *ic, int msgCode, void *data)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, ic->ic_ac.ac_if.controller);
#ifdef AIRPORT_WCL
    if (!that)
        return;
#endif
    IO80211SkywalkInterface *interface = that->fNetIf;
    if (!interface)
        return;
#ifdef AIRPORT_WCL
    static_cast<AirportItlwmSkywalkInterface *>(interface)->handleWCLEvent(msgCode, data);
#else
    switch (msgCode) {
        case IEEE80211_EVT_COUNTRY_CODE_UPDATE:
            interface->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0, 0);
            break;
        case IEEE80211_EVT_STA_ASSOC_DONE:
            interface->postMessage(APPLE80211_M_ASSOC_DONE, NULL, 0, 0);
            break;
        case IEEE80211_EVT_STA_DEAUTH:
            interface->postMessage(APPLE80211_M_DEAUTH_RECEIVED, NULL, 0, 0);
            break;
        default:
            break;
    }
#endif
}

void AirportItlwm::watchdogAction(IOTimerEventSource *timer)
{
    if (fHalService == NULL || pmStopping)
        return;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    (*ifp->if_watchdog)(ifp);
    checkDataStall();

    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
}

void AirportItlwm::fakeScanDone(OSObject *owner, IOTimerEventSource *sender)
{
#ifndef AIRPORT_WCL
    UInt32 msg = 0;
#endif
    AirportItlwm *that = (AirportItlwm *)owner;
#ifdef AIRPORT_WCL
    if (that->fNetIf)
        static_cast<AirportItlwmSkywalkInterface *>(that->fNetIf)->completeWCLScan();
#else
    that->fNetIf->postMessage(APPLE80211_M_SCAN_DONE, &msg, 4, 0);
#endif
}

bool AirportItlwm::init(OSDictionary *properties)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    bool ret = super::init(properties);
    awdlSyncEnable = true;
    power_state = 0;
    memset(geo_location_cc, 0, sizeof(geo_location_cc));
    return ret;
}

IOService* AirportItlwm::probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    IOPCIEDeviceWrapper *wrapper = OSDynamicCast(IOPCIEDeviceWrapper, provider);
    if (!wrapper) {
        XYLog("%s Not a IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
    pciNub = wrapper->pciNub;
    fHalService = wrapper->fHalService;
    if (!pciNub || !fHalService) {
        XYLog("%s Not a valid IOPCIEDeviceWrapper instance\n", __FUNCTION__);
        return NULL;
    }
    return super::probe(provider, score);
}

#define LOWER32(x)  ((uint64_t)(x) & 0xffffffff)
#define HIGHER32(x) ((uint64_t)(x) >> 32)

bool AirportItlwm::
initCCLogs()
{
    CCPipeOptions driverLogOptions = { 0 };
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 1;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "Itlwm_Logs", sizeof(driverLogOptions.file_name));
    snprintf(driverLogOptions.name, sizeof(driverLogOptions.name), "wlan%d", 0);
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = 0x1000000;
    driverLogOptions.pad10 = 2;
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverLogPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DriverLogs", &driverLogOptions);
    XYLog("%s driverLogPipeRet %d\n", __FUNCTION__, driverLogPipe != NULL);
#ifdef AIRPORT_WCL
    if (!driverLogPipe || !getWorkLoop())
        return false;
    // The factory only initializes the pipe. startPipe creates its event sources.
    // Qualify the native method: do not dispatch through the legacy CCPipe header.
    driverLogPipeStarted = driverLogPipe->CCPipe::startPipe();
    if (!driverLogPipeStarted)
        return false;
    CCStreamOptions loggerOptions = {};
    CCStream *logger = CCStream::withPipeAndName(driverLogPipe, "AirportItlwm", &loggerOptions);
    driverLogger = OSDynamicCast(CCLogStream, logger);
    if (!driverLogger) {
        OSSafeReleaseNULL(logger);
        return false;
    }
#endif

    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0;
    driverLogOptions.log_data_type = 0;
    driverLogOptions.pipe_size = 0x200000;
    driverLogOptions.min_log_size_notify = 0xccccc;
    driverLogOptions.notify_threshold = 1000;
    strlcpy(driverLogOptions.file_name, "AppleBCMWLAN_Datapath", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pad9 = HIGHER32(0x202800000);
    driverLogOptions.pad10 = LOWER32(0x202800000);
    driverLogOptions.file_options = 0;
    driverLogOptions.log_policy = 0;
    driverDataPathPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "DatapathEvents", &driverLogOptions);
    XYLog("%s driverDataPathPipeRet %d\n", __FUNCTION__, driverDataPathPipe != NULL);
#ifdef AIRPORT_WCL
    if (!driverDataPathPipe)
        return false;
    driverDataPathPipeStarted = driverDataPathPipe->CCPipe::startPipe();
    if (!driverDataPathPipeStarted)
        return false;
#endif

    memset(&driverLogOptions, 0, sizeof(driverLogOptions));
    driverLogOptions.pipe_type = 0x200000001;
    driverLogOptions.log_data_type = 2;
    strlcpy(driverLogOptions.file_name, "StateSnapshots", sizeof(driverLogOptions.file_name));
    strlcpy(driverLogOptions.name, "0", sizeof(driverLogOptions.name));
    strlcpy(driverLogOptions.directory_name, "WiFi", sizeof(driverLogOptions.directory_name));
    driverLogOptions.pipe_size = 128;
    driverSnapshotsPipe = CCPipe::withOwnerNameCapacity(this, "com.zxystd.AirportItlwm", "StateSnapshots", &driverLogOptions);
    XYLog("%s driverSnapshotsPipeRet %d\n", __FUNCTION__, driverSnapshotsPipe != NULL);
#ifdef AIRPORT_WCL
    if (!driverSnapshotsPipe)
        return false;
    driverSnapshotsPipeStarted = driverSnapshotsPipe->CCPipe::startPipe();
    if (!driverSnapshotsPipeStarted)
        return false;
#endif

    CCStreamOptions faultReportOptions = { 0 };
    faultReportOptions.stream_type = 1;
    faultReportOptions.console_level = 0xFFFFFFFFFFFFFFFF;
#ifdef AIRPORT_WCL
    driverFaultStream = CCStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &faultReportOptions);
    CCDataStream *dataStream = OSDynamicCast(CCDataStream, driverFaultStream);
    if (!dataStream || !_fWorkloop) {
        return false;
    }
    driverCoreFaultReporter = CCFaultReporter::withStreamWorkloop(dataStream, _fWorkloop);
    if (!driverCoreFaultReporter) {
        return false;
    }
    driverFaultReporter = IO80211FaultReporter::allocWithParams(driverCoreFaultReporter);
    if (!OSDynamicCast(CommonFaultReporter, driverFaultReporter)) {
        return false;
    }
#else
    driverFaultReporter = CCStream::withPipeAndName(driverSnapshotsPipe, "FaultReporter", &faultReportOptions);
#endif
    XYLog("%s driverFaultReporterRet %d\n", __FUNCTION__, driverFaultReporter != NULL);
    return driverLogPipe && driverDataPathPipe && driverSnapshotsPipe && driverFaultReporter;
}

bool AirportItlwm::isCommandProhibited(int command)
{
    static uint8_t seen[1024 / 8];

    if (command >= 0 && command < 1024 && !(seen[command / 8] & (1 << (command % 8)))) {
        seen[command / 8] |= 1 << (command % 8);
        qwz_log("airport: ioctl %d (%s) first asked of the driver\n", command, convertApple80211IOCTLToString(command));
    }
    return false;
}

bool AirportItlwm::start(IOService *provider)
{
#if __IO80211_TARGET == __MAC_15_2
    if (version_major != 24 || version_minor < 2)
        return false;
#endif
    XYLog("%s\n", __PRETTY_FUNCTION__);
    struct IOSkywalkEthernetInterface::RegistrationInfo registInfo;
    int boot_value = 0;

    UInt8 builtIn = 0;
    setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
    setProperty("DriverKitDriver", kOSBooleanFalse);
#ifdef AIRPORT_WCL
    qwz_log("qwz: trace: AirportItlwm::start: initCCLogs\n");
    if (!initCCLogs()) {
        releaseAll();
        return false;
    }
#endif
    qwz_log("qwz: trace: AirportItlwm::start: IO80211Controller::start\n");
    if (!super::start(provider)) {
#ifdef AIRPORT_WCL
        releaseAll();
#endif
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: super started; PCI enables\n");
    pciNub->setBusMasterEnable(true);
    pciNub->setIOEnable(true);
    pciNub->setMemoryEnable(true);
    /* QwzMac: MSI only; never let the card fall back to its INTx line. */
    pciNub->configWrite16(kIOPCIConfigCommand,
        pciNub->configRead16(kIOPCIConfigCommand) | 0x0400);
    if (pciNub->requestPowerDomainState(kIOPMPowerOn,
                                        (IOPowerConnection *) getParentEntry(gIOPowerPlane), IOPMLowestState) != IOPMNoErr) {
        super::stop(provider);
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: initPCIPowerManagment\n");
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
    qwz_log("qwz: trace: AirportItlwm::start: command gate\n");
    _fCommandGate = IOCommandGate::commandGate(this, (IOCommandGate::Action)AirportItlwm::tsleepHandler);
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
    qwz_log("qwz: trace: AirportItlwm::start: initWithController\n");
    fHalService->initWithController(this, _fWorkloop, _fCommandGate);
    fHalService->get80211Controller()->ic_event_handler = eventHandler;

    qwz_log("qwz: pm: sleep/wake build 4r: quiesce before sleep, single-owner wake recovery, Wi-Fi off/on chip reset, data-stall restart\n");
    if (PE_parse_boot_argn("-qwznowakefix", &boot_value, sizeof(boot_value))) {
        noWakeFix = true;
        qwz_log("qwz: pm: -qwznowakefix: sleep/wake keeps the old interface-down/up handlers\n");
        qwz_log("qwz: pm: -qwznowakefix: no wake ordering, Wi-Fi off/on reset or data-stall restart either\n");
    }
    if (PE_parse_boot_argn("-qwznoradioreset", &boot_value, sizeof(boot_value))) {
        noRadioReset = true;
        qwz_log("qwz: pm: -qwznoradioreset: Wi-Fi off/on only stops and starts the interface\n");
    }
    if (PE_parse_boot_argn("-qwznostallfix", &boot_value, sizeof(boot_value))) {
        noStallFix = true;
        qwz_log("qwz: pm: -qwznostallfix: no data-stall restart\n");
    }
    if (PE_parse_boot_argn("-qwzstallsbr", &boot_value, sizeof(boot_value))) {
        stallSbr = true;
        qwz_log("qwz: pm: -qwzstallsbr: the second data-stall restart also does a secondary bus reset\n");
    }
    if (PE_parse_boot_argn("-novht", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOVHT;
    if (PE_parse_boot_argn("-noht40", &boot_value, sizeof(boot_value)))
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_NOHT40;

#ifdef AIRPORT_WCL
    int he_enabled = 0;
    if (PE_parse_boot_argn("itlwm_he", &he_enabled, sizeof(he_enabled)) && he_enabled == 1)
        fHalService->get80211Controller()->ic_userflags |= IEEE80211_F_HEON;
#endif

    qwz_log("qwz: trace: AirportItlwm::start: HAL attach\n");
    if (!fHalService->attach(pciNub)) {
        qwz_log("qwz: trace: AirportItlwm::start: HAL attach failed; stopping\n");
        super::stop(pciNub);
        qwz_log("qwz: trace: AirportItlwm::start: super stopped; releaseAll\n");
        releaseAll();
        qwz_log("qwz: trace: AirportItlwm::start: released\n");
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: HAL attached; watchdog\n");

    fWatchdogWorkLoop = IOWorkLoop::workLoop();
    if (fWatchdogWorkLoop == NULL) {
        XYLog("init watchdog workloop fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    watchdogTimer = IOTimerEventSource::timerEventSource(this, OSMemberFunctionCast(IOTimerEventSource::Action, this, &AirportItlwm::watchdogAction));
    if (!watchdogTimer) {
        XYLog("init watchdog fail\n");
        fHalService->detach(pciNub);
        super::stop(pciNub);
        releaseAll();
        return false;
    }
    fWatchdogWorkLoop->addEventSource(watchdogTimer);
    scanSource = IOTimerEventSource::timerEventSource(this, &fakeScanDone);
#ifdef AIRPORT_WCL
    if (!scanSource || _fWorkloop->addEventSource(scanSource) != kIOReturnSuccess) {
        super::stop(provider);
        releaseAll();
        return false;
    }
#else
    _fWorkloop->addEventSource(scanSource);
#endif
    scanSource->enable();

    qwz_log("qwz: trace: AirportItlwm::start: new AirportItlwmSkywalkInterface\n");
    fNetIf = new AirportItlwmSkywalkInterface;
#ifdef AIRPORT_WCL
    if (!fNetIf || !static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->init(this)) {
#else
    if (!fNetIf->init(this)) {
#endif
        XYLog("Skywalk interface init fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: interface init done; setInterfaceRole\n");
    fNetIf->setInterfaceRole(1);
    checkInterfaceRole("after setInterfaceRole", false);
    fNetIf->setInterfaceId(1);

#ifndef AIRPORT_WCL
    if (!initCCLogs()) {
        XYLog("CCLog init fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
#endif
    checkInterfaceRole("before attach", false);
    if (!fNetIf->attach(this)) {
        XYLog("attach to service fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: attachInterface\n");
    if (!attachInterface(fNetIf, this)) {
        XYLog("attach to interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    qwz_log("qwz: trace: AirportItlwm::start: attach bsd interface\n");
    if (!IONetworkController::attachInterface((IONetworkInterface **)&bsdInterface,
#ifdef AIRPORT_WCL
                                             false
#else
                                             true
#endif
                                             )) {
        XYLog("attach to IONetworkController interface fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    memset(&registInfo, 0, sizeof(registInfo));
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        XYLog("initRegistrationInfo fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
#ifdef AIRPORT_WCL
    fNetIf->mExpansionData->fRegistrationInfo = (struct IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkNetworkInterface::RegistrationInfo));
    fNetIf->mExpansionData2->fRegistrationInfo = (struct IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkEthernetInterface::RegistrationInfo));
    if (!fNetIf->mExpansionData->fRegistrationInfo || !fNetIf->mExpansionData2->fRegistrationInfo) {
        super::stop(provider);
        releaseAll();
        return false;
    }
    memcpy(fNetIf->mExpansionData->fRegistrationInfo, &registInfo, sizeof(registInfo));
    memcpy(fNetIf->mExpansionData2->fRegistrationInfo, &registInfo, sizeof(registInfo));
    if (fNetIf->getInterfaceRole() == 1)
        fNetIf->deferBSDAttach(true);
    checkInterfaceRole("before interface start", false);
    if (!fNetIf->start(this)) {
        super::stop(provider);
        releaseAll();
        return false;
    }

#else
    if (!fNetIf->initRegistrationInfo(&registInfo, 1, sizeof(registInfo))) {
        XYLog("initRegistrationInfo fail\n");
        super::stop(provider);
        releaseAll();
        return false;
    }
    fNetIf->mExpansionData->fRegistrationInfo = (struct IOSkywalkNetworkInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkNetworkInterface::RegistrationInfo));
    fNetIf->mExpansionData2->fRegistrationInfo = (struct IOSkywalkEthernetInterface::RegistrationInfo *)IOMalloc(sizeof(struct IOSkywalkEthernetInterface::RegistrationInfo));
    memcpy(fNetIf->mExpansionData->fRegistrationInfo, &registInfo, sizeof(registInfo));
    memcpy(fNetIf->mExpansionData2->fRegistrationInfo, &registInfo, sizeof(registInfo));
    if (fNetIf->getInterfaceRole() == 1)
        fNetIf->deferBSDAttach(true);
    fNetIf->start(this);

#endif
    checkInterfaceRole("after interface start", true);
    setLinkStatus(kIONetworkLinkValid);
    if (TAILQ_EMPTY(&fHalService->get80211Controller()->ic_ess))
        fHalService->get80211Controller()->ic_flags |= IEEE80211_F_AUTO_JOIN;
#ifdef AIRPORT_WCL
    qwz_log("qwz: trace: AirportItlwm::start: registerService\n");
    bsdInterface->registerService();
#endif
    registerService();
    checkInterfaceRole("after registerService", true);
    roleChecks = 0;
    roleTimer = IOTimerEventSource::timerEventSource(this, &AirportItlwm::roleTimerFired);
    if (roleTimer && _fWorkloop->addEventSource(roleTimer) == kIOReturnSuccess)
        roleTimer->setTimeoutMS(1000);
    else
        OSSafeReleaseNULL(roleTimer);
    return true;
}

/*
 * Without this the firmware keeps running (and the driver's stop() tears it
 * down late, with parts of the system gone) through power-off, which froze
 * the shutdown. Quiet the chip while everything is still alive.
 */
void AirportItlwm::systemWillShutdown(IOOptionBits specifier)
{
    if ((specifier == kIOMessageSystemWillPowerOff ||
         specifier == kIOMessageSystemWillRestart) && fHalService != NULL) {
        qwz_logc("qwz: ==== system shutdown (0x%x): turning the radio off ====\n", (unsigned)specifier);
        fHalService->shutdownHardware();
        qwz_logc("qwz: shutdown: radio off; calling super\n");
    }
    super::systemWillShutdown(specifier);
    qwz_logc("qwz: shutdown: super::systemWillShutdown returned\n");
}

void AirportItlwm::stop(IOService *provider)
{
    qwz_logc("qwz: stop: enter\n");
    quiescePM();
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    if (roleTimer) {
        roleTimer->cancelTimeout();
        _fWorkloop->removeEventSource(roleTimer);
        OSSafeReleaseNULL(roleTimer);
    }
    qwz_logc("qwz: stop: role timer gone; stopWCL\n");
#ifdef AIRPORT_WCL
    if (fNetIf)
        static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->stopWCL();
#endif
    qwz_logc("qwz: stop: super::stop\n");
    super::stop(provider);
    qwz_logc("qwz: stop: disableAdapter\n");
    disableAdapter(bsdInterface);
    setLinkStatus(kIONetworkLinkValid);
    qwz_logc("qwz: stop: HAL detach\n");
    fHalService->detach(pciNub);
    qwz_logc("qwz: stop: ether_ifdetach\n");
    ether_ifdetach(ifp);
    qwz_logc("qwz: stop: detachInterface\n");
    detachInterface(fNetIf, true);
    OSSafeReleaseNULL(fNetIf);
    qwz_logc("qwz: stop: releaseAll\n");
    releaseAll();
    qwz_logc("qwz: stop: done\n");
}

void AirportItlwm::free()
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (fHalService != NULL) {
        fHalService->release();
        fHalService = NULL;
    }
    if (syncFrameTemplate != NULL && syncFrameTemplateLength > 0) {
        IOFree(syncFrameTemplate, syncFrameTemplateLength);
        syncFrameTemplateLength = 0;
        syncFrameTemplate = NULL;
    }
    if (roamProfile != NULL) {
        IOFree(roamProfile, sizeof(struct apple80211_roam_profile_band_data));
        roamProfile = NULL;
    }
    if (btcProfile != NULL) {
        IOFree(btcProfile, sizeof(struct apple80211_btc_profiles_data));
        btcProfile = NULL;
    }
    super::free();
}

bool AirportItlwm::createWorkQueue()
{
    XYLog("%s %d\n", __FUNCTION__, _fWorkloop != 0);
    return _fWorkloop != 0;
}

IO80211WorkQueue *AirportItlwm::getWorkQueue()
#ifdef AIRPORT_WCL
const
#endif
{
    return _fWorkloop;
}

#ifdef AIRPORT_WCL
CommonFaultReporter *AirportItlwm::getFaultReporterFromDriver()
#else
void *AirportItlwm::getFaultReporterFromDriver()
#endif
{
    return driverFaultReporter;
}

IOReturn AirportItlwm::enable(IO80211SkywalkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    super::enable(netif);
    _fCommandGate->enable();
    if (power_state)
        enableAdapter(bsdInterface);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::disable(IO80211SkywalkInterface *netif)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    super::disable(netif);
    setLinkStatus(kIONetworkLinkValid);
    return kIOReturnSuccess;
}

bool AirportItlwm::configureInterface(IONetworkInterface *netif)
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

IONetworkInterface *AirportItlwm::createInterface()
{
    AirportItlwmEthernetInterface *netif = new AirportItlwmEthernetInterface;
    if (!netif)
        return NULL;
    if (!netif->initWithSkywalkInterfaceAndProvider(this, fNetIf)) {
        netif->release();
        return NULL;
    }
    return netif;
}

bool AirportItlwm::createMediumTables(const IONetworkMedium **primary)
{
    IONetworkMedium    *medium;

    OSDictionary *mediumDict = OSDictionary::withCapacity(2);
    if (mediumDict == NULL) {
        XYLog("Cannot allocate OSDictionary\n");
        return false;
    }

    medium = IONetworkMedium::medium(kIOMediumIEEE80211, 54000000);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();
    if (primary) {
        *primary = medium;
    }
    medium = IONetworkMedium::medium(kIOMediumIEEE80211None, 0);
    IONetworkMedium::addMedium(mediumDict, medium);
    medium->release();

    bool result = publishMediumDictionary(mediumDict);
    if (!result) {
        XYLog("Cannot publish medium dictionary!\n");
    }

    mediumDict->release();
    return result;
}

IOReturn AirportItlwm::selectMedium(const IONetworkMedium *medium) {
    setSelectedMedium(medium);
    return kIOReturnSuccess;
}

bool AirportItlwm::
setLinkStatus(UInt32 status, const IONetworkMedium * activeMedium, UInt64 speed, OSData * data)
{
    struct _ifnet *ifq = &fHalService->get80211Controller()->ic_ac.ac_if;
    if (status == currentStatus) {
        return true;
    }
    bool ret = super::setLinkStatus(status, activeMedium, speed, data);
    currentStatus = status;
    if (fNetIf) {
        if (status & kIONetworkLinkActive) {
#ifdef __PRIVATE_SPI__
            bsdInterface->startOutputThread();
#endif
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkUp, (void *)0);
//            fNetIf->setLinkQualityMetric(100);
        } else if (!(status & kIONetworkLinkNoNetworkChange)) {
#ifdef __PRIVATE_SPI__
            bsdInterface->stopOutputThread();
            bsdInterface->flushOutputQueue();
#endif
            ifq_flush(&ifq->if_snd);
            mq_purge(&fHalService->get80211Controller()->ic_mgtq);
            getCommandGate()->runAction(setLinkStateGated, (void *)kIO80211NetworkLinkDown, (void *)fHalService->get80211Controller()->ic_deauth_reason);
        }
    }
    return ret;
}

IOReturn AirportItlwm::
setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3)
{
    AirportItlwm *that = OSDynamicCast(AirportItlwm, target);
#ifdef AIRPORT_WCL
    that->bsdInterface->setLinkState((IO80211LinkState)(uint64_t)arg0);
    static_cast<AirportItlwmSkywalkInterface *>(that->fNetIf)->handleWCLLink(
        (IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp);
    return kIOReturnSuccess;
#else
    IOReturn ret = that->fNetIf->setLinkState((IO80211LinkState)(uint64_t)arg0, (unsigned int)(uint64_t)arg1);
    that->fNetIf->setRunningState((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp);
    that->fNetIf->postMessage(APPLE80211_M_LINK_CHANGED, NULL, 0, false);
    that->fNetIf->postMessage(APPLE80211_M_BSSID_CHANGED, NULL, 0, false);
    that->fNetIf->postMessage(APPLE80211_M_SSID_CHANGED, NULL, 0, false);
    if ((IO80211LinkState)(uint64_t)arg0 == kIO80211NetworkLinkUp) {
        that->fNetIf->reportLinkStatus(3, 0x80);
    } else {
        that->fNetIf->reportLinkStatus(1, 0);
    }
    that->bsdInterface->setLinkState((IO80211LinkState)(uint64_t)arg0);
    return ret;
#endif
}

#ifdef __PRIVATE_SPI__
IOReturn AirportItlwm::outputStart(IONetworkInterface *interface, IOOptionBits options)
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

IOReturn AirportItlwm::networkInterfaceNotification(
                    IONetworkInterface * interface,
                    uint32_t              type,
                    void *                  argument )
{
    XYLog("%s\n", __FUNCTION__);
    return kIOReturnSuccess;
}
#endif

extern const char* hexdump(uint8_t *buf, size_t len);

#ifdef AIRPORT_WCL
UInt32 AirportItlwm::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;

    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (!m || mbuf_type(m) == MBUF_TYPE_FREE) {
        ifp->netStat->outputErrors++;
        return kIOReturnOutputDropped;
    }
    if (!(mbuf_flags(m) & MBUF_PKTHDR)) {
        ifp->netStat->outputErrors++;
        freePacket(m);
        return kIOReturnOutputDropped;
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return ret;
}
#else
UInt32 AirportItlwm::outputPacket(mbuf_t m, void *param)
{
//    XYLog("%s\n", __FUNCTION__);
    IOReturn ret = kIOReturnOutputSuccess;
    struct _ifnet *ifp = &fHalService->get80211Controller()->ic_ac.ac_if;
    
    if (fHalService->get80211Controller()->ic_state != IEEE80211_S_RUN || ifp->if_snd.queue == NULL) {
        if (m && mbuf_type(m) != MBUF_TYPE_FREE)
            freePacket(m);
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
    size_t len = mbuf_len(m);
    ether_header_t *eh = (ether_header_t *)mbuf_data(m);
    if (len >= sizeof(ether_header_t) && eh->ether_type == htons(ETHERTYPE_PAE)) { // EAPOL packet
        const char* dump = hexdump((uint8_t*)mbuf_data(m), len);
        XYLog("output EAPOL packet, len: %zu, data: %s\n", len, dump ? dump : "Failed to allocate memory");
        if (dump)
            IOFree((void*)dump, 3 * len + 1);
    }
    if (!ifp->if_snd.queue->lockEnqueue(m)) {
        freePacket(m);
        ret = kIOReturnOutputDropped;
    }
    (*ifp->if_start)(ifp);
    return ret;
}
#endif

const OSString * AirportItlwm::newVendorString() const
{
    return OSString::withCString("Apple");
}

const OSString * AirportItlwm::newModelString() const
{
    return OSString::withCString(fHalService->getDriverInfo()->getFirmwareName());
}

IOReturn AirportItlwm::getHardwareAddress(IOEthernetAddress *addrP)
{
    if (IEEE80211_ADDR_EQ(etheranyaddr, fHalService->get80211Controller()->ic_myaddr))
        return kIOReturnError;
    else {
        IEEE80211_ADDR_COPY(addrP, fHalService->get80211Controller()->ic_myaddr);
        return kIOReturnSuccess;
    }
}

IOReturn AirportItlwm::setHardwareAddress(const void *addrP, UInt32 addrBytes)
{
    if (!fNetIf || !addrP)
        return kIOReturnError;
#ifdef AIRPORT_WCL
    const UInt8 *address = static_cast<const UInt8 *>(addrP);
    auto ic = fHalService->get80211Controller();
    bool initializing = (ic->ic_if.if_flags & IFF_UP) && !(ic->ic_if.if_flags & IFF_RUNNING);
    auto update = AirportWCL::planMacUpdate(ic->ic_myaddr, address, addrBytes, initializing);
    if (update == AirportWCL::MacUpdate::Invalid)
        return kIOReturnBadArgument;
    if (update == AirportWCL::MacUpdate::Busy) {
        return kIOReturnBusy;
    }
    if (!bsdInterface || !bsdInterface->getIfnet())
        return kIOReturnNotReady;
    // WCL calls this directly, bypassing IOEthernetInterface's normal MAC ioctl.
    // Update the BSD address used by DHCP/ARP as well as the firmware address.
    errno_t error = ifnet_set_lladdr(bsdInterface->getIfnet(), address, addrBytes);
    if (error)
        return kIOReturnError;
    bsdInterface->setProperty(kIOMACAddress, const_cast<UInt8 *>(address), addrBytes);
    // WCL replays the current address after every driver-available notification.
    // Keep BSD synchronized, but do not restart firmware for a replay.
    if (update == AirportWCL::MacUpdate::Unchanged) {
        return kIOReturnSuccess;
    }
#endif
    if_setlladdr(&fHalService->get80211Controller()->ic_ac.ac_if, (const UInt8 *)addrP);
#ifdef AIRPORT_WCL
    if (ic->ic_if.if_flags & IFF_RUNNING) {
        ic->ic_wcl_mac_reconfig = true;
#else
    if (fHalService->get80211Controller()->ic_state > IEEE80211_S_INIT) {
#endif
        fHalService->disable(bsdInterface);
        fHalService->enable(bsdInterface);
    }
    return kIOReturnSuccess;
}

UInt32 AirportItlwm::getFeatures() const
{
    return fHalService->getDriverInfo()->supportedFeatures();
}

IOReturn AirportItlwm::setPromiscuousMode(IOEnetPromiscuousMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastMode(IOEnetMulticastMode mode)
{
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::setMulticastList(IOEthernetAddress* addr, UInt32 len)
{
    return fHalService->getDriverController()->setMulticastList(addr, len);
}

IOReturn AirportItlwm::getPacketFilters(const OSSymbol *group, UInt32 *filters) const
{
    IOReturn    rtn = kIOReturnSuccess;
    if (group == gIOEthernetWakeOnLANFilterGroup && magicPacketSupported)
        *filters = kIOEthernetWakeOnMagicPacket;
    else if (group == gIONetworkFilterGroup)
        *filters = kIOPacketFilterMulticast | kIOPacketFilterPromiscuous;
    else
        rtn = IOEthernetController::getPacketFilters(group, filters);
    return rtn;
}

SInt32 AirportItlwm::
enableFeature(IO80211FeatureCode code, void *data)
{
    if (code == kIO80211Feature80211n) {
        return 0;
    }
    return 102;
}

bool AirportItlwm::getLogPipes(CCPipe**logPipe, CCPipe**eventPipe, CCPipe**snapshotsPipe)
{
    bool ret = false;
    if (logPipe) {
        *logPipe = driverLogPipe;
        ret = true;
    }
    if (eventPipe) {
        *eventPipe = driverDataPathPipe;
        ret = true;
    }
    if (snapshotsPipe) {
        *snapshotsPipe = driverSnapshotsPipe;
        ret = true;
    }
    return ret;
}

#define APPLE80211_CAPA_AWDL_FEATURE_AUTO_UNLOCK    0x00000004
#define APPLE80211_CAPA_AWDL_FEATURE_WOW            0x00000080

IOReturn AirportItlwm::
getCARD_CAPABILITIES(OSObject *object,
                                     struct apple80211_capability_data *cd)
{
    uint32_t caps = fHalService->get80211Controller()->ic_caps;
    memset(cd, 0, sizeof(struct apple80211_capability_data));

    if (caps & IEEE80211_C_WEP)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_WEP;
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[0] |= 1 << APPLE80211_CAP_TKIP | 1 << APPLE80211_CAP_AES_CCM;
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_PMGT)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_PMGT;
    // if (caps & IEEE80211_C_IBSS)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_IBSS;
    // if (caps & IEEE80211_C_HOSTAP)
    //     cd->capabilities[0] |= 1 << APPLE80211_CAP_HOSTAP;
    // AES not enabled, like on Apple cards

    if (caps & IEEE80211_C_SHSLOT)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHSLOT - 8);
    if (caps & IEEE80211_C_SHPREAMBLE)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_SHPREAMBLE - 8);
    if (caps & IEEE80211_C_RSN)
        cd->capabilities[1] |= 1 << (APPLE80211_CAP_WPA1 - 8) | 1 << (APPLE80211_CAP_WPA2 - 8) | 1 << (APPLE80211_CAP_TKIPMIC - 8);
    // Disable not implemented capabilities
    // if (caps & IEEE80211_C_TXPMGT)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_TXPMGT - 8);
    // if (caps & IEEE80211_C_MONITOR)
    //     cd->capabilities[1] |= 1 << (APPLE80211_CAP_MONITOR - 8);
    // WPA not enabled, like on Apple cards

    cd->version = APPLE80211_VERSION;
#ifdef AIRPORT_WCL
    // WPA2 can use TKIP for group traffic even when unicast uses CCMP.
    // net80211 handles TKIP/MIC in software; hiding it makes Apple80211
    // reject these networks before it submits an association request.
    cd->capabilities[0] = (caps & IEEE80211_C_RSN) ?
        (1 << APPLE80211_CAP_AES_CCM) | (1 << APPLE80211_CAP_TKIP) : 0;
    cd->capabilities[1] &= ~(1 << (APPLE80211_CAP_WPA1 - 8));
    cd->capabilities[5] = 0x40; // Scan completion events.
    cd->capabilities[2] = (1 << (APPLE80211_CAP_WME - 16)) |
        (1 << (APPLE80211_CAP_SHORT_GI_40MHZ - 16)) |
        (1 << (APPLE80211_CAP_SHORT_GI_20MHZ - 16));
    // Tahoe WCLDeviceConfiguration::isHeSupported reads byte 17, bit 7.
    // WCL uses this capability together with the BSS HE IE for PHY reporting.
    if (caps & IEEE80211_C_HE)
        cd->capabilities[17] |= 0x80;

    // Capability index 32: wifip2pd only requests an AWDL interface when it is set,
    // and System Information prints "AirDrop: Supported" from the same index.
    cd->capabilities[4] |= 0x01;
#else
    cd->capabilities[2] = 0xFF; // BURST, WME, SHORT_GI_40MHZ, SHORT_GI_20MHZ, WOW, TSN, ?, ?
    cd->capabilities[3] = 0x2B;
    cd->capabilities[5] = 0x40;
    cd->capabilities[6] = (
//                           1 |    //MFP capable
                           0x8 |
                           0x4 |
                           0x80
                           );
    *(uint16_t *)&cd->capabilities[8] = 0x201;
//
//    cd->capabilities[2] |= 0x10;
//    cd->capabilities[5] |= 0x1;
//
//    cd->capabilities[2] |= 0x2;
//
//    cd->capabilities[3] |= 0x20;
//
//    cd->capabilities[0] |= 0x80;
//
//    cd->capabilities[3] |= 0x80;
//    cd->capabilities[4] |= 0x4;
//
//    cd->capabilities[4] |= 0x1;
//    cd->capabilities[3] |= 0x1;
//    cd->capabilities[6] |= 0x8;
//
//    cd->capabilities[3] |= 3;
//    cd->capabilities[4] |= 2;
//    cd->capabilities[6] |= 0x10;
//    cd->capabilities[5] |= 0x20;
//    cd->capabilities[5] |= 0x80;
//
//    if (cd->capabilities[6] & 0x20) {
//        cd->capabilities[2] |= 8;
//    }
//    cd->capabilities[5] |= 8;
//    cd->capabilities[8] |= 2;
//
//    cd->capabilities[11] |= (2 | 4 | 8 | 0x10 | 0x20 | 0x40 | 0x80);

#endif
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getDRIVER_VERSION(OSObject *object,
                                  struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    snprintf(hv->string, sizeof(hv->string), "itlwm: %s%s fw: %s", ITLWM_VERSION, GIT_COMMIT, fHalService->getDriverInfo()->getFirmwareVersion());
    hv->string_len = strlen(hv->string);
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getHARDWARE_VERSION(OSObject *object,
                                    struct apple80211_version_data *hv)
{
    if (!hv)
        return kIOReturnError;
    hv->version = APPLE80211_VERSION;
    strncpy(hv->string, fHalService->getDriverInfo()->getFirmwareVersion(), sizeof(hv->string));
    hv->string_len = strlen(fHalService->getDriverInfo()->getFirmwareVersion());
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getCOUNTRY_CODE(OSObject *object,
                                struct apple80211_country_code_data *cd)
{
    char user_override_cc[3];
    const char *cc_fw = fHalService->getDriverInfo()->getFirmwareCountryCode();

    if (!cd)
        return kIOReturnError;
    cd->version = APPLE80211_VERSION;
    memset(user_override_cc, 0, sizeof(user_override_cc));
    PE_parse_boot_argn("itlwm_cc", user_override_cc, 3);
    /* user_override_cc > firmware_cc > geo_location_cc */
    strncpy((char*)cd->cc, user_override_cc[0] ? user_override_cc : ((cc_fw[0] == 'Z' && cc_fw[1] == 'Z' && geo_location_cc[0]) ? geo_location_cc : cc_fw), sizeof(cd->cc));
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
setCOUNTRY_CODE(OSObject *object, struct apple80211_country_code_data *data)
{
#ifdef AIRPORT_WCL
    if (!data)
        return kIOReturnBadArgument;
    if (data->cc[0] != 'x' && data->cc[0] != 'X' &&
        memcmp(geo_location_cc, data->cc, sizeof(geo_location_cc)) != 0) {
        memcpy(geo_location_cc, data->cc, sizeof(geo_location_cc));
        if (fNetIf)
            static_cast<AirportItlwmSkywalkInterface *>(fNetIf)->handleWCLEvent(
                IEEE80211_EVT_COUNTRY_CODE_UPDATE, nullptr);
    }
#else
    XYLog("%s cc=%s\n", __FUNCTION__, data->cc);
    if (data && data->cc[0] != 120 && data->cc[0] != 88) {
        memcpy(geo_location_cc, data->cc, sizeof(geo_location_cc));
        fNetIf->postMessage(APPLE80211_M_COUNTRY_CODE_CHANGED, NULL, 0, 0);
    }
#endif
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
getPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
    if (!pd)
        return kIOReturnError;
    pd->version = APPLE80211_VERSION;
    pd->num_radios = 4;
    pd->power_state[0] = power_state;
    pd->power_state[1] = power_state;
    pd->power_state[2] = power_state;
    pd->power_state[3] = power_state;
    return kIOReturnSuccess;
}

IOReturn AirportItlwm::
setPOWER(OSObject *object,
                         struct apple80211_power_data *pd)
{
#ifdef AIRPORT_WCL
    if (!pd)
        return kIOReturnError;
    IOLog("itlwm: setPOWER: num_radios[%d]  power_state(0:%u  1:%u  2:%u  3:%u)\n", pd->num_radios, pd->power_state[0], pd->power_state[1], pd->power_state[2], pd->power_state[3]);
    if (pd->num_radios > 0) {
        bool isRunning = (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & IFF_RUNNING) != 0;
        /* The requested state counts from here on, through the long teardown or recovery below. */
        power_state = (pd->power_state[0]);
        if (pd->power_state[0] == 0) {
            bool isUp = (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & (IFF_UP | IFF_RUNNING)) != 0;
            /*
             * Wi-Fi off is a real reset (stop the firmware, power the chip down
             * as for system sleep), but only when there is something to reset:
             * at boot with Wi-Fi left off the chip was never used, and the
             * workloop thread cannot sleep for the teardown.
             */
            bool reset = !noWakeFix && !noRadioReset && (isUp || chipUsed) && !onWorkLoopThread();

            fHalService->get80211Controller()->ic_wcl_mac_reconfig = false;
            changePowerStateToPriv(kPowerStateOff);
            if (isUp)
                net80211_ifstats(fHalService->get80211Controller());
            if (reset) {
                /*
                 * If the power-state machine also runs setPowerStateOff for the
                 * change above, its chip-down finds the chip down and does nothing.
                 */
                _fCommandGate->runAction(radioOffGated);
            } else if (isUp)
                disableAdapter(bsdInterface);
        } else {
            changePowerStateToPriv(kPowerStateOn);
            if (!isRunning)
                enableAdapter(bsdInterface, kWakeViaSetPower);
        }
    }

    return kIOReturnSuccess;
#else
    if (!pd)
        return kIOReturnError;
    IOLog("itlwm: setPOWER: num_radios[%d]  power_state(0:%u  1:%u  2:%u  3:%u)\n", pd->num_radios, pd->power_state[0], pd->power_state[1], pd->power_state[2], pd->power_state[3]);
    if (pd->num_radios > 0) {
        bool isRunning = (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & (IFF_UP | IFF_RUNNING)) != 0;
        power_state = (pd->power_state[0]);
        if (pd->power_state[0] == 0) {
            changePowerStateToPriv(1);
            if (isRunning) {
                net80211_ifstats(fHalService->get80211Controller());
                disableAdapter(bsdInterface);
            }
        } else {
            changePowerStateToPriv(2);
            if (!isRunning)
                enableAdapter(bsdInterface, kWakeViaSetPower);
        }
    }

    return kIOReturnSuccess;
#endif
}

SInt32 AirportItlwm::apple80211_ioctl(IO80211SkywalkInterface *interface,unsigned long cmd,void *data, bool b1, bool b2)
{
    if (!ml_at_interrupt_context())
        XYLog("%s cmd: %s b1: %d b2: %d\n", __FUNCTION__, convertApple80211IOCTLToString((unsigned int)cmd), b1, b2);
#ifdef AIRPORT_WCL
    return kIOReturnUnsupported;
#else
    return super::apple80211_ioctl(interface, cmd, data, b1, b2);
#endif
}

SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 1 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return kIOReturnUnsupported;
}

SInt32 AirportItlwm::apple80211SkywalkRequest(UInt request,int cmd,IO80211SkywalkInterface *interface,void *data,void *)
{
    if (!ml_at_interrupt_context())
        XYLog("%s 2 cmd: %s request: %d\n", __FUNCTION__, convertApple80211IOCTLToString(cmd), request);
    return kIOReturnUnsupported;
}

/* Indexed by kWakeVia*: the "recovery run from ..." log line. */
static const char *const wakeViaName[] = {
    "power-state thread", "setPOWER", "enable()", "retry", "data-stall restart", "deferred request"
};
static_assert(sizeof(wakeViaName) / sizeof(wakeViaName[0]) == kWakeViaCount,
              "wakeViaName[] needs one entry per kWakeVia* value");

/* How long a thread waits for a running chip transition (the gate is released meanwhile). */
#define QWZ_CHIP_WAIT_MS	10000
#define QWZ_PM_WAIT_MS		4000	/* the power-state thread: its acknowledge budget is 15 s / 20 s */
#define QWZ_PM_QUIESCE_SKIP_MS	6000	/* no quiesce wait if the power-down thread already used this much */
#define QWZ_QUIESCE_MS		3000

static uint64_t pmNowMs(void)
{
    uint64_t t;

    clock_get_uptime(&t);
    absolutetime_to_nanoseconds(t, &t);
    return t / 1000000ULL;
}

static uint32_t waitMsFor(int via)
{
    return via == kWakeViaPmThread ? QWZ_PM_WAIT_MS : QWZ_CHIP_WAIT_MS;
}

/* The bare enable: wakeRecover (and the build-2 handlers) end up here. */
IOReturn AirportItlwm::enableAdapterNow(IONetworkInterface *netif)
{
#ifdef AIRPORT_WCL
    IOReturn status = fHalService->enable(netif);
    if (status != kIOReturnSuccess)
        return status;
#else
    fHalService->enable(netif);
#endif
    chipUsed = true;
    watchdogTimer->setTimeoutMS(kWatchDogTimerPeriod);
    watchdogTimer->enable();
    return kIOReturnSuccess;
}

/*
 * Every enable request lands here. After a chip power-down (sleep, Wi-Fi off,
 * data-stall restart) a bare enable would run qwz_init on a chip nobody has
 * brought back yet: airportd's setPOWER(1) did that before the power-state
 * thread restored the PCI state. Whoever asks first runs the whole recovery.
 */
IOReturn AirportItlwm::enableAdapter(IONetworkInterface *netif, int via)
{
    if (noWakeFix)
        return enableAdapterNow(netif);
    return _fCommandGate->runAction(enableAdapterGated, netif, (void *)(intptr_t)via);
}

IOReturn AirportItlwm::enableAdapterGated(OSObject *owner, void *netif, void *via, void *, void *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->fHalService == NULL)
        return kIOReturnError;
    if (!self->chipWait(wakeViaName[(intptr_t)via], waitMsFor((int)(intptr_t)via)))
        return kIOReturnBusy;
    if (self->fHalService->wakeNeedsRecovery()) {
        /* The firmware init sleeps: the workloop thread cannot run it. A thread call will. */
        if (self->onWorkLoopThread()) {
            qwz_log("qwz: pm: wake: %s asked on the workloop thread: queueing the recovery on a thread that may sleep\n",
                    wakeViaName[(intptr_t)via]);
            qwz_log_publish();
            self->scheduleWakeRetry(100, kWakeViaDeferred);
            return kIOReturnBusy;
        }
        self->wakeRecover((int)(intptr_t)via);
        return self->fHalService->wakeNeedsRecovery() ? kIOReturnNotReady : kIOReturnSuccess;
    }
    return self->enableAdapterNow((IONetworkInterface *)netif);
}

void AirportItlwm::disableAdapter(IONetworkInterface *netif)
{
    watchdogTimer->cancelTimeout();
    watchdogTimer->disable();
    fHalService->disable(netif);
}

IOReturn AirportItlwm::
tsleepHandler(OSObject* owner, void* arg0, void* arg1, void* arg2, void* arg3)
{
    AirportItlwm* dev = OSDynamicCast(AirportItlwm, owner);
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

bool AirportItlwm::initPCIPowerManagment(IOPCIDevice *provider)
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
        if (pciPMCReg & kPCIPMCPMESupportFromD3Cold)
            magicPacketSupported = true;
        provider->configWrite16((pmPCICapPtr + 4), 0x8000 );
        IOSleep(10);
    }
    return true;
}

static IOPMPowerState powerStateArray[kPowerStateCount] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

/*
 * Stop every thread call and wait for one that is running: the handlers use
 * the gate and the HAL, which the teardown below frees. Idempotent.
 */
void AirportItlwm::quiescePM()
{
    thread_call_t calls[] = { wakeRetryCall, stallCall, powerOffThreadCall, powerOnThreadCall };

    pmStopping = true;
    if (watchdogTimer) {
        watchdogTimer->cancelTimeout();
        watchdogTimer->disable();
    }
    for (unsigned i = 0; i < sizeof(calls) / sizeof(calls[0]); i++) {
        /* TRUE: it was queued and is cancelled now; FALSE: idle, or it ran to the end meanwhile */
        if (calls[i] != NULL && thread_call_cancel_wait(calls[i]))
            release();	/* the cancelled call's reference */
    }
    stallPending = false;
}

void AirportItlwm::unregistPM()
{
    quiescePM();
    if (wakeRetryCall) {
        thread_call_free(wakeRetryCall);
        wakeRetryCall = NULL;
    }
    if (stallCall) {
        thread_call_free(stallCall);
        stallCall = NULL;
    }
    if (powerOffThreadCall) {
        thread_call_free(powerOffThreadCall);
        powerOffThreadCall = NULL;
    }
    if (powerOnThreadCall) {
        thread_call_free(powerOnThreadCall);
        powerOnThreadCall = NULL;
    }
}

IOReturn AirportItlwm::setPowerState(unsigned long powerStateOrdinal, IOService *policyMaker)
{
    IOReturn result = IOPMAckImplied;

    if (pmStopping)
        return result;
    /* Before the early return below: a queued stall restart must see the sleep coming, and the wake clears it. */
    if (powerStateOrdinal == kPowerStateOff)
        sleepPending = true;
    else if (powerStateOrdinal == kPowerStateOn)
        sleepPending = false;
    if (pmPowerState == powerStateOrdinal)
        return result;
    switch (powerStateOrdinal) {
        case kPowerStateOff:
            if (powerOffThreadCall) {
                retain();
                if (thread_call_enter(powerOffThreadCall))
                    release();
                result = 15000000;	/* chip power-down before sleep: QMI stop + MHI M3 poll */
            }
            break;
        case kPowerStateOn:
            if (powerOnThreadCall) {
                retain();
                if (thread_call_enter(powerOnThreadCall))
                    release();
                result = 20000000;	/* wake recovery: PCIe link + full firmware load */
            }
            break;

        default:
            break;
    }
    return result;
}

IOReturn AirportItlwm::setWakeOnMagicPacket(bool active)
{
    magicPacketEnabled = active;
    return kIOReturnSuccess;
}

static void handleSetPowerStateOff(thread_call_param_t param0,
                             thread_call_param_t param1)
{
    AirportItlwm *self = (AirportItlwm *)param0;

    if (param1 == 0)
    {
        if (!self->pmReady())
        {
            /* Stopping: no gate or HAL to work with, but the power change still wants its acknowledge. */
            self->pmAcknowledge();
            self->release();
            return;
        }
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOff,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOff();
        self->release();
    }
}

static void handleSetPowerStateOn(thread_call_param_t param0,
                            thread_call_param_t param1)
{
    AirportItlwm *self = (AirportItlwm *) param0;

    if (param1 == 0)
    {
        if (!self->pmReady())
        {
            self->pmAcknowledge();
            self->release();
            return;
        }
        self->getCommandGate()->runAction((IOCommandGate::Action)
                                           handleSetPowerStateOn,
                                           (void *) 1);
    }
    else
    {
        self->setPowerStateOn();
        self->release();
    }
}

static void handleWakeRetry(thread_call_param_t param0, thread_call_param_t param1);
static void handleStallRestart(thread_call_param_t param0, thread_call_param_t param1);

IOReturn AirportItlwm::registerWithPolicyMaker(IOService *policyMaker)
{
    IOReturn ret;

    pmPowerState = kPowerStateOn;
    pmPolicyMaker = policyMaker;

    powerOffThreadCall = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOff,
                                            (thread_call_param_t)this);
    wakeRetryCall = thread_call_allocate((thread_call_func_t)handleWakeRetry,
                                         (thread_call_param_t)this);
    stallCall = thread_call_allocate((thread_call_func_t)handleStallRestart,
                                     (thread_call_param_t)this);
    powerOnThreadCall  = thread_call_allocate(
                                            (thread_call_func_t)handleSetPowerStateOn,
                                              (thread_call_param_t)this);
    ret = pmPolicyMaker->registerPowerDriver(this,
                                             powerStateArray,
                                             kPowerStateCount);
    return ret;
}

bool AirportItlwm::pmReady()
{
    return !pmStopping && fHalService != NULL && getCommandGate() != NULL;
}

void AirportItlwm::pmAcknowledge()
{
    if (pmPolicyMaker != NULL)
        pmPolicyMaker->acknowledgeSetPowerState();
}

bool AirportItlwm::onWorkLoopThread()
{
    IOWorkLoop *wl = _fCommandGate != NULL ? _fCommandGate->getWorkLoop() : NULL;

    return wl != NULL && wl->onThread();
}

/* A recovery may run: the system is awake and Wi-Fi is (or, without the radio reset, need not be) on. */
bool AirportItlwm::wakeAllowed()
{
    return !pmStopping && !sleepPending && pmPowerState == kPowerStateOn &&
        (power_state || noRadioReset);
}

/*
 * The acknowledge budgets are 15 s (off) and 20 s (on); the power-state thread
 * therefore waits at most QWZ_PM_WAIT_MS for a transition another thread is
 * running, and skips the quiesce wait if that wait used most of its time.
 * The acknowledge happens exactly once on every path, after the work.
 */
void AirportItlwm::setPowerStateOff()
{
    uint64_t start = pmNowMs();

    pmPowerState = kPowerStateOff;
    if (noWakeFix) {
        disableAdapter(bsdInterface);
        pmAcknowledge();
        return;
    }
    cancelWakeRetry();
    cancelStallRestart();
    stallRestarts = 0;		/* a new wake/boot window */
    stallNoted = false;
    wakeRetryCount = 0;
    if (!chipDown("system sleep", "system wake", QWZ_PM_WAIT_MS, start)) {
        qwz_log("qwz: pm: system sleep: the chip was not powered down by this thread (see above); acknowledging anyway\n");
        qwz_log_publish();
    }
    pmAcknowledge();
}

void AirportItlwm::setPowerStateOn()
{
    pmPowerState = kPowerStateOn;
    if (noWakeFix) {
        enableAdapterNow(bsdInterface);
        pmAcknowledge();
        return;
    }
    if (fHalService->wakeNeedsRecovery()) {
        if (!power_state && !noRadioReset)
            qwz_log("qwz: pm: wake: Wi-Fi is off: the chip stays down until it is switched on\n");
        else if (!wakeRecover(kWakeViaPmThread)) {
            /* Another thread owns the chip and may fail or be cut off: make sure someone looks again. */
            qwz_log("qwz: pm: wake: the chip is busy with another transition: a recovery check is queued\n");
            scheduleWakeRetry(5000, kWakeViaDeferred);
        }
    } else if (fHalService->get80211Controller()->ic_ac.ac_if.if_flags & IFF_RUNNING) {
        /* airportd's setPOWER(1) got here first and recovered the chip. */
        qwz_log("qwz: pm: wake: already recovered; the power-state thread only acknowledges\n");
    } else
        enableAdapter(bsdInterface, kWakeViaPmThread);
    qwz_log_publish();
    pmAcknowledge();
}

/*
 * Wait, with the gate released, until no chip power transition is running;
 * false if that took longer than maxMs, or if this is the workloop thread
 * (which must stay free for the interrupts the other thread is waiting for).
 */
bool AirportItlwm::chipWait(const char *who, uint32_t maxMs)
{
    uint64_t deadline;

    if (!chipBusy)
        return true;
    if (onWorkLoopThread()) {
        qwz_log("qwz: pm: %s: a chip power transition is running and this is the workloop thread: not waiting\n", who);
        return false;
    }
    qwz_log("qwz: pm: %s: a chip power transition is running; waiting up to %u ms for it\n", who, (unsigned)maxMs);
    qwz_log_publish();
    clock_interval_to_deadline(maxMs, NSEC_PER_MSEC, &deadline);
    while (chipBusy) {
        if (_fCommandGate->commandSleep(&chipBusy, deadline, THREAD_UNINT) == kIOReturnTimeout) {
            qwz_log("qwz: pm: %s: gave up waiting for the running chip power transition\n", who);
            qwz_log_publish();
            return false;
        }
    }
    return true;
}

bool AirportItlwm::chipClaim(const char *who, uint32_t maxMs)
{
    if (!chipWait(who, maxMs))
        return false;
    chipBusy = true;
    return true;
}

void AirportItlwm::chipRelease()
{
    chipBusy = false;
    _fCommandGate->commandWakeup(&chipBusy);
}

/*
 * Interface down and chip power-down: what system sleep does (stop the
 * association in order, firmware stop, MHI stop, SoC reset, save the PCI
 * config). Wi-Fi off and the data-stall restart use it too, so each of them
 * is a real reset. Runs holding the command gate. Returns false when it did
 * not own the chip (another transition still running, or the workloop thread):
 * the hardware is then untouched. A chip that is already down (Wi-Fi off,
 * then sleep) is left alone, whoever got there first. budgetStartMs is the
 * start of a time-limited caller (the power-state thread), 0 for none.
 */
bool AirportItlwm::chipDown(const char *why, const char *wakeName, uint32_t maxWaitMs, uint64_t budgetStartMs)
{
    if (onWorkLoopThread()) {
        qwz_log("qwz: pm: %s: asked on the workloop thread, which cannot sleep for the teardown: chip left as it is\n", why);
        qwz_log_publish();
        return false;
    }
    if (!chipClaim(why, maxWaitMs)) {
        qwz_log("qwz: pm: %s: another chip transition is still running: the hardware is not touched\n", why);
        qwz_log_publish();
        return false;
    }
    if (chipIsDown && fHalService->wakeNeedsRecovery() && !fHalService->firmwareUp()) {
        qwz_log("qwz: pm: %s: the chip is already powered down\n", why);
        qwz_log_publish();
    } else {
        wakeLabel = wakeName;
        qwz_log("qwz: pm: ==== %s: interface down, chip power-down ====\n", why);
        qwz_log_publish();
        if (budgetStartMs != 0 && pmNowMs() - budgetStartMs > QWZ_PM_QUIESCE_SKIP_MS) {
            qwz_log("qwz: pm: %s: %llu ms of the acknowledge budget are gone: not waiting for in-flight state work\n",
                    why, pmNowMs() - budgetStartMs);
            qwz_log_publish();
        } else
            fHalService->sleepQuiesce(QWZ_QUIESCE_MS);
        disableAdapter(bsdInterface);
        fHalService->sleepHardware();
        chipIsDown = fHalService->wakeNeedsRecovery();
        if (chipIsDown)
            chipUsed = false;
    }
    chipRelease();
    return true;
}

IOReturn AirportItlwm::radioOffGated(OSObject *owner, void *, void *, void *, void *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->fHalService == NULL)
        return kIOReturnError;
    self->cancelWakeRetry();
    self->cancelStallRestart();
    self->stallRestarts = 0;
    self->stallNoted = false;
    self->wakeRetryCount = 0;
    if (!self->chipDown("Wi-Fi off", "Wi-Fi on", QWZ_CHIP_WAIT_MS, 0)) {
        qwz_log("qwz: pm: Wi-Fi off: the chip reset did not run (see above); the interface is stopped only\n");
        qwz_log_publish();
        if (!self->onWorkLoopThread())
            self->disableAdapter(self->bsdInterface);
    }
    return kIOReturnSuccess;
}

/*
 * Wake after a chip power-down: PCIe link and config recovery, then the
 * interface enable, whose qwz_init does the full firmware init (the firmware
 * is gone: fw_initialized was cleared at sleep). Runs holding the command gate.
 * A failure queues a retry with back-off (wakeRetryNext).
 *
 * Several threads can ask for this at once after a wake (airportd's setPOWER,
 * the power-state thread): the first runs it, the others wait for it (qwz_init
 * sleeps with the gate released) and find nothing left to do, so the PCI
 * restore never runs beside a firmware boot and the init runs once.
 * Returns false if the chip could not be claimed (nothing was done).
 */
#define QWZ_WAKE_RETRY_MAX	12

static void wakeRetryNext(AirportItlwm *self)
{
    /* 5 s, 30 s, 60 s, then every 5 minutes; 12 tries in all (about 50 minutes) */
    static const uint32_t delaysSec[] = { 5, 30, 60 };
    uint32_t n = self->wakeRetryCount, sec;

    if (n >= QWZ_WAKE_RETRY_MAX) {
        if (n == QWZ_WAKE_RETRY_MAX) {
            self->wakeRetryCount++;
            qwz_log("qwz: pm: wake: GIVING UP after %u failed recoveries: the chip stays down; toggle Wi-Fi or sleep and wake\n", n);
            qwz_log_publish();
        }
        return;
    }
    sec = n < sizeof(delaysSec) / sizeof(delaysSec[0]) ? delaysSec[n] : 300;
    self->wakeRetryCount = n + 1;
    qwz_log("qwz: pm: wake: recovery failed; retry %u of %u in %u s\n", n + 1, QWZ_WAKE_RETRY_MAX, sec);
    qwz_log_publish();
    self->scheduleWakeRetry(sec * 1000, kWakeViaRetry);
}

bool AirportItlwm::wakeRecover(int via)
{
    uint64_t t0, t1;
    bool ok = false;
    bool isRetry = via == kWakeViaRetry;
    IOReturn r;

    if (!chipClaim(wakeViaName[via], waitMsFor(via)))
        return false;
    if (!fHalService->wakeNeedsRecovery()) {
        qwz_log("qwz: pm: wake: already recovered; %s has nothing left to do\n", wakeViaName[via]);
        qwz_log_publish();
        chipRelease();
        return true;
    }
    chipIsDown = false;
    clock_get_uptime(&t0);
    qwz_log("qwz: pm: ==== %s%s: recovering the Wi-Fi chip ====\n", wakeLabel ? wakeLabel : "system wake",
            isRetry ? " (retry)" : "");
    qwz_log("qwz: pm: wake: recovery run from %s\n", wakeViaName[via]);
    qwz_log_publish();
    r = fHalService->wakeHardware();
    if (r == kIOReturnSuccess) {
        qwz_log("qwz: pm: wake: firmware re-init (enable interface)\n");
        qwz_log_publish();
        r = enableAdapterNow(bsdInterface);
        if (r == kIOReturnSuccess) {
            ok = true;
        } else {
            qwz_log("qwz: pm: wake: firmware re-init FAILED (0x%x): leaving the interface down\n", (unsigned)r);
            disableAdapter(bsdInterface);
        }
    } else
        qwz_log("qwz: pm: wake: PCI recovery FAILED (0x%x): leaving the chip down\n", (unsigned)r);
    fHalService->wakeComplete(ok);
    clock_get_uptime(&t1);
    absolutetime_to_nanoseconds(t1 - t0, &t1);
    qwz_log("qwz: pm: wake: %s, total wake recovery %llu ms\n", ok ? "SUCCESS" : "FAILED", t1 / 1000000ULL);
    qwz_log_publish();
    if (ok)
        wakeRetryCount = 0;
    chipRelease();
    if (!ok) {
        if (wakeAllowed())
            wakeRetryNext(this);
        else {
            qwz_log("qwz: pm: wake: no retry: the system is going to sleep, or Wi-Fi is off; the next wake or Wi-Fi on recovers\n");
            qwz_log_publish();
        }
    }
    return true;
}

void AirportItlwm::scheduleWakeRetry(uint32_t ms, int via)
{
    uint64_t deadline;

    if (wakeRetryCall == NULL || pmStopping)
        return;
    wakeRetryVia = via;
    clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
    retain();
    if (thread_call_enter_delayed(wakeRetryCall, deadline))
        release();	/* was already queued: that one holds a reference */
}

IOReturn AirportItlwm::wakeRetryGated(OSObject *owner, void *, void *, void *, void *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->fHalService == NULL || self->pmStopping)
        return kIOReturnError;
    if (!self->wakeAllowed() || !self->fHalService->wakeNeedsRecovery()) {
        qwz_log("qwz: pm: wake retry: not needed any more\n");
        qwz_log_publish();
        return kIOReturnSuccess;
    }
    self->wakeRecover(self->wakeRetryVia);
    return kIOReturnSuccess;
}

static void handleWakeRetry(thread_call_param_t param0, thread_call_param_t)
{
    AirportItlwm *self = (AirportItlwm *) param0;

    if (self->pmReady())
        self->getCommandGate()->runAction(AirportItlwm::wakeRetryGated);
    self->release();
}

void AirportItlwm::cancelWakeRetry()
{
    if (wakeRetryCall != NULL && thread_call_cancel(wakeRetryCall))
        release();	/* the cancelled call's reference */
}

/*
 * Data-stall restart. ItlQwz::dataStalled() reports an interface that sat in
 * RUN without one data frame while management frames kept coming: the
 * firmware is up but its data path is dead (seen after a sleep that left a
 * half-deleted peer). The cure is what a wake does, a full firmware restart.
 * The watchdog (once a second, no gate) only polls; a thread call does the
 * work holding the gate. At most twice per wake/boot window, 60 s apart; with
 * -qwzstallsbr the second one also resets the PCIe link. A window ends at
 * sleep, Wi-Fi off, or after five minutes of a healthy link.
 */
#define QWZ_STALL_MAX_RESTARTS	2
#define QWZ_STALL_BACKOFF_MS	60000

void AirportItlwm::checkDataStall()
{
    uint64_t now;
    bool healthy = false, stalled;

    if (noWakeFix || noStallFix || stallCall == NULL || pmStopping || sleepPending ||
        pmPowerState != kPowerStateOn || !power_state || chipBusy)
        return;
    stalled = fHalService->dataStalled(&healthy);
    if (healthy && (stallRestarts != 0 || stallNoted)) {
        qwz_log("qwz: pm: data-stall: the link has been healthy for a while: the restart count (%u) is reset\n", stallRestarts);
        qwz_log_publish();
        stallRestarts = 0;
        stallNoted = false;
    }
    if (!stalled || stallPending)
        return;
    now = pmNowMs();
    if (stallRestarts >= QWZ_STALL_MAX_RESTARTS) {
        if (!stallNoted) {
            stallNoted = true;
            qwz_log("qwz: pm: data-stall: still stalled after %u restarts in this wake/boot window: no more restarts (toggle Wi-Fi, or sleep and wake)\n",
                    stallRestarts);
            qwz_log_publish();
        }
        return;
    }
    if (stallRestarts > 0 && now - stallLastMs < QWZ_STALL_BACKOFF_MS) {
        if (!stallNoted) {
            stallNoted = true;
            qwz_log("qwz: pm: data-stall: back-off: the next restart is allowed in %llu s\n",
                    (QWZ_STALL_BACKOFF_MS - (now - stallLastMs)) / 1000ULL);
            qwz_log_publish();
        }
        return;
    }
    stallPending = true;
    retain();
    if (thread_call_enter(stallCall))
        release();	/* was already queued: that one holds a reference */
}

void AirportItlwm::stallRestart()
{
    if (noStallFix || pmStopping || sleepPending || pmPowerState != kPowerStateOn || !power_state || chipBusy ||
        !fHalService->dataStalled(NULL)) {
        qwz_log("qwz: pm: data-stall: gone, or the power state changed, before the restart ran: leaving it\n");
        qwz_log_publish();
        return;
    }
    stallRestarts++;
    stallLastMs = pmNowMs();
    stallNoted = false;
    qwz_log("qwz: pm: data-stall: restarting the firmware (restart %u of %u in this wake/boot window)\n",
            stallRestarts, QWZ_STALL_MAX_RESTARTS);
    if (stallSbr && stallRestarts > 1) {
        qwz_log("qwz: pm: data-stall: this restart also resets the PCIe link (-qwzstallsbr)\n");
        fHalService->requestBusReset();
    }
    qwz_log_publish();
    if (!chipDown("data-stall restart", "data-stall restart", QWZ_CHIP_WAIT_MS, 0))
        return;
    /* chipDown sleeps with the gate released: a sleep or Wi-Fi off may have arrived meanwhile. */
    if (!wakeAllowed()) {
        qwz_log("qwz: pm: data-stall: a sleep or Wi-Fi off arrived during the power-down: the chip stays down for the wake / Wi-Fi on\n");
        qwz_log_publish();
        return;
    }
    wakeRecover(kWakeViaStall);
}

IOReturn AirportItlwm::stallRestartGated(OSObject *owner, void *, void *, void *, void *)
{
    AirportItlwm *self = OSDynamicCast(AirportItlwm, owner);

    if (self == NULL || self->fHalService == NULL || self->pmStopping) {
        if (self != NULL)
            self->stallPending = false;
        return kIOReturnError;
    }
    self->stallRestart();
    self->stallPending = false;
    return kIOReturnSuccess;
}

static void handleStallRestart(thread_call_param_t param0, thread_call_param_t)
{
    AirportItlwm *self = (AirportItlwm *) param0;

    if (self->pmReady())
        self->getCommandGate()->runAction(AirportItlwm::stallRestartGated);
    else
        self->stallPending = false;
    self->release();
}

void AirportItlwm::cancelStallRestart()
{
    if (stallCall != NULL && thread_call_cancel(stallCall)) {
        stallPending = false;
        release();	/* the cancelled call's reference */
    }
}

/*
 * AWDL virtual interface creation — called by IO80211Controller::attachVirtualInterface.
 * Based on ReAWDL project (maddog860/Sonoma-ReAWDL-Project).
 */
/*
 * AWDL virtual interface creation — called by IO80211Controller::attachVirtualInterface.
 * Based on ReAWDL project (maddog860/Sonoma-ReAWDL-Project).
 *
 * We let the base IO80211Controller::createVirtualInterface handle it,
 * which allocates IO80211VirtualInterface and calls init().
 * The base implementation logs "failure to create virtual interface" and
 * returns NULL — but only because it doesn't know the prefix.
 * On Tahoe, attachVirtualInterface handles the full lifecycle.
 */
IO80211VirtualInterface *
AirportItlwm::createVirtualInterface(ether_addr *ether, UInt role)
{
    qwz_log("airport: AWDL createVirtualInterface role=%u\n", role);
    qwz_log_publish();

    /* Let the superclass handle it — on Tahoe, IO80211Controller's
     * attachVirtualInterface does the allocation internally when
     * createVirtualInterface returns non-NULL. We just need to not
     * return NULL for AWDL role. */
    return super::createVirtualInterface(ether, role);
}
