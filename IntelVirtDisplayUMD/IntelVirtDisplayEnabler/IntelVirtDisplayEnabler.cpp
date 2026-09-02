/*===========================================================================
; IntelVirtDisplayEnabler.cpp
;----------------------------------------------------------------------------
; Copyright (C) 2021 Intel Corporation
; SPDX-License-Identifier: MIT
;
; File Description:
;   This file will disable MSFT Display Path (MBDA)
;--------------------------------------------------------------------------*/

#include "pch.h"
#include "Trace.h"
#include "IntelVirtDisplayEnabler.tmh"
#include <Windows.h>
#include <stdio.h>
#include <string.h>
#include <sddl.h>
#include <string>
#include <initguid.h> 
#include <Setupapi.h>
#include <Ntddvdeo.h>
#include <Devpkey.h>
#include "Trace_override.h"

extern "C" __declspec(dllexport) int CALLBACK intelvirtdisplayenabler_init(
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
	DBGPRINT("IntelVirtDisplayEnabler init dve_event\n");
	HANDLE hp_event = NULL;
	HANDLE dve_event = NULL;
	// Create Security Descriptor for HOTPLUG_EVENT.
	// Grants SYNCHRONIZE | EVENT_MODIFY_STATE to Local System (SY), Local Service (LS),
	// and the interactive user (IU) only. Matches the descriptor used in IntelVirtDisplayUMD.
	PSECURITY_DESCRIPTOR hp_psd = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			L"D:(A;;0x00100002;;;SY)(A;;0x00100002;;;LS)(A;;0x00100002;;;IU)", SDDL_REVISION_1, &hp_psd, NULL)) {
		ERR("Failed to create security descriptor for HOTPLUG event, error: %d\n", GetLastError());
		WPP_CLEANUP();
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	SECURITY_ATTRIBUTES hp_sa = {0};
	hp_sa.nLength = sizeof(hp_sa);
	hp_sa.lpSecurityDescriptor = hp_psd;
	hp_sa.bInheritHandle = FALSE;

	hp_event = CreateEvent(&hp_sa, FALSE, FALSE, HOTPLUG_EVENT);
	DWORD hp_last_error = GetLastError();
	LocalFree(hp_psd);
	hp_psd = NULL;
	if (NULL == hp_event) {
		if (hp_last_error == ERROR_ACCESS_DENIED) {
			DBGPRINT("HOTPLUG_EVENT already exists, opening by name\n");
			hp_event = OpenEvent(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, HOTPLUG_EVENT);
		}
		if (NULL == hp_event) {
			ERR("Cannot create or open HOTPLUG event! GetLastError: %d\n", GetLastError());
			WPP_CLEANUP();
			return INTELVIRTDISPLAYENABLER_FAILURE;
		}
	}

	// Create Security Descriptor for DVE_EVENT.
	// Grants SYNCHRONIZE | EVENT_MODIFY_STATE to Local System (SY), Local Service (LS),
	// and the interactive user (IU) only. Matches the descriptor used in IntelVirtDisplayUMD.
	PSECURITY_DESCRIPTOR dve_psd = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			L"D:(A;;0x00100002;;;SY)(A;;0x00100002;;;LS)(A;;0x00100002;;;IU)", SDDL_REVISION_1, &dve_psd, NULL)) {
		ERR("Failed to create security descriptor for DVE event, error: %d\n", GetLastError());
		WPP_CLEANUP();
		CloseHandle(hp_event);
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	SECURITY_ATTRIBUTES dve_sa = {0};
	dve_sa.nLength = sizeof(dve_sa);
	dve_sa.lpSecurityDescriptor = dve_psd;
	dve_sa.bInheritHandle = FALSE;

	dve_event = CreateEvent(&dve_sa, FALSE, FALSE, DVE_EVENT);
	DWORD dve_last_error = GetLastError();
	LocalFree(dve_psd);
	dve_psd = NULL;
	if (NULL == dve_event) {
		if (dve_last_error == ERROR_ACCESS_DENIED) {
			DBGPRINT("DVE_EVENT already exists, opening by name\n");
			dve_event = OpenEvent(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, DVE_EVENT);
		}
		if (NULL == dve_event) {
			ERR("Cannot create or open DVE event! GetLastError: %d\n", GetLastError());
			WPP_CLEANUP();
			CloseHandle(hp_event);
			return INTELVIRTDISPLAYENABLER_FAILURE;
		}
	}

	// Create Security Descriptor for RESIZE_EVENT.
	// Grants SYNCHRONIZE | EVENT_MODIFY_STATE to Local System (SY), Local Service (LS),
	// and the interactive user (IU) only. Matches the descriptor used in IntelVirtDisplayUMD.
	PSECURITY_DESCRIPTOR resize_psd = NULL;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
			L"D:(A;;0x00100002;;;SY)(A;;0x00100002;;;LS)(A;;0x00100002;;;IU)", SDDL_REVISION_1, &resize_psd, NULL)) {
		ERR("Failed to create security descriptor for RESIZE event, error: %d\n", GetLastError());
		WPP_CLEANUP();
		CloseHandle(hp_event);
		CloseHandle(dve_event);
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	SECURITY_ATTRIBUTES resize_sa = {0};
	resize_sa.nLength = sizeof(resize_sa);
	resize_sa.lpSecurityDescriptor = resize_psd;
	resize_sa.bInheritHandle = FALSE;

	HANDLE resize_event = CreateEvent(&resize_sa, FALSE, FALSE, RESIZE_EVENT);
	DWORD resize_last_error = GetLastError();
	LocalFree(resize_psd);
	resize_psd = NULL;
	if (NULL == resize_event) {
		if (resize_last_error == ERROR_ACCESS_DENIED) {
			DBGPRINT("RESIZE_EVENT already exists, opening by name\n");
			resize_event = OpenEvent(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, RESIZE_EVENT);
		}
		if (NULL == resize_event) {
			ERR("Cannot create or open RESIZE event! GetLastError: %d\n", GetLastError());
			WPP_CLEANUP();
			CloseHandle(hp_event);
			CloseHandle(dve_event);
			return INTELVIRTDISPLAYENABLER_FAILURE;
		}
	}

	HANDLE waitHandles[2] = { dve_event, resize_event };
	DWORD waitResult = WAIT_OBJECT_0;

	while (1) {
		if (IsSystemLocked()) {
			DBGPRINT("System is in locked state, so wait untill system gets unlocked");
			continue;
		}
		if (waitResult == WAIT_OBJECT_0) {
			DBGPRINT("DVE_EVENT is set, so handle the HP event");
			if (!HandleHPEvent(hp_event)){
				continue;
			}
		}
		else if (waitResult == WAIT_OBJECT_0 + 1) {
			DBGPRINT("RESIZE_EVENT is set, so handle the RESIZE event");
			HandleResizeEvent();
		}
		else {
			DBGPRINT("WaitForMultipleObjects returned unexpected value: %d\n", waitResult);
		}
		DBGPRINT("Waiting for DVE_EVENT or RESIZE_EVENT to be set\n");
		waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
	}

	return 0;
}

