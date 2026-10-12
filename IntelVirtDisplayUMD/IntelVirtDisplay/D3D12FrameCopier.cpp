/*===========================================================================
; D3D12FrameCopier.cpp
;----------------------------------------------------------------------------
; SPDX-License-Identifier: MS-PL
;
; File Description:
;   Copies IddCx swapchain surfaces into a D3D12 GPU upload heap buffer, see
;   D3D12FrameCopier.h.
;--------------------------------------------------------------------------*/

#include "Driver.h"
#include "Trace.h"
#include "D3D12FrameCopier.tmh"
#include "Trace_override.h"

using namespace Microsoft::IndirectDisp;
using Microsoft::WRL::ComPtr;

// Upper bound for a single surface copy. Anything longer means the GPU hung or
// was removed; the caller then restarts the swapchain.
#define COPY_TIMEOUT_MS 1000

// Committed buffers are allocated in 64KB granules; sizing the buffer to that
// keeps the page rounded range the KMD maps inside the allocation.
#define TARGET_ALIGNMENT (64 * 1024)

D3D12FrameCopier::D3D12FrameCopier()
	: m_d3d12Module(NULL), m_interopValue(0), m_copyValue(0), m_copyEvent(NULL), m_footprint{}, m_data{},
	  m_rowPitch(0), m_nextSurface(0)
{}

D3D12FrameCopier::~D3D12FrameCopier()
{
	TRACING();

	// Make sure the GPU is done with the target before it goes away.
	if (m_copyFence && m_copyEvent) {
		WaitForCopy(m_copyValue);
	}

	ReleaseTarget();

	// The COM objects must be gone before d3d12.dll may be unloaded.
	m_copyFence.Reset();
	m_interopFence.Reset();
	m_d3d11Fence.Reset();
	m_d3d11Context.Reset();
	m_list.Reset();
	m_allocator.Reset();
	m_queue.Reset();
	m_device.Reset();

	if (m_copyEvent) {
		CloseHandle(m_copyEvent);
		m_copyEvent = NULL;
	}
	if (m_d3d12Module) {
		FreeLibrary(m_d3d12Module);
		m_d3d12Module = NULL;
	}
}

