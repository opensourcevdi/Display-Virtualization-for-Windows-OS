/*===========================================================================
; DVServeredid.cpp
;----------------------------------------------------------------------------
; Copyright (C) 2021 Intel Corporation
; SPDX-License-Identifier: MS-PL
;
; File Description:
;   This file sends edid ioctl request to kmd & trim the unsupported modelist
;--------------------------------------------------------------------------*/

#include "DVServeredid.h"
#include <DVServeredid.tmh>

using namespace Microsoft::IndirectDisp;
PSP_DEVICE_INTERFACE_DETAIL_DATA device_iface_edid_data;
struct edid_info *edata = NULL;
struct screen_info *mdata = NULL;
ULONG bytesReturned = 0;

unsigned int blacklisted_resolution_list[][2] = {{1400, 1050}}; // blacklisted resolution can be appended here

/*******************************************************************************
 *
 * Description
 *
 * get_edid_data - This function gets the handle of DVserverKMD edid device node
 * and send an ioctl(IOCTL_DVSERVER_GET_EDID_DATA)to DVserverKMD to get edid data
 *
 * Parameters
 * Device frame Handle to DVServerKMD
 * pointer to IndirectSampleMonitor structure
 * Screen ID
 *
 * Return val
 * int - 0 == SUCCESS, -1 = ERROR
 *
 ******************************************************************************/
int get_edid_data(HANDLE devHandle, void *m, DWORD id, BOOL d_edid)
{
	TRACING();
	char err[256];
	memset(err, 0, 256);

	IndirectSampleMonitor *monitor = (IndirectSampleMonitor *)m;
	unsigned int i = 0, edid_mode_index = 0;

	if (!devHandle || !m) {
		ERR("Invalid parameter\n");
		return DVSERVERUMD_FAILURE;
	}

	static const struct IndirectSampleMonitor s_SampleMonitors[] = {
		// Modified EDID from Dell S2719DGF
		{{0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x10, 0xAC, 0xE6, 0xD0, 0x55, 0x5A, 0x4A, 0x30,
		  0x24, 0x1D, 0x01, 0x04, 0xA5, 0x3C, 0x22, 0x78, 0xFB, 0x6C, 0xE5, 0xA5, 0x55, 0x50, 0xA0, 0x23,
		  0x0B, 0x50, 0x54, 0x00, 0x02, 0x00, 0xD1, 0xC0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x58, 0xE3, 0x00, 0xA0, 0xA0, 0xA0, 0x29, 0x50, 0x30, 0x20,
		  0x35, 0x00, 0x55, 0x50, 0x21, 0x00, 0x00, 0x1A, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x37, 0x4A, 0x51,
		  0x58, 0x42, 0x59, 0x32, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFC, 0x00, 0x53,
		  0x32, 0x37, 0x31, 0x39, 0x44, 0x47, 0x46, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFD,
		  0x00, 0x28, 0x9B, 0xFA, 0xFA, 0x40, 0x01, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x2C},
		 {
			 {2560, 1440, 144},
			 {1920, 1080, 60},
			 {1024, 768, 60},
		 },
		 0},
		// Modified EDID from Lenovo Y27fA
		{{0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x30, 0xAE, 0xBF, 0x65, 0x01, 0x01, 0x01, 0x01,
		  0x20, 0x1A, 0x01, 0x04, 0xA5, 0x3C, 0x22, 0x78, 0x3B, 0xEE, 0xD1, 0xA5, 0x55, 0x48, 0x9B, 0x26,
		  0x12, 0x50, 0x54, 0x00, 0x08, 0x00, 0xA9, 0xC0, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
		  0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x68, 0xD8, 0x00, 0x18, 0xF1, 0x70, 0x2D, 0x80, 0x58, 0x2C,
		  0x45, 0x00, 0x53, 0x50, 0x21, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00,
		  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFD, 0x00, 0x30,
		  0x92, 0xB4, 0xB4, 0x22, 0x01, 0x0A, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00, 0x00, 0xFC,
		  0x00, 0x4C, 0x45, 0x4E, 0x20, 0x59, 0x32, 0x37, 0x66, 0x41, 0x0A, 0x20, 0x20, 0x20, 0x00, 0x11},
		 {
			 {3840, 2160, 60},
			 {1600, 900, 60},
			 {1024, 768, 60},
		 },
		 0}
		// Another EDID
		// https://github.com/roshkins/IddSampleDriver/blob/df7238c1f242e1093cdcab0ea749f34094570283/IddSampleDriver/Driver.cpp#L419
	};

	if (d_edid == TRUE) {
		DBGPRINT("get Default EDID for Primary Index monitor \n");
		memcpy_s(monitor, sizeof(s_SampleMonitors), s_SampleMonitors, sizeof(s_SampleMonitors));
		return DVSERVERUMD_SUCCESS;
	}

	edata = (struct edid_info *)malloc(sizeof(struct edid_info));
	if (edata == NULL) {
		ERR("Failed to allocate edid structure\n");
		return DVSERVERUMD_FAILURE;
	}
	SecureZeroMemory(edata, sizeof(struct edid_info));
	edata->screen_num = id;

	DBGPRINT("Requesting EDID info through EDID IOCTL for screen = %d\n", edata->screen_num);
	if (!DeviceIoControl(devHandle, IOCTL_DVSERVER_GET_EDID_DATA, edata, sizeof(struct edid_info), edata,
						 sizeof(struct edid_info), &bytesReturned, NULL)) {
		FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), err,
					   255, NULL);
		ERR("IOCTL_DVSERVER_GET_EDID_DATA call failed with error: %s!\n", err);
		free(edata);
		return DVSERVERUMD_FAILURE;
	}
	if (edata->mode_size > MODE_LIST_MAX_SIZE) {
		ERR("Invalid id \n");
		free(edata);
		return DVSERVERUMD_FAILURE;
	}

	memcpy_s(monitor->pEdidBlock, monitor->szEdidBlock, edata->edid_data, monitor->szEdidBlock);
	monitor->ulPreferredModeIdx = 0;

	DBGPRINT("Modes\n");
	for (i = 0; i < edata->mode_size; i++) {
		// TRIMMING LOGIC: Restricting EDID size to 32 and discarding modes with width more than 3840 & less than 1024
		if ((edata->mode_list[i].width <= WIDTH_UPPER_CAP) && (edata->mode_list[i].width >= WIDTH_LOWER_CAP) &&
			(edid_mode_index < monitor->szModeList) &&
			(is_blacklist(edata->mode_list[i].width, edata->mode_list[i].height) == 0)) {
			monitor->pModeList[edid_mode_index].Width = edata->mode_list[i].width;
			monitor->pModeList[edid_mode_index].Height = edata->mode_list[i].height;
			if ((DWORD)edata->mode_list[i].refreshrate == REFRESH_RATE_59)
				monitor->pModeList[edid_mode_index].VSync = REFRESH_RATE_60;
			else
				monitor->pModeList[edid_mode_index].VSync = (DWORD)edata->mode_list[i].refreshrate;
			DBGPRINT("[%d]: %dx%d@%d\n", edid_mode_index, monitor->pModeList[edid_mode_index].Width,
					 monitor->pModeList[edid_mode_index].Height, monitor->pModeList[edid_mode_index].VSync);
			edid_mode_index++;
		}
	}
	monitor->currentModeList = edid_mode_index;
	free(edata);
	return DVSERVERUMD_SUCCESS;
}

