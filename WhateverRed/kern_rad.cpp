//
//  kern_rad.cpp
//  WhateverRed
//
//  Copyright © 2017 vit9696. All rights reserved.
//  Copyright © 2022 VisualDevelopment. All rights reserved.
//

#include "kern_rad.hpp"

#include <Availability.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>

#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <Headers/kern_file.hpp>
#include <Headers/kern_iokit.hpp>

#include "kern_fw.hpp"
#include "kern_netdbg.hpp"

static const char *pathFramebuffer[] = {
    "/System/Library/Extensions/AMDFramebuffer.kext/Contents/MacOS/"
    "AMDFramebuffer"};
static const char *pathSupport[] = {
    "/System/Library/Extensions/AMDSupport.kext/Contents/MacOS/AMDSupport"};
static const char *pathRadeonX6000[] = {
    "/System/Library/Extensions/AMDRadeonX6000.kext/Contents/MacOS/"
    "AMDRadeonX6000"};
static const char *pathRadeonX6000HWLibs[] = {
    "/System/Library/Extensions/AMDRadeonX6000HWServices.kext/Contents/PlugIns/"
    "AMDRadeonX6000HWLibs.kext/Contents/MacOS/AMDRadeonX6000HWLibs",
};
static const char *pathRadeonX6000Framebuffer[] = {
    "/System/Library/Extensions/AMDRadeonX6000Framebuffer.kext/Contents/MacOS/"
    "AMDRadeonX6000Framebuffer"};

static KernelPatcher::KextInfo kextRadeonFramebuffer{
    "com.apple.kext.AMDFramebuffer",  pathFramebuffer, 1, {}, {},
    KernelPatcher::KextInfo::Unloaded};
static KernelPatcher::KextInfo kextRadeonSupport{
    "com.apple.kext.AMDSupport",      pathSupport, 1, {}, {},
    KernelPatcher::KextInfo::Unloaded};
static KernelPatcher::KextInfo kextRadeonX6000HWLibs{
    "com.apple.kext.AMDRadeonX6000HWLibs", pathRadeonX6000HWLibs, 1, {}, {},
    KernelPatcher::KextInfo::Unloaded};
static KernelPatcher::KextInfo kextRadeonX6000Framebuffer{
    "com.apple.kext.AMDRadeonX6000Framebuffer",
    pathRadeonX6000Framebuffer,
    1,
    {},
    {},
    KernelPatcher::KextInfo::Unloaded};

static KernelPatcher::KextInfo kextRadeonHardware[] = {
    {"com.apple.kext.AMDRadeonX6000",
     pathRadeonX6000,
     arrsize(pathRadeonX6000),
     {},
     {},
     KernelPatcher::KextInfo::Unloaded},
};

/**
 *  Power-gating flags
 *  Each symbol corresponds to a bit provided in a radpg argument mask
 */
static const char *powerGatingFlags[] = {
    "CAIL_DisableDrmdmaPowerGating",       "CAIL_DisableGfxCGPowerGating",
    "CAIL_DisableUVDPowerGating",          "CAIL_DisableVCEPowerGating",
    "CAIL_DisableDynamicGfxMGPowerGating", "CAIL_DisableGmcPowerGating",
    "CAIL_DisableAcpPowerGating",          "CAIL_DisableSAMUPowerGating",
};

RAD *RAD::callbackRAD;

void RAD::init() {
    callbackRAD = this;

    currentPropProvider.init();

    force24BppMode = checkKernelArgument("-rad24");

    if (force24BppMode) lilu.onKextLoadForce(&kextRadeonFramebuffer);

    dviSingleLink = checkKernelArgument("-raddvi");
    fixConfigName = checkKernelArgument("-radcfg");
    forceVesaMode = checkKernelArgument("-radvesa");
    forceCodecInfo = checkKernelArgument("-radcodec");

    lilu.onKextLoadForce(&kextRadeonX6000Framebuffer);
    lilu.onKextLoadForce(&kextRadeonSupport);
    lilu.onKextLoadForce(&kextRadeonX6000HWLibs);

    initHardwareKextMods();

    // FIXME: autodetect?
    uint32_t powerGatingMask = 0;
    PE_parse_boot_argn("radpg", &powerGatingMask, sizeof(powerGatingMask));
    for (size_t i = 0; i < arrsize(powerGatingFlags); i++) {
        if (!(powerGatingMask & (1 << i))) {
            DBGLOG("rad", "not enabling %s", powerGatingFlags[i]);
            powerGatingFlags[i] = nullptr;
        } else {
            DBGLOG("rad", "enabling %s", powerGatingFlags[i]);
        }
    }
}

void RAD::deinit() {}

[[noreturn]] [[gnu::cold]] void RAD::wrapPanic(const char *fmt, ...) {
    va_list args, netdbg_args;
    va_start(args, fmt);
    va_copy(netdbg_args, args);
    NETDBG::printf("panic: ");
    NETDBG::vprintf(fmt, netdbg_args);
    va_end(netdbg_args);
    IOSleep(1000);
    FunctionCast(wrapPanic, callbackRAD->orgPanic)(fmt, args);
    PE_enter_debugger("Panic Fallback");
    for (;;) {
        asm volatile("hlt");
    }
}

[[noreturn]] [[gnu::cold]] void RAD::wrapEnterDebugger(const char *cause) {
    NETLOG("rad", "Debugger requested: %s", cause);
    IOSleep(1000);
    FunctionCast(wrapEnterDebugger, callbackRAD->orgEnterDebugger)(cause);
    panic("Debugger call somehow returned");
}

void RAD::processKernel(KernelPatcher &patcher, DeviceInfo *info) {
    for (size_t i = 0; i < info->videoExternal.size(); i++) {
        if (info->videoExternal[i].vendor == WIOKit::VendorID::ATIAMD) {
            if (info->videoExternal[i].video->getProperty("enable-gva-support"))
                enableGvaSupport = true;

            auto smufw =
                OSDynamicCast(OSData, info->videoExternal[i].video->getProperty(
                                          "Force_Load_FalconSMUFW"));
            if (smufw && smufw->getLength() == 1) {
                info->videoExternal[i].video->setProperty(
                    "Force_Load_FalconSMUFW",
                    *static_cast<const uint8_t *>(smufw->getBytesNoCopy())
                        ? kOSBooleanTrue
                        : kOSBooleanFalse);
            }
        }
    }

    int gva;
    if (PE_parse_boot_argn("radgva", &gva, sizeof(gva)))
        enableGvaSupport = gva != 0;

    KernelPatcher::RouteRequest requests[] = {
        {"__ZN15IORegistryEntry11setPropertyEPKcPvj", wrapSetProperty,
         orgSetProperty},
        {"__ZNK15IORegistryEntry11getPropertyEPKc", wrapGetProperty,
         orgGetProperty},
        {"_panic", wrapPanic, orgPanic},
        {"_PE_enter_debugger", wrapEnterDebugger},
    };
    if (!patcher.routeMultipleLong(KernelPatcher::KernelID, requests)) {
        panic("Failed to route kernel symbols");
    }
}

void RAD::wrapAmdTtlServicesConstructor(IOService *that,
                                        IOPCIDevice *provider) {
    NETDBG::enabled = true;
    NETLOG("rad", "patching device type table");
    MachInfo::setKernelWriting(true, KernelPatcher::kernelWriteLock);
    *(uint32_t *)callbackRAD->orgDeviceTypeTable =
        provider->extendedConfigRead16(kIOPCIConfigDeviceID);
    *((uint32_t *)callbackRAD->orgDeviceTypeTable + 1) = 1;
    MachInfo::setKernelWriting(false, KernelPatcher::kernelWriteLock);

    NETLOG("rad", "calling original AmdTtlServices constructor");
    FunctionCast(wrapAmdTtlServicesConstructor,
                 callbackRAD->orgAmdTtlServicesConstructor)(that, provider);
}

uint64_t RAD::wrapIpiSmuSwInit(void *tlsInstance) {
    NETLOG("rad", "_ipi_smu_sw_init: tlsInstance = %p", tlsInstance);
    auto ret = FunctionCast(wrapIpiSmuSwInit,
                            callbackRAD->orgIpiSmuSwInit)(tlsInstance);
    NETLOG("rad", "_ipi_smu_sw_init returned 0x%llX", ret);
    return ret;
}

uint64_t RAD::wrapSmuSwInit(void *input, uint64_t *output) {
    NETLOG("rad", "_smu_sw_init: input = %p output = %p", input, output);
    auto ret =
        FunctionCast(wrapSmuSwInit, callbackRAD->orgSmuSwInit)(input, output);
    NETLOG("rad", "_smu_sw_init: output 0:0x%llX 1:0x%llX", output[0],
           output[1]);
    NETLOG("rad", "_smu_sw_init returned 0x%llX", ret);
    return ret;
}

uint32_t RAD::wrapSmuInternalSwInit(uint64_t param1, uint64_t param2,
                                    void *param3) {
    NETLOG("rad",
           "_smu_internal_sw_init: param1 = 0x%llX param2 = 0x%llX param3 = %p",
           param1, param2, param3);
    auto ret =
        FunctionCast(wrapSmuInternalSwInit, callbackRAD->orgSmuInternalSwInit)(
            param1, param2, param3);
    NETLOG("rad", "_smu_internal_sw_init returned 0x%X", ret);
    return ret;
}

uint64_t RAD::wrapSmuGetHwVersion(uint64_t param1, uint32_t param2) {
    NETLOG("rad", "_smu_get_hw_version: param1 = 0x%llX param2 = 0x%X", param1,
           param2);
    auto ret = FunctionCast(wrapSmuGetHwVersion,
                            callbackRAD->orgSmuGetHwVersion)(param1, param2);
    NETLOG("rad", "_smu_get_hw_version returned 0x%llX", ret);
    switch (ret) {
        case 0x2:
            NETLOG("rad", "Spoofing SMU v10 to v9.0.1");
            return 0x1;
        case 0xB:
            [[fallthrough]];
        case 0xC:
            NETLOG("rad", "Spoofing SMU v11/v12 to v11");
            return 0x3;
        default:
            return ret;
    }
}

