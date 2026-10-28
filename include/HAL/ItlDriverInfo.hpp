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

#ifndef ItlDriverInfo_h
#define ItlDriverInfo_h

class ItlDriverInfo {
    
public:
    
    virtual const char *getFirmwareVersion() = 0;
    
    virtual int16_t getBSSNoise() = 0;
    
    virtual bool is5GBandSupport() = 0;
    
    virtual int getTxNSS() = 0;
    
    virtual const char *getFirmwareName() = 0;
    
    virtual UInt32 supportedFeatures() = 0;

    virtual const char *getFirmwareCountryCode() = 0;

    virtual uint32_t getTxQueueSize() = 0;

    /* Native Wi-Fi: rate of the last received data frame, 0 / -1 if unknown. */
    virtual uint32_t getRxRateKbps() { return 0; }
    virtual int getRxRateMcs() { return -1; }
    virtual int getRxRateNss() { return 0; }
    virtual int getRxRateSgi() { return -1; }	/* 1 short GI, 0 long, -1 unknown */

    /*
     * Native Wi-Fi: RSSI (dBm) of the associated BSS as macOS should see it.
     * `raw` is the node's last-frame value; a driver that smooths it returns
     * the smoothed one, and anything else (not associated, no sample yet)
     * returns `raw` unchanged.
     */
    virtual int8_t getBSSRssi(int8_t raw) { return raw; }
};

#endif /* ItlDriverInfo_h */
