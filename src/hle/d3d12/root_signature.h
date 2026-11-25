#pragma once

#include "hle/d3d12/d3d12_objects.h"

// Root signature serialization, exported by d3d12_xs.dll under the D3D12Serialize* names.
HRESULT WINAPI console_serialize_versioned_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                                          ID3DBlob** blob, ID3DBlob** error);
HRESULT WINAPI console_serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC* desc,
                                                D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob,
                                                ID3DBlob** error);