uint64_t RAD::wrapPspSwInit(uint32_t *param1, uint32_t *param2) {
    NETLOG("rad", "_psp_sw_init: param1 = %p param2 = %p", param1, param2);
    NETLOG("rad",
           "_psp_sw_init: param1: 0:0x%X 1:0x%X 2:0x%X 3:0x%X 4:0x%X 5:0x%X",
           param1[0], param1[1], param1[2], param1[3], param1[4], param1[5]);
    switch (param1[3]) {
        case 0xA:
            NETLOG("rad", "Spoofing PSP version v10 to v9.0.2");
            param1[3] = 0x9;
            param1[4] = 0x0;
            param1[5] = 0x2;
            break;
        case 0xB:
            [[fallthrough]];
        case 0xC:
            NETLOG("rad", "Spoofing PSP version v10/v11/v12 to v11");
            param1[3] = 0xB;
            param1[4] = 0x0;
            param1[5] = 0x0;
            break;
        default:
            break;
    }
    auto ret =
        FunctionCast(wrapPspSwInit, callbackRAD->orgPspSwInit)(param1, param2);
    NETLOG("rad", "_psp_sw_init returned 0x%llX", ret);
    return ret;
}

uint32_t RAD::wrapGcGetHwVersion(uint32_t *param1) {
    NETLOG("rad", "_gc_get_hw_version: param1 = %p", param1);
    auto ret = FunctionCast(wrapGcGetHwVersion,
                            callbackRAD->orgGcGetHwVersion)(param1);
    NETLOG("rad", "_gc_get_hw_version returned 0x%X", ret);
    switch (ret & 0xFFFF00) {
        case 0x90100:
            NETLOG("rad", "Spoofing GC version 9.1.x to 9.0.1");
            return 0x90001;
        case 0x90200:
            NETLOG("rad", "Spoofing GC version 9.2.x to 9.2.1");
            return 0x90201;
        default:
            return ret;
    }
}

uint32_t RAD::wrapInternalCosReadFw(uint64_t param1, uint64_t *param2) {
    NETLOG("rad", "_internal_cos_read_fw: param1 = 0x%llX param2 = %p", param1,
           param2);
    auto ret = FunctionCast(wrapInternalCosReadFw,
                            callbackRAD->orgInternalCosReadFw)(param1, param2);
    NETLOG("rad", "_internal_cos_read_fw returned 0x%X", ret);
    return ret;
}

void RAD::wrapPopulateFirmwareDirectory(uint64_t that) {
    NETLOG(
        "rad",
        "AMDRadeonX6000_AMDRadeonHWLibsX6000::populateFirmwareDirectory this "
        "= 0x%llX",
        that);
    FunctionCast(wrapPopulateFirmwareDirectory,
                 callbackRAD->orgPopulateFirmwareDirectory)(that);
    NETLOG("rad", "injecting ativvaxy_rv.dat!");
    auto *fwDesc = getFWDescByName("ativvaxy_rv.dat");
    auto *fwDescBackdoor = getFWDescByName("atidmcub_0.dat");
    if (!fwDesc) {
        panic("Somehow ativvaxy_rv.dat is missing");
    }
    if (!fwDescBackdoor) {
        panic("Somehow atidmcub_0.dat is missing");
    }

    auto *fw = callbackRAD->orgCreateFirmware(fwDesc->getBytesNoCopy(),
                                              fwDesc->getLength(), 0x200,
                                              "ativvaxy_rv.dat");
    auto *fwBackdoor = callbackRAD->orgCreateFirmware(
        fwDesc->getBytesNoCopy(), fwDesc->getLength(), 0x200, "atidmcub_0.dat");
    auto *fwDir = *reinterpret_cast<void **>(that + 0xB8);
    NETLOG("rad", "fwDir = %p", fwDir);
    if (!callbackRAD->orgPutFirmware(fwDir, 1, fw)) {
        panic("Failed to inject ativvaxy_rv.dat firmware");
    }
    if (!callbackRAD->orgPutFirmware(fwDir, 1, fwBackdoor)) {
        panic("Failed to inject atidmcub_0.dat firmware");
    }
}

void *RAD::wrapCreateAtomBiosProxy(void *param1) {
    NETLOG("rad",
           "------------------------------------------------------------"
           "----------");
    NETLOG("rad", "createAtomBiosProxy: param1 = %p", param1);
    auto ret = FunctionCast(wrapCreateAtomBiosProxy,
                            callbackRAD->orgCreateAtomBiosProxy)(param1);
    NETLOG("rad", "createAtomBiosProxy returned %p", ret);
    return ret;
}

IOReturn RAD::wrapPopulateDeviceMemory(void *that, uint32_t reg) {
    DBGLOG("rad", "populateDeviceMemory: this = %p reg = 0x%X", that, reg);
    auto ret = FunctionCast(wrapPopulateDeviceMemory,
                            callbackRAD->orgPopulateDeviceMemory)(that, reg);
    DBGLOG("rad", "populateDeviceMemory returned 0x%X", ret);
    return kIOReturnSuccess;
}

uint64_t RAD::wrapMCILUpdateGfxCGPG(void *param1) {
    NETLOG("rad",
           "------------------------------------------------------------"
           "----------");
    NETLOG("rad", "_Cail_MCILUpdateGfxCGPG: param1 = %p", param1);
    auto ret = FunctionCast(wrapMCILUpdateGfxCGPG,
                            callbackRAD->orgMCILUpdateGfxCGPG)(param1);
    NETLOG("rad", "_Cail_MCILUpdateGfxCGPG returned 0x%llX", ret);
    return ret;
}

IOReturn RAD::wrapQueryEngineRunningState(void *that, void *param1,
                                          void *param2) {
    NETLOG("rad", "queryEngineRunningState: this = %p param1 = %p param2 = %p",
           that, param1, param2);
    NETLOG("rad", "queryEngineRunningState: *param2 = 0x%X",
           *static_cast<uint32_t *>(param2));
    auto ret = FunctionCast(wrapQueryEngineRunningState,
                            callbackRAD->orgQueryEngineRunningState)(
        that, param1, param2);
    NETLOG("rad", "queryEngineRunningState: after *param2 = 0x%X",
           *static_cast<uint32_t *>(param2));
    NETLOG("rad", "queryEngineRunningState returned 0x%X", ret);
    return ret;
}

IOReturn RAD::wrapQueryComputeQueueIsIdle(void *that, uint64_t param1) {
    NETLOG("rad", "QueryComputeQueueIsIdle: this = %p param1 = 0x%llX", that,
           param1);
    auto ret =
        FunctionCast(wrapQueryComputeQueueIsIdle,
                     callbackRAD->orgQueryComputeQueueIsIdle)(that, param1);
    NETLOG("rad", "QueryComputeQueueIsIdle returned 0x%X", ret);
    return ret;
}

uint64_t RAD::wrapCAILQueryEngineRunningState(void *param1, uint32_t *param2,
                                              uint64_t param3) {
    NETLOG(
        "rad",
        "_CAILQueryEngineRunningState: param1 = %p param2 = %p param3 = %llX",
        param1, param2, param3);
    NETLOG("rad", "_CAILQueryEngineRunningState: *param2 = 0x%X", *param2);
    auto ret = FunctionCast(wrapCAILQueryEngineRunningState,
                            callbackRAD->orgCAILQueryEngineRunningState)(
        param1, param2, param3);
    NETLOG("rad", "_CAILQueryEngineRunningState: after *param2 = 0x%X",
           *param2);
    NETLOG("rad", "_CAILQueryEngineRunningState returned 0x%llX", ret);
    return ret;
}

uint64_t RAD::wrapCailMonitorEngineInternalState(void *that, uint32_t param1,
                                                 uint32_t *param2) {
    NETLOG(
        "rad",
        "_CailMonitorEngineInternalState: this = %p param1 = 0x%X param2 = %p",
        that, param1, param2);
    NETLOG("rad", "_CailMonitorEngineInternalState: *param2 = 0x%X", *param2);
    auto ret = FunctionCast(wrapCailMonitorEngineInternalState,
                            callbackRAD->orgCailMonitorEngineInternalState)(
        that, param1, param2);
    NETLOG("rad", "_CailMonitorEngineInternalState: after *param2 = 0x%X",
           *param2);
    NETLOG("rad", "_CailMonitorEngineInternalState returned 0x%llX", ret);
    return ret;
}

uint64_t RAD::wrapCailMonitorPerformanceCounter(void *that, uint32_t *param1) {
    NETLOG("rad", "_CailMonitorPerformanceCounter: this = %p param1 = %p", that,
           param1);
    NETLOG("rad", "_CailMonitorPerformanceCounter: *param1 = 0x%X", *param1);
    auto ret = FunctionCast(wrapCailMonitorPerformanceCounter,
                            callbackRAD->orgCailMonitorPerformanceCounter)(
        that, param1);
    NETLOG("rad", "_CailMonitorPerformanceCounter: after *param1 = 0x%X",
           *param1);
    NETLOG("rad", "_CailMonitorPerformanceCounter returned 0x%llX", ret);
    return ret;
}

bool RAD::wrapAMDHWChannelWaitForIdle(void *that, uint64_t param1) {
    NETLOG(
        "rad",
        "AMDRadeonX6000_AMDHWChannel::waitForIdle: this = %p param1 = 0x%llx",
        that, param1);
    auto ret =
        FunctionCast(wrapAMDHWChannelWaitForIdle,
                     callbackRAD->orgAMDHWChannelWaitForIdle)(that, param1);
    NETLOG("rad", "AMDRadeonX6000_AMDHWChannel::waitForIdle returned %d", ret);
    return ret;
}

