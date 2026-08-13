/*===========================================================================
; DVEnabler.cpp
;----------------------------------------------------------------------------
; Copyright (C) 2021 Intel Corporation
; SPDX-License-Identifier: MIT
;
; File Description:
;   This file will disable MSFT Display Path (MBDA)
;--------------------------------------------------------------------------*/

#include "pch.h"
#include "Trace.h"
#include "DVEnabler.tmh"
#include <Windows.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <initguid.h> 
#include <Setupapi.h>
#include <Ntddvdeo.h>
#include <Devpkey.h>
#include "Trace_override.h"

bool operator==(const LUID &a, const LUID &b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }


struct DisplayConfigState
{
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};

static bool QueryCurrentDisplayConfig(DisplayConfigState& state)
{
    UINT32 pathCount = 0, modeCount = 0;
    LONG result;

    // Retry loop: topology can change between the size query and the real query
    do
    {
        if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS)
        {
            ERR("GetDisplayConfigBufferSizes failed\n");
            return false;
        }

        state.paths.resize(pathCount);
        state.modes.resize(modeCount);

        result = QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, state.paths.data(),
                                     &modeCount, state.modes.data(), nullptr);
    } while (result == ERROR_INSUFFICIENT_BUFFER);

    if (result != ERROR_SUCCESS)
    {
        ERR("QueryDisplayConfig failed with %ld\n", result);
        return false;
    }

    state.paths.resize(pathCount);
    state.modes.resize(modeCount);
    return true;
}