// Raises the GPU scheduling priority of the whole driver process: the D3D12
// copy queue isn't covered by IddCxSetRealtimeGPUPriority (D3D11 device only),
// and global realtime queue priority is refused (DXGI_ERROR_ACCESS_DENIED), so
// with a focused full-screen load (FurMark) the copy lost (~40 frames/s, 74
// unfocused). Tries realtime, then high; logged. Once per process. Loaded from
// gdi32.dll at runtime (D3DKMTSetProcessSchedulingPriorityClass, d3dkmthk.h).
static void raise_process_gpu_priority()
{
	static LONG done = 0;
	if (InterlockedExchange(&done, 1)) {
		return;
	}
	typedef LONG(APIENTRY * SetPriorityClassFn)(HANDLE, INT);
	HMODULE gdi = LoadLibraryExW(L"gdi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	SetPriorityClassFn set =
		gdi ? reinterpret_cast<SetPriorityClassFn>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass"))
			: NULL;
	if (set == NULL) {
		ERR("D3DKMTSetProcessSchedulingPriorityClass not available\n");
		return;
	}
	// D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME = 5, _HIGH = 4 (NTSTATUS 0 = success).
	LONG status = set(GetCurrentProcess(), 5);
	if (status == 0) {
		ERR("GPU process priority: realtime\n");
		return;
	}
	LONG high = set(GetCurrentProcess(), 4);
	if (high == 0) {
		ERR("GPU process priority: realtime refused (0x%08X), high\n", status);
	} else {
		ERR("GPU process priority: realtime refused (0x%08X), high refused (0x%08X)\n", status, high);
	}
}

HRESULT D3D12FrameCopier::Init(IDXGIAdapter1 *adapter, ID3D11Device *d3d11Device, ID3D11DeviceContext *d3d11Context)
{
	TRACING();
	HRESULT hr;

	raise_process_gpu_priority();

	// Loaded at runtime so the driver still starts where D3D12 is unavailable;
	// the caller then stays on the system memory staging path.
	m_d3d12Module = LoadLibraryExW(L"d3d12.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (m_d3d12Module == NULL) {
		ERR("Failed to load d3d12.dll (%lu)\n", GetLastError());
		return HRESULT_FROM_WIN32(GetLastError());
	}

	PFN_D3D12_CREATE_DEVICE createDevice =
		reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(GetProcAddress(m_d3d12Module, "D3D12CreateDevice"));
	if (createDevice == NULL) {
		ERR("D3D12CreateDevice not found\n");
		return E_NOINTERFACE;
	}

	hr = createDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
	if (FAILED(hr)) {
		ERR("D3D12CreateDevice failed (0x%08X)\n", hr);
		return hr;
	}

	// GPU upload heaps are CPU visible device local memory. On a discrete GPU
	// this needs the whole local memory to be reachable through the BAR, which
	// is the case for an SR-IOV VF whose memory BAR covers its LMEM quota.
	D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
	hr = m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, sizeof(options16));
	if (FAILED(hr) || !options16.GPUUploadHeapSupported) {
		ERR("D3D12 GPU upload heaps are not supported (0x%08X)\n", hr);
		return E_NOTIMPL;
	}

	// A copy queue: on Intel GPUs it runs on the copy engine (blitter), not
	// the 3D engine, so the frame copy doesn't queue up behind a game or
	// benchmark keeping the 3D engine busy (FurMark: ~17 frames/s with a
	// direct queue; test). CopyTextureRegion, the fences and the implicit
	// promotion of COMMON resources to COPY_SOURCE/COPY_DEST work on copy
	// queues as well.
	const D3D12_COMMAND_LIST_TYPE listType = D3D12_COMMAND_LIST_TYPE_COPY;
	// Global realtime priority, the D3D12 counterpart of
	// IddCxSetRealtimeGPUPriority (which only takes the D3D11 device): with
	// HIGH the copy lost against a focused full-screen load (FurMark: ~40
	// frames/s, 74 with FurMark unfocused). It may need a privilege the
	// driver's process doesn't have; then fall back to HIGH. Logged either
	// way so the log shows which one the frames get.
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = listType;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_GLOBAL_REALTIME;
	hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue));
	if (SUCCEEDED(hr)) {
		ERR("D3D12 copy queue: global realtime priority\n");
	} else {
		ERR("D3D12 copy queue: global realtime priority refused (0x%08X), using high\n", hr);
		queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
		hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue));
		if (FAILED(hr)) {
			ERR("CreateCommandQueue failed (0x%08X)\n", hr);
			return hr;
		}
	}

	hr = m_device->CreateCommandAllocator(listType, IID_PPV_ARGS(&m_allocator));
	if (FAILED(hr)) {
		ERR("CreateCommandAllocator failed (0x%08X)\n", hr);
		return hr;
	}

	hr = m_device->CreateCommandList(0, listType, m_allocator.Get(), nullptr,
									 IID_PPV_ARGS(&m_list));
	if (FAILED(hr)) {
		ERR("CreateCommandList failed (0x%08X)\n", hr);
		return hr;
	}
	m_list->Close();

	hr = m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_copyFence));
	if (FAILED(hr)) {
		ERR("CreateFence failed (0x%08X)\n", hr);
		return hr;
	}

	m_copyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (m_copyEvent == NULL) {
		ERR("CreateEvent failed (%lu)\n", GetLastError());
		return HRESULT_FROM_WIN32(GetLastError());
	}

	// The IddCx surface is produced on the D3D11 device. A shared fence orders
	// the D3D12 copy after the work queued there.
	ComPtr<ID3D11Device5> d3d11Device5;
	hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device5));
	if (FAILED(hr)) {
		ERR("ID3D11Device5 not available (0x%08X)\n", hr);
		return hr;
	}

	hr = d3d11Context->QueryInterface(IID_PPV_ARGS(&m_d3d11Context));
	if (FAILED(hr)) {
		ERR("ID3D11DeviceContext4 not available (0x%08X)\n", hr);
		return hr;
	}

	hr = d3d11Device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_d3d11Fence));
	if (FAILED(hr)) {
		ERR("D3D11 CreateFence failed (0x%08X)\n", hr);
		return hr;
	}

	HANDLE fenceHandle = NULL;
	hr = m_d3d11Fence->CreateSharedHandle(NULL, GENERIC_ALL, NULL, &fenceHandle);
	if (FAILED(hr)) {
		ERR("D3D11 fence CreateSharedHandle failed (0x%08X)\n", hr);
		return hr;
	}

	hr = m_device->OpenSharedHandle(fenceHandle, IID_PPV_ARGS(&m_interopFence));
	CloseHandle(fenceHandle);
	if (FAILED(hr)) {
		ERR("Opening the D3D11 fence in D3D12 failed (0x%08X)\n", hr);
		return hr;
	}

	DBGPRINT("D3D12 device memory frame copier initialised\n");
	return S_OK;
}

