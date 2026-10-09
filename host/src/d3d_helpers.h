// D3D11 device helpers for the VRStream host.
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace vrstream {

using Microsoft::WRL::ComPtr;

struct D3dAdapter {
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::string name;
    size_t dedicatedVramMb = 0;
};

// Creates D3D11 devices on all hardware adapters (NVENC-capable ones are
// picked by the encoder probe; on hybrid laptops the NVIDIA dGPU is what we
// want even though the Intel iGPU drives the display).
std::vector<D3dAdapter> enumerateD3dAdapters();

}  // namespace vrstream