IOReturn RAD::wrapPpEnable(void *that, bool param1) {
    NETLOG("rad", "ppEnable: this = %p param1 = %d", that, param1);
    auto ret =
        FunctionCast(wrapPpEnable, callbackRAD->orgPpEnable)(that, param1);
    NETLOG("rad", "ppEnable returned 0x%X", ret);
    return ret;
}

uint64_t RAD::wrapPECISetupInitInfo(uint32_t *param1, uint32_t *param2) {
    NETLOG("rad", "_PECI_SetupInitInfo: param1 = %p param2 = %p", param1,
           param2);
    NETLOG("rad", "_PECI_SetupInitInfo: *param1 = 0x%X", *param1);
    NETLOG("rad",
           "_PECI_SetupInitInfo: param2 before: 0:0x%X 1:0x%X 2:0x%X 3:0x%X",
           param2[0], param2[1], param2[2], param2[3]);
    auto ret = FunctionCast(wrapPECISetupInitInfo,
                            callbackRAD->orgPECISetupInitInfo)(param1, param2);
    NETLOG("rad",
           "_PECI_SetupInitInfo: param2 after: 0:0x%X 1:0x%X 2:0x%X 3:0x%X",
           param2[0], param2[1], param2[2], param2[3]);
    NETLOG("rad", "_PECI_SetupInitInfo returned 0x%llX", ret);
    return ret;
}

uint64_t RAD::wrapPECIReadRegistry(void *param1, char *key, uint64_t param3,
                                   uint64_t param4) {
    NETLOG("rad",
           "_PECI_ReadRegistry param1 = %p key = %p param3 = 0x%llX param4 = "
           "0x%llX",
           param1, key, param3, param4);
    NETLOG("rad", "_PECI_ReadRegistry key is %s", key);
    auto ret =
        FunctionCast(wrapPECIReadRegistry, callbackRAD->orgPECIReadRegistry)(
            param1, key, param3, param4);
    NETLOG("rad", "_PECI_ReadRegistry returned 0x%llX", ret);
    return ret;
}

uint64_t RAD::wrapSMUMInitialize(uint64_t param1, uint32_t *param2,
                                 uint64_t param3) {
    NETLOG("rad",
           "_SMUM_Initialize: param1 = 0x%llX param2 = %p param3 = 0x%llX",
           param1, param2, param3);
    auto ret = FunctionCast(wrapSMUMInitialize, callbackRAD->orgSMUMInitialize)(
        param1, param2, param3);
    NETLOG("rad", "_SMUM_Initialize returned 0x%llX", ret);
    return ret;
}

uint64_t RAD::wrapPECIRetrieveBiosDataTable(void *param1, uint64_t param2,
                                            uint64_t **param3) {
    NETLOG(
        "rad",
        "_PECI_RetrieveBiosDataTable: param1 = %p param2 = 0x%llX param3 = %p",
        param1, param2, param3);
    auto ret = FunctionCast(wrapPECIRetrieveBiosDataTable,
                            callbackRAD->orgPECIRetrieveBiosDataTable)(
        param1, param2, param3);
    NETLOG("rad", "_PECI_RetrieveBiosDataTable returned 0x%llX", ret);
    return ret;
}

void *RAD::wrapCreatePowerTuneServices(void *param1, void *param2) {
    NETLOG("rad", "createPowerTuneServices: param1 = %p param2 = %p", param1,
           param2);
    auto *ret = IOMallocZero(0x18);
    callbackRAD->orgVega10PowerTuneServicesConstructor(ret, param1, param2);
    return ret;
}

uint16_t RAD::wrapGetFamilyId() {
    // Usually, the value is hardcoded to 0x8d which is Vega 10
    // So we now hard code it to Raven
    return 0x8e;
}

uint32_t RAD::wrapGetHwRevision(uint32_t major, uint32_t minor,
                                uint32_t patch) {
    NETLOG("rad", "_get_hw_revision: minor = 0x%X major = 0x%X patch = 0x%X",
           minor, major, patch);
    return (minor << 0x8) | (major << 0x10) | patch;
}

IOReturn RAD::wrapPopulateDeviceInfo(uint64_t that) {
    NETLOG("rad",
           "AMDRadeonX6000_AmdAsicInfoNavi::populateDeviceInfo: this = 0x%llX",
           that);
    auto ret = FunctionCast(wrapPopulateDeviceInfo,
                            callbackRAD->orgPopulateDeviceInfo)(that);
    auto *pciDev = *reinterpret_cast<IOPCIDevice **>(that + 0x18);
    auto *familyId = reinterpret_cast<uint32_t *>(that + 0x60);
    auto *deviceId = reinterpret_cast<uint32_t *>(that + 0x64);
    auto *revision = reinterpret_cast<uint32_t *>(that + 0x68);
    auto *emulatedRevision = reinterpret_cast<uint32_t *>(that + 0x6c);
    NETLOG("rad",
           "before: familyId = 0x%X deviceId = 0x%X revision = 0x%X "
           "emulatedRevision = 0x%X",
           *familyId, *deviceId, *revision, *emulatedRevision);
    *familyId = 0x8e;
    switch (pciDev->configRead16(kIOPCIConfigDeviceID)) {
        case 0x15d8:
            *emulatedRevision = *revision + 0x41;
            break;
        case 0x15dd:
            if (*revision >= 0x8) {
                *emulatedRevision = *revision + 0x79;
            }
            break;
        default:
            if (*revision == 1) {
                *emulatedRevision = *revision + 0x20;
            } else {
                *emulatedRevision = *revision + 0x10;
            }
            break;
    }
    NETLOG("rad", "after: familyId = 0x%X emulatedRevision = 0x%X", *familyId,
           *emulatedRevision);
    NETLOG("rad",
           "AMDRadeonX6000_AmdAsicInfoNavi::populateDeviceInfo returned 0x%X",
           ret);
    return ret;
}

uint64_t RAD::wrapSmuGetFwConstants() {
    /*
     * According to Linux AMDGPU source code,
     * on APUs, the System BIOS is the one that loads the SMC Firmware, and
     * therefore, we must not load any firmware ourselves.
     * We fix that by making _smu_get_fw_constants a no-op.
     */
    return 0;
}

bool RAD::wrapTtlDevIsVega10Device() {
    /*
     * AMD iGPUs are Vega 10 based.
     */
    return true;
}

uint64_t RAD::wrapSmu901InternalHwInit() {
    /*
     * This is _smu_9_0_1_internal_hw_init.
     * The original function waits for the firmware to be loaded,
     * which in the Linux AMDGPU code is smu9_is_smc_ram_running.
     * SMU 10 doesn't do this, therefore, we just return 0.
     */
    return 0;
}

void RAD::wrapCosDebugPrint(char *fmt, ...) {
    NETDBG::printf("AMD TTL COS: ");
    va_list args, netdbg_args;
    va_start(args, fmt);
    va_copy(netdbg_args, args);
    NETDBG::vprintf(fmt, netdbg_args);
    va_end(netdbg_args);
    FunctionCast(wrapCosDebugPrint, callbackRAD->orgCosDebugPrint)(fmt, args);
    va_end(args);
}

void RAD::wrapMCILDebugPrint(uint32_t level_max, char *fmt, uint64_t param3,
                             uint64_t param4, uint64_t param5, uint level) {
    NETDBG::printf("_MCILDebugPrint PARAM1 = 0x%X: ", level_max);
    NETDBG::printf(fmt, param3, param4, param5, level);
    FunctionCast(wrapMCILDebugPrint, callbackRAD->orgMCILDebugPrint)(
        level_max, fmt, param3, param4, param5, level);
}

void RAD::wrapCosDebugPrintVaList(void *ttl, char *header, char *fmt,
                                  va_list args) {
    NETDBG::printf("AMD TTL COS: %s ", header);
    va_list netdbg_args;
    va_copy(netdbg_args, args);
    NETDBG::vprintf(fmt, netdbg_args);
    NETDBG::printf("\n");
    va_end(netdbg_args);
    FunctionCast(wrapCosDebugPrintVaList, callbackRAD->orgCosDebugPrintVaList)(
        ttl, header, fmt, args);
}

void RAD::wrapCosReleasePrintVaList(void *ttl, char *header, char *fmt,
                                    va_list args) {
    NETDBG::printf("AMD TTL COS: %s ", header);
    va_list netdbg_args;
    va_copy(netdbg_args, args);
    NETDBG::vprintf(fmt, netdbg_args);
    va_end(netdbg_args);
    FunctionCast(wrapCosReleasePrintVaList,
                 callbackRAD->orgCosReleasePrintVaList)(ttl, header, fmt, args);
}

uint32_t RAD::wrapGetVideoMemoryType(void *that) {
    NETLOG("rad", "getVideoMemoryType: this = %p", that);
    auto ret = FunctionCast(wrapGetVideoMemoryType,
                            callbackRAD->orgGetVideoMemoryType)(that);
    NETLOG("rad", "getVideoMemoryType returned 0x%X", ret);
    return ret != 0 ? ret : 4;
}

uint32_t RAD::wrapGetVideoMemoryBitWidth(void *that) {
    NETLOG("rad", "getVideoMemoryBitWidth: this = %p", that);
    auto ret = FunctionCast(wrapGetVideoMemoryBitWidth,
                            callbackRAD->orgGetVideoMemoryBitWidth)(that);
    NETLOG("rad",
           "getVideoMemoryBitWidth "
           "returned 0x%X",
           ret);
    return ret != 0 ? ret : 64;
}

IOReturn RAD::wrapPopulateVramInfo(void *that, void *param1) {
    NETLOG("rad", "populateVramInfo: this = %p param1 = %p", that, param1);
    return kIOReturnSuccess;
}

