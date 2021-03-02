#include "TextureManager.h"

#include <vector>
#include <sstream>

#include "External/Dx12Helpers/d3dx12.h"
#include "External/lodepng/lodepng.h"
#include "DXrenderer/RenderContext.h"

#include "DXhelpers.h"

namespace DirectxPlayground
{

TextureManager::TextureManager(RenderContext& ctx)
{
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 1;
    ThrowIfFailed(ctx.Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap)));
}

void TextureManager::CreateTexture(RenderContext& ctx, const std::string& filename)
{
    std::vector<unsigned char> buffer;
    UINT w = 0, h = 0;
    UINT error = lodepng::decode(buffer, w, h, filename);
    if (error)
    {
        std::stringstream ss;
        ss << "PNG decoding error " << error << " : " << lodepng_error_text(error) << std::endl;
        LOG(ss.str().c_str());
    }
    for (size_t i = 0; i < buffer.size(); ++i)
    {
        buffer[i] = 1;
    }

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.DepthOrArraySize = 1;
    texDesc.SampleDesc.Count = 1;
    texDesc.SampleDesc.Quality = 0;
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

    size_t texSize = buffer.size() * sizeof(unsigned char);
    CD3DX12_HEAP_PROPERTIES uploadHeapProps(D3D12_HEAP_TYPE_UPLOAD);
    CD3DX12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(texSize);
    ThrowIfFailed(ctx.Device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &bufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_uploadResource)));

    D3D12_SUBRESOURCE_DATA texData = {};
    texData.pData = buffer.data();
    texData.RowPitch = LONG(w) * 4L;
    texData.SlicePitch = texData.RowPitch * h;

    ThrowIfFailed(ctx.Device->CreateCommittedResource(&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
        D3D12_HEAP_FLAG_NONE,
        &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&m_resource)));

    UpdateSubresources(ctx.CommandList, m_resource.Get(), m_uploadResource.Get(), 0, 0, 1, &texData);

    CD3DX12_RESOURCE_BARRIER toDest = CD3DX12_RESOURCE_BARRIER::Transition(m_resource.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ctx.CommandList->ResourceBarrier(1, &toDest);

    D3D12_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
    viewDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    viewDesc.Format = m_resource->GetDesc().Format;
    viewDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = m_resource->GetDesc().MipLevels;
    viewDesc.Texture2D.MostDetailedMip = 0;
    viewDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    ctx.Device->CreateShaderResourceView(m_resource.Get(), &viewDesc, m_srvHeap->GetCPUDescriptorHandleForHeapStart());
}

}