static bool HandleHPEvent(HANDLE hp_event)
{
	unsigned int path_count = NULL, mode_count = NULL;
	bool found_id_path = FALSE, found_non_id_path = FALSE;
	int status;
	char err[256];
	memset(err, 0, 256);
	disp_info dinfo = {0};
	DISPLAYCONFIG_TARGET_BASE_TYPE baseType;
	/* Initializing the baseType.baseOutputTechnology to default OS value(failcase) */
	baseType.baseOutputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;
	DBGPRINT("IntelVirtDisplayEnabler HandleHPEvent dve_event\n");
	/* Step 0: Get the size of buffers w.r.t active paths and modes, required for QueryDisplayConfig */
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
		FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
						err, 255, NULL);
		ERR("GetDisplayConfigBufferSizes failed with %s. Exiting!!!\n", err);
		return false;
	}
	DBGPRINT("path_count = %d, mode_count = %d\n", path_count, mode_count);
	/* Initializing STL vectors for all the paths and its respective modes */
	std::vector<DISPLAYCONFIG_PATH_INFO> path_list(path_count);
	std::vector<DISPLAYCONFIG_MODE_INFO> mode_list(mode_count);

	// Get the Display info shared from IntelVirtDisplayUMD
	if (GetDisplayCount(&dinfo) == INTELVIRTDISPLAYENABLER_FAILURE) {
		ERR("shared mem read failed");
		return true;
	}
	DBGPRINT("disp_count = %d\n", dinfo.disp_count);
	/* Step 1: Retrieve information about all possible display paths for all display devices */
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, path_list.data(), &mode_count, mode_list.data(),
							nullptr) != ERROR_SUCCESS) {
		FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
						err, 255, NULL);
		ERR("QueryDisplayConfig failed with %s. Exiting!!!\n", err);
		return false;
	}

	DBGPRINT("QueryDisplayConfig returned %d active paths and %d modes\n", path_count, mode_count);
	for (auto &activepath_loopindex : path_list) {
		DBGPRINT("loop");
		baseType.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_BASE_TYPE;
		baseType.header.size = sizeof(baseType);
		baseType.header.adapterId = activepath_loopindex.sourceInfo.adapterId;
		baseType.header.id = activepath_loopindex.targetInfo.id;

		/* Step 2 : DisplayConfigGetDeviceInfo function retrieves display configuration information about the device
			*/
		if (DisplayConfigGetDeviceInfo(&baseType.header) != ERROR_SUCCESS) {
			ERR("DisplayConfigGetDeviceInfo failed... Continuing with other active paths!!!\n");
			return false;
		}

		DBGPRINT("baseType.baseOutputTechnology = %d\n", baseType.baseOutputTechnology);
		if (!(found_non_id_path && found_id_path)) {
			/* Step 3: Check for the "outputTechnology" it should be
				"DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED" for IDD path ONLY, In case of MSFT display we need
				to disable the active display path  */
			if (baseType.baseOutputTechnology != DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED) {

				/* Step 4: Clear the DISPLAYCONFIG_PATH_INFO.flags for MSFT path*/
				activepath_loopindex.flags = 0;
				DBGPRINT("Clearing Microsoft activepath_loopindex.flags.\n");
				found_non_id_path = true;
			} else {
				/* Move the IDD source co-ordinates to (0,0)  if MSBDA monitor is listed as first monitor in the
					* path list*/
				if (found_non_id_path && !found_id_path) {
					mode_list[activepath_loopindex.sourceInfo.modeInfoIdx].sourceMode.position.x = 0;
					mode_list[activepath_loopindex.sourceInfo.modeInfoIdx].sourceMode.position.y = 0;
					DBGPRINT("x, y  = %dX%x\n",
								mode_list[activepath_loopindex.sourceInfo.modeInfoIdx].sourceMode.position.x,
								mode_list[activepath_loopindex.sourceInfo.modeInfoIdx].sourceMode.position.y);
				}
				found_id_path = true;
			}
		}
	}

	if ((found_non_id_path && (path_count != static_cast<unsigned int>(dinfo.disp_count + 1))) ||
		(!found_non_id_path && (path_count != static_cast<unsigned int>(dinfo.disp_count)))) {
		if (found_non_id_path) {
			DBGPRINT("MSFT display is present. Path count not updated, so loop again");
		} else {
			DBGPRINT("MSFT display is not present. Path count not updated, so loop again");
		}
		DBGPRINT("disp_count = %d, path count = %d", dinfo.disp_count, path_count);
		return false;
	}

	if (found_non_id_path && found_id_path) {
		/* Step 5: SetDisplayConfig modifies the display topology by exclusively enabling/disabling the specified
					paths in the current session. */
		if (SetDisplayConfig(path_count, path_list.data(), mode_count, mode_list.data(),
								SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE) != ERROR_SUCCESS) {
			FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
							MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), err, 255, NULL);
			ERR("SetDisplayConfig failed with %s\n", err);
			return false;
		}
	} else {
		DBGPRINT("Skipping SetDisplayConfig as did not find ID and non-ID path. found_non_id_path = %d, "
					"found_id_path = %d\n",
					found_non_id_path, found_id_path);
	}

	/*If there is any display config change at the time of reboot / shutdown.
	At this stage, Since the IntelVirtDisplayEnabler is not running, Changed display config will not be saved in windows
	persistence, So at this case MSFT path will be enabled and since the DV enabler starts only after user login The
	login page  will have blank screen after boot, untill we enter the password. To over come this blank out
	issue...In UMD always we will always boot with single display config After login, IntelVirtDisplayEnabler will set the below
	event to enable the HPD path Once this event is set our IntelVirtDisplay UMD driver will enable the Hot plug path and
	get the display status from KMD So this event is Set once after every boot to enable the HPD path in our
	IntelVirtDisplay UMD driver */
	status = SetEvent(hp_event);
	if (status == NULL) {
		ERR(" Set HPevent failed with error [%d]\n ", GetLastError());
		return false;
	}
	return true;
}