uint64_t RAD::wrapIsAsicCapEnabled(void *that, uint32_t cap) {
    /*
     * The AMD kexts have no ASIC Capabilities for iGPUs,
     * so the logic fails to query for capabilities, therefore
     * all capabilities return false, and the wrong logic is used.
     * From our RE efforts, we have found that for 0x148 it needs
     * to return true.
     */
    NETLOG("rad", "isAsicCapEnabled: that = %p cap = 0x%X", that, cap);
    switch (cap) {
        case 0x134:
            [[fallthrough]];
        case 0x148:
            NETLOG("rad", "isAsicCapEnabled: returning true");
            return true;
        default:
            NETLOG("rad", "isAsicCapEnabled: returning false");
            return false;
    }
}

uint32_t RAD::wrapDcePanelCntlHwInit(void *panel_cntl) {
    callbackRAD->panelCntlPtr = panel_cntl;
    callbackRAD
        ->updatePwmMaxBrightnessFromInternalDisplay();  // read max brightness
                                                        // value from IOReg
    uint32_t ret = FunctionCast(wrapDcePanelCntlHwInit,
                                callbackRAD->orgDcePanelCntlHwInit)(panel_cntl);
    return ret;
}

IOReturn RAD::wrapAMDRadeonX6000AmdRadeonFramebufferSetAttribute(
    IOService *framebuffer, IOIndex connectIndex, IOSelect attribute,
    uintptr_t value) {
    auto ret = FunctionCast(
        wrapAMDRadeonX6000AmdRadeonFramebufferSetAttribute,
        callbackRAD->orgAMDRadeonX6000AmdRadeonFramebufferSetAttribute)(
        framebuffer, connectIndex, attribute, value);
    if (attribute != (UInt32)'bklt') {
        return ret;
    }

    if (callbackRAD->maxPwmBacklightLvl == 0) {
        NETLOG("rad",
               "wrapAMDRadeonX6000AmdRadeonFramebufferSetAttribute zero "
               "maxPwmBacklightLvl");
        return 0;
    }

    if (callbackRAD->panelCntlPtr == nullptr) {
        NETLOG("rad",
               "wrapAMDRadeonX6000AmdRadeonFramebufferSetAttribute null panel "
               "cntl");
        return 0;
    }

    if (callbackRAD->orgDceDriverSetBacklight == nullptr) {
        NETLOG("rad",
               "wrapAMDRadeonX6000AmdRadeonFramebufferSetAttribute null "
               "orgDcLinkSetBacklightLevel");
        return 0;
    }

    // set the backlight of AMD navi10 driver
    callbackRAD->curPwmBacklightLvl = (uint32_t)value;
    uint32_t btlper =
        callbackRAD->curPwmBacklightLvl * 100 / callbackRAD->maxPwmBacklightLvl;
    uint32_t pwmval = 0;
    if (btlper >= 100) {
        // This is from the dmcu_set_backlight_level function of Linux source
        // ...
        // if (backlight_pwm_u16_16 & 0x10000)
        // 	   backlight_8_bit = 0xFF;
        // else
        // 	   backlight_8_bit = (backlight_pwm_u16_16 >> 8) & 0xFF;
        // ...
        // The max brightness should have 0x10000 bit set
        pwmval = 0x1FF00;
    } else {
        pwmval = ((btlper * 0xFF) / 100) << 8U;
    }

    callbackRAD->orgDceDriverSetBacklight(callbackRAD->panelCntlPtr, pwmval);
    return 0;
}

IOReturn RAD::wrapAMDRadeonX6000AmdRadeonFramebufferGetAttribute(
    IOService *framebuffer, IOIndex connectIndex, IOSelect attribute,
    uintptr_t *value) {
    IOReturn ret = FunctionCast(
        wrapAMDRadeonX6000AmdRadeonFramebufferGetAttribute,
        callbackRAD->orgAMDRadeonX6000AmdRadeonFramebufferGetAttribute)(
        framebuffer, connectIndex, attribute, value);
    if (attribute == (UInt32)'bklt') {
        // enable the backlight feature of AMD navi10 driver
        *value = callbackRAD->curPwmBacklightLvl;
        ret = 0;
    }
    return ret;
}

