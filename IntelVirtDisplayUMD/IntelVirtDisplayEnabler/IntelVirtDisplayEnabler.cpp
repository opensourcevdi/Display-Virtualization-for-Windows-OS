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
#include "Trace_override.h"

int intelvirtdisplayenabler_init()
{
	WPP_INIT_TRACING(NULL);
	TRACING();
	DBGPRINT("IntelVirtDisplayEnabler init dve_event\n");
	DISPLAYCONFIG_TARGET_BASE_TYPE baseType;
	HANDLE hp_event = NULL;
	HANDLE dve_event = NULL;
	char err[256];
	memset(err, 0, 256);
	int status;
	unsigned int path_count = NULL, mode_count = NULL;
	bool found_id_path = FALSE, found_non_id_path = FALSE;
	disp_info dinfo = {0};
	/* Initializing the baseType.baseOutputTechnology to default OS value(failcase) */
	baseType.baseOutputTechnology = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_OTHER;

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

	while (1) {
		if (IsSystemLocked()) {
			DBGPRINT("System is in locked state, so wait untill system gets unlocked");
			continue;
		}

		// Reset the flags before doing QDC
		path_count = NULL, mode_count = NULL;
		found_id_path = FALSE, found_non_id_path = FALSE;

		/* Step 0: Get the size of buffers w.r.t active paths and modes, required for QueryDisplayConfig */
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
			FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
						   err, 255, NULL);
			ERR("GetDisplayConfigBufferSizes failed with %s. Exiting!!!\n", err);
			continue;
		}

		/* Initializing STL vectors for all the paths and its respective modes */
		std::vector<DISPLAYCONFIG_PATH_INFO> path_list(path_count);
		std::vector<DISPLAYCONFIG_MODE_INFO> mode_list(mode_count);

		// Get the Display info shared from IntelVirtDisplayUMD
		if (GetDisplayCount(&dinfo) == INTELVIRTDISPLAYENABLER_FAILURE) {
			ERR("shared mem read failed");
			goto end;
		}

		// Apply the resolution published by IntelVirtDisplayUMD on a resize. Updating the
		// IDD mode list only makes the mode available, it does not make it active.
		ApplyResolution(&dinfo);

		/* Step 1: Retrieve information about all possible display paths for all display devices */
		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, path_list.data(), &mode_count, mode_list.data(),
							   nullptr) != ERROR_SUCCESS) {
			FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
						   err, 255, NULL);
			ERR("QueryDisplayConfig failed with %s. Exiting!!!\n", err);
			continue;
		}

		for (auto &activepath_loopindex : path_list) {
			baseType.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_BASE_TYPE;
			baseType.header.size = sizeof(baseType);
			baseType.header.adapterId = activepath_loopindex.sourceInfo.adapterId;
			baseType.header.id = activepath_loopindex.targetInfo.id;

			/* Step 2 : DisplayConfigGetDeviceInfo function retrieves display configuration information about the device
			 */
			if (DisplayConfigGetDeviceInfo(&baseType.header) != ERROR_SUCCESS) {
				ERR("DisplayConfigGetDeviceInfo failed... Continuing with other active paths!!!\n");
				continue;
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
			continue;
		}

		if (found_non_id_path && found_id_path) {
			/* Step 5: SetDisplayConfig modifies the display topology by exclusively enabling/disabling the specified
					   paths in the current session. */
			if (SetDisplayConfig(path_count, path_list.data(), mode_count, mode_list.data(),
								 SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE) != ERROR_SUCCESS) {
				FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
							   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), err, 255, NULL);
				ERR("SetDisplayConfig failed with %s\n", err);
				continue;
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
			continue;
		}

	end:
		// wait for arraival or departure call from UMD
		WaitForSingleObject(dve_event, INFINITE);
	}

	return 0;
}