/*******************************************************************************
 *
 * Description
 *
 * get_total_screens - This function sends an ioctl(IOCTL_DVSERVER_GET_TOTAL_SCREENS)
 * to DVserverKMD to get the total number of screens
 *
 * Parameters
 * Device frame Handle to DVServerKMD
 *
 * Return val
 * int - -1 = ERROR, any other value = number of total screens connected
 *
 ******************************************************************************/
int get_total_screens(HANDLE devHandle)
{
	TRACING();
	char err[256];
	memset(err, 0, 256);

	int ret = DVSERVERUMD_FAILURE;

	if (!devHandle) {
		ERR("Invalid devHandle\n");
		return ret;
	}

	mdata = (struct screen_info *)malloc(sizeof(struct screen_info));
	if (mdata == NULL) {
		ERR("Failed to allocate Screen structure\n");
		return ret;
	}
	SecureZeroMemory(mdata, sizeof(struct screen_info));

	DBGPRINT("Requesting Screen Count through Screen IOCTL\n");
	if (!DeviceIoControl(devHandle, IOCTL_DVSERVER_GET_TOTAL_SCREENS, mdata, sizeof(struct screen_info), mdata,
						 sizeof(struct screen_info), &bytesReturned, NULL)) {
		FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), err,
					   255, NULL);
		ERR("IOCTL_DVSERVER_GET_TOTAL_SCREENS call failed with error: %s!\n", err);
		free(mdata);
		return ret;
	}

	DBGPRINT("Total screens = %d\n", mdata->total_screens);
	ret = mdata->total_screens;
	free(mdata);
	return ret;
}

/*******************************************************************************
 *
 * Description
 *
 * is_blacklist - This function blacklists the resolution it receives from
 * DVserverKMD comparing it from the unsuported reolutions formats which get
 * recieved from Qemu.
 *
 * Parameters
 * width - resolution width
 * height - resolution height
 *
 * Return val
 * int - 0 == SUCCESS, -1 = ERROR
 *
 ******************************************************************************/
int is_blacklist(unsigned int width, unsigned int height)
{
	unsigned int i = 0;

	for (i = 0; i < ARRAY_SIZE(blacklisted_resolution_list); i++) {
		if ((width == blacklisted_resolution_list[i][0]) && (height == blacklisted_resolution_list[i][1])) {
			return DVSERVERUMD_FAILURE;
		}
	}
	return DVSERVERUMD_SUCCESS;
}
