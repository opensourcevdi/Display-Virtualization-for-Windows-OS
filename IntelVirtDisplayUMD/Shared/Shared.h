/*===========================================================================
; shared.h
;----------------------------------------------------------------------------
; Shared display-info struct and monitor container ID logic used by both
; IntelVirtDisplayUMD (Driver.cpp) and IntelVirtDisplayEnabler. Keeping this
; in one header guarantees the two binaries agree on the shared-memory
; layout and on how container IDs are derived.
;--------------------------------------------------------------------------*/

#ifndef __SHARED_H__
#define __SHARED_H__

#include <windows.h>
#include "..\..\IntelVirtDisplayKMD\Public.h"
struct disp_target_res
{
	UINT32 cx;
	UINT32 cy;
	DWORD refresh;
	uint8_t enabled;
	uint8_t set;
};

struct disp_info
{
	int disp_count;
	disp_target_res disp_target_res[MAX_SCAN_OUT];
};

inline GUID GetStableMonitorContainerId(UINT ConnectorIndex)
{
	static const GUID Namespace =
	{ 0x7d51b9b0, 0x5eb5, 0x40ab, { 0xa5, 0x99, 0xff, 0x1c, 0x89, 0x77, 0x28, 0xb0 } };

	GUID Id = Namespace;
	Id.Data1 ^= ConnectorIndex;   // fold in the connector index deterministically
	return Id;
}

#endif //__SHARED_H__