int GetDisplayCount(disp_info *pdinfo)
{
	HANDLE hSharedMem = OpenFileMapping(FILE_MAP_READ, FALSE, DISP_INFO);
	if (hSharedMem == NULL) {
		ERR("Failed to open shared memory section (%d)\n", GetLastError());
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	struct disp_info *pSharedMem = (struct disp_info *)MapViewOfFile(hSharedMem, FILE_MAP_READ, 0, 0, 0);
	if (pSharedMem == NULL) {
		ERR("Failed to map view of shared memory section (%d)\n", GetLastError());
		CloseHandle(hSharedMem);
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	// Open the named mutex by name - never read a HANDLE from shared memory,
	// as HANDLEs are per-process and invalid across process boundaries.
	HANDLE hDispMutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, DISP_INFO_MUTEX);
	if (hDispMutex == NULL) {
		ERR("Failed to open named mutex for shared memory (%d)\n", GetLastError());
		UnmapViewOfFile(pSharedMem);
		CloseHandle(hSharedMem);
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	WaitForSingleObject(hDispMutex, INFINITE);
	*pdinfo = *pSharedMem;
	ReleaseMutex(hDispMutex);

	CloseHandle(hDispMutex);
	UnmapViewOfFile(pSharedMem);
	CloseHandle(hSharedMem);

	return INTELVIRTDISPLAYENABLER_SUCCESS;
}

/*******************************************************************************
 *
 * Description
 *
 * same_luid - Compares two LUIDs. A LUID is not a scalar, so both halves have
 * to be compared to identify a display adapter.
 *
 * Parameters
 * left, right - the LUIDs to compare
 *
 * Return val
 * bool - true when both LUIDs are identical
 *
 ******************************************************************************/
static bool same_luid(const LUID &left, const LUID &right)
{
	return (left.LowPart == right.LowPart) && (left.HighPart == right.HighPart);
}

/*******************************************************************************
 *
 * Description
 *
 * ApplyResolution - Applies the per screen resolution published by
 * IntelVirtDisplayUMD after a resize. IddCxMonitorUpdateModes only refreshes the
 * mode list reported by the monitor, the OS keeps running the mode it already
 * selected, so the new resolution has to be committed from user mode here.
 *
 * Parameters
 * pdinfo - pointer to the disp_info read from the shared memory section
 *
 * Return val
 * int - 0 == SUCCESS, -1 = ERROR
 *
 ******************************************************************************/
int ApplyResolution(const disp_info *pdinfo)
{
	unsigned int path_count = 0, mode_count = 0;
	int ret = INTELVIRTDISPLAYENABLER_SUCCESS;

	if (pdinfo == NULL) {
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	// Nothing was published, so there is no resize to apply.
	bool resize_pending = false;
	for (int screen = 0; screen < MAX_SCAN_OUT; screen++) {
		if (pdinfo->pending[screen] && pdinfo->identity_valid[screen] && (pdinfo->width[screen] != 0) &&
			(pdinfo->height[screen] != 0)) {
			resize_pending = true;
			break;
		}
	}
	if (!resize_pending) {
		return INTELVIRTDISPLAYENABLER_SUCCESS;
	}

	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS) {
		ERR("GetDisplayConfigBufferSizes failed in ApplyResolution (%d)\n", GetLastError());
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	std::vector<DISPLAYCONFIG_PATH_INFO> path_list(path_count);
	std::vector<DISPLAYCONFIG_MODE_INFO> mode_list(mode_count);

	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, path_list.data(), &mode_count, mode_list.data(),
						   nullptr) != ERROR_SUCCESS) {
		ERR("QueryDisplayConfig failed in ApplyResolution (%d)\n", GetLastError());
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	bool mode_changed = false;
	std::vector<disp_resolution_ack> applied_resolutions;
	// GDI source paths already driven in this pass. In a clone topology several
	// targets share one source, so two connectors can resolve to the same device
	// name and the second mode set would silently overwrite the first.
	std::vector<std::wstring> applied_sources;

	// Walk the requests, not the paths. Each connector carries the display target
	// identity the OS handed to the UMD at monitor arrival, so the owning path can
	// be located exactly. Relying on the IDD paths being enumerated in connector
	// order is only an assumption and would silently resize the wrong display.
	for (unsigned int cur_screen = 0; cur_screen < MAX_SCAN_OUT; cur_screen++) {
		if (pdinfo->pending[cur_screen] == 0) {
			continue;
		}

		unsigned int width = pdinfo->width[cur_screen];
		unsigned int height = pdinfo->height[cur_screen];
		unsigned int refresh_rate = pdinfo->refresh_rate[cur_screen];
		if ((width == 0) || (height == 0)) {
			continue;
		}

		if (pdinfo->identity_valid[cur_screen] == 0) {
			ERR("No display identity for screen %d, leaving the request pending\n", cur_screen);
			ret = INTELVIRTDISPLAYENABLER_FAILURE;
			continue;
		}

		bool matched = false;
		for (auto &path : path_list) {
			if (!same_luid(path.targetInfo.adapterId, pdinfo->adapter_id[cur_screen]) ||
				(path.targetInfo.id != pdinfo->target_id[cur_screen])) {
				continue;
			}
			matched = true;

			// Resolve the GDI device name for this path, ChangeDisplaySettingsEx works on it.
			DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName = {};
			sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
			sourceName.header.size = sizeof(sourceName);
			sourceName.header.adapterId = path.sourceInfo.adapterId;
			sourceName.header.id = path.sourceInfo.id;
			if (DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
				ERR("Failed to get source name for screen %d\n", cur_screen);
				ret = INTELVIRTDISPLAYENABLER_FAILURE;
				break;
			}

			// Another connector is already driving this source, so applying here would
			// overwrite that mode. Leave the request pending rather than clobber it.
			bool shared_source = false;
			for (const std::wstring &applied : applied_sources) {
				if (applied.compare(sourceName.viewGdiDeviceName) == 0) {
					shared_source = true;
					break;
				}
			}
			if (shared_source) {
				ERR("Screen %d shares source %ws with another pending request; "
					"independent clone resizing is not supported\n",
					cur_screen, sourceName.viewGdiDeviceName);
				ret = INTELVIRTDISPLAYENABLER_FAILURE;
				break;
			}

			DEVMODE devmode = {0};
			devmode.dmSize = sizeof(devmode);
			if (!EnumDisplaySettings(sourceName.viewGdiDeviceName, ENUM_CURRENT_SETTINGS, &devmode)) {
				ERR("EnumDisplaySettings failed for %ws (%d)\n", sourceName.viewGdiDeviceName, GetLastError());
				ret = INTELVIRTDISPLAYENABLER_FAILURE;
				break;
			}

			// Already running the requested mode, avoid a redundant mode set. The request
			// is still acknowledged, the requested state is what is on screen.
			if ((devmode.dmPelsWidth == width) && (devmode.dmPelsHeight == height)) {
				applied_resolutions.push_back({cur_screen, pdinfo->generation[cur_screen]});
				applied_sources.push_back(sourceName.viewGdiDeviceName);
				break;
			}

			devmode.dmPelsWidth = width;
			devmode.dmPelsHeight = height;
			devmode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
			if (refresh_rate != 0) {
				devmode.dmDisplayFrequency = refresh_rate;
				devmode.dmFields |= DM_DISPLAYFREQUENCY;
			}

			LONG status = ChangeDisplaySettingsEx(sourceName.viewGdiDeviceName, &devmode, NULL,
												  CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
			if (status != DISP_CHANGE_SUCCESSFUL) {
				ERR("ChangeDisplaySettingsEx failed for screen %d (%ws) with %d\n", cur_screen,
					sourceName.viewGdiDeviceName, status);
				ret = INTELVIRTDISPLAYENABLER_FAILURE;
				break;
			}

			DBGPRINT("Applied resolution %dx%d on screen %d (%ws) adapter=%08X:%08X target=%d generation=%I64u\n",
					 width, height, cur_screen, sourceName.viewGdiDeviceName,
					 (unsigned int)pdinfo->adapter_id[cur_screen].HighPart,
					 (unsigned int)pdinfo->adapter_id[cur_screen].LowPart, pdinfo->target_id[cur_screen],
					 pdinfo->generation[cur_screen]);
			applied_resolutions.push_back({cur_screen, pdinfo->generation[cur_screen]});
			applied_sources.push_back(sourceName.viewGdiDeviceName);
			mode_changed = true;
			break;
		}

		if (!matched) {
			// The display is not in the active path list, so the request cannot be applied
			// yet. Leave it pending instead of guessing at another path.
			ERR("No active display path matched screen %d adapter=%08X:%08X target=%d\n", cur_screen,
				(unsigned int)pdinfo->adapter_id[cur_screen].HighPart,
				(unsigned int)pdinfo->adapter_id[cur_screen].LowPart, pdinfo->target_id[cur_screen]);
			ret = INTELVIRTDISPLAYENABLER_FAILURE;
		}
	}

	// Commit all the cached changes in one go.
	if (mode_changed) {
		if (ChangeDisplaySettingsEx(NULL, NULL, NULL, 0, NULL) != DISP_CHANGE_SUCCESSFUL) {
			ERR("ChangeDisplaySettingsEx commit failed\n");
			// The commit failed, so nothing was applied. Leave every request pending so
			// it is retried on the next event instead of being silently dropped.
			applied_resolutions.clear();
			ret = INTELVIRTDISPLAYENABLER_FAILURE;
		}
	}

	if (!applied_resolutions.empty()) {
		ClearPendingResolutions(applied_resolutions);
	}

	return ret;
}

/*******************************************************************************
 *
 * Description
 *
 * ClearPendingResolutions - Acknowledges the resize requests that have been
 * applied by clearing their pending flag in the shared memory section. The
 * generation is re-checked under the mutex, so a request that the UMD replaced
 * while the mode set was in flight is left pending for the next pass instead of
 * being acknowledged by mistake.
 *
 * Parameters
 * applied_resolutions - connector index and generation of each applied request
 *
 * Return val
 * int - 0 == SUCCESS, -1 = ERROR
 *
 ******************************************************************************/
int ClearPendingResolutions(const std::vector<disp_resolution_ack> &applied_resolutions)
{
	HANDLE hSharedMem = OpenFileMapping(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, DISP_INFO);
	if (hSharedMem == NULL) {
		ERR("Failed to open shared memory section for write (%d)\n", GetLastError());
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	struct disp_info *pSharedMem =
		(struct disp_info *)MapViewOfFile(hSharedMem, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
	if (pSharedMem == NULL) {
		ERR("Failed to map view of shared memory section for write (%d)\n", GetLastError());
		CloseHandle(hSharedMem);
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	HANDLE hDispMutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, DISP_INFO_MUTEX);
	if (hDispMutex == NULL) {
		ERR("Failed to open named mutex for shared memory (%d)\n", GetLastError());
		UnmapViewOfFile(pSharedMem);
		CloseHandle(hSharedMem);
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	WaitForSingleObject(hDispMutex, INFINITE);
	for (const disp_resolution_ack &ack : applied_resolutions) {
		if (ack.connector_index >= MAX_SCAN_OUT) {
			continue;
		}

		// The UMD published a newer request while this one was being applied, so the
		// applied mode is already stale. Leave it pending to be retried.
		if (pSharedMem->generation[ack.connector_index] != ack.generation) {
			DBGPRINT("Screen %d was superseded while applying (applied generation %I64u, "
					 "current %I64u), leaving it pending\n",
					 ack.connector_index, ack.generation, pSharedMem->generation[ack.connector_index]);
			continue;
		}

		pSharedMem->pending[ack.connector_index] = 0;
		pSharedMem->width[ack.connector_index] = 0;
		pSharedMem->height[ack.connector_index] = 0;
		pSharedMem->refresh_rate[ack.connector_index] = 0;
	}
	ReleaseMutex(hDispMutex);

	CloseHandle(hDispMutex);
	UnmapViewOfFile(pSharedMem);
	CloseHandle(hSharedMem);

	return INTELVIRTDISPLAYENABLER_SUCCESS;
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
	FILE *fp;
	char buffer[128];
	int status = TRUE;

	// Run the PowerShell command to get the system lock status
	fp =
		_popen("powershell.exe -WindowStyle Hidden -Command \"(quser 2>$null) -and (get-process logonui -ea 0)\"", "r");
	if (fp == NULL) {
		ERR("Failed to run PowerShell command.\n");
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	// Read the output of the PowerShell command
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		// Check if the output is "true" (indicating locked) and act accordingly
		if (strstr(buffer, "True") != NULL) {
			DBGPRINT("System is locked\n");
			status = TRUE;
		} else if (strstr(buffer, "False") != NULL) {
			DBGPRINT("System is unlocked\n");
			status = FALSE;
		} else {
			ERR("Unexpected output\n");
			status = INTELVIRTDISPLAYENABLER_FAILURE;
		}
	}

	// Close the pipe and print any errors
	if (_pclose(fp) != 0) {
		ERR("Error occurred while running PowerShell command.\n");
		return INTELVIRTDISPLAYENABLER_FAILURE;
	}

	return status;
}