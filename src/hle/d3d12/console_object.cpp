#include "hle/d3d12/console_object.h"

#include <stdio.h>
#include <stdlib.h>

#include "common/log.h"
#include "loader/stubs.h"

void* const* console_build_table(const char* interface_name, const char* const* entry_names, u32 count,
                                 const ConsoleEntry* impl, u32 impl_count) {
    void** table = (void**)calloc(count, sizeof(void*));
    if (!table)
        FATAL("out of memory building the %s function table", interface_name);
    for (u32 i = 0; i < impl_count; i++) {
        if (impl[i].index >= count)
            FATAL("%s: entry %u out of range (%u entries)", interface_name, impl[i].index, count);
        table[impl[i].index] = impl[i].fn;
    }
    for (u32 i = 0; i < count; i++) {
        if (table[i])
            continue;
        char name[128];
        snprintf(name, sizeof(name), "%s::%s", interface_name, entry_names[i]);
        table[i] = stub_create_ex("d3d12_xs.dll", name, 0, (u64)(u32)E_NOTIMPL);
    }
    return table;
}