HRESULT D3D12FrameCopier::Resize(UINT width, UINT height, DXGI_FORMAT format)
{
	TRACING();
	HRESULT hr;

	ReleaseTarget();

	D3D12_RESOURCE_DESC texDesc = {};
	texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	texDesc.Width = width;
	texDesc.Height = height;
	texDesc.DepthOrArraySize = 1;
	texDesc.MipLevels = 1;
	texDesc.Format = format;
	texDesc.SampleDesc.Count = 1;
	texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

	UINT numRows = 0;
	UINT64 rowSize = 0, totalBytes = 0;
	m_device->GetCopyableFootprints(&texDesc, 0, 1, 0, &m_footprint, &numRows, &rowSize, &totalBytes);
	if (totalBytes == UINT64_MAX) {
		ERR("No copyable footprint for %ux%u format %d\n", width, height, format);
		return E_INVALIDARG;
	}

	// The KMD exposes pitch * height bytes to the host, so size the buffer for
	// full rows rather than the tighter totalBytes.
	UINT64 size = (UINT64)m_footprint.Footprint.RowPitch * height;
	size = (size + TARGET_ALIGNMENT - 1) & ~(UINT64)(TARGET_ALIGNMENT - 1);

	D3D12_HEAP_PROPERTIES heapProps = {};
	heapProps.Type = D3D12_HEAP_TYPE_GPU_UPLOAD;

	D3D12_RESOURCE_DESC bufDesc = {};
	bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bufDesc.Width = size;
	bufDesc.Height = 1;
	bufDesc.DepthOrArraySize = 1;
	bufDesc.MipLevels = 1;
	bufDesc.Format = DXGI_FORMAT_UNKNOWN;
	bufDesc.SampleDesc.Count = 1;
	bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Device1> device1;
	m_device.As(&device1);
	for (UINT i = 0; i < TARGET_COUNT; i++) {
		hr = m_device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
											   D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_target[i]));
		if (FAILED(hr)) {
			ERR("Creating GPU upload buffer %u (%I64u bytes) failed (0x%08X)\n", i, size, hr);
			ReleaseTarget();
			return hr;
		}

		// The KMD does not pin device memory, it only notices a relocation
		// after the fact. Ask the memory manager to keep it in place.
		if (device1) {
			ID3D12Pageable *pageable = m_target[i].Get();
			D3D12_RESIDENCY_PRIORITY priority = D3D12_RESIDENCY_PRIORITY_MAXIMUM;
			device1->SetResidencyPriority(1, &pageable, &priority);
		}

		// Persistent mapping; the CPU never reads it, the VA is only
		// translated to guest physical addresses by the KMD.
		D3D12_RANGE noRead = {0, 0};
		hr = m_target[i]->Map(0, &noRead, &m_data[i]);
		if (FAILED(hr)) {
			ERR("Mapping GPU upload buffer %u failed (0x%08X)\n", i, hr);
			m_data[i] = NULL;
			ReleaseTarget();
			return hr;
		}
		DBGPRINT("GPU upload buffer %u: %ux%u size %I64u mapped at %p\n", i, width, height, size, m_data[i]);
	}

	m_rowPitch = m_footprint.Footprint.RowPitch;
	return S_OK;
}

