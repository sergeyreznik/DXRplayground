#include "DXrenderer/Textures/EnvironmentMap.h"

#include "DXrenderer/Buffers/UploadBuffer.h"
#include "DXrenderer/Shader.h"
#include "DXrenderer/PsoManager.h"
#include "DXrenderer/RenderPipeline.h"
#include "Utils/Helpers.h"
#include "Utils/PixProfiler.h"

namespace DirectxPlayground
{

EnvironmentMap::EnvironmentMap(RenderContext& ctx, const std::string& path, UINT cubemapSize, UINT irradianceMapSize)
    : mDataBuffer(new UploadBuffer(*ctx.Device, sizeof(mGraphicsData), true, 1))
    , mConvolutionDataBuffer(new UploadBuffer(*ctx.Device, sizeof(mConvolutionData), true, 1)), mCubemapSize(cubemapSize)
    , mIrradianceMapSize(irradianceMapSize)
{
    CreateRootSig(ctx);
    CreateDescriptorHeap(ctx);
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = GetDefaultComputePsoDescriptor(mRootSig.Get());

    auto shaderPath = ASSETS_DIR_W + std::wstring(L"Shaders//EnvMapConvertion.hlsl");
    ctx.PsoManager->CreatePso(ctx, mPsoName, shaderPath, desc);

    shaderPath = ASSETS_DIR_W + std::wstring(L"Shaders//IrradianceMapConvertion.hlsl");
    ctx.PsoManager->CreatePso(ctx, mConvolutionPsoName, shaderPath, desc);

    mEnvMapData = ctx.TexManager->CreateTexture(ctx, path, true);
    mEnvMapData.Resource->SetName(L"EnvEquirectMap");
    mCubemapData = ctx.TexManager->CreateCubemap(ctx, mCubemapSize, DXGI_FORMAT_R32G32B32A32_FLOAT, true);
    mCubemapData.Resource->SetName(L"EnvCubemap");
    mIrradianceMapData = ctx.TexManager->CreateCubemap(ctx, mIrradianceMapSize, DXGI_FORMAT_R32G32B32A32_FLOAT, true);
    mIrradianceMapData.Resource->SetName(L"IrradianceMap");

    mGraphicsData.EqMapCubeMapWH.x = static_cast<float>(mEnvMapData.Resource->Get()->GetDesc().Width);
    mGraphicsData.EqMapCubeMapWH.y = static_cast<float>(mEnvMapData.Resource->Get()->GetDesc().Height);
    mGraphicsData.EqMapCubeMapWH.z = static_cast<float>(mCubemapData.Resource->Get()->GetDesc().Width);
    mGraphicsData.EqMapCubeMapWH.w = static_cast<float>(mCubemapData.Resource->Get()->GetDesc().Height);

    mConvolutionData.size = static_cast<float>(irradianceMapSize);

    CreateViews(ctx);

    mDataBuffer->UploadData(0, mGraphicsData);
    mConvolutionDataBuffer->UploadData(0, mConvolutionData);
}

EnvironmentMap::~EnvironmentMap()
{
    SafeDelete(mDataBuffer);
    SafeDelete(mConvolutionDataBuffer);
}

void EnvironmentMap::ConvertToCubemap(RenderContext& ctx)
{
    GPU_SCOPED_EVENT(ctx, "ConverToCubemap");
    ctx.Pipeline->Flush();

    D3D12_RESOURCE_STATES cubeState = mCubemapData.Resource->GetCurrentState();
    D3D12_RESOURCE_STATES texState = mEnvMapData.Resource->GetCurrentState();
    D3D12_RESOURCE_STATES irrMapState = mIrradianceMapData.Resource->GetCurrentState();
    std::array<CD3DX12_RESOURCE_BARRIER, 3> barriers;
    barriers[0] = mCubemapData.Resource->GetBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    barriers[1] = mEnvMapData.Resource->GetBarrier(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barriers[2] = mIrradianceMapData.Resource->GetBarrier(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ctx.CommandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

    ctx.CommandList->SetComputeRootSignature(mRootSig.Get());
    ctx.CommandList->SetPipelineState(ctx.PsoManager->GetPso(mPsoName));

    ID3D12DescriptorHeap* descHeaps[] = { mHeap.Get() };
    ctx.CommandList->SetDescriptorHeaps(1, descHeaps);

    ctx.CommandList->SetComputeRootConstantBufferView(0, mDataBuffer->GetFrameDataGpuAddress(0));
    CD3DX12_GPU_DESCRIPTOR_HANDLE tableHandle(mHeap->GetGPUDescriptorHandleForHeapStart());
    ctx.CommandList->SetComputeRootDescriptorTable(1, tableHandle);
    tableHandle.Offset(ctx.CbvSrvUavDescriptorSize);
    ctx.CommandList->SetComputeRootDescriptorTable(2, tableHandle);

    ctx.CommandList->Dispatch(static_cast<UINT>(mCubemapData.Resource->Get()->GetDesc().Width / 32), static_cast<UINT>(mCubemapData.Resource->Get()->GetDesc().Height / 32), 6);
    barriers[0] = mCubemapData.Resource->GetBarrier(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barriers[1] = mEnvMapData.Resource->GetBarrier(texState);
    ctx.CommandList->ResourceBarrier(2U, barriers.data());

    Convolute(ctx);

    barriers[0] = mCubemapData.Resource->GetBarrier(cubeState);
    barriers[1] = mIrradianceMapData.Resource->GetBarrier(irrMapState);
    ctx.CommandList->ResourceBarrier(2U, barriers.data());
    ctx.Pipeline->Flush();
}

bool EnvironmentMap::IsConvertedToCubemap() const
{
    return mConverted;
}

void EnvironmentMap::CreateRootSig(const RenderContext& ctx)
{
    std::vector<CD3DX12_ROOT_PARAMETER1> params;
    params.emplace_back();
    params.back().InitAsConstantBufferView(0, 0);

    D3D12_DESCRIPTOR_RANGE1 envMap{};
    envMap.NumDescriptors = 1;
    envMap.BaseShaderRegister = 0;
    envMap.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    envMap.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    envMap.RegisterSpace = 0;
    envMap.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    params.emplace_back();
    params.back().InitAsDescriptorTable(1, &envMap, D3D12_SHADER_VISIBILITY_ALL);

    D3D12_DESCRIPTOR_RANGE1 cubeMap{};
    cubeMap.NumDescriptors = 1;
    cubeMap.BaseShaderRegister = 0;
    cubeMap.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
    cubeMap.Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE;
    cubeMap.RegisterSpace = 0;
    cubeMap.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    params.emplace_back();
    params.back().InitAsDescriptorTable(1, &cubeMap, D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_STATIC_SAMPLER_DESC linearClamp(
        0,
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    std::array<CD3DX12_STATIC_SAMPLER_DESC, 1> staticSamplers = { linearClamp };

    D3D12_FEATURE_DATA_ROOT_SIGNATURE signatureData = {};
    signatureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

    if (FAILED(ctx.Device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &signatureData, sizeof(signatureData))))
        signatureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;

    D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init_1_1(static_cast<UINT>(params.size()), params.data(), static_cast<UINT>(staticSamplers.size()), staticSamplers.data(), flags);

    Microsoft::WRL::ComPtr<ID3DBlob> signature;
    Microsoft::WRL::ComPtr<ID3DBlob> rootSignatureCreationError;
    if (HRESULT hr = D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, signatureData.HighestVersion, &signature, &rootSignatureCreationError); hr != S_OK)
    {
        OutputDebugStringA(reinterpret_cast<char*>(rootSignatureCreationError->GetBufferPointer()));
        return;
    }
    ctx.Device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&mRootSig));
}

void EnvironmentMap::CreateDescriptorHeap(RenderContext& ctx)
{
    // 2d eqmap
    // uav envmap target
    // srv envmap src
    // irradiance map target
    D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = 4;
    ThrowIfFailed(ctx.Device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&mHeap)));
    NAME_D3D12_OBJECT(mHeap, L"EnvMap heap");
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(mHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_SHADER_RESOURCE_VIEW_DESC src{};
    src.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    src.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    src.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    src.Texture2D.MipLevels = 1;
    src.Texture2D.MostDetailedMip = 0;
    src.Texture2D.ResourceMinLODClamp = 0.0f;
    ctx.Device->CreateShaderResourceView(nullptr, &src, handle);

    handle.Offset(static_cast<int>(ctx.CbvSrvUavDescriptorSize));

    D3D12_UNORDERED_ACCESS_VIEW_DESC viewDesc = {};
    viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    viewDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    ctx.Device->CreateUnorderedAccessView(nullptr, nullptr, &viewDesc, handle);

    handle.Offset(static_cast<int>(ctx.CbvSrvUavDescriptorSize));

    src.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    ctx.Device->CreateShaderResourceView(nullptr, &src, handle);

    handle.Offset(static_cast<int>(ctx.CbvSrvUavDescriptorSize));

    ctx.Device->CreateUnorderedAccessView(nullptr, nullptr, &viewDesc, handle);
}

void EnvironmentMap::CreateViews(RenderContext& ctx) const
{
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(mHeap->GetCPUDescriptorHandleForHeapStart());

    D3D12_RESOURCE_DESC envRDesc = mEnvMapData.Resource->Get()->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC envRtvD{};
    envRtvD.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    envRtvD.Format = envRDesc.Format;
    envRtvD.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    envRtvD.Texture2D.MipLevels = envRDesc.MipLevels;
    envRtvD.Texture2D.MostDetailedMip = 0;
    envRtvD.Texture2D.ResourceMinLODClamp = 0.0f;
    ctx.Device->CreateShaderResourceView(mEnvMapData.Resource->Get(), &envRtvD, handle);
    handle.Offset(ctx.CbvSrvUavDescriptorSize);

    D3D12_UNORDERED_ACCESS_VIEW_DESC viewDesc = {};
    viewDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
    viewDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    viewDesc.Texture2DArray.ArraySize = 6;

    ctx.Device->CreateUnorderedAccessView(mCubemapData.Resource->Get(), nullptr, &viewDesc, handle);
    handle.Offset(ctx.CbvSrvUavDescriptorSize);

    D3D12_RESOURCE_DESC envCubeRDesc = mCubemapData.Resource->Get()->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC envCubeRView{};
    envCubeRView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    envCubeRView.Format = envCubeRDesc.Format;
    envCubeRView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    envCubeRView.TextureCube.MipLevels = envCubeRDesc.MipLevels;
    envCubeRView.TextureCube.MostDetailedMip = 0;
    envCubeRView.TextureCube.ResourceMinLODClamp = 0.0f;
    ctx.Device->CreateShaderResourceView(mCubemapData.Resource->Get(), &envCubeRView, handle);
    handle.Offset(ctx.CbvSrvUavDescriptorSize);

    ctx.Device->CreateUnorderedAccessView(mIrradianceMapData.Resource->Get(), nullptr, &viewDesc, handle);
};

void EnvironmentMap::Convolute(RenderContext& ctx) const
{
    GPU_SCOPED_EVENT(ctx, "Convolution");
    ctx.CommandList->SetPipelineState(ctx.PsoManager->GetPso(mConvolutionPsoName));
    ctx.CommandList->SetComputeRootConstantBufferView(0, mConvolutionDataBuffer->GetFrameDataGpuAddress(0));
    CD3DX12_GPU_DESCRIPTOR_HANDLE tableHandle(mHeap->GetGPUDescriptorHandleForHeapStart());
    tableHandle.Offset(ctx.CbvSrvUavDescriptorSize * 2);
    ctx.CommandList->SetComputeRootDescriptorTable(1, tableHandle);
    tableHandle.Offset(ctx.CbvSrvUavDescriptorSize);
    ctx.CommandList->SetComputeRootDescriptorTable(2, tableHandle);

    ctx.CommandList->Dispatch(static_cast<UINT>(mIrradianceMapData.Resource->Get()->GetDesc().Width / 32), static_cast<UINT>(mIrradianceMapData.Resource->Get()->GetDesc().Height / 32), 6);
}

}