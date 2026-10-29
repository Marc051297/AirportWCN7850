//
//  AirportItlwmV2.hpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#ifndef AirportItlwmV2_hpp
#define AirportItlwmV2_hpp

#include "Apple80211.h"
#ifdef AIRPORT_WCL
#if __IO80211_TARGET == __MAC_15_2
#include <Airport/macOS15/FaultReporter.h>
#else
#include <Airport/macOS26/FaultReporter.h>
#endif
#endif

#include "IOKit/network/IOGatedOutputQueue.h"
#include <libkern/c++/OSString.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOLib.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <IOKit/IOFilterInterruptEventSource.h>

#include "ItlQwz.hpp"

/* Intel's RSSI floor; the MediaTek HAL keeps ni_rssi the same way (dBm + 100). */
#ifndef IWM_MIN_DBM
#define IWM_MIN_DBM -100
#endif

#include "AirportItlwmEthernetInterface.hpp"

enum
{
    kPowerStateOff = 0,
    kPowerStateOn,
    kPowerStateCount
};

/* Who asked for a wake recovery; logged as "recovery run from ...". */
enum
{
    kWakeViaPmThread = 0,
    kWakeViaSetPower,
    kWakeViaEnable,
    kWakeViaRetry,
    kWakeViaStall,
    kWakeViaDeferred,
    kWakeViaCount
};

#define kWatchDogTimerPeriod 1000
#ifdef AIRPORT_WCL
#define AIRPORT_LEGACY_OVERRIDE
#else
#define AIRPORT_LEGACY_OVERRIDE override
#endif

extern "C" {
const char *convertApple80211IOCTLToString(signed int cmd);
}

class AirportItlwm : public IO80211Controller {
    OSDeclareDefaultStructors(AirportItlwm)
#define IOCTL(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCGA80211) { \
ret = get##REQ(interface, (struct DATA_TYPE* )data); \
} else { \
ret = set##REQ(interface, (struct DATA_TYPE* )data); \
}
    
#define IOCTL_GET(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCGA80211) { \
ret = get##REQ(interface, (struct DATA_TYPE* )data); \
}
#define IOCTL_SET(REQ_TYPE, REQ, DATA_TYPE) \
if (REQ_TYPE == SIOCSA80211) { \
ret = set##REQ(interface, (struct DATA_TYPE* )data); \
}
#define FUNC_IOCTL(REQ, DATA_TYPE) \
FUNC_IOCTL_GET(REQ, DATA_TYPE) \
FUNC_IOCTL_SET(REQ, DATA_TYPE)
#define FUNC_IOCTL_GET(REQ, DATA_TYPE) \
IOReturn get##REQ(OSObject *object, struct DATA_TYPE *data);
#define FUNC_IOCTL_SET(REQ, DATA_TYPE) \
IOReturn set##REQ(OSObject *object, struct DATA_TYPE *data);
    
