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

#ifndef ItlHalService_hpp
#define ItlHalService_hpp

#include <libkern/c++/OSObject.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/network/IOEthernetController.h>
#include <IOKit/network/IOEthernetInterface.h>

#include "ItlDriverInfo.hpp"
#include "ItlDriverController.hpp"

#include <net80211/ieee80211_var.h>

class ItlHalService : public OSObject {
    OSDeclareAbstractStructors(ItlHalService)
    
public:
    
    virtual bool attach(IOPCIDevice *device) = 0;
    
    virtual void detach(IOPCIDevice *device) = 0;
    
    virtual IOReturn enable(IONetworkInterface *interface) = 0;
    
    virtual IOReturn disable(IONetworkInterface *interface) = 0;

    /*
     * System power-off/restart: stop the interface and quiesce the chip
     * while the system is still fully alive; detach() then skips the
     * hardware teardown. Default: nothing.
     */
    virtual void shutdownHardware() {}

    /*
     * System sleep/wake (AirportWCN7850). All three run holding the command
     * gate. sleepHardware(): power the chip down cleanly and save the PCI
     * config state. wakeNeedsRecovery(): a full re-init is pending.
     * wakeHardware(): bring the PCIe link and config space back (the caller
     * then re-enables the interface, which re-inits the firmware).
     * wakeComplete(): the caller reports the final outcome. Default: nothing.
     *
     * sleepQuiesce(): called before the interface goes down for sleep or
     * Wi-Fi off; waits (bounded) until in-flight state work (a leave-network
     * peer delete, key and block-ack tasks, an init) has finished, so the
     * stop that follows tears the association down in order; it waits at most
     * maxMs.
     * dataStalled(): polled once a second; true when the interface sat in RUN
     * for a while without one data frame while management frames kept coming.
     * *healthy (if given) says data has flowed in RUN for a long time.
     * requestBusReset(): the next wakeHardware() also does a secondary bus reset.
     * firmwareUp(): the firmware is running (fw_initialized).
     */
    virtual void sleepQuiesce(uint32_t maxMs) { (void)maxMs; }
    virtual bool firmwareUp() { return false; }
    virtual void sleepHardware() {}
    virtual bool wakeNeedsRecovery() { return false; }
    virtual IOReturn wakeHardware() { return kIOReturnSuccess; }
    virtual void wakeComplete(bool ok) { (void)ok; }
    virtual bool dataStalled(bool *healthy) { if (healthy) *healthy = false; return false; }
    virtual void requestBusReset() {}
    
    virtual struct ieee80211com *get80211Controller() = 0;
    
#ifdef AIRPORT_WCL
    /* Native Wi-Fi (AirportWCN7850): off-channel time per channel macOS asks scans to use. */
    virtual IOReturn setScanHomeAwayTime(uint32_t) { return kIOReturnUnsupported; }
#endif
    virtual ItlDriverInfo *getDriverInfo() = 0;
    
    virtual ItlDriverController *getDriverController() = 0;
    
    virtual void free() override;

public:
    virtual bool initWithController(IOEthernetController *controller, IOWorkLoop *workloop, IOCommandGate *commandGate);
    
protected:
    
    int tsleep_nsec(void *ident, int priority, const char *wmesg, int timo);
    
    void wakeupOn(void* ident);
    
    IOEthernetController *getController();
    
    IOCommandGate *getMainCommandGate();
    
    IOWorkLoop *getMainWorkLoop();
    
private:
    IOEthernetController *controller;
    IOCommandGate *mainCommandGate;
    IOWorkLoop *mainWorkLoop;

    lck_grp_t *inner_gp;
    lck_grp_attr_t *inner_gp_attr;
    lck_attr_t *inner_attr;
    lck_mtx_t *inner_lock;
};

#endif /* ItlHalService_hpp */
