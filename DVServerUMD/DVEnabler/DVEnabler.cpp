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

bool operator==(const LUID &a, const LUID &b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

int dvenabler_init()
{
	WPP_INIT_TRACING(NULL);
	TRACING();
	DBGPRINT("DVenabler init dve_event\n");
	DISPLAYCONFIG_TARGET_BASE_TYPE baseType;
	HANDLE hp_event = NULL;
	HANDLE dve_event = NULL;
	char err[256];
	memset(err, 0, 256);
	int status;
	unsigned int path_count = NULL, mode_count = NULL;
	bool found_id_path = FALSE, found_non_id_path = FALSE;
	bool set_disp = FALSE;
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
		return DVENABLER_FAILURE;
	}

	// Create Security Descriptor for DVE_EVENT, To allow the DVServerUMD to access the event
	PSECURITY_DESCRIPTOR dve_psd = (PSECURITY_DESCRIPTOR)LocalAlloc(LPTR, SECURITY_DESCRIPTOR_MIN_LENGTH);
	InitializeSecurityDescriptor(dve_psd, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(dve_psd, TRUE, NULL, FALSE);

	SECURITY_ATTRIBUTES dve_sa = {0};
	dve_sa.nLength = sizeof(dve_sa);
	dve_sa.lpSecurityDescriptor = dve_psd;
	dve_sa.bInheritHandle = FALSE;

	dve_event = CreateEvent(&dve_sa, FALSE, FALSE, DVE_EVENT);
	if (NULL == dve_event) {
		ERR("Cannot create DVE event!\n");
		CloseHandle(hp_event);
		return DVENABLER_FAILURE;
	}

	while (1) {
		if (IsSystemLocked()) {
			DBGPRINT("System is in locked state, so wait untill system gets unlocked");
			continue;
		}
		Sleep(2000);
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
		std::vector<LUID> idds(path_count);
		// Get the Display info shared from DVServerUMD
		if (GetDisplayCount(&dinfo) == DVENABLER_FAILURE) {
			ERR("shared mem read failed");
			goto end;
		}

		/* Step 1: Retrieve information about all possible display paths for all display devices */
		if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, path_list.data(), &mode_count, mode_list.data(),
							   nullptr) != ERROR_SUCCESS) {
			FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
						   err, 255, NULL);
			ERR("QueryDisplayConfig failed with %s. Exiting!!!\n", err);
			continue;
		}
		DBGPRINT("QueryDisplayConfig  mode_list.size: %Iu", mode_list.size());



		
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
			if (baseType.baseOutputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED) {
				
				idds.push_back(baseType.header.adapterId);
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

			// force preferred resolution
			// if (baseType.baseOutputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED) {
			// 	DISPLAYCONFIG_TARGET_PREFERRED_MODE preferredMode = {};
			// 	preferredMode.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_PREFERRED_MODE;
			// 	preferredMode.header.size = sizeof(DISPLAYCONFIG_TARGET_PREFERRED_MODE);
			// 	preferredMode.header.adapterId = activepath_loopindex.sourceInfo.adapterId;
			// 	preferredMode.header.id = activepath_loopindex.targetInfo.id;
			// 	if (DisplayConfigGetDeviceInfo(&preferredMode.header) == ERROR_SUCCESS) {
			// 		int native_width = preferredMode.width;
			// 		int native_height = preferredMode.height;

			// 		DISPLAYCONFIG_MODE_INFO &mode = mode_list[activepath_loopindex.sourceInfo.modeInfoIdx];
			// 		mode.targetMode.targetVideoSignalInfo.totalSize.cx = native_width;
			// 		mode.targetMode.targetVideoSignalInfo.totalSize.cy = native_height;
			// 		// Ensure the source mode matches the target mode
			// 		mode.sourceMode.width = native_width;
			// 		mode.sourceMode.height = native_height;
			// 		// Crucial: Set the scaling to fit
			// 		activepath_loopindex.targetInfo.scaling = DISPLAYCONFIG_SCALING_ASPECTRATIOCENTEREDMAX;

			// 	}

			// }
		}
		for (auto &mode : mode_list) {

			// TOOD CHECK CORRECT ADAPTER

			if (!(std::find(idds.begin(), idds.end(), mode.adapterId) != idds.end())) {
				break;
			}

			int monIndex = 0; // mode.id;
			if (monIndex > 4) {
				DBGPRINT("Error: monIndex==%d", monIndex);
			} else {
				
				UINT32 newWidth = dinfo.disp_target_res[monIndex].cx;
				UINT32 newHeight = dinfo.disp_target_res[monIndex].cy;
				DWORD newRefresh = dinfo.disp_target_res[monIndex].refresh;
				DBGPRINT("Displ: %d, infoType: %d newWidth: %u, newHeight: %u, newRefresh: %d", monIndex, mode.infoType,
						 newWidth, newHeight, newRefresh);
				if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {

					if (mode.sourceMode.width != newWidth || mode.sourceMode.height != newHeight) {
						mode.sourceMode.width = newWidth;
						mode.sourceMode.height = newHeight;
						set_disp = TRUE;
					}
				}
				if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {

					FillSignalInfo(mode.targetMode.targetVideoSignalInfo, newWidth, newHeight, newRefresh);
				}

				// set_disp = TRUE;
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

		if (set_disp || (found_non_id_path && found_id_path)) {
			/* Step 5: SetDisplayConfig modifies the display topology by exclusively enabling/disabling the specified
					   paths in the current session. */
			set_disp = FALSE;
			if (SetDisplayConfig(path_count, path_list.data(), mode_count, mode_list.data(),
								 SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG) != ERROR_SUCCESS) {
				FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(),
							   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), err, 255, NULL);
				ERR("SetDisplayConfig failed with %s\n", err);
				//continue;
			}
		} else {
			DBGPRINT("Skipping SetDisplayConfig as did not find ID and non-ID path. found_non_id_path = %d, "
					 "found_id_path = %d\n",
					 found_non_id_path, found_id_path);
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
		if (status == NULL) {
			ERR(" Set HPevent failed with error [%d]\n ", GetLastError());
			continue;
		}

	end:
		// wait for arraival or departure call from UMD
		WaitForSingleObject(dve_event, INFINITE);
	}
	WPP_CLEANUP();
	CloseHandle(hp_event);
	CloseHandle(dve_event);

	return 0;
}



static inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& Mode, DWORD Width, DWORD Height, DWORD VSync)
{
    Mode.totalSize.cx = Mode.activeSize.cx = Width;
    Mode.totalSize.cy = Mode.activeSize.cy = Height;

    Mode.AdditionalSignalInfo.vSyncFreqDivider = 1;
    Mode.AdditionalSignalInfo.videoStandard = 255; // Custom standard

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
	HANDLE hSharedMem = OpenFileMapping(FILE_MAP_READ, FALSE, DISP_INFO);

	if (hSharedMem == NULL) {
		ERR("Failed to open shared memory section (%d)\n", GetLastError());
		return DVENABLER_FAILURE;
	}

	// Map the shared memory into the process's address space
	struct disp_info *pSharedMem =
		(struct disp_info *)MapViewOfFile(hSharedMem,	 // Handle to the shared memory section
										  FILE_MAP_READ, // Read access
										  0,			 // File offset - high-order DWORD
										  0,			 // File offset - low-order DWORD
										  0);			 // Mapping size (0 means to map the entire section)

	if (pSharedMem == NULL) {
		ERR(L"Failed to map view of shared memory section (%d)\n", GetLastError());
		CloseHandle(hSharedMem);
		return DVENABLER_FAILURE;
	}

	WaitForSingleObject(pSharedMem->mutex, INFINITE);
	*pdinfo = *pSharedMem;
	ReleaseMutex(pSharedMem->mutex);

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
	FILE *fp;
	char buffer[128];
	int status = TRUE;

	// Run the PowerShell command to get the system lock status
	fp =
		_popen("powershell.exe -WindowStyle Hidden -Command \"(quser 2>$null) -and (get-process logonui -ea 0)\"", "r");
	if (fp == NULL) {
		ERR("Failed to run PowerShell command.\n");
		return DVENABLER_FAILURE;
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
			status = DVENABLER_FAILURE;
		}
	}

	// Close the pipe and print any errors
	if (_pclose(fp) != 0) {
		ERR("Error occurred while running PowerShell command.\n");
		return DVENABLER_FAILURE;
	}

	return status;
}