void D3D12FrameCopier::ReleaseTarget()
{
	TRACING();

	for (UINT i = 0; i < SURFACE_CACHE_SIZE; i++) {
		m_surfaces[i].d3d11 = nullptr;
		m_surfaces[i].d3d12.Reset();
	}
	m_nextSurface = 0;

	for (UINT i = 0; i < TARGET_COUNT; i++) {
		if (m_target[i]) {
			if (m_data[i]) {
				m_target[i]->Unmap(0, nullptr);
			}
			m_target[i].Reset();
		}
		m_data[i] = NULL;
	}
	m_rowPitch = 0;
	m_footprint = {};
}

HRESULT D3D12FrameCopier::OpenSurface(ID3D11Texture2D *surface, ID3D12Resource **resource)
{
	HRESULT hr;

	for (UINT i = 0; i < SURFACE_CACHE_SIZE; i++) {
		if (m_surfaces[i].d3d11 == surface && m_surfaces[i].d3d12) {
			return m_surfaces[i].d3d12.CopyTo(resource);
		}
	}

	ComPtr<IDXGIResource1> dxgiResource;
	hr = surface->QueryInterface(IID_PPV_ARGS(&dxgiResource));
	if (FAILED(hr)) {
		ERR("IDXGIResource1 not available on the swapchain surface (0x%08X)\n", hr);
		return hr;
	}

	HANDLE handle = NULL;
	hr = dxgiResource->CreateSharedHandle(NULL, DXGI_SHARED_RESOURCE_READ, NULL, &handle);
	if (FAILED(hr)) {
		ERR("CreateSharedHandle on the swapchain surface failed (0x%08X)\n", hr);
		return hr;
	}

	ComPtr<ID3D12Resource> opened;
	hr = m_device->OpenSharedHandle(handle, IID_PPV_ARGS(&opened));
	CloseHandle(handle);
	if (FAILED(hr)) {
		ERR("Opening the swapchain surface in D3D12 failed (0x%08X)\n", hr);
		return hr;
	}

	// Only the surface identity is kept (no reference), IddCx owns the
	// surfaces. The previous occupant of the slot is idle: every copy is
	// waited for before Copy() returns.
	CachedSurface &slot = m_surfaces[m_nextSurface];
	m_nextSurface = (m_nextSurface + 1) % SURFACE_CACHE_SIZE;
	slot.d3d11 = surface;
	slot.d3d12 = opened;

	DBGPRINT("Opened swapchain surface %p in D3D12\n", surface);
	return opened.CopyTo(resource);
}

