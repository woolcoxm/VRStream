#include "d3d_helpers.h"

namespace vrstream {

std::vector<D3dAdapter> enumerateD3dAdapters() {
    std::vector<D3dAdapter> out;

    IDXGIFactory1* factoryRaw = nullptr;
    if (CreateDXGIFactory1(IID_PPV_ARGS(&factoryRaw)) != S_OK) return out;
    ComPtr<IDXGIFactory1> factory(factoryRaw);

    for (UINT i = 0;; i++) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;

        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        // Skip Microsoft Basic Render Driver (software).
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        D3D_FEATURE_LEVEL fl{};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> ctx;
        HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                                       nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(),
                                       &fl, ctx.GetAddressOf());
        if (hr != S_OK) continue;
        // The debug flag may be unavailable; retry without it.
        if (!device) {
            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr,
                                   0, D3D11_SDK_VERSION, device.GetAddressOf(), &fl,
                                   ctx.GetAddressOf());
            if (hr != S_OK) continue;
        }

        char name[128]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr,
                            nullptr);
        D3dAdapter a;
        a.adapter = adapter;
        a.device = device;
        a.context = ctx;
        a.name = name;
        a.dedicatedVramMb = desc.DedicatedVideoMemory / (1024 * 1024);
        out.push_back(std::move(a));
    }
    return out;
}

}  // namespace vrstream
