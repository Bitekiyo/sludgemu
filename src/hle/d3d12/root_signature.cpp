// Console root signatures: serialization (d3d12_xs exports) and ID3D12RootSignature.
//
// Console root signature descriptions are the desktop ones (versions 1.0 and 1.1) plus a few
// console flags in the high bits that select console shader ABIs. Our serializer writes the
// desktop format, since CreateRootSignature, also ours, is its only consumer.

#include "common/log.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"
#include "hle/d3d12/root_signature.h"

#define CONSOLE_ROOT_SIGNATURE_FLAGS 0xFF000000u

static const GUID* const k_root_signature_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                                    &__uuidof(ID3D12RootSignature)};

static void root_signature_destroy(void* object);

static const ConsoleClass k_root_signature_class = {
    "ID3D12RootSignature", k_root_signature_iids, ARRAY_COUNT(k_root_signature_iids), root_signature_destroy};

static void* const* g_root_signature_table = nullptr;

// --- serialization -----------------------------------------------------------------------------------

static D3D12_VERSIONED_ROOT_SIGNATURE_DESC
without_console_flags(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc) {
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = desc;
    D3D12_ROOT_SIGNATURE_FLAGS* flags =
        d.Version == D3D_ROOT_SIGNATURE_VERSION_1_0 ? &d.Desc_1_0.Flags : &d.Desc_1_1.Flags;
    if (*flags & CONSOLE_ROOT_SIGNATURE_FLAGS)
        LOG_D("d3d12", "root signature: dropping console ABI flags 0x%x",
              *flags & CONSOLE_ROOT_SIGNATURE_FLAGS);
    *flags = (D3D12_ROOT_SIGNATURE_FLAGS)(*flags & ~CONSOLE_ROOT_SIGNATURE_FLAGS);
    return d;
}

HRESULT WINAPI console_serialize_versioned_root_signature(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc,
                                                          ID3DBlob** blob, ID3DBlob** error) {
    if (!desc)
        return E_INVALIDARG;
    if (desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_0 && desc->Version != D3D_ROOT_SIGNATURE_VERSION_1_1) {
        LOG_E("d3d12", "root signature version %d is not a console version", desc->Version);
        return E_INVALIDARG;
    }
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = without_console_flags(*desc);
    HRESULT hr = D3D12SerializeVersionedRootSignature(&d, blob, error);
    if (FAILED(hr))
        LOG_E("d3d12", "serializing a root signature failed (0x%08lx)", (unsigned long)hr);
    return hr;
}

HRESULT WINAPI console_serialize_root_signature(const D3D12_ROOT_SIGNATURE_DESC* desc,
                                                D3D_ROOT_SIGNATURE_VERSION version, ID3DBlob** blob,
                                                ID3DBlob** error) {
    if (!desc)
        return E_INVALIDARG;
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC d = {};
    d.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
    d.Desc_1_0 = *desc;
    if (version != D3D_ROOT_SIGNATURE_VERSION_1_0)
        LOG_W("d3d12", "D3D12SerializeRootSignature asked for version %d of a 1.0 description", version);
    return console_serialize_versioned_root_signature(&d, blob, error);
}

// --- object ------------------------------------------------------------------------------------------

static void root_signature_destroy(void* object) {
    ConsoleRootSignature* rs = (ConsoleRootSignature*)object;
    if (rs->deserializer)
        rs->deserializer->Release();
}

void console_root_signature_init() {
    g_root_signature_table = console_build_table("ID3D12RootSignature", k_root_signature_entries,
                                                 ARRAY_COUNT(k_root_signature_entries), nullptr, 0);
}

HRESULT console_create_root_signature(ConsoleDevice* device, const void* blob, usize size, REFIID iid,
                                      void** out) {
    if (!blob || size == 0)
        return E_INVALIDARG;
    ID3D12VersionedRootSignatureDeserializer* deserializer = nullptr;
    HRESULT hr = D3D12CreateVersionedRootSignatureDeserializer(blob, size, IID_PPV_ARGS(&deserializer));
    if (FAILED(hr)) {
        const u8* b = (const u8*)blob;
        LOG_E("d3d12",
              "root signature blob of %zu bytes is not in a known format (%02x %02x %02x %02x): 0x%08lx",
              size, b[0], size > 1 ? b[1] : 0, size > 2 ? b[2] : 0, size > 3 ? b[3] : 0, (unsigned long)hr);
        return hr;
    }
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
    hr = deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc);
    if (FAILED(hr)) {
        deserializer->Release();
        return hr;
    }
    ID3D12RootSignature* desktop = nullptr;
    hr = device->desktop->CreateRootSignature(0, blob, size, IID_PPV_ARGS(&desktop));
    if (FAILED(hr)) {
        LOG_E("d3d12", "CreateRootSignature failed (0x%08lx)", (unsigned long)hr);
        deserializer->Release();
        return hr;
    }
    ConsoleRootSignature* rs = (ConsoleRootSignature*)console_alloc(
        sizeof(ConsoleRootSignature), &k_root_signature_class, g_root_signature_table, desktop);
    rs->desc = &desc->Desc_1_1;
    rs->deserializer = deserializer;
    rs->desktop = desktop;
    u32 constants = 0;
    for (u32 i = 0; i < rs->desc->NumParameters && i < ROOT_MAX_PARAMETERS; i++) {
        const D3D12_ROOT_PARAMETER1& p = rs->desc->pParameters[i];
        rs->constant_offset[i] = (u8)constants;
        if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
            constants += p.Constants.Num32BitValues;
    }
    LOG_D("d3d12", "root signature: %u parameters, %u static samplers, flags 0x%x", rs->desc->NumParameters,
          rs->desc->NumStaticSamplers, rs->desc->Flags);
    return console_return(rs, iid, out);
}