bool RAD::processKext(KernelPatcher &patcher, size_t index,
                      mach_vm_address_t address, size_t size) {
    if (kextRadeonFramebuffer.loadIndex == index) {
        if (force24BppMode)
            process24BitOutput(patcher, kextRadeonFramebuffer, address, size);

        return true;
    } else if (kextRadeonSupport.loadIndex == index) {
        processConnectorOverrides(patcher, address, size);

        KernelPatcher::RouteRequest requests[] = {
            {"__ZN13ATIController8TestVRAME13PCI_REG_INDEXb", doNotTestVram},
            {"__"
             "ZN16AtiDeviceControl16notifyLinkChangeE31kAGDCRegisterLinkControl"
             "Even"
             "t_tmj",
             wrapNotifyLinkChange, orgNotifyLinkChange},
            {"__ZN23AtiVramInfoInterface_V214createVramInfoEP14AtiVBiosHelperj",
             createVramInfo},
            {"__ZN13AtomBiosProxy19createAtomBiosProxyER16AtomBiosInitData",
             wrapCreateAtomBiosProxy, orgCreateAtomBiosProxy},
            {"__ZN13ATIController20populateDeviceMemoryE13PCI_REG_INDEX",
             wrapPopulateDeviceMemory, orgPopulateDeviceMemory},
        };
        if (!patcher.routeMultipleLong(index, requests, arrsize(requests),
                                       address, size))
            panic("Failed to route AMDSupport symbols");

        return true;
    } else if (kextRadeonX6000HWLibs.loadIndex == index) {
        DBGLOG("rad", "resolving device type table");
        orgDeviceTypeTable =
            patcher.solveSymbol(index, "__ZL15deviceTypeTable");
        if (!orgDeviceTypeTable) {
            panic("RAD: Failed to resolve device type table");
        }
        orgCreateFirmware =
            reinterpret_cast<t_createFirmware>(patcher.solveSymbol(
                index, "__ZN11AMDFirmware14createFirmwareEPhjjPKc"));
        if (!this->orgCreateFirmware) {
            panic("RAD: Failed to resolve AMDFirmware::createFirmware");
        }
        orgPutFirmware = reinterpret_cast<t_putFirmware>(
            patcher.solveSymbol(index,
                                "__ZN20AMDFirmwareDirectory11putFirmwareE16_"
                                "AMD_DEVICE_TYPEP11AMDFirmware"));
        if (!orgPutFirmware) {
            panic("RAD: Failed to resolve AMDFirmwareDirectory::putFirmware");
        }

        orgVega10PowerTuneServicesConstructor =
            reinterpret_cast<t_Vega10PowerTuneServicesConstructor>(
                patcher.solveSymbol(
                    index,
                    "__ZN31AtiAppleVega10PowerTuneServicesC1EP11PP_"
                    "InstanceP18PowerPlayCallbacks"));
        if (!orgVega10PowerTuneServicesConstructor) {
            panic(
                "RAD: Failed to resolve AtiAppleVega10PowerTuneServices "
                "constructor");
        }

        KernelPatcher::RouteRequest requests[] = {
            {"__ZN14AmdTtlServicesC2EP11IOPCIDevice",
             wrapAmdTtlServicesConstructor, orgAmdTtlServicesConstructor},
            {"_ipi_smu_sw_init", wrapIpiSmuSwInit, orgIpiSmuSwInit},
            {"_smu_sw_init", wrapSmuSwInit, orgSmuSwInit},
            {"_smu_internal_sw_init", wrapSmuInternalSwInit,
             orgSmuInternalSwInit},
            {"_smu_get_hw_version", wrapSmuGetHwVersion, orgSmuGetHwVersion},
            {"_psp_sw_init", wrapPspSwInit, orgPspSwInit},
            {"_gc_get_hw_version", wrapGcGetHwVersion, orgGcGetHwVersion},
            {"_internal_cos_read_fw", wrapInternalCosReadFw,
             orgInternalCosReadFw},
            {"__ZN35AMDRadeonX6000_"
             "AMDRadeonHWLibsX600025populateFirmwareDirectoryEv",
             wrapPopulateFirmwareDirectory, orgPopulateFirmwareDirectory},
            {"__"
             "ZN15AmdCailServices23queryEngineRunningStateEP17CailHwEngineQueue"
             "P22CailEngineRunningState",
             wrapQueryEngineRunningState, orgQueryEngineRunningState},
            {"_CAILQueryEngineRunningState", wrapCAILQueryEngineRunningState,
             orgCAILQueryEngineRunningState},
            {"_CailMonitorEngineInternalState",
             wrapCailMonitorEngineInternalState,
             orgCailMonitorEngineInternalState},
            {"_CailMonitorPerformanceCounter",
             wrapCailMonitorPerformanceCounter,
             orgCailMonitorPerformanceCounter},
            {"__ZN20AtiPowerPlayServices8ppEnableEb", wrapPpEnable,
             orgPpEnable},
            {"_PECI_SetupInitInfo", wrapPECISetupInitInfo,
             orgPECISetupInitInfo},
            {"_PECI_ReadRegistry", wrapPECIReadRegistry, orgPECIReadRegistry},
            {"_SMUM_Initialize", wrapSMUMInitialize, orgSMUMInitialize},
            {"_PECI_RetrieveBiosDataTable", wrapPECIRetrieveBiosDataTable,
             orgPECIRetrieveBiosDataTable},
            {"__ZN25AtiApplePowerTuneServices23createPowerTuneServicesEP11PP_"
             "InstanceP18PowerPlayCallbacks",
             wrapCreatePowerTuneServices},
            {"_get_hw_revision", wrapGetHwRevision},
            {"_smu_get_fw_constants", wrapSmuGetFwConstants},
            {"_ttlDevIsVega10Device", wrapTtlDevIsVega10Device},
            {"_smu_9_0_1_internal_hw_init", wrapSmu901InternalHwInit},
            {"__ZN14AmdTtlServices13cosDebugPrintEPKcz", wrapCosDebugPrint,
             orgCosDebugPrint},
            {"_MCILDebugPrint", wrapMCILDebugPrint, orgMCILDebugPrint},
            {"__ZN14AmdTtlServices19cosDebugPrintVaListEPvPKcS2_P13__va_list_"
             "tag",
             wrapCosDebugPrintVaList, orgCosDebugPrintVaList},
            {"__ZN14AmdTtlServices21cosReleasePrintVaListEPvPKcS2_P13__va_list_"
             "tag",
             wrapCosReleasePrintVaList, orgCosReleasePrintVaList},
            {"__ZN20AtiAppleCailServices16isAsicCapEnabledEPvm",
             wrapIsAsicCapEnabled},
        };
        if (!patcher.routeMultipleLong(index, requests, arrsize(requests),
                                       address, size))
            panic("RAD: Failed to route AMDRadeonX6000HWLibs symbols");

        uint8_t find_asic_reset[] = {0x55, 0x48, 0x89, 0xe5, 0x8b, 0x56,
                                     0x04, 0xbe, 0x3b, 0x00, 0x00, 0x00,
                                     0x5d, 0xe9, 0x51, 0xfe, 0xff, 0xff};
        uint8_t repl_asic_reset[] = {0x55, 0x48, 0x89, 0xe5, 0x8b, 0x56,
                                     0x04, 0xbe, 0x1e, 0x00, 0x00, 0x00,
                                     0x5d, 0xe9, 0x51, 0xfe, 0xff, 0xff};

        uint8_t find_load_asd_pt1[] = {0x0f, 0x85, 0x83, 0x00, 0x00, 0x00, 0x48,
                                       0x8d, 0x35, 0xf7, 0x93, 0xf4, 0x00, 0xba,
                                       0x00, 0xc1, 0x02, 0x00, 0x4c, 0x89, 0xff,
                                       0xe8, 0xf2, 0xa6, 0x56, 0x02};
        uint8_t repl_load_asd_pt1[] = {0x0f, 0x85, 0x83, 0x00, 0x00, 0x00, 0x48,
                                       0x8b, 0xf1, 0x4c, 0x89, 0xc2, 0x90, 0x90,
                                       0x90, 0x90, 0x90, 0x90, 0x4c, 0x89, 0xff,
                                       0xe8, 0xf2, 0xa6, 0x56, 0x02};
        uint8_t find_load_asd_pt2[] = {0x44, 0x89, 0x66, 0x08, 0x48, 0xc7, 0x46,
                                       0x0c, 0x00, 0xc1, 0x02, 0x00, 0x48, 0xc7,
                                       0x46, 0x14, 0x00, 0x00, 0x00, 0x00};
        uint8_t repl_load_asd_pt2[] = {0x44, 0x89, 0x66, 0x08, 0x4c, 0x89, 0x84,
                                       0x26, 0x0c, 0x00, 0x00, 0x00, 0x48, 0xc7,
                                       0x46, 0x14, 0x00, 0x00, 0x00, 0x00};
        uint8_t find_load_dtm_pt1[] = {
            0x48, 0x8b, 0xbb, 0xf8, 0x0a, 0x00, 0x00, 0x48, 0x8d, 0x35, 0x47,
            0x4f, 0xf7, 0x00, 0xba, 0x00, 0x21, 0x00, 0x00, 0xe8, 0x45, 0xa1,
            0x56, 0x02, 0x48, 0x8d, 0xb5, 0x70, 0xfc, 0xff, 0xff};
        uint8_t repl_load_dtm_pt1[] = {
            0x48, 0x8b, 0xbb, 0xf8, 0x0a, 0x00, 0x00, 0x48, 0x8b, 0xf1, 0x49,
            0x8b, 0xd0, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xe8, 0x45, 0xa1,
            0x56, 0x02, 0x48, 0x8d, 0xb5, 0x70, 0xfc, 0xff, 0xff};
        uint8_t find_load_dtm_pt2[] = {0x44, 0x89, 0x76, 0x08, 0xc7, 0x46,
                                       0x0c, 0x00, 0x21, 0x00, 0x00, 0x48,
                                       0x8b, 0x83, 0xb8, 0x2d, 0x00, 0x00};
        uint8_t repl_load_dtm_pt2[] = {0x44, 0x89, 0x76, 0x08, 0x44, 0x89,
                                       0x86, 0x0c, 0x00, 0x00, 0x00, 0x48,
                                       0x8b, 0x83, 0xb8, 0x2d, 0x00, 0x00};
        KernelPatcher::LookupPatch patches[] = {
            /*
             * Patch for _smu_9_0_1_full_asic_reset
             * This function performs a full ASIC reset.
             * The patch corrects the sent message to 0x1E;
             * the original code sends 0x3B, which is wrong for SMU 10.
             */
            {&kextRadeonX6000HWLibs, find_asic_reset, repl_asic_reset,
             arrsize(find_asic_reset), 2},
            /*
             * Patches for _psp_asd_load.
             * _psp_asd_load loads a hardcoded ASD firmware binary
             * included in the kext as _psp_asd_bin.
             * The copied data isn't in a table, it is a single
             * binary copied over to the PSP private memory.
             * We can't replicate such logic in any AMDGPU kext function,
             * as the memory accesses to GPU data is inaccessible
             * from external kexts, therefore, we have to do a hack.
             * The hack is very straight forward; we have replaced the
             * assembly that loads hardcoded values from
             *     lea rsi, [_psp_asd_bin]
             *     mov edx, 0x2c100
             *     mov rdi, r15
             *     call _memcpy
             * to
             *     mov rsi, rcx
             *     mov rdx, r8
             *     mov rdi, r15
             *     call _memcpy
             * so that it gets the pointer and size from parameter 4 and 5.
             * Register choice was because the parameter 2 and 3 registers
             * get overwritten before this call to memcpy
             * The hack we came up with looks like terrible practice,
             * but this will have to do.
             * Pain.
             */
            {&kextRadeonX6000HWLibs, find_load_asd_pt1, repl_load_asd_pt1,
             arrsize(find_load_asd_pt1), 2},
            {&kextRadeonX6000HWLibs, find_load_asd_pt2, repl_load_asd_pt2,
             arrsize(find_load_asd_pt2), 2},
            /*
             * Patches for _psp_dtm_load.
             * Same idea as _psp_asd_load.
             */
            {&kextRadeonX6000HWLibs, find_load_dtm_pt1, repl_load_dtm_pt1,
             arrsize(find_load_dtm_pt1), 2},
            {&kextRadeonX6000HWLibs, find_load_dtm_pt2, repl_load_dtm_pt2,
             arrsize(find_load_dtm_pt2), 2},
        };
        for (auto &patch : patches) {
            patcher.applyLookupPatch(&patch);
            patcher.clearError();
        }

        return true;
    } else if (kextRadeonX6000Framebuffer.loadIndex == index) {
        orgDceDriverSetBacklight = reinterpret_cast<t_DceDriverSetBacklight>(
            patcher.solveSymbol(index, "_dce_driver_set_backlight"));
        if (patcher.getError() != KernelPatcher::Error::NoError) {
            SYSLOG("rad", "Unable to resolve _dce_driver_set_backlight");
            patcher.clearError();
        }

        KernelPatcher::RouteRequest requests[] = {
            {"_dce_panel_cntl_hw_init", wrapDcePanelCntlHwInit,
             orgDcePanelCntlHwInit},
            {"__ZN35AMDRadeonX6000_"
             "AmdRadeonFramebuffer25setAttributeForConnectionEijm",
             wrapAMDRadeonX6000AmdRadeonFramebufferSetAttribute,
             orgAMDRadeonX6000AmdRadeonFramebufferSetAttribute},
            {"__ZN35AMDRadeonX6000_"
             "AmdRadeonFramebuffer25getAttributeForConnectionEijPm",
             wrapAMDRadeonX6000AmdRadeonFramebufferGetAttribute,
             orgAMDRadeonX6000AmdRadeonFramebufferGetAttribute},
            {"__ZNK34AMDRadeonX6000_AmdBiosParserHelper18getVideoMemoryTypeEv",
             wrapGetVideoMemoryType, orgGetVideoMemoryType},
            {"__ZNK34AMDRadeonX6000_"
             "AmdBiosParserHelper22getVideoMemoryBitWidthEv",
             wrapGetVideoMemoryBitWidth, orgGetVideoMemoryBitWidth},
            {"__ZN15AmdAtomVramInfo14createVramInfoEP15AmdAtomFwHelperj",
             wrapCreateVramInfo},
            {"__ZNK15AmdAtomVramInfo16populateVramInfoER16AtomFirmwareInfo",
             wrapPopulateVramInfo},
            {"__ZNK26AMDRadeonX6000_AmdAsicInfo11getFamilyIdEv",
             wrapGetFamilyId},
            {"__ZN30AMDRadeonX6000_AmdAsicInfoNavi18populateDeviceInfoEv",
             wrapPopulateDeviceInfo, orgPopulateDeviceInfo},
            {"__ZN20AmdAppleCailServices16isAsicCapEnabledEPvm",
             wrapIsAsicCapEnabled},
        };

        uint8_t find_null_check[] = {0x48, 0x89, 0x83, 0x88, 0x00, 0x00, 0x00,
                                     0x48, 0x85, 0xc0, 0x0f, 0x84, 0xa1, 0x00,
                                     0x00, 0x00, 0x48, 0x8b, 0x7b, 0x18};
        uint8_t repl_null_check[] = {0x48, 0x89, 0x83, 0x88, 0x00, 0x00, 0x00,
                                     0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                                     0x90, 0x90, 0x48, 0x8b, 0x7b, 0x18};
        /*
         * Neutralise PSP Firmware Info creation null check
         * to proceed with Controller Core Services initialisation.
         */
        KernelPatcher::LookupPatch patch = {&kextRadeonX6000Framebuffer,
                                            find_null_check, repl_null_check,
                                            arrsize(find_null_check), 2};
        patcher.applyLookupPatch(&patch);
        patcher.clearError();

        if (!patcher.routeMultiple(index, requests, arrsize(requests), address,
                                   size)) {
            panic("RAD: Failed to route AMDRadeonX6000Framebuffer symbols");
        }

        return true;
    }

    for (size_t i = 0; i < maxHardwareKexts; i++) {
        if (kextRadeonHardware[i].loadIndex == index) {
            processHardwareKext(patcher, i, address, size);
            return true;
        }
    }

    return false;
}

