// Console ID3D12CommandSignature.

#include <stdlib.h>
#include <string.h>

#include "common/log.h"
#include "hle/d3d12/d3d12_objects.h"
#include "hle/d3d12/d3d12_tables.h"

static const GUID* const k_command_signature_iids[] = {&__uuidof(ID3D12Object), &__uuidof(ID3D12DeviceChild),
                                                       &__uuidof(ID3D12Pageable),
                                                       &__uuidof(ID3D12CommandSignature)};

static void command_signature_destroy(void* object);

static const ConsoleClass k_command_signature_class = {"ID3D12CommandSignature", k_command_signature_iids,
                                                       ARRAY_COUNT(k_command_signature_iids),
                                                       command_signature_destroy};

static void* const* g_command_signature_table = nullptr;

static void command_signature_destroy(void* object) {
    ConsoleCommandSignature* s = (ConsoleCommandSignature*)object;
    free(s->arguments);
    if (s->root_signature)
        console_release(s->root_signature);
}

void console_command_signature_init() {
    g_command_signature_table = console_build_table("ID3D12CommandSignature", k_command_signature_entries,
                                                    ARRAY_COUNT(k_command_signature_entries), nullptr, 0);
}

HRESULT console_create_command_signature(ConsoleDevice* device, const D3D12_COMMAND_SIGNATURE_DESC* desc,
                                         void* root_signature, REFIID iid, void** out) {
    if (!desc || (desc->NumArgumentDescs && !desc->pArgumentDescs))
        return E_INVALIDARG;
    D3D12_INDIRECT_ARGUMENT_DESC* arguments = nullptr;
    if (desc->NumArgumentDescs) {
        usize bytes = desc->NumArgumentDescs * sizeof(D3D12_INDIRECT_ARGUMENT_DESC);
        arguments = (D3D12_INDIRECT_ARGUMENT_DESC*)malloc(bytes);
        if (!arguments)
            return E_OUTOFMEMORY;
        memcpy(arguments, desc->pArgumentDescs, bytes);
    }
    for (u32 i = 0; i < desc->NumArgumentDescs; i++)
        LOG_D("d3d12", "command signature (stride %u) argument %u: type %d", desc->ByteStride, i,
              arguments[i].Type);

    ConsoleCommandSignature* s = (ConsoleCommandSignature*)console_alloc(
        sizeof(ConsoleCommandSignature), &k_command_signature_class, g_command_signature_table, nullptr);
    s->byte_stride = desc->ByteStride;
    s->argument_count = desc->NumArgumentDescs;
    s->arguments = arguments;
    if (root_signature && console_try_add_ref(root_signature))
        s->root_signature = root_signature;
    return console_return(s, iid, out);
}
