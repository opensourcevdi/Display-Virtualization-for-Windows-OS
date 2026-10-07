// pch.h: This is a precompiled header file.
// Files listed below are compiled only once, improving build performance for future builds.
// This also affects IntelliSense performance, including code completion and many code browsing features.
// However, files listed here are ALL re-compiled if any one of them is updated between builds.
// Do not add files here that you will be updating frequently as this negates the performance advantage.

#ifndef PCH_H
#define PCH_H

// add headers that you want to pre-compile here
#include "framework.h"
#include <vector>
#include <string>

/* INTELVIRTDISPLAYENABLER Error Codes */
#define INTELVIRTDISPLAYENABLER_SUCCESS 0
#define INTELVIRTDISPLAYENABLER_FAILURE -1

#define HOTPLUG_EVENT L"Global\\HOTPLUG_EVENT"
#define DVE_EVENT L"Global\\DVE_EVENT"
#define DISP_INFO L"Global\\DISP_INFO"
#define DISP_INFO_MUTEX L"Global\\DISP_INFO_MUTEX"
#define DELAY_TIME 50
// Must match MAX_SCAN_OUT in IntelVirtDisplayKMD\Public.h, the disp_info shared
// memory layout is shared with the UMD driver.
#define MAX_SCAN_OUT 4
int intelvirtdisplayenabler_init();
struct disp_info
{
	int disp_count;
	// Per-screen resolution published by the UMD on a resize so the enabler can
	// apply the new mode. A zero width/height means there is nothing to apply.
	unsigned int width[MAX_SCAN_OUT];
	unsigned int height[MAX_SCAN_OUT];
	unsigned int refresh_rate[MAX_SCAN_OUT];
	// Which screens are currently connected. The enabler walks the active IDD
	// paths in order, so it needs this to map the n'th path back to the screen
	// index used above when the connected screens are not contiguous.
	unsigned int screen_present[MAX_SCAN_OUT];
	// Set by the UMD when a new resolution is published and cleared by the enabler
	// only after it has been applied, so a failed or missed apply is retried on the
	// next event instead of being dropped.
	unsigned int pending[MAX_SCAN_OUT];
	// Windows display target identity reported by IddCxMonitorArrival, this is the
	// display adapter LUID and not the render adapter LUID. It is the only reliable
	// key to map a connector onto its DISPLAYCONFIG_PATH_INFO, path enumeration
	// order is not guaranteed to follow the connector order.
	LUID adapter_id[MAX_SCAN_OUT];
	unsigned int target_id[MAX_SCAN_OUT];
	unsigned int identity_valid[MAX_SCAN_OUT];
	// Bumped by the UMD every time a request is published or invalidated. The
	// enabler acknowledges only the generation it actually applied, so a request
	// that was replaced while a mode set was in flight is not falsely cleared.
	unsigned long long generation[MAX_SCAN_OUT];
};

// One acknowledgement for a resize request the enabler actually applied.
struct disp_resolution_ack
{
	unsigned int connector_index;
	unsigned long long generation;
};
int GetDisplayCount(disp_info *pdinfo);
int ApplyResolution(const disp_info *pdinfo);
int ClearPendingResolutions(const std::vector<disp_resolution_ack> &applied_resolutions);
int IsSystemLocked();
#endif // PCH_H
