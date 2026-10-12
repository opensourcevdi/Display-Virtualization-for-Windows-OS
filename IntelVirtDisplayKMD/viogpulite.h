/*
 * Copyright (C) 2021 Intel Corporation
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

#include "edid.h"
#include "viogpu.h"
#include "helper.h"

extern "C" {
#include "..\EDIDParser\edidshared.h"
}
#pragma pack(push)
#pragma pack(1)

typedef struct
{
	UINT DriverStarted : 1;
	UINT HardwareInit : 1;
	UINT PointerEnabled : 1;
	UINT VgaDevice : 1;
	UINT Unused : 28;
} DEVICE_STATUS_FLAG;

#pragma pack(pop)

typedef struct _CURRENT_MODE
{
	DXGK_DISPLAY_INFORMATION DispInfo;
	D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation;
	D3DKMDT_VIDPN_PRESENT_PATH_SCALING Scaling;
	UINT SrcModeWidth;
	UINT SrcModeHeight;
	UINT Stride;
	struct _CURRENT_MODE_FLAGS
	{
		UINT SourceNotVisible : 1;
		UINT FullscreenPresent : 1;
		UINT FrameBufferIsActive : 1;
		UINT DoNotMapOrUnmap : 1;
		UINT IsInternal : 1;
		UINT Unused : 27;
	} Flags;

	// Set when this CURRENT_MODE originates from an IOCTL_INTELVIRTDISPLAY_SET_MODE
	// rather than a steady-state frame present. A SET_MODE means the UMD has
	// rebuilt its staging buffer (including after a swapchain restart), so any
	// outstanding flush back-pressure from the previous swapchain is stale.
	BOOLEAN SetMode;

	// FrameBuffer.Ptr maps device local memory of the render GPU (the VF memory
	// BAR) instead of guest RAM. See FRAME_MEM_DEVICE.
	BOOLEAN DeviceMemory;

	PHYSICAL_ADDRESS ZeroedOutStart;
	PHYSICAL_ADDRESS ZeroedOutEnd;

	union
	{
		VOID *Ptr;
		ULONG64 Force8Bytes;
	} FrameBuffer;
} CURRENT_MODE;

typedef struct _POINTER_SHAPE
{
	UINT X;
	UINT Y;
	DXGKARG_SETPOINTERSHAPE pointer;
} POINTER_SHAPE;

// Number of consecutive frames that may be dropped due to an outstanding flush
// before the KMD assumes the host completion was lost and force-recovers. At
// ~60fps this is roughly a second of frozen output, long enough to never trip
// during normal back-pressure but short enough to be imperceptible on recovery.
#define FLUSH_STALL_LIMIT 60

class ScreenInfo
{
public:
	// Scanout resources per screen. The UMD's device memory path rotates
	// through three frame buffers (one on screen, one being presented, one
	// being written); each gets its own persistent resource here, so
	// switching between them is a SET_SCANOUT_BLOB, not a new host import.
	// The system memory staging path only ever uses one.
	static constexpr size_t FRAMEBUFFER_COUNT = 3;
	PVIDEO_MODE_INFORMATION m_ModeInfo;
	ULONG m_ModeCount;
	PUSHORT m_ModeNumbers;
	USHORT m_CurrentMode;
	USHORT m_CustomMode;
	BYTE m_EDIDs[EDID_V1_BLOCK_SIZE];
	GPU_DISP_MODE_EXT gpu_disp_mode_ext[MAX_MODELIST_SIZE];
	output_modelist mode_list;
	KEVENT m_DisplayInfoEvent;
	KEVENT m_EdidEvent;
	KEVENT m_FlushEvent;
	// Completion of a synchronous SET_SCANOUT_BLOB, see CreateFrameBufferObj().
	KEVENT m_ScanoutEvent;
	// Backing of slot 0 (also the kernel's own frame buffer at init, see
	// HWInit); slots 1.. use m_ExtraSegments. Use Segment(slot).
	VioGpuMemSegment m_FrameSegment;
	VioGpuMemSegment m_ExtraSegments[FRAMEBUFFER_COUNT - 1];
	// important must be alligned to because of InterlockedExchangePointer usage
	VioGpuObj *m_pFrameBuf[FRAMEBUFFER_COUNT];
	// What each slot's persistent resource was built from: the guest backing
	// pointer, dimensions, stride and memory type. A frame whose identity
	// matches a slot reuses its resource; anything else replaces a slot.
	struct FbIdentity
	{
		PVOID addr;
		UINT width;
		UINT height;
		UINT stride;
		BOOLEAN deviceMem;
		// Order of last use, to replace the least recently used slot.
		ULONG lastUse;
	} m_Fb[FRAMEBUFFER_COUNT];
	// The slot currently set as the scanout (-1: none).
	INT m_ScanoutSlot;
	ULONG m_UseCounter;
	VioGpuObj *m_pCursorBuf;
	VioGpuMemSegment m_CursorSegment;
	BOOL m_FlushCount;
	// Number of consecutive frames dropped because m_FlushCount stayed non-zero.
	// Guards against a lost/dropped flush completion wedging the screen forever.
	UINT m_FlushStallCount;
	BOOL enabled;
	KMUTEX m_segmentMutex;

public:
	ScreenInfo();
	~ScreenInfo();
	PVIDEO_MODE_INFORMATION GetModeInfo(UINT idx) { return &m_ModeInfo[idx]; }
	ULONG GetModeCount(void) { return m_ModeCount; }
	USHORT GetModeNumber(USHORT idx) { return m_ModeNumbers[idx]; }
	USHORT GetCurrentModeIndex(void) { return m_CurrentMode; }
	VioGpuMemSegment &Segment(UINT slot) { return slot == 0 ? m_FrameSegment : m_ExtraSegments[slot - 1]; }
	// The resource on screen, NULL if none.
	VioGpuObj *GetScanoutObj() { return m_ScanoutSlot >= 0 ? m_pFrameBuf[m_ScanoutSlot] : NULL; }
	// Slot whose resource was built for this frame buffer, -1 if none.
	INT FindSlot(PVOID addr, UINT width, UINT height, UINT stride, BOOLEAN deviceMem);
	// Slot to build a new resource in: an empty one, else the least recently
	// used one that isn't on screen.
	UINT PickSlot();
	void ForgetSlot(UINT slot);
	void SetCurrentModeIndex(USHORT idx) { m_CurrentMode = idx; }
	void SetCustomDisplay(_In_ USHORT xres, _In_ USHORT yres);
	void SetVideoModeInfo(UINT Idx, PGPU_DISP_MODE_EXT pModeInfo);
	void Reset();
};

class IVioGpuAdapterLite
{
public:
	IVioGpuAdapterLite(_In_ PVOID pvDevcieContext)
	{
		m_pvDeviceContext = pvDevcieContext;
		m_bEDID = FALSE;
		m_Id = 0;
		RtlZeroMemory(&m_screen_mutex, sizeof(m_screen_mutex));
	}
	virtual ~IVioGpuAdapterLite(void) { ; }
	virtual NTSTATUS SetPowerState(DEVICE_POWER_STATE DevicePowerState) = 0;
	virtual NTSTATUS HWInit(WDFCMRESLIST pResList, DXGK_DISPLAY_INFORMATION *pDispInfo) = 0;
	virtual NTSTATUS HWClose(void) = 0;
	virtual BOOLEAN InterruptRoutine(_In_ ULONG MessageNumber) = 0;
	virtual VOID DpcRoutine(void) = 0;
	virtual VOID ResetDevice(void) = 0;
	virtual VOID BlackOutScreen(CURRENT_MODE *pCurrentMod) = 0;
	virtual NTSTATUS SetPointerShape(_In_ CONST POINTER_SHAPE *pSetPointerShape, _In_ CONST UINT cf,
									 _In_ CONST UINT cv) = 0;
	virtual NTSTATUS SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition) = 0;
	ULONG GetInstanceId(void) { return m_Id; }
	PVOID GetVioGpu(void) { return m_pvDeviceContext; }
	virtual PBYTE GetEdidData(UINT Idx) = 0;
	virtual PHYSICAL_ADDRESS GetFrameBufferPA(void) = 0;
	DXGK_DISPLAY_INFORMATION DisplayInfo = {1024, 768, 4096, D3DDDIFMT_X8R8G8B8, 0, 0, 0};

protected:
	virtual NTSTATUS GetModeList(DXGK_DISPLAY_INFORMATION *pDispInfo) = 0;

protected:
	ULONG m_Id;
	ScreenInfo m_screen[MAX_SCAN_OUT];
	BOOLEAN m_bEDID;
	KMUTEX m_screen_mutex;

public:
	PVOID m_pvDeviceContext;
};

class VioGpuAdapterLite : public IVioGpuAdapterLite
{
public:
	VioGpuAdapterLite(_In_ PVOID pvDeviceContext);
	~VioGpuAdapterLite(void);
	NTSTATUS SetCurrentModeExt(CURRENT_MODE *pCurrentMode);
	// Tear down the persistent scanout resource for a screen. Called when the
	// UMD is about to unmap the staging buffer those guest pages belong to.
	NTSTATUS ReleaseFrameBuffer(UINT32 screen_num);
	NTSTATUS SetPowerState(DEVICE_POWER_STATE DevicePowerState);
	NTSTATUS HWInit(WDFCMRESLIST pResList, DXGK_DISPLAY_INFORMATION *pDispInfo);
	NTSTATUS HWClose(void);
	NTSTATUS ExecutePresentDisplayZeroCopy(_In_ BYTE *SrcAddr, _In_ UINT SrcBytesPerPixel, _In_ LONG SrcPitch,
										   _In_ UINT SrcWidth, _In_ UINT SrcHeight, _In_ UINT ScreenNum,
										   _In_ UINT Stride, _In_ BOOLEAN DeviceMemory);
	VOID BlackOutScreen(CURRENT_MODE *pCurrentMod);
	BOOLEAN InterruptRoutine(_In_ ULONG MessageNumber);
	VOID DpcRoutine(void);
	VOID ResetDevice(VOID);
	NTSTATUS SetPointerShape(_In_ CONST POINTER_SHAPE *pSetPointerShape, _In_ CONST UINT cf, _In_ CONST UINT cv);
	NTSTATUS SetPointerPosition(_In_ CONST DXGKARG_SETPOINTERPOSITION *pSetPointerPosition);
	CPciResources *GetPciResources(void) { return &m_PciResources; }
	BOOLEAN IsMSIEnabled() { return m_PciResources.IsMSIEnabled(); }
	PHYSICAL_ADDRESS GetFrameBufferPA(void) { return m_PciResources.GetPciBar(0)->GetPA(); }
	NTSTATUS IoDeviceControl();
	BOOLEAN IsHardwareInit() const { return m_Flags.HardwareInit; }
	UINT32 GetNumScreens() { return m_u32NumScanouts; }
	UINT32 GetModeListSize(UINT32 screen_num) { return m_screen[screen_num].mode_list.modelist_size; }
	VOID CopyResolution(UINT32 screen_num, struct edid_info *edata);
	// Backing of the resource on screen (slot 0 if none yet).
	PVOID GetFbVAddr(UINT32 screen_num)
	{
		INT slot = m_screen[screen_num].m_ScanoutSlot;
		return m_screen[screen_num].Segment(slot >= 0 ? (UINT)slot : 0).GetFbVAddr();
	}
	PBYTE GetEdidData(UINT Idx);
	VOID FillPresentStatus(struct hp_info *info);
	VOID SetEvent(HANDLE event);
	void DestroyFrameBufferCursorObjExt();
	void DisableInterruptExt();

private:
	void SetHardwareInit(BOOLEAN init) { m_Flags.HardwareInit = init; }
	NTSTATUS VioGpuAdapterLiteInit();
	void VioGpuAdapterLiteClose(void);
	NTSTATUS GetModeList(DXGK_DISPLAY_INFORMATION *pDispInfo);
	BOOLEAN AckFeature(UINT64 Feature);
	BOOLEAN GetDisplayInfo(UINT32 screen_num);
	void ProcessEdid(UINT32 screen_num);
	BOOLEAN GetEdids(UINT32 screen_num);
	void AddEdidModes(UINT32 screen_num);
	void CreateFrameBufferObj(PVIDEO_MODE_INFORMATION pModeInfo, UINT slot, CURRENT_MODE *pCurrentMode);
	void FlushFrameBufferObj(PVIDEO_MODE_INFORMATION pModeInfo, UINT slot, CURRENT_MODE *pCurrentMode);
	void DestroyFrameBufferSlotObj(UINT32 screen_num, UINT slot, BOOLEAN bReset);
	void DestroyFrameBufferObj(VioGpuObj **ppFbuf, BOOLEAN bReset);
	BOOLEAN CreateCursor(_In_ CONST POINTER_SHAPE *pSetPointerShape, _In_ CONST UINT cf);
	void DestroyCursor(UINT32 screen_num);
	BOOLEAN GpuObjectAttach(UINT res_id, VioGpuObj *obj, ULONGLONG width, ULONGLONG height, ULONGLONG stride);
	void static ThreadWork(_In_ PVOID Context);
	void ThreadWorkRoutine(void);
	void ConfigChanged(void);
	NTSTATUS VirtIoDeviceInit(void);
	DEVICE_STATUS_FLAG m_Flags;
	VirtIODevice m_VioDev;
	CPciResources m_PciResources;
	UINT64 m_u64HostFeatures;
	UINT64 m_u64GuestFeatures;
	UINT32 m_u32NumCapsets;
	UINT32 m_u32NumScanouts;
	CtrlQueue m_CtrlQueue;
	CrsrQueue m_CursorQueue;
	VioGpuBuf m_GpuBuf;
	VioGpuIdr m_Idr;
	volatile ULONG m_PendingWorks;
	KEVENT m_ConfigUpdateEvent;
	PETHREAD m_pWorkThread;
	BOOLEAN m_bStopWorkThread;
	CURRENT_MODE m_CurrentModeInfo;
	BOOLEAN m_bBlobSupported;
	PKEVENT hpd_event;
};