void RAD::initHardwareKextMods() {
    lilu.onKextLoadForce(kextRadeonHardware, maxHardwareKexts);
}

void RAD::process24BitOutput(KernelPatcher &patcher,
                             KernelPatcher::KextInfo &info,
                             mach_vm_address_t address, size_t size) {
    auto bitsPerComponent = patcher.solveSymbol<int *>(
        info.loadIndex, "__ZL18BITS_PER_COMPONENT", address, size);
    if (bitsPerComponent) {
        while (bitsPerComponent && *bitsPerComponent) {
            if (*bitsPerComponent == 10) {
                auto ret = MachInfo::setKernelWriting(
                    true, KernelPatcher::kernelWriteLock);
                if (ret == KERN_SUCCESS) {
                    DBGLOG("rad", "fixing BITS_PER_COMPONENT");
                    *bitsPerComponent = 8;
                    MachInfo::setKernelWriting(false,
                                               KernelPatcher::kernelWriteLock);
                } else {
                    NETLOG("rad",
                           "failed to disable write protection for "
                           "BITS_PER_COMPONENT");
                }
            }
            bitsPerComponent++;
        }
    } else {
        NETLOG("rad", "failed to find BITS_PER_COMPONENT");
        patcher.clearError();
    }

    DBGLOG("rad", "fixing pixel types");

    KernelPatcher::LookupPatch pixelPatch{
        &info,
        reinterpret_cast<const uint8_t *>("--RRRRRRRRRRGGGGGGGGGGBBBBBBBBBB"),
        reinterpret_cast<const uint8_t *>("--------RRRRRRRRGGGGGGGGBBBBBBBB"),
        32, 2};

    patcher.applyLookupPatch(&pixelPatch);
    if (patcher.getError() != KernelPatcher::Error::NoError) {
        NETLOG("rad", "failed to patch RGB mask for 24-bit output");
        patcher.clearError();
    }
}

void RAD::processConnectorOverrides(KernelPatcher &patcher,
                                    mach_vm_address_t address, size_t size) {
    KernelPatcher::RouteRequest requests[] = {
        {"__ZN14AtiBiosParser116getConnectorInfoEP13ConnectorInfoRh",
         wrapGetConnectorsInfoV1, orgGetConnectorsInfoV1},
        {"__ZN14AtiBiosParser216getConnectorInfoEP13ConnectorInfoRh",
         wrapGetConnectorsInfoV2, orgGetConnectorsInfoV2},
        {"__"
         "ZN14AtiBiosParser126translateAtomConnectorInfoERN30AtiObjectInfoTable"
         "Interface_V117AtomConnectorInfoER13ConnectorInfo",
         wrapTranslateAtomConnectorInfoV1, orgTranslateAtomConnectorInfoV1},
        {"__"
         "ZN14AtiBiosParser226translateAtomConnectorInfoERN30AtiObjectInfoTable"
         "Interface_V217AtomConnectorInfoER13ConnectorInfo",
         wrapTranslateAtomConnectorInfoV2, orgTranslateAtomConnectorInfoV2},
        {"__ZN13ATIController5startEP9IOService", wrapATIControllerStart,
         orgATIControllerStart},
    };
    patcher.routeMultiple(kextRadeonSupport.loadIndex, requests, address, size);
}

uint64_t RAD::wrapGetState(void *that) {
    DBGLOG("rad", "getState this = %p", that);
    auto ret = FunctionCast(wrapGetState, callbackRAD->orgGetState)(that);
    DBGLOG("rad", "getState returned 0x%llX", ret);
    return ret;
}

bool RAD::wrapInitializeTtl(void *that, void *param1) {
    NETLOG("rad", "initializeTtl this = %p", that);
    auto ret = FunctionCast(wrapInitializeTtl, callbackRAD->orgInitializeTtl)(
        that, param1);
    NETLOG("rad", "initializeTtl returned %d", ret);
    return ret;
}

void RAD::processHardwareKext(KernelPatcher &patcher, size_t hwIndex,
                              mach_vm_address_t address, size_t size) {
    auto &hardware = kextRadeonHardware[hwIndex];

    KernelPatcher::RouteRequest requests[] = {
        {"__ZN27AMDRadeonX6000_AMDHWHandler8getStateEv", wrapGetState,
         orgGetState},
        {"__ZN28AMDRadeonX6000_AMDRTHardware13initializeTtlEP16_GART_"
         "PARAMETERS",
         wrapInitializeTtl, orgInitializeTtl},
        {"__ZN32AMDRadeonX6000_AMDGFX10PM4Engine23QueryComputeQueueIsIdleE18_"
         "eAMD_HW_RING_TYPE",
         wrapQueryComputeQueueIsIdle, orgQueryComputeQueueIsIdle},
        {"__ZN27AMDRadeonX6000_AMDHWChannel11waitForIdleEj",
         wrapAMDHWChannelWaitForIdle, orgAMDHWChannelWaitForIdle},
    };
    if (!patcher.routeMultipleLong(hardware.loadIndex, requests,
                                   arrsize(requests), address, size)) {
        panic("Failed to route X6000 symbols");
    }

    // Patch AppleGVA support for non-supported models
    if (forceCodecInfo && getHWInfoProcNames[hwIndex] != nullptr) {
        KernelPatcher::RouteRequest request(getHWInfoProcNames[hwIndex],
                                            wrapGetHWInfo[hwIndex],
                                            orgGetHWInfo[hwIndex]);
        if (!patcher.routeMultipleLong(hardware.loadIndex, &request, 1, address,
                                       size)) {
            panic("Failed to route X6000 symbols for AppleGVA support");
        }
    }
}

void RAD::mergeProperty(OSDictionary *props, const char *name,
                        OSObject *value) {
    // The only type we could make from device properties is data.
    // To be able to override other types we do a conversion here.
    auto data = OSDynamicCast(OSData, value);
    if (data) {
        // It is hard to make a boolean even from ACPI, so we make a hack here:
        // 1-byte OSData with 0x01 / 0x00 values becomes boolean.
        auto val = static_cast<const uint8_t *>(data->getBytesNoCopy());
        auto len = data->getLength();
        if (val && len == sizeof(uint8_t)) {
            if (val[0] == 1) {
                props->setObject(name, kOSBooleanTrue);
                DBGLOG("rad", "prop %s was merged as kOSBooleanTrue", name);
                return;
            } else if (val[0] == 0) {
                props->setObject(name, kOSBooleanFalse);
                DBGLOG("rad", "prop %s was merged as kOSBooleanFalse", name);
                return;
            }
        }

        // Consult the original value to make a decision
        auto orgValue = props->getObject(name);
        if (val && orgValue) {
            DBGLOG("rad", "prop %s has original value", name);
            if (len == sizeof(uint32_t) && OSDynamicCast(OSNumber, orgValue)) {
                auto num = *reinterpret_cast<const uint32_t *>(val);
                auto osnum = OSNumber::withNumber(num, 32);
                if (osnum) {
                    DBGLOG("rad", "prop %s was merged as number %u", name, num);
                    props->setObject(name, osnum);
                    osnum->release();
                }
                return;
            } else if (len > 0 && val[len - 1] == '\0' &&
                       OSDynamicCast(OSString, orgValue)) {
                auto str = reinterpret_cast<const char *>(val);
                auto osstr = OSString::withCString(str);
                if (osstr) {
                    DBGLOG("rad", "prop %s was merged as string %s", name, str);
                    props->setObject(name, osstr);
                    osstr->release();
                }
                return;
            }
        } else {
            DBGLOG("rad", "prop %s has no original value", name);
        }
    }

    // Default merge as is
    props->setObject(name, value);
    DBGLOG("rad", "prop %s was merged", name);
}

void RAD::mergeProperties(OSDictionary *props, const char *prefix,
                          IOService *provider) {
    // Should be ok, but in case there are issues switch to
    // dictionaryWithProperties();
    auto dict = provider->getPropertyTable();
    if (dict) {
        auto iterator = OSCollectionIterator::withCollection(dict);
        if (iterator) {
            OSSymbol *propname;
            size_t prefixlen = strlen(prefix);
            while ((propname = OSDynamicCast(
                        OSSymbol, iterator->getNextObject())) != nullptr) {
                auto name = propname->getCStringNoCopy();
                if (name && propname->getLength() > prefixlen &&
                    !strncmp(name, prefix, prefixlen)) {
                    auto prop = dict->getObject(propname);
                    if (prop)
                        mergeProperty(props, name + prefixlen, prop);
                    else {
                        DBGLOG("rad", "prop %s was not merged due to no value",
                               name);
                    }
                } else {
                    DBGLOG("rad", "prop %s does not match %s prefix",
                           safeString(name), prefix);
                }
            }

            iterator->release();
        } else {
            NETLOG("rad", "prop merge failed to iterate over properties");
        }
    } else {
        NETLOG("rad", "prop merge failed to get properties");
    }

    if (!strcmp(prefix, "CAIL,")) {
        for (size_t i = 0; i < arrsize(powerGatingFlags); i++) {
            if (powerGatingFlags[i] && props->getObject(powerGatingFlags[i])) {
                DBGLOG("rad", "cail prop merge found %s, replacing",
                       powerGatingFlags[i]);
                auto num = OSNumber::withNumber(1, 32);
                if (num) {
                    props->setObject(powerGatingFlags[i], num);
                    num->release();
                }
            }
        }
    }
}