HRESULT D3D12FrameCopier::Copy(ID3D11Texture2D *surface, UINT target)
{
	HRESULT hr;

	if (target >= TARGET_COUNT || !m_target[target] || !m_data[target]) {
		return E_NOT_VALID_STATE;
	}

	ComPtr<ID3D12Resource> source;
	hr = OpenSurface(surface, &source);
	if (FAILED(hr)) {
		return hr;
	}

	D3D12_RESOURCE_DESC srcDesc = source->GetDesc();
	if (srcDesc.Width != m_footprint.Footprint.Width || srcDesc.Height != m_footprint.Footprint.Height ||
		srcDesc.Format != m_footprint.Footprint.Format) {
		ERR("Surface %I64ux%u format %d does not match target %ux%u format %d\n", srcDesc.Width, srcDesc.Height,
			srcDesc.Format, m_footprint.Footprint.Width, m_footprint.Footprint.Height, m_footprint.Footprint.Format);
		return E_INVALIDARG;
	}

	// Order the copy after everything queued on the D3D11 context so far,
	// which includes the wait for the surface's producer. The flush submits
	// the signal; without it the D3D12 queue could wait forever.
	m_interopValue++;
	hr = m_d3d11Context->Signal(m_d3d11Fence.Get(), m_interopValue);
	if (FAILED(hr)) {
		ERR("D3D11 fence signal failed (0x%08X)\n", hr);
		return hr;
	}
	m_d3d11Context->Flush();

	hr = m_queue->Wait(m_interopFence.Get(), m_interopValue);
	if (FAILED(hr)) {
		ERR("D3D12 queue wait failed (0x%08X)\n", hr);
		return hr;
	}

	hr = m_allocator->Reset();
	if (FAILED(hr)) {
		ERR("CommandAllocator Reset failed (0x%08X)\n", hr);
		return hr;
	}
	hr = m_list->Reset(m_allocator.Get(), nullptr);
	if (FAILED(hr)) {
		ERR("CommandList Reset failed (0x%08X)\n", hr);
		return hr;
	}

	// Both resources are in the COMMON state: the surface is implicitly
	// promoted to COPY_SOURCE and the buffer to COPY_DEST, and both decay back
	// once the list has executed, so no barriers are needed.
	D3D12_TEXTURE_COPY_LOCATION dst = {};
	dst.pResource = m_target[target].Get();
	dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
	dst.PlacedFootprint = m_footprint;

	D3D12_TEXTURE_COPY_LOCATION src = {};
	src.pResource = source.Get();
	src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
	src.SubresourceIndex = 0;

	m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

	hr = m_list->Close();
	if (FAILED(hr)) {
		ERR("CommandList Close failed (0x%08X)\n", hr);
		return hr;
	}

	ID3D12CommandList *lists[] = {m_list.Get()};
	m_queue->ExecuteCommandLists(ARRAYSIZE(lists), lists);

	m_copyValue++;
	hr = m_queue->Signal(m_copyFence.Get(), m_copyValue);
	if (FAILED(hr)) {
		ERR("D3D12 queue signal failed (0x%08X)\n", hr);
		return hr;
	}

	// The host scans the buffer out as soon as the KMD flushes it, and IddCx
	// may reuse the surface once the frame is finished, so the copy has to be
	// complete before returning.
	return WaitForCopy(m_copyValue);
}

HRESULT D3D12FrameCopier::WaitForCopy(UINT64 value)
{
	UINT64 completed = m_copyFence->GetCompletedValue();
	if (completed == UINT64_MAX) {
		// The fence reads all ones once the device has been removed.
		ERR("D3D12 device removed (0x%08X)\n", m_device->GetDeviceRemovedReason());
		return DXGI_ERROR_DEVICE_REMOVED;
	}
	if (completed >= value) {
		return S_OK;
	}

	HRESULT hr = m_copyFence->SetEventOnCompletion(value, m_copyEvent);
	if (FAILED(hr)) {
		ERR("SetEventOnCompletion failed (0x%08X)\n", hr);
		return hr;
	}

	DWORD waitResult = WaitForSingleObject(m_copyEvent, COPY_TIMEOUT_MS);
	if (waitResult != WAIT_OBJECT_0) {
		HRESULT reason = m_device->GetDeviceRemovedReason();
		ERR("Waiting for the D3D12 copy failed (wait %lu, removed reason 0x%08X)\n", waitResult, reason);
		return FAILED(reason) ? reason : DXGI_ERROR_WAIT_TIMEOUT;
	}

	if (m_copyFence->GetCompletedValue() == UINT64_MAX) {
		ERR("D3D12 device removed (0x%08X)\n", m_device->GetDeviceRemovedReason());
		return DXGI_ERROR_DEVICE_REMOVED;
	}
	return S_OK;
}