static bool StageDisplayChange(DisplayConfigState& state, const std::wstring& devicePath,
                                    const disp_target_res* target_res, bool disable = false, bool zero_pos = false)
{
    UINT32 pathCount = (UINT32)state.paths.size();

    for (UINT32 i = 0; i < pathCount; i++)
    {
        DISPLAYCONFIG_TARGET_DEVICE_NAME name = {};
        name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        name.header.size = sizeof(name);
        name.header.adapterId = state.paths[i].targetInfo.adapterId;
        name.header.id = state.paths[i].targetInfo.id;

        if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS)
            continue;

        if (_wcsicmp(name.monitorDevicePath, devicePath.c_str()) != 0)
            continue;

		if(disable)
		{
			state.paths[i].flags = 0;
			return true;
		}

		if (target_res == nullptr) {
			ERR("Target resolution is null for connector %ws\n", devicePath.c_str());
			return false;
		}

        UINT32 srcIdx = state.paths[i].sourceInfo.modeInfoIdx;
        UINT32 tgtIdx = state.paths[i].targetInfo.modeInfoIdx;

        // Reuse an existing source mode if another path already shares this source
        UINT32 existingSrcIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        for (UINT32 k = 0; k < pathCount; k++)
        {
            if (k == i) continue;
            if (state.paths[k].sourceInfo.adapterId.LowPart  == state.paths[i].sourceInfo.adapterId.LowPart &&
                state.paths[k].sourceInfo.adapterId.HighPart == state.paths[i].sourceInfo.adapterId.HighPart &&
                state.paths[k].sourceInfo.id == state.paths[i].sourceInfo.id &&
                state.paths[k].sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
            {
                existingSrcIdx = state.paths[k].sourceInfo.modeInfoIdx;
                break;
            }
        }

        if (existingSrcIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
        {
            srcIdx = existingSrcIdx;
            state.paths[i].sourceInfo.modeInfoIdx = srcIdx;
        }
        else if (srcIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
        {
            DISPLAYCONFIG_MODE_INFO srcModeInfo = {};
            srcModeInfo.infoType  = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
            srcModeInfo.adapterId = state.paths[i].sourceInfo.adapterId;
            srcModeInfo.id        = state.paths[i].sourceInfo.id;
            state.modes.push_back(srcModeInfo);
            srcIdx = (UINT32)state.modes.size() - 1;
            state.paths[i].sourceInfo.modeInfoIdx = srcIdx;
        }

        if (tgtIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
        {
            DISPLAYCONFIG_MODE_INFO tgtModeInfo = {};
            tgtModeInfo.infoType  = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
            tgtModeInfo.adapterId = state.paths[i].targetInfo.adapterId;
            tgtModeInfo.id        = state.paths[i].targetInfo.id;
            state.modes.push_back(tgtModeInfo);
            tgtIdx = (UINT32)state.modes.size() - 1;
            state.paths[i].targetInfo.modeInfoIdx = tgtIdx;
        }

        // Fetch refs AFTER the push_back calls above — push_back can reallocate
        auto& srcMode = state.modes[srcIdx].sourceMode;
        srcMode.width       = target_res->cx;
        srcMode.height      = target_res->cy;
        srcMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
        if(zero_pos)
        {
            srcMode.position.x = 0;
            srcMode.position.y = 0;
        }


        FillSignalInfo(state.modes[tgtIdx].targetMode.targetVideoSignalInfo,
                        target_res->cx, target_res->cy, target_res->refresh);
        state.paths[i].flags |= DISPLAYCONFIG_PATH_ACTIVE;

        return true;
        
    }

    ERR("No matching path found for ConnectorIndex %ws\n", devicePath.c_str());
    return false;
}




bool IterateDisplays(const disp_info& dinfo)
{
	DisplayConfigState state;
    if (!QueryCurrentDisplayConfig(state))
        return false;
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_MONITOR, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);
	bool anyStaged = false;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(devInfo, nullptr, &GUID_DEVINTERFACE_MONITOR, i, &ifData); i++)
    {
        SP_DEVINFO_DATA devInfoData = {};
        devInfoData.cbSize = sizeof(devInfoData);

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, nullptr, 0, &requiredSize, &devInfoData);
        if (requiredSize == 0) continue;

        std::vector<BYTE> buffer(requiredSize);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

        if (!SetupDiGetDeviceInterfaceDetailW(devInfo, &ifData, detail, requiredSize, nullptr, &devInfoData))
            continue;

        GUID containerId;
        DEVPROPTYPE propType;
        if (SetupDiGetDevicePropertyW(devInfo, &devInfoData, &DEVPKEY_Device_ContainerId,
                &propType, reinterpret_cast<PBYTE>(&containerId), sizeof(containerId), nullptr, 0)
		    ) {
		    bool isIdd = false;
			for (int connectorIndex = 0; connectorIndex < 4; connectorIndex++) {
				GUID id = GetStableMonitorContainerId(connectorIndex);
				if (IsEqualGUID(containerId, id)) {
					DBGPRINT("Found device path for connector index %d: %ws\n", connectorIndex, detail->DevicePath);
					isIdd = true;
					if(dinfo.disp_target_res[connectorIndex].set && dinfo.disp_target_res[connectorIndex].enabled){
   				    	anyStaged |= StageDisplayChange(state,  detail->DevicePath, &dinfo.disp_target_res[connectorIndex],false, connectorIndex == 0);
					}
 					break;
				}
			}
			if(isIdd == false) {
				// disable non-IDD displays aka MSFT display
				DBGPRINT("Disabling non-IDD display: %ws\n", detail->DevicePath);
				anyStaged |= StageDisplayChange(state,  detail->DevicePath, nullptr, true);
			}
		}

	
    }
    SetupDiDestroyDeviceInfoList(devInfo);

	if (!anyStaged)
        return false;
	LONG result = SetDisplayConfig(
        (UINT32)state.paths.size(), state.paths.data(),
        (UINT32)state.modes.size(), state.modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE);

    if (result != ERROR_SUCCESS)
    {
        ERR("SetDisplayConfig failed with %ld\n", result);
        return false;
    }

	return true;
}