public:
    virtual bool init(OSDictionary *properties) override;
    virtual void free() override;
    virtual IOService* probe(IOService* provider, SInt32* score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void systemWillShutdown(IOOptionBits specifier) override;
    virtual IOReturn enable(IO80211SkywalkInterface *netif) AIRPORT_LEGACY_OVERRIDE;
    virtual IOReturn disable(IO80211SkywalkInterface *netif) AIRPORT_LEGACY_OVERRIDE;
    virtual IOReturn setHardwareAddress(const void *addr, UInt32 addrBytes) override;
    virtual IOReturn getHardwareAddress(IOEthernetAddress* addrP) override;
    virtual IOReturn getPacketFilters(const OSSymbol *group, UInt32 *filters) const override;
    virtual IOReturn setPromiscuousMode(IOEnetPromiscuousMode mode) override;
    virtual IOReturn setMulticastMode(IOEnetMulticastMode mode) override;
    virtual IOReturn setMulticastList(IOEthernetAddress* addr, UInt32 len) override;
    virtual UInt32 getFeatures() const override;
    virtual const OSString * newVendorString() const override;
    virtual const OSString * newModelString() const override;
    virtual IOReturn selectMedium(const IONetworkMedium *medium) override;
    virtual bool createWorkQueue() override;
    virtual IONetworkInterface * createInterface() override;
    virtual bool configureInterface(IONetworkInterface *netif) override;
    virtual UInt32 outputPacket(mbuf_t, void * param) override;
#ifdef __PRIVATE_SPI__
    virtual IOReturn outputStart(IONetworkInterface *interface, IOOptionBits options) override;
    virtual IOReturn networkInterfaceNotification(
                        IONetworkInterface * interface,
                        uint32_t              type,
                        void *                  argument ) override;
#endif
    virtual bool setLinkStatus(
                               UInt32                  status,
                               const IONetworkMedium * activeMedium = 0,
                               UInt64                  speed        = 0,
                               OSData *                data         = 0) override;
    static IOReturn setLinkStateGated(OSObject *target, void *arg0, void *arg1, void *arg2, void *arg3);
    
    static IOReturn tsleepHandler(OSObject* owner, void* arg0 = 0, void* arg1 = 0, void* arg2 = 0, void* arg3 = 0);
    static void eventHandler(struct ieee80211com *, int, void *);
    /* While a wake recovery is pending this runs it (PCI, then the full firmware init) instead of a bare enable. */
    IOReturn enableAdapter(IONetworkInterface *netif, int via = kWakeViaEnable);
    IOReturn enableAdapterNow(IONetworkInterface *netif);
    static IOReturn enableAdapterGated(OSObject *owner, void *netif, void *via, void *, void *);
    void disableAdapter(IONetworkInterface *netif);
    bool initCCLogs();
    
    #ifdef AIRPORT_WCL
    virtual IO80211WorkQueue *getWorkQueue() const override;
    virtual CCLogStream *getLogger() const override { return driverLogger; }
    CCLogStream *driverLogger;
#else
    virtual IO80211WorkQueue *getWorkQueue() override;
#endif
    virtual bool requiresExplicitMBufRelease() override {
        return false;
    }
    virtual bool flowIdSupported() override {
        return false;
    }
    virtual SInt32 monitorModeSetEnabled(bool, UInt) override {
        return kIOReturnSuccess;
    }
    virtual IOReturn requestQueueSizeAndTimeout(unsigned short *queue, unsigned short *timeout) override {
        XYLog("%s\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    
    virtual bool getLogPipes(CCPipe**, CCPipe**, CCPipe**) override;
    
#ifdef AIRPORT_WCL
    virtual CommonFaultReporter *getFaultReporterFromDriver() override;
#else
    virtual void *getFaultReporterFromDriver() override;
#endif
    
    virtual SInt32 apple80211_ioctl(IO80211SkywalkInterface *,unsigned long,void *, bool, bool) AIRPORT_LEGACY_OVERRIDE;
    virtual SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *) AIRPORT_LEGACY_OVERRIDE;
    virtual SInt32 apple80211SkywalkRequest(UInt,int,IO80211SkywalkInterface *,void *,void *) AIRPORT_LEGACY_OVERRIDE;

    bool createMediumTables(const IONetworkMedium **primary);
    void releaseAll();
    /*
     * macOS (Apple80211's _isInfraInterface, used by airportd and the installer)
     * only takes an interface whose registry entry has IO80211InterfaceRole =
     * Infrastructure. IO80211SkywalkInterface::setInterfaceRole() sets it, yet it
     * was gone after start; these log where it disappears and put it back.
     */
    void checkInterfaceRole(const char *when, bool restore);
    static void roleTimerFired(OSObject *owner, IOTimerEventSource *sender);
    IOTimerEventSource *roleTimer;
    uint32_t roleChecks;
    void watchdogAction(IOTimerEventSource *timer);

    /* AWDL virtual interface — override to create awdl0 */
    virtual IO80211VirtualInterface *createVirtualInterface(ether_addr *ether, UInt role) override;

    virtual SInt32 enableFeature(IO80211FeatureCode, void*) override;
    /* Every request the family does not answer itself passes here first; the first of each id is logged. */
    virtual bool isCommandProhibited(int command) override;
    virtual SInt32 handleCardSpecific(IO80211SkywalkInterface *,unsigned long,void *,bool) override {
        XYLog("%s\n", __FUNCTION__);
        return 0;
    };
    virtual IOReturn getDRIVER_VERSION(IO80211SkywalkInterface *interface,apple80211_version_data *data) override {
        XYLog("%s\n", __FUNCTION__);
        return getDRIVER_VERSION((OSObject *)interface, data);
    };
    virtual IOReturn getHARDWARE_VERSION(IO80211SkywalkInterface *interface,apple80211_version_data *data) override {
        XYLog("%s\n", __FUNCTION__);
        return getHARDWARE_VERSION((OSObject *)interface, data);
    };
    virtual IOReturn getCARD_CAPABILITIES(IO80211SkywalkInterface *interface,apple80211_capability_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getCARD_CAPABILITIES((OSObject *)interface, data);
    }
    virtual IOReturn getPOWER(IO80211SkywalkInterface *interface,apple80211_power_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getPOWER((OSObject *)interface, data);
    }
    virtual IOReturn setPOWER(IO80211SkywalkInterface *interface,apple80211_power_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return setPOWER((OSObject *)interface, data);
    }
    virtual IOReturn getCOUNTRY_CODE(IO80211SkywalkInterface *interface,apple80211_country_code_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return getCOUNTRY_CODE((OSObject *)interface, data);
    }
    virtual IOReturn setCOUNTRY_CODE(IO80211SkywalkInterface *interface,apple80211_country_code_data *data) override {
//        XYLog("%s\n", __FUNCTION__);
        return setCOUNTRY_CODE((OSObject *)interface, data);
    }
    virtual IOReturn setGET_DEBUG_INFO(IO80211SkywalkInterface *interface,apple80211_debug_command *data) override {
        XYLog("%s\n", __FUNCTION__);
        return kIOReturnSuccess;
    }
    
    //scan
    static void fakeScanDone(OSObject *owner, IOTimerEventSource *sender);
    
    //-----------------------------------------------------------------------
    // Power management support.
    //-----------------------------------------------------------------------
    virtual IOReturn registerWithPolicyMaker( IOService * policyMaker ) override;
    virtual IOReturn setPowerState( unsigned long powerStateOrdinal,
                                    IOService *   policyMaker) override;
    virtual IOReturn setWakeOnMagicPacket( bool active ) override;
    void setPowerStateOff(void);
    void setPowerStateOn(void);
    void unregistPM();
    bool initPCIPowerManagment(IOPCIDevice *provider);

    FUNC_IOCTL_GET(CARD_CAPABILITIES, apple80211_capability_data)
    FUNC_IOCTL(POWER, apple80211_power_data)
    FUNC_IOCTL_GET(DRIVER_VERSION, apple80211_version_data)
    FUNC_IOCTL_GET(HARDWARE_VERSION, apple80211_version_data)
    FUNC_IOCTL(COUNTRY_CODE, apple80211_country_code_data)
    
public:
    IOInterruptEventSource* fInterrupt;
    IOTimerEventSource *watchdogTimer;
    IOPCIDevice *pciNub;
    IONetworkStats *fpNetStats;
    AirportItlwmEthernetInterface *bsdInterface;
    #ifdef AIRPORT_WCL
    IO80211InfraInterface *fNetIf;
#else
    IO80211SkywalkInterface *fNetIf;
#endif
    IOWorkLoop *fWatchdogWorkLoop;
    ItlHalService *fHalService;
    
    //IO80211
    uint8_t power_state;
    struct ieee80211_node *fNextNodeToSend;
    bool fScanResultWrapping;
    IOTimerEventSource *scanSource;
    
    u_int32_t current_authtype_lower;
    u_int32_t current_authtype_upper;
    UInt64 currentSpeed;
    UInt32 currentStatus;
    bool disassocIsVoluntary;
    char geo_location_cc[3];
    
    //pm
    thread_call_t powerOnThreadCall;
    thread_call_t powerOffThreadCall;
    thread_call_t wakeRetryCall;
    thread_call_t stallCall;
    bool noWakeFix;
    bool noRadioReset;  /* -qwznoradioreset: Wi-Fi off/on only stops and starts the interface */
    bool noStallFix;  /* -qwznostallfix: no data-stall restart */
    bool stallSbr;  /* -qwzstallsbr: the second data-stall restart also resets the PCIe link */
    /*
     * A chip power-down or wake recovery is running. Both sleep with the
     * gate released (the firmware stop and load wait for interrupts), so a
     * second thread must wait for the first, never run beside it.
     */
    bool chipBusy;
    bool chipClaim(const char *who, uint32_t maxMs);
    bool chipWait(const char *who, uint32_t maxMs);
    void chipRelease();
    const char *wakeLabel;  /* what the next recovery is called in the log */
    bool chipIsDown;  /* a chip power-down completed and no recovery has started since */
    bool chipUsed;  /* the interface was enabled since the firmware was last (re)started */
    volatile bool sleepPending;  /* setPowerState(Off) arrived and the wake has not: no stall restarts */
    bool pmStopping;  /* stop() began: the thread calls do nothing more */
    uint32_t wakeRetryCount;  /* failed recoveries in this chain; the back-off index */
    int wakeRetryVia;  /* what the queued thread call runs the recovery as */
    bool chipDown(const char *why, const char *wakeName, uint32_t maxWaitMs, uint64_t budgetStartMs);
    bool wakeRecover(int via);
    bool wakeAllowed();
    bool onWorkLoopThread();
    bool pmReady();
    void pmAcknowledge();
    void quiescePM();
    void scheduleWakeRetry(uint32_t ms, int via);
    void cancelWakeRetry();
    static IOReturn wakeRetryGated(OSObject *owner, void *, void *, void *, void *);
    static IOReturn radioOffGated(OSObject *owner, void *, void *, void *, void *);
    /* Data-stall restart: the watchdog polls, a thread call restarts the firmware. */
    bool stallPending;  /* a restart is queued or running */
    bool stallNoted;  /* why no restart is made was logged for this stall */
    uint32_t stallRestarts;  /* restarts in this wake/boot window */
    uint64_t stallLastMs;  /* when the last one began */
    void checkDataStall();
    void stallRestart();
    void cancelStallRestart();
    static IOReturn stallRestartGated(OSObject *owner, void *, void *, void *, void *);
    UInt32 pmPowerState;
    IOService *pmPolicyMaker;
    UInt8 pmPCICapPtr;
    bool magicPacketEnabled;
    bool magicPacketSupported;
    
    //AWDL
    uint8_t *syncFrameTemplate;
    uint32_t syncFrameTemplateLength;
    uint8_t awdlBSSID[6];
    uint32_t awdlSyncState;
    uint32_t awdlElectionId;
    uint32_t awdlPresenceMode;
    uint16_t awdlMasterChannel;
    uint16_t awdlSecondaryMasterChannel;
    uint8_t *roamProfile;
    struct apple80211_btc_profiles_data *btcProfile;
    struct apple80211_btc_config_data btcConfig;
    uint32_t btcMode;
    uint32_t btcOptions;
    bool awdlSyncEnable;
    
    CCPipe *driverLogPipe;
    CCPipe *driverDataPathPipe;
    CCPipe *driverSnapshotsPipe;
    
#ifdef AIRPORT_WCL
    bool driverLogPipeStarted = false;
    bool driverDataPathPipeStarted = false;
    bool driverSnapshotsPipeStarted = false;
    CCStream *driverFaultStream;
    CCFaultReporter *driverCoreFaultReporter;
    IO80211FaultReporter *driverFaultReporter;
#else
    CCStream *driverFaultReporter;
#endif
};

#endif /* AirportItlwmV2_hpp */