int GetDisplayCount(disp_info *pdinfo)
{
	HANDLE hSharedMem = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, DISP_INFO);
	if (hSharedMem == NULL) {
		ERR("Failed to open shared memory section (%d)\n", GetLastError());
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	struct disp_info *pSharedMem = (struct disp_info *)MapViewOfFile(hSharedMem, FILE_MAP_ALL_ACCESS, 0, 0, 0);
	if (pSharedMem == NULL) {
		ERR("Failed to map view of shared memory section (%d)\n", GetLastError());
		CloseHandle(hSharedMem);
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	// Open the named mutex by name - never read a HANDLE from shared memory,
	// as HANDLEs are per-process and invalid across process boundaries.
	HANDLE hDispMutex = OpenMutexW(MUTEX_ALL_ACCESS, FALSE, DISP_INFO_MUTEX);
	if (hDispMutex == NULL) {
		ERR("Failed to open named mutex for shared memory (%d)\n", GetLastError());
		UnmapViewOfFile(pSharedMem);
		CloseHandle(hSharedMem);
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	WaitForSingleObject(hDispMutex, INFINITE);
	*pdinfo = *pSharedMem;
	for (auto &disp : pSharedMem->disp_target_res)
	{
		disp.set = false;
	}
	ReleaseMutex(hDispMutex);

	CloseHandle(hDispMutex);
	UnmapViewOfFile(pSharedMem);
	CloseHandle(hSharedMem);

	return INTELVIRTDISPLAYENABLER_SUCCESS;
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
		
		if (target_res->cx == 0 || target_res->cy == 0 || target_res->refresh == 0) {
			if (state.paths[i].sourceInfo.modeInfoIdx == DISPLAYCONFIG_PATH_MODE_IDX_INVALID) {
				state.paths[i].flags |= DISPLAYCONFIG_PATH_ACTIVE;
			}
			return true;
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

		state.paths[i].flags |= DISPLAYCONFIG_PATH_ACTIVE;

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
        

        return true;
        
    }

    ERR("No matching path found for ConnectorIndex %ws\n", devicePath.c_str());
    return false;
}

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
			for (int connectorIndex = 0; connectorIndex < 4; connectorIndex++) {
				GUID id = GetStableMonitorContainerId(connectorIndex);
				if (IsEqualGUID(containerId, id)) {
					DBGPRINT("Found device path for connector index %d: %ws\n", connectorIndex, detail->DevicePath);
					if(dinfo.disp_target_res[connectorIndex].set && dinfo.disp_target_res[connectorIndex].enabled){
   				    	anyStaged |= StageDisplayChange(state,  detail->DevicePath, &dinfo.disp_target_res[connectorIndex],false, connectorIndex == 0);
					}
 					break;
				}
			}
		}

    }
    SetupDiDestroyDeviceInfoList(devInfo);

	if (!anyStaged)
        return false;
	LONG result = SetDisplayConfig(
        (UINT32)state.paths.size(), state.paths.data(),
        (UINT32)state.modes.size(), state.modes.data(),
        SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES | SDC_SAVE_TO_DATABASE | SDC_FORCE_MODE_ENUMERATION);

    if (result != ERROR_SUCCESS)
    {
        ERR("SetDisplayConfig failed with %ld\n", result);
        return false;
    }

	return true;
}

static void HandleResizeEvent()
{
	disp_info dinfo;
    if (GetDisplayCount(&dinfo) == INTELVIRTDISPLAYENABLER_SUCCESS) {
        IterateDisplays(dinfo);
    }
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
        return INTELVIRTDISPLAYENABLER_FAILURE;
    }
    
    CloseDesktop(hDesk);
	DBGPRINT("System is unlocked\n");
    return FALSE;
}