void RAD::applyPropertyFixes(IOService *service, uint32_t connectorNum) {
    if (!service->getProperty("CFG,CFG_FB_LIMIT")) {
        DBGLOG("rad", "setting fb limit to %u", connectorNum);
        service->setProperty("CFG_FB_LIMIT", connectorNum, 32);
    }
}

uint32_t RAD::wrapGetConnectorsInfoV1(void *that,
                                      RADConnectors::Connector *connectors,
                                      uint8_t *sz) {
    uint32_t code =
        FunctionCast(wrapGetConnectorsInfoV1,
                     callbackRAD->orgGetConnectorsInfoV1)(that, connectors, sz);
    auto props = callbackRAD->currentPropProvider.get();

    if (code == 0 && sz && props && *props) {
        callbackRAD->updateConnectorsInfo(nullptr, nullptr, *props, connectors,
                                          sz);
    } else
        NETLOG("rad", "getConnectorsInfoV1 failed %X or undefined %d", code,
               props == nullptr);

    return code;
}

void RAD::updateConnectorsInfo(void *atomutils,
                               t_getAtomObjectTableForType gettable,
                               IOService *ctrl,
                               RADConnectors::Connector *connectors,
                               uint8_t *sz) {
    if (atomutils) {
        NETLOG("rad", "getConnectorsInfo found %u connectors", *sz);
        RADConnectors::print(connectors, *sz);
    }

    auto cons = ctrl->getProperty("connectors");
    if (cons) {
        auto consData = OSDynamicCast(OSData, cons);
        if (consData) {
            auto consPtr = consData->getBytesNoCopy();
            auto consSize = consData->getLength();

            uint32_t consCount;
            if (WIOKit::getOSDataValue(ctrl, "connector-count", consCount)) {
                *sz = consCount;
                NETLOG("rad", "getConnectorsInfo got size override to %u", *sz);
            }

            if (consPtr && consSize > 0 && *sz > 0 &&
                RADConnectors::valid(consSize, *sz)) {
                RADConnectors::copy(
                    connectors, *sz,
                    static_cast<const RADConnectors::Connector *>(consPtr),
                    consSize);
                NETLOG("rad", "getConnectorsInfo installed %u connectors", *sz);
                applyPropertyFixes(ctrl, *sz);
            } else {
                NETLOG("rad",
                       "getConnectorsInfo conoverrides have invalid size %u "
                       "for %u num",
                       consSize, *sz);
            }
        } else {
            NETLOG("rad", "getConnectorsInfo conoverrides have invalid type");
        }
    } else {
        if (atomutils) {
            NETLOG("rad", "getConnectorsInfo attempting to autofix connectors");
            uint8_t sHeader = 0, displayPathNum = 0, connectorObjectNum = 0;
            auto baseAddr =
                static_cast<uint8_t *>(gettable(
                    atomutils, AtomObjectTableType::Common, &sHeader)) -
                sizeof(uint32_t);
            auto displayPaths = static_cast<AtomDisplayObjectPath *>(gettable(
                atomutils, AtomObjectTableType::DisplayPath, &displayPathNum));
            auto connectorObjects = static_cast<AtomConnectorObject *>(
                gettable(atomutils, AtomObjectTableType::ConnectorObject,
                         &connectorObjectNum));
            if (displayPathNum == connectorObjectNum) {
                autocorrectConnectors(baseAddr, displayPaths, displayPathNum,
                                      connectorObjects, connectorObjectNum,
                                      connectors, *sz);
            } else {
                NETLOG("rad",
                       "getConnectorsInfo found different displaypaths %u and "
                       "connectors %u",
                       displayPathNum, connectorObjectNum);
            }
        }

        applyPropertyFixes(ctrl, *sz);

        const uint8_t *senseList = nullptr;
        uint8_t senseNum = 0;
        auto priData =
            OSDynamicCast(OSData, ctrl->getProperty("connector-priority"));
        if (priData) {
            senseList = static_cast<const uint8_t *>(priData->getBytesNoCopy());
            senseNum = static_cast<uint8_t>(priData->getLength());
            NETLOG("rad",
                   "getConnectorInfo found %u senses in connector-priority",
                   senseNum);
            reprioritiseConnectors(senseList, senseNum, connectors, *sz);
        } else {
            NETLOG("rad", "getConnectorInfo leaving unchaged priority");
        }
    }

    NETLOG("rad", "getConnectorsInfo resulting %u connectors follow", *sz);
    RADConnectors::print(connectors, *sz);
}

uint32_t RAD::wrapTranslateAtomConnectorInfoV1(
    void *that, RADConnectors::AtomConnectorInfo *info,
    RADConnectors::Connector *connector) {
    uint32_t code = FunctionCast(wrapTranslateAtomConnectorInfoV1,
                                 callbackRAD->orgTranslateAtomConnectorInfoV1)(
        that, info, connector);

    if (code == 0 && info && connector) {
        RADConnectors::print(connector, 1);

        uint8_t sense = getSenseID(info->i2cRecord);
        if (sense) {
            NETLOG("rad", "translateAtomConnectorInfoV1 got sense id %02X",
                   sense);

            // We need to extract usGraphicObjIds from info->hpdRecord, which is
            // of type ATOM_SRC_DST_TABLE_FOR_ONE_OBJECT: struct
            // ATOM_SRC_DST_TABLE_FOR_ONE_OBJECT {
            //   uint8_t ucNumberOfSrc;
            //   uint16_t usSrcObjectID[ucNumberOfSrc];
            //   uint8_t ucNumberOfDst;
            //   uint16_t usDstObjectID[ucNumberOfDst];
            // };
            // The value we need is in usSrcObjectID. The structure is
            // byte-packed.

            uint8_t ucNumberOfSrc = info->hpdRecord[0];
            for (uint8_t i = 0; i < ucNumberOfSrc; i++) {
                auto usSrcObjectID = *reinterpret_cast<uint16_t *>(
                    info->hpdRecord + sizeof(uint8_t) + i * sizeof(uint16_t));
                NETLOG("rad",
                       "translateAtomConnectorInfoV1 checking %04X object id",
                       usSrcObjectID);
                if (((usSrcObjectID & OBJECT_TYPE_MASK) >> OBJECT_TYPE_SHIFT) ==
                    GRAPH_OBJECT_TYPE_ENCODER) {
                    uint8_t txmit = 0, enc = 0;
                    if (getTxEnc(usSrcObjectID, txmit, enc))
                        callbackRAD->autocorrectConnector(
                            getConnectorID(info->usConnObjectId),
                            getSenseID(info->i2cRecord), txmit, enc, connector,
                            1);
                    break;
                }
            }
        } else {
            NETLOG("rad",
                   "translateAtomConnectorInfoV1 failed to detect sense for "
                   "translated connector");
        }
    }

    return code;
}

void RAD::autocorrectConnectors(uint8_t *baseAddr,
                                AtomDisplayObjectPath *displayPaths,
                                uint8_t displayPathNum,
                                AtomConnectorObject *connectorObjects,
                                [[maybe_unused]] uint8_t connectorObjectNum,
                                RADConnectors::Connector *connectors,
                                uint8_t sz) {
    for (uint8_t i = 0; i < displayPathNum; i++) {
        if (!isEncoder(displayPaths[i].usGraphicObjIds)) {
            NETLOG("rad", "autocorrectConnectors not encoder %X at %u",
                   displayPaths[i].usGraphicObjIds, i);
            continue;
        }

        uint8_t txmit = 0, enc = 0;
        if (!getTxEnc(displayPaths[i].usGraphicObjIds, txmit, enc)) {
            continue;
        }

        uint8_t sense =
            getSenseID(baseAddr + connectorObjects[i].usRecordOffset);
        if (!sense) {
            NETLOG(
                "rad",
                "autocorrectConnectors failed to detect sense for %u connector",
                i);
            continue;
        }

        NETLOG(
            "rad",
            "autocorrectConnectors found txmit %02X enc %02X sense %02X for %u "
            "connector",
            txmit, enc, sense, i);

        autocorrectConnector(getConnectorID(displayPaths[i].usConnObjectId),
                             sense, txmit, enc, connectors, sz);
    }
}

void RAD::autocorrectConnector(uint8_t connector, uint8_t sense, uint8_t txmit,
                               [[maybe_unused]] uint8_t enc,
                               RADConnectors::Connector *connectors,
                               uint8_t sz) {
    if (callbackRAD->dviSingleLink) {
        if (connector != CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_I &&
            connector != CONNECTOR_OBJECT_ID_DUAL_LINK_DVI_D &&
            connector != CONNECTOR_OBJECT_ID_LVDS) {
            NETLOG("rad",
                   "autocorrectConnector found unsupported connector type %02X",
                   connector);
            return;
        }

        auto fixTransmit = [](auto &con, uint8_t idx, uint8_t sense,
                              uint8_t txmit) {
            if (con.sense == sense) {
                if (con.transmitter != txmit &&
                    (con.transmitter & 0xCF) == con.transmitter) {
                    NETLOG("rad",
                           "autocorrectConnector replacing txmit %02X with "
                           "%02X for %u "
                           "connector sense %02X",
                           con.transmitter, txmit, idx, sense);
                    con.transmitter = txmit;
                }
                return true;
            }
            return false;
        };

        bool isModern = RADConnectors::modern();
        for (uint8_t j = 0; j < sz; j++) {
            if (isModern) {
                auto &con = (&connectors->modern)[j];
                if (fixTransmit(con, j, sense, txmit)) break;
            } else {
                auto &con = (&connectors->legacy)[j];
                if (fixTransmit(con, j, sense, txmit)) break;
            }
        }
    } else
        NETLOG("rad",
               "autocorrectConnector use -raddvi to enable dvi autocorrection");
}