extern "C" __declspec(dllexport) void CALLBACK dvenabler_init(
    HWND hwnd,        // Handle to owner window
    HINSTANCE hinst,  // Instance handle of the DLL
    LPSTR lpszCmdLine,// Command line string
    int nCmdShow      // Window show state
){
	UNREFERENCED_PARAMETER(hwnd);
	UNREFERENCED_PARAMETER(hinst);
	UNREFERENCED_PARAMETER(lpszCmdLine);
	UNREFERENCED_PARAMETER(nCmdShow);
	WPP_INIT_TRACING(NULL);
	TRACING();
	DBGPRINT("DVenabler init dve_event\n");
	HANDLE hp_event = NULL;
	HANDLE dve_event = NULL;
	int status;
	disp_info dinfo = {0};
	// Create Security Descriptor for HOTPLUG_EVENT, To allow the DVServerUMD to access the event
	PSECURITY_DESCRIPTOR hp_psd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
	InitializeSecurityDescriptor(hp_psd, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(hp_psd, TRUE, NULL, FALSE);

	SECURITY_ATTRIBUTES hp_sa = {0};
	hp_sa.nLength = sizeof(hp_sa);
	hp_sa.lpSecurityDescriptor = hp_psd;
	hp_sa.bInheritHandle = FALSE;

	hp_event = CreateEvent(&hp_sa, FALSE, FALSE, HOTPLUG_EVENT);
	if (NULL == hp_event) {
		ERR("Cannot create HOTPULG event!\n");
		return;
	}
	DBGPRINT("HOTPLUG_EVENT created successfully\n");
	// Create Security Descriptor for DVE_EVENT, To allow the DVServerUMD to access the event
	PSECURITY_DESCRIPTOR dve_psd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
	InitializeSecurityDescriptor(dve_psd, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(dve_psd, TRUE, NULL, FALSE);
	DBGPRINT("Security Descriptor created successfully\n");
	SECURITY_ATTRIBUTES dve_sa = {0};
	dve_sa.nLength = sizeof(dve_sa);
	dve_sa.lpSecurityDescriptor = dve_psd;
	dve_sa.bInheritHandle = FALSE;

	dve_event = CreateEvent(&dve_sa, FALSE, FALSE, DVE_EVENT);
	if (NULL == dve_event) {
		ERR("Cannot create DVE event!\n");
		CloseHandle(hp_event);
		return;
	}

	DBGPRINT("Pre Loop");
	
	while (1) {
		DBGPRINT("Loop");
		if (IsSystemLocked()) {
			DBGPRINT("System is in locked state, so wait untill system gets unlocked");
			continue;
		}
		if (GetDisplayCount(&dinfo) == DVENABLER_FAILURE) {
			ERR("shared mem read failed");
			goto end;
		}
		IterateDisplays(dinfo);
		
	
		/*If there is any display config change at the time of reboot / shutdown.
		At this stage, Since the Dvenabler is not running, Changed display config will not be saved in windows
		persistence, So at this case MSFT path will be enabled and since the DV enabler starts only after user login The
		login page  will have blank screen after boot, untill we enter the password. To over come this blank out
		issue...In UMD always we will always boot with single display config After login, Dvenabler will set the below
		event to enable the HPD path Once this event is set our DVserver UMD driver will enable the Hot plug path and
		get the display status from KMD So this event is Set once after every boot to enable the HPD path in our
		DVServer UMD driver */
		status = SetEvent(hp_event);
		DBGPRINT("Set HPevent status = %d\n", status);
		if (status == NULL) {
			ERR(" Set HPevent failed with error [%d]\n ", GetLastError());
			continue;
		}

	end:
		// wait for arraival or departure call from UMD
		WaitForSingleObject(dve_event, INFINITE);
	}
	DBGPRINT("DVenabler exiting");
	WPP_CLEANUP();
	CloseHandle(hp_event);
	CloseHandle(dve_event);

}

static void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    Mode.AdditionalSignalInfo.vSyncFreqDivider = 1;
    Mode.AdditionalSignalInfo.videoStandard = 255;

    Mode.vSyncFreq.Numerator = VSync;
    Mode.vSyncFreq.Denominator = 1;
    Mode.hSyncFreq.Numerator = VSync * Height;
    Mode.hSyncFreq.Denominator = 1;

    Mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
	Mode.pixelRate =  (UINT64)VSync * (UINT64)Width * (UINT64)Height;
}

GUID GetStableMonitorContainerId(UINT ConnectorIndex)
{
    static const GUID Namespace =
    { 0x7d51b9b0, 0x5eb5, 0x40ab, { 0xa5, 0x99, 0xff, 0x1c, 0x89, 0x77, 0x28, 0xb0 } };

    GUID Id = Namespace;
    Id.Data1 ^= ConnectorIndex;
    return Id;
}

int GetDisplayCount(disp_info *pdinfo)
{

	// Open the existing shared memory section by its name
	HANDLE hSharedMem = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, DISP_INFO);

	if (hSharedMem == NULL) {
		ERR("Failed to open shared memory section (%d)\n", GetLastError());
		return DVENABLER_FAILURE;
	}

	// Map the shared memory into the process's address space
	struct disp_info *pSharedMem =
		(struct disp_info *)MapViewOfFile(hSharedMem,	 // Handle to the shared memory section
										  FILE_MAP_ALL_ACCESS, // Read/write access
										  0,			 // File offset - high-order DWORD
										  0,			 // File offset - low-order DWORD
										  0);			 // Mapping size (0 means to map the entire section)

	if (pSharedMem == NULL) {
		ERR(L"Failed to map view of shared memory section (%d)\n", GetLastError());
		CloseHandle(hSharedMem);
		return DVENABLER_FAILURE;
	}

	HANDLE hMutex = OpenMutexW(MUTEX_ALL_ACCESS, FALSE, L"Global\\DVEnablerMutex");
    if (hMutex != NULL) {
		DWORD rc = WaitForSingleObject(hMutex, INFINITE);
		if (rc != WAIT_OBJECT_0 && rc != WAIT_ABANDONED)
		{
			ERR("WaitForSingleObject failed (%d)\n", GetLastError());
			CloseHandle(hMutex);
			UnmapViewOfFile(pSharedMem);
			CloseHandle(hSharedMem);
			return DVENABLER_FAILURE;
		}
	} else {
		ERR("Failed to open mutex (%d)\n", GetLastError());
		UnmapViewOfFile(pSharedMem);
		CloseHandle(hSharedMem);
		return DVENABLER_FAILURE;
	}
	*pdinfo = *pSharedMem;
	for (auto &disp : pSharedMem->disp_target_res)
	{
		disp.set = false;
	}
	ReleaseMutex(hMutex);
    CloseHandle(hMutex);

	UnmapViewOfFile(pSharedMem);
	CloseHandle(hSharedMem);

	return DVENABLER_SUCCESS;
}


/*******************************************************************************
 *
 * Description
 *
 * IsSystemLocked - This function is used to check if the system is in locked
 * or unlocked state.
 *
 * Parameters
 * Null
 *
 * Return val
 * int - 0 = Unlocked, -1 = ERROR, 1 = Locked
 *
 ******************************************************************************/

int IsSystemLocked()
{
    HDESK hDesk = OpenInputDesktop(0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (hDesk == NULL) 
    {
        // If OpenInputDesktop fails with Access Denied while a user is logged in, 
        // it typically means the lock screen (LogonUI) is active.
        if (GetLastError() == ERROR_ACCESS_DENIED) 
        {
			DBGPRINT("System is locked\n");
            return TRUE;
        }
		ERR("Unexpected output\n");
        return DVENABLER_FAILURE;
    }
    
    CloseDesktop(hDesk);
	DBGPRINT("System is unlocked\n");
    return FALSE;
}