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
#include <string>
#include <initguid.h> 
#include <Setupapi.h>
#include <Ntddvdeo.h>
#include <Devpkey.h>
#include "Trace_override.h"
bool operator==(const LUID &a, const LUID &b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

bool FindMonitorDevicePathByContainerId(const GUID& targetContainerId, std::wstring& outDevicePath)
{
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_MONITOR, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (devInfo == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA ifData = {};
    ifData.cbSize = sizeof(ifData);

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
		 
			DBGPRINT("Comparing device path: %ws GUID: %!GUID!\n", detail->DevicePath, &containerId);
			if(IsEqualGUID(containerId, targetContainerId))
			{
				outDevicePath = detail->DevicePath;
				SetupDiDestroyDeviceInfoList(devInfo);
				return true;
        }
		}
    }

    SetupDiDestroyDeviceInfoList(devInfo);
    return false;
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
	DISPLAYCONFIG_TARGET_BASE_TYPE baseType;
	HANDLE hp_event = NULL;
	HANDLE dve_event = NULL;
	bool anyApplied = false;
	bool anyIDDActive = false;
	int msftDisplayIndex = -1;
	char err[256];
	memset(err, 0, 256);
	int status;
	disp_info dinfo = {0};
	/* Initializing the baseType.baseOutputTechnology to default OS value(failcase) */
	baseType.baseOutputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;

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
		msftDisplayIndex = -1;
		anyIDDActive = false;
		anyApplied = false;
		UINT32 pathCount, modeCount;
		if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
			ERR("GetDisplayConfigBufferSizes failed with %s. Exiting!!!\n", err);
			return;
		}
			

		std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
		std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

		if (QueryDisplayConfig(QDC_ALL_PATHS, &pathCount, paths.data(),
							&modeCount, modes.data(), nullptr) != ERROR_SUCCESS){
			ERR("QueryDisplayConfig failed with %s. Exiting!!!\n", err);
			goto end;
		}

		if (GetDisplayCount(&dinfo) == DVENABLER_FAILURE) {
			ERR("shared mem read failed");
			goto end;
		}
		for (auto &disp : dinfo.disp_target_res) {
			DBGPRINT("Display: %d, enabled: %d, set: %d, res: %dx%d @ %dHz\n", (int)(&disp - dinfo.disp_target_res),
					 disp.enabled, disp.set, disp.cx, disp.cy, disp.refresh);
		}

		for (auto &disp : dinfo.disp_target_res)
		{
			if (disp.set == (uint8_t) false) {
				if(disp.enabled ==(uint8_t) true){
					anyIDDActive = true;
				}
				continue;
			}
			int connectorIndex = (int)(&disp - dinfo.disp_target_res);
			std::wstring devicePath;
			GUID id = GetStableMonitorContainerId(connectorIndex);
			if (!FindMonitorDevicePathByContainerId(id, devicePath))
			{
				ERR(" Could not resolve device path for ConnectorIndex %u\n", connectorIndex);
				continue;
			}
			bool found = false;
			DBGPRINT("pathCount = %d, modeCount = %d\n", pathCount, modeCount);
			DBGPRINT("Paths");
			for (UINT32 i = 0; i < pathCount; i++)
			{
				UINT32 srcIdx = paths[i].sourceInfo.modeInfoIdx;
				UINT32 tgtIdx = paths[i].targetInfo.modeInfoIdx;
				if (srcIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
				{
					DBGPRINT("Source Mode: %dx%d position: x=%d y=%d\n", modes[srcIdx].sourceMode.width,
							 modes[srcIdx].sourceMode.height, modes[srcIdx].sourceMode.position.x,
							 modes[srcIdx].sourceMode.position.y);
				}
					
				if (tgtIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
				{
					DBGPRINT("Target Mode: %dx%d active: %dx%d \n", modes[tgtIdx].targetMode.targetVideoSignalInfo.totalSize.cx, modes[tgtIdx].targetMode.targetVideoSignalInfo.totalSize.cy, modes[tgtIdx].targetMode.targetVideoSignalInfo.activeSize.cx, modes[tgtIdx].targetMode.targetVideoSignalInfo.activeSize.cy);
				}
			}
			DBGPRINT("MODES");
			for (UINT32 i = 0; i < modeCount; i++)
			{
				
				if(modes[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE)
				{
					DBGPRINT("Source Mode: %dx%d\n", modes[i].sourceMode.width, modes[i].sourceMode.height);
				}
				else if(modes[i].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
				{
					DBGPRINT("Target Mode: %dx%d active: %dx%d ", modes[i].targetMode.targetVideoSignalInfo.totalSize.cx, modes[i].targetMode.targetVideoSignalInfo.totalSize.cy, modes[i].targetMode.targetVideoSignalInfo.activeSize.cx, modes[i].targetMode.targetVideoSignalInfo.activeSize.cy);
				}

			}

			for (UINT32 i = 0; i < pathCount; i++)
			{
				DISPLAYCONFIG_TARGET_DEVICE_NAME name = {};
				name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
				name.header.size = sizeof(name);
				name.header.adapterId = paths[i].targetInfo.adapterId;
				name.header.id = paths[i].targetInfo.id;

				if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS){
					DBGPRINT("DisplayConfigGetDeviceInfo failed for path %d\n", i);
					continue;
				}
					
			
				DBGPRINT("Comparing device path: %ws with %ws\n", name.monitorDevicePath, devicePath.c_str());

				if (_wcsicmp(name.monitorDevicePath, devicePath.c_str()) != 0)
					continue;

				UINT32 srcIdx = paths[i].sourceInfo.modeInfoIdx;
				UINT32 tgtIdx = paths[i].targetInfo.modeInfoIdx;

				UINT32 existingSrcIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
				for (UINT32 k = 0; k < pathCount; k++) {
					if (k == i) continue;
					if (paths[k].sourceInfo.adapterId.LowPart == paths[i].sourceInfo.adapterId.LowPart &&
						paths[k].sourceInfo.adapterId.HighPart == paths[i].sourceInfo.adapterId.HighPart &&
						paths[k].sourceInfo.id == paths[i].sourceInfo.id &&
						paths[k].sourceInfo.modeInfoIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
					{
						existingSrcIdx = paths[k].sourceInfo.modeInfoIdx;
						break;
					}
				}
				if (existingSrcIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
					srcIdx = existingSrcIdx;
					paths[i].sourceInfo.modeInfoIdx = srcIdx;
				} else {
				
					DISPLAYCONFIG_MODE_INFO srcModeInfo = {};
					srcModeInfo.infoType  = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
					srcModeInfo.adapterId = paths[i].sourceInfo.adapterId;
					srcModeInfo.id        = paths[i].sourceInfo.id;

					modes.push_back(srcModeInfo);
					srcIdx = (UINT32)modes.size() - 1;
					paths[i].sourceInfo.modeInfoIdx = srcIdx;
				}

				if (tgtIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
				{
					DISPLAYCONFIG_MODE_INFO tgtModeInfo = {};
					tgtModeInfo.infoType  = DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
					tgtModeInfo.adapterId = paths[i].targetInfo.adapterId;
					tgtModeInfo.id        = paths[i].targetInfo.id;

					modes.push_back(tgtModeInfo);
					tgtIdx = (UINT32)modes.size() - 1;
					paths[i].targetInfo.modeInfoIdx = tgtIdx;
				}

				// From here on, srcIdx/tgtIdx are guaranteed valid — write the resolution unconditionally
				auto& srcMode = modes[srcIdx].sourceMode;
				srcMode.width       = disp.cx;
				srcMode.height      = disp.cy;
				srcMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;

				FillSignalInfo(modes[tgtIdx].targetMode.targetVideoSignalInfo, disp.cx, disp.cy, disp.refresh);

				DBGPRINT("Applied display %d %ws settings: %dx%d @ %dHz\n",
						connectorIndex, devicePath.c_str(), disp.cx, disp.cy, disp.refresh);
				
				
				if(!anyApplied && disp.enabled ){
					modes[srcIdx].sourceMode.position.x = 0;
					modes[srcIdx].sourceMode.position.y = 0;
				}
				if (disp.enabled == false)
				{
					DBGPRINT("Disabling display %d\n", connectorIndex);
					paths[i].flags &= ~DISPLAYCONFIG_PATH_ACTIVE;
				}
				else
				{
					DBGPRINT("Enabling display %d\n", connectorIndex);
					paths[i].flags |= DISPLAYCONFIG_PATH_ACTIVE;
					anyIDDActive = true;
					
				}
				anyApplied = true;
				found = true;
				
				break; // matched this request, move to next
			}

			if (!found) {
				DBGPRINT("No active path found for ConnectorIndex %u (device path %ws)\n",
					connectorIndex, devicePath.c_str());
			}
		}
		/*
		// find the MSFT display path index
		for (UINT32 i = 0; i < pathCount; i++)
		{
			DISPLAYCONFIG_TARGET_DEVICE_NAME name = {};
			name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
			name.header.size = sizeof(name);
			name.header.adapterId = paths[i].targetInfo.adapterId;
			name.header.id = paths[i].targetInfo.id;

			if (DisplayConfigGetDeviceInfo(&name.header) != ERROR_SUCCESS){
				DBGPRINT("DisplayConfigGetDeviceInfo failed for path %d\n", i);
				continue;
			}
			baseType.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_BASE_TYPE;
			baseType.header.size = sizeof(baseType);
			baseType.header.adapterId = paths[i].targetInfo.adapterId;
			baseType.header.id = paths[i].targetInfo.id;
			if (DisplayConfigGetDeviceInfo(&baseType.header) != ERROR_SUCCESS) {
				ERR("DisplayConfigGetDeviceInfo failed... Continuing with other active paths!!!\n");
				continue;
			}
			if (baseType.baseOutputTechnology != DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED)
			{
				DBGPRINT("Found MSFT Display Path at index %d at Path %ws\n", i, name.monitorDevicePath);
				msftDisplayIndex = i;
				break;
			}
			
		}

		//make sure always one display is active, if all IDD are disabled then enable MSFT display path
		if (msftDisplayIndex != -1) {
			if (anyIDDActive) {
				DBGPRINT("Disabling MSFT Display Path\n");
				paths[msftDisplayIndex].flags  &= ~DISPLAYCONFIG_PATH_ACTIVE;
			}
			else {
				UINT32 srcIdx = paths[msftDisplayIndex].sourceInfo.modeInfoIdx;
				UINT32 tgtIdx = paths[msftDisplayIndex].targetInfo.modeInfoIdx;
				if(srcIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID || tgtIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID)
				{
					DBGPRINT("MSFT Display Path has invalid source or target mode index\n");
				}
				DBGPRINT("Enabling MSFT Display Path\n");
				paths[msftDisplayIndex].flags |= DISPLAYCONFIG_PATH_ACTIVE;
			}
		} else {
			DBGPRINT("No MSFT Display Path found\n");
		}*/
		if (anyApplied) {
			DBGPRINT("Applying display configuration changes\n");
			LONG result = SetDisplayConfig(pathCount, paths.data(), (UINT32)modes.size(), modes.data(),
				SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG  | SDC_SAVE_TO_DATABASE);
			if (result != ERROR_SUCCESS) {
				DBGPRINT("SetDisplayConfig failed with error %ld\n", result);
				//continue;
			}
		}
		
	
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

GUID GetStableMonitorContainerId(UINT ConnectorIndex)
{
    static const GUID Namespace =
    { 0x12345678, 0x1234, 0x5678, { 0x9a, 0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78 } };

    GUID Id = Namespace;
    Id.Data1 ^= ConnectorIndex;
    return Id;
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
            return TRUE; // System is locked
        }
		ERR("Unexpected output\n");
        return DVENABLER_FAILURE;
    }
    
    CloseDesktop(hDesk);
	DBGPRINT("System is unlocked\n");
    return FALSE; // System is unlocked
}