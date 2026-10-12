/*===========================================================================
; D3D12FrameCopier.h
;----------------------------------------------------------------------------
; SPDX-License-Identifier: MS-PL
;
; File Description:
;   Copies IddCx swapchain surfaces into a linear buffer that lives in CPU
;   visible device local memory (D3D12 GPU upload heap). The CPU mapping of
;   that buffer goes through the render GPU's memory BAR, so the guest physical
;   addresses the KMD hands to virtio-gpu point into the VFIO BAR of the
;   passed through VF and QEMU can export the frame as a VFIO dmabuf, without
;   the frame ever leaving GPU memory.
;--------------------------------------------------------------------------*/
#pragma once

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <wrl.h>

namespace Microsoft {
namespace IndirectDisp {

class D3D12FrameCopier
{
public:
	D3D12FrameCopier();
	~D3D12FrameCopier();

	// Creates a D3D12 device on the render adapter and the interop fence with
	// the IddCx D3D11 device. Fails if GPU upload heaps are not available.
	HRESULT Init(IDXGIAdapter1 *adapter, ID3D11Device *d3d11Device, ID3D11DeviceContext *d3d11Context);

	// Frame buffers the swapchain rotates through: one on screen, one being
	// presented to the host, one to copy the next frame into.
	static constexpr UINT TARGET_COUNT = 3;

	// (Re)creates the persistently mapped target buffers for surfaces of the
	// given size/format. Drops all cached surfaces.
	HRESULT Resize(UINT width, UINT height, DXGI_FORMAT format);

	// Copies the surface into target buffer `target` and waits until the copy
	// has completed. The caller must hold the lock serialising the D3D11
	// immediate context, as a signal is queued on it.
	HRESULT Copy(ID3D11Texture2D *surface, UINT target);

	// Releases the target buffers and all cached surfaces. The caller must make
	// sure the KMD no longer references them.
	void ReleaseTarget();

	void *GetData(UINT target) const { return target < TARGET_COUNT ? m_data[target] : NULL; }
	UINT GetRowPitch() const { return m_rowPitch; }

private:
	struct CachedSurface
	{
		// Identity only, no reference: IddCx owns the surfaces.
		ID3D11Texture2D *d3d11 = nullptr;
		Microsoft::WRL::ComPtr<ID3D12Resource> d3d12;
	};

	// IddCx rotates through a small set of surfaces; opening a shared handle is
	// too expensive to do on every frame.
	static constexpr UINT SURFACE_CACHE_SIZE = 4;

	HRESULT OpenSurface(ID3D11Texture2D *surface, ID3D12Resource **resource);
	HRESULT WaitForCopy(UINT64 value);

	HMODULE m_d3d12Module;
	Microsoft::WRL::ComPtr<ID3D12Device> m_device;
	Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_queue;
	Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_allocator;
	Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_list;

	// Signalled on the D3D11 context once the surface is ready to be read,
	// waited on by the D3D12 queue before the copy.
	Microsoft::WRL::ComPtr<ID3D11DeviceContext4> m_d3d11Context;
	Microsoft::WRL::ComPtr<ID3D11Fence> m_d3d11Fence;
	Microsoft::WRL::ComPtr<ID3D12Fence> m_interopFence;
	UINT64 m_interopValue;

	// Signalled by the D3D12 queue once the copy has completed.
	Microsoft::WRL::ComPtr<ID3D12Fence> m_copyFence;
	UINT64 m_copyValue;
	HANDLE m_copyEvent;

	Microsoft::WRL::ComPtr<ID3D12Resource> m_target[TARGET_COUNT];
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_footprint;
	void *m_data[TARGET_COUNT];
	UINT m_rowPitch;

	CachedSurface m_surfaces[SURFACE_CACHE_SIZE];
	UINT m_nextSurface;
};

} // namespace IndirectDisp
} // namespace Microsoft