void RAD::reprioritiseConnectors(const uint8_t *senseList, uint8_t senseNum,
                                 RADConnectors::Connector *connectors,
                                 uint8_t sz) {
    static constexpr uint32_t typeList[] = {
        RADConnectors::ConnectorLVDS, RADConnectors::ConnectorDigitalDVI,
        RADConnectors::ConnectorHDMI, RADConnectors::ConnectorDP,
        RADConnectors::ConnectorVGA,
    };
    static constexpr uint8_t typeNum{static_cast<uint8_t>(arrsize(typeList))};

    bool isModern = RADConnectors::modern();
    uint16_t priCount = 1;
    for (uint8_t i = 0; i < senseNum + typeNum + 1; i++) {
        for (uint8_t j = 0; j < sz; j++) {
            auto reorder = [&](auto &con) {
                if (i == senseNum + typeNum) {
                    if (con.priority == 0) con.priority = priCount++;
                } else if (i < senseNum) {
                    if (con.sense == senseList[i]) {
                        NETLOG("rad",
                               "reprioritiseConnectors setting priority of "
                               "sense %02X to "
                               "%u by sense",
                               con.sense, priCount);
                        con.priority = priCount++;
                        return true;
                    }
                } else {
                    if (con.priority == 0 &&
                        con.type == typeList[i - senseNum]) {
                        NETLOG("rad",
                               "reprioritiseConnectors setting priority of "
                               "sense %02X to "
                               "%u by type",
                               con.sense, priCount);
                        con.priority = priCount++;
                    }
                }
                return false;
            };

            if ((isModern && reorder((&connectors->modern)[j])) ||
                (!isModern && reorder((&connectors->legacy)[j]))) {
                break;
            }
        }
    }
}

void RAD::updateAccelConfig([[maybe_unused]] size_t hwIndex,
                            IOService *accelService, const char **accelConfig) {
    if (accelService && accelConfig) {
        if (fixConfigName) {
            auto gpuService = accelService->getParentEntry(gIOServicePlane);

            if (gpuService) {
                auto model =
                    OSDynamicCast(OSData, gpuService->getProperty("model"));
                if (model) {
                    auto modelStr =
                        static_cast<const char *>(model->getBytesNoCopy());
                    if (modelStr) {
                        if (modelStr[0] == 'A' &&
                            ((modelStr[1] == 'M' && modelStr[2] == 'D') ||
                             (modelStr[1] == 'T' && modelStr[2] == 'I')) &&
                            modelStr[3] == ' ')
                            modelStr += 4;

                        DBGLOG("rad", "updateAccelConfig found gpu model %s",
                               modelStr);
                        *accelConfig = modelStr;
                    } else
                        DBGLOG("rad", "updateAccelConfig found null gpu model");
                } else
                    DBGLOG("rad", "updateAccelConfig failed to find gpu model");
            } else
                DBGLOG("rad",
                       "updateAccelConfig failed to find accelerator parent");
        }
    }
}

bool RAD::wrapSetProperty(IORegistryEntry *that, const char *aKey, void *bytes,
                          unsigned length) {
    if (length > 10 && aKey &&
        reinterpret_cast<const uint32_t *>(aKey)[0] == 'edom' &&
        reinterpret_cast<const uint16_t *>(aKey)[2] == 'l') {
        DBGLOG("rad", "SetProperty caught model %u (%.*s)", length, length,
               static_cast<char *>(bytes));
        if (*static_cast<uint32_t *>(bytes) == ' DMA' ||
            *static_cast<uint32_t *>(bytes) == ' ITA' ||
            *static_cast<uint32_t *>(bytes) == 'edaR') {
            if (FunctionCast(wrapGetProperty, callbackRAD->orgGetProperty)(
                    that, aKey)) {
                DBGLOG("rad", "SetProperty ignored setting %s to %s", aKey,
                       static_cast<char *>(bytes));
                return true;
            }
            DBGLOG("rad", "SetProperty missing %s, fallback to %s", aKey,
                   static_cast<char *>(bytes));
        }
    }

    return FunctionCast(wrapSetProperty, callbackRAD->orgSetProperty)(
        that, aKey, bytes, length);
}

OSObject *RAD::wrapGetProperty(IORegistryEntry *that, const char *aKey) {
    auto obj =
        FunctionCast(wrapGetProperty, callbackRAD->orgGetProperty)(that, aKey);
    auto props = OSDynamicCast(OSDictionary, obj);

    if (props && aKey) {
        const char *prefix{nullptr};
        auto provider =
            OSDynamicCast(IOService, that->getParentEntry(gIOServicePlane));
        if (provider) {
            if (aKey[0] == 'a') {
                if (!strcmp(aKey, "aty_config"))
                    prefix = "CFG,";
                else if (!strcmp(aKey, "aty_properties"))
                    prefix = "PP,";
            } else if (aKey[0] == 'c' && !strcmp(aKey, "cail_properties")) {
                prefix = "CAIL,";
            }

            if (prefix) {
                DBGLOG("rad",
                       "GetProperty discovered property merge request for %s",
                       aKey);
                auto rawProps = props->copyCollection();
                if (rawProps) {
                    auto newProps = OSDynamicCast(OSDictionary, rawProps);
                    if (newProps) {
                        callbackRAD->mergeProperties(newProps, prefix,
                                                     provider);
                        that->setProperty(aKey, newProps);
                        obj = newProps;
                    }
                    rawProps->release();
                }
            }
        }
    }

    return obj;
}

uint32_t RAD::wrapGetConnectorsInfoV2(void *that,
                                      RADConnectors::Connector *connectors,
                                      uint8_t *sz) {
    uint32_t code =
        FunctionCast(wrapGetConnectorsInfoV2,
                     callbackRAD->orgGetConnectorsInfoV2)(that, connectors, sz);
    auto props = callbackRAD->currentPropProvider.get();

    if (code == 0 && sz && props && *props)
        callbackRAD->updateConnectorsInfo(nullptr, nullptr, *props, connectors,
                                          sz);
    else
        NETLOG("rad", "getConnectorsInfoV2 failed %X or undefined %d", code,
               props == nullptr);

    return code;
}

uint32_t RAD::wrapTranslateAtomConnectorInfoV2(
    void *that, RADConnectors::AtomConnectorInfo *info,
    RADConnectors::Connector *connector) {
    uint32_t code = FunctionCast(wrapTranslateAtomConnectorInfoV2,
                                 callbackRAD->orgTranslateAtomConnectorInfoV2)(
        that, info, connector);

    if (code == 0 && info && connector) {
        RADConnectors::print(connector, 1);

        uint8_t sense = getSenseID(info->i2cRecord);
        if (sense) {
            NETLOG("rad", "translateAtomConnectorInfoV2 got sense id %02X",
                   sense);
            uint8_t txmit = 0, enc = 0;
            if (getTxEnc(info->usGraphicObjIds, txmit, enc))
                callbackRAD->autocorrectConnector(
                    getConnectorID(info->usConnObjectId),
                    getSenseID(info->i2cRecord), txmit, enc, connector, 1);
        } else {
            NETLOG("rad",
                   "translateAtomConnectorInfoV2 failed to detect sense for "
                   "translated connector");
        }
    }

    return code;
}

bool RAD::wrapATIControllerStart(IOService *ctrl, IOService *provider) {
    NETLOG("rad", "starting controller " PRIKADDR, CASTKADDR(current_thread()));
    if (callbackRAD->forceVesaMode) {
        NETLOG("rad", "disabling video acceleration on request");
        return false;
    }

    callbackRAD->currentPropProvider.set(provider);
    bool r = FunctionCast(wrapATIControllerStart,
                          callbackRAD->orgATIControllerStart)(ctrl, provider);
    NETLOG("rad", "starting controller done %d " PRIKADDR, r,
           CASTKADDR(current_thread()));
    callbackRAD->currentPropProvider.erase();

    return r;
}

bool RAD::doNotTestVram([[maybe_unused]] IOService *ctrl,
                        [[maybe_unused]] uint32_t reg,
                        [[maybe_unused]] bool retryOnFail) {
    NETLOG("rad", "TestVRAM called! Returning true");
    return true;
}

bool RAD::wrapNotifyLinkChange(void *atiDeviceControl,
                               kAGDCRegisterLinkControlEvent_t event,
                               void *eventData, uint32_t eventFlags) {
    auto ret =
        FunctionCast(wrapNotifyLinkChange, callbackRAD->orgNotifyLinkChange)(
            atiDeviceControl, event, eventData, eventFlags);

    if (event == kAGDCValidateDetailedTiming) {
        auto cmd = static_cast<AGDCValidateDetailedTiming_t *>(eventData);
        NETLOG("rad", "AGDCValidateDetailedTiming %u -> %d (%u)",
               cmd->framebufferIndex, ret, cmd->modeStatus);
        if (ret == false || cmd->modeStatus < 1 || cmd->modeStatus > 3) {
            cmd->modeStatus = 2;
            ret = true;
        }
    }

    return ret;
}

void RAD::updateGetHWInfo(IOService *accelVideoCtx, void *hwInfo) {
    IOService *accel, *pciDev;
    accel = OSDynamicCast(IOService,
                          accelVideoCtx->getParentEntry(gIOServicePlane));
    if (accel == NULL) {
        NETLOG("rad", "getHWInfo: no parent found for accelVideoCtx!");
        return;
    }
    pciDev = OSDynamicCast(IOService, accel->getParentEntry(gIOServicePlane));
    if (pciDev == NULL) {
        NETLOG("rad", "getHWInfo: no parent found for accel!");
        return;
    }
    uint16_t &org = getMember<uint16_t>(hwInfo, 0x4);
    uint32_t dev = org;
    if (!WIOKit::getOSDataValue(pciDev, "codec-device-id", dev)) {
        WIOKit::getOSDataValue(pciDev, "device-id", dev);
    }
    NETLOG("rad", "getHWInfo: original PID: 0x%04X, replaced PID: 0x%04X", org,
           dev);
    org = static_cast<uint16_t>(dev);
}
