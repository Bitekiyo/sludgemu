#include "loader/pe_image.h"

#include "common/log.h"
#include "common/str.h"

static bool read_at(HANDLE file, u64 offset, void* dst, u32 bytes) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(file, li, nullptr, FILE_BEGIN))
        return false;
    u8* out = (u8*)dst;
    while (bytes) {
        DWORD chunk = bytes > MiB(64) ? (DWORD)MiB(64) : bytes;
        DWORD got = 0;
        if (!ReadFile(file, out, chunk, &got, nullptr) || got != chunk)
            return false;
        out += got;
        bytes -= got;
    }
    return true;
}

bool PeImage::map_from_file(const wchar_t* file_path) {
    str_copy_w(path, ARRAY_COUNT(path), file_path);
    str_to_narrow(name, sizeof(name), path_file_name_w(file_path));
    for (char* c = name; *c; c++)
        if (*c >= 'A' && *c <= 'Z')
            *c = (char)(*c - 'A' + 'a');

    HANDLE file = CreateFileW(file_path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        LOG_E("loader", "cannot open %s (error %lu)", name, GetLastError());
        return false;
    }

    bool ok = false;
    u8 hdr[4096];
    do {
        if (!read_at(file, 0, hdr, sizeof(hdr))) {
            LOG_E("loader", "%s: cannot read headers", name);
            break;
        }
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hdr;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
            (u32)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > sizeof(hdr)) {
            LOG_E("loader", "%s: bad DOS header", name);
            break;
        }
        IMAGE_NT_HEADERS64* fnt = (IMAGE_NT_HEADERS64*)(hdr + dos->e_lfanew);
        if (fnt->Signature != IMAGE_NT_SIGNATURE || fnt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            fnt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
            LOG_E("loader", "%s: not an x64 PE32+ image", name);
            break;
        }
        const IMAGE_OPTIONAL_HEADER64& opt = fnt->OptionalHeader;
        preferred_base = opt.ImageBase;
        size = opt.SizeOfImage;
        is_dll = (fnt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;

        base = (u8*)VirtualAlloc((void*)preferred_base, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!base) {
            bool can_relocate = !(fnt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED) &&
                                opt.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC].Size != 0;
            if (!can_relocate) {
                LOG_E("loader", "%s: preferred base 0x%llx is taken and image has no relocations", name,
                      preferred_base);
                break;
            }
            base = (u8*)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!base) {
                LOG_E("loader", "%s: cannot allocate 0x%x bytes", name, size);
                break;
            }
            relocated = true;
        }

        // Headers: re-read in full in case SizeOfHeaders exceeds our probe buffer.
        if (!read_at(file, 0, base, opt.SizeOfHeaders)) {
            LOG_E("loader", "%s: cannot read headers", name);
            break;
        }

        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt());
        u16 count = nt()->FileHeader.NumberOfSections;
        bool sections_ok = true;
        for (u16 i = 0; i < count; i++) {
            const IMAGE_SECTION_HEADER& s = sec[i];
            if (!s.SizeOfRawData)
                continue;
            u32 bytes = s.SizeOfRawData;
            if ((u64)s.VirtualAddress + bytes > size)
                bytes = size - s.VirtualAddress;
            if (!read_at(file, s.PointerToRawData, base + s.VirtualAddress, bytes)) {
                LOG_E("loader", "%s: cannot read section %.8s", name, (const char*)s.Name);
                sections_ok = false;
                break;
            }
        }
        if (!sections_ok)
            break;

        if (relocated) {
            s64 delta = (s64)((u64)base - preferred_base);
            if (!apply_relocations(delta))
                break;
            nt()->OptionalHeader.ImageBase = (u64)base;
            LOG_W("loader", "%s relocated to %p (preferred 0x%llx), addresses won't match the disassembler", name, base,
                  preferred_base);
        }
        ok = true;
    } while (0);

    CloseHandle(file);
    if (!ok)
        unmap();
    else
        LOG_I("loader", "mapped %s at %p..%p (%s)", name, base, base + size, is_dll ? "dll" : "exe");
    return ok;
}

void PeImage::unmap() {
    if (base)
        VirtualFree(base, 0, MEM_RELEASE);
    base = nullptr;
    size = 0;
}

bool PeImage::apply_relocations(s64 delta) {
    const IMAGE_DATA_DIRECTORY& d = dir(IMAGE_DIRECTORY_ENTRY_BASERELOC);
    u8* p = base + d.VirtualAddress;
    u8* end = p + d.Size;
    while (p < end) {
        IMAGE_BASE_RELOCATION* block = (IMAGE_BASE_RELOCATION*)p;
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION))
            break;
        u32 n = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(u16);
        u16* entries = (u16*)(block + 1);
        for (u32 i = 0; i < n; i++) {
            u16 type = entries[i] >> 12;
            u16 off = entries[i] & 0xFFF;
            u8* target = base + block->VirtualAddress + off;
            switch (type) {
            case IMAGE_REL_BASED_ABSOLUTE:
                break;
            case IMAGE_REL_BASED_DIR64:
                *(u64*)target += (u64)delta;
                break;
            case IMAGE_REL_BASED_HIGHLOW:
                *(u32*)target += (u32)delta;
                break;
            default:
                LOG_E("loader", "%s: unsupported relocation type %u", name, type);
                return false;
            }
        }
        p += block->SizeOfBlock;
    }
    return true;
}

void PeImage::protect_sections() {
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt());
    u16 count = nt()->FileHeader.NumberOfSections;
    DWORD old;
    VirtualProtect(base, nt()->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);
    for (u16 i = 0; i < count; i++) {
        const IMAGE_SECTION_HEADER& s = sec[i];
        u32 vsize = s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData;
        if (!vsize)
            continue;
        DWORD c = s.Characteristics;
        bool x = (c & IMAGE_SCN_MEM_EXECUTE) != 0;
        bool r = (c & IMAGE_SCN_MEM_READ) != 0;
        bool w = (c & IMAGE_SCN_MEM_WRITE) != 0;
        DWORD prot;
        if (x)
            prot = w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
        else if (w)
            prot = PAGE_READWRITE;
        else if (r)
            prot = PAGE_READONLY;
        else
            prot = PAGE_NOACCESS;
        if (!VirtualProtect(base + s.VirtualAddress, vsize, prot, &old))
            LOG_W("loader", "%s: VirtualProtect(%.8s) failed (%lu)", name, (const char*)s.Name, GetLastError());
    }
    FlushInstructionCache(GetCurrentProcess(), base, size);
}

bool PeImage::register_unwind_info() {
    const IMAGE_DATA_DIRECTORY& d = dir(IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    if (!d.Size)
        return true;
    RUNTIME_FUNCTION* table = at_rva<RUNTIME_FUNCTION>(d.VirtualAddress);
    DWORD count = d.Size / sizeof(RUNTIME_FUNCTION);
    if (!RtlAddFunctionTable(table, count, (DWORD64)base)) {
        LOG_E("loader", "%s: RtlAddFunctionTable failed", name);
        return false;
    }
    LOG_D("loader", "%s: registered %lu unwind entries", name, count);
    return true;
}

void* PeImage::entry_point() const {
    u32 rva = nt()->OptionalHeader.AddressOfEntryPoint;
    return rva ? base + rva : nullptr;
}

void* PeImage::find_export(const char* fn_name) const {
    const IMAGE_DATA_DIRECTORY& d = dir(IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!d.Size)
        return nullptr;
    IMAGE_EXPORT_DIRECTORY* ed = at_rva<IMAGE_EXPORT_DIRECTORY>(d.VirtualAddress);
    u32* names = at_rva<u32>(ed->AddressOfNames);
    u16* ordinals = at_rva<u16>(ed->AddressOfNameOrdinals);
    // Binary search: export names are sorted.
    s64 lo = 0, hi = (s64)ed->NumberOfNames - 1;
    while (lo <= hi) {
        s64 mid = (lo + hi) / 2;
        int c = strcmp(fn_name, at_rva<const char>(names[mid]));
        if (c == 0)
            return find_export_ordinal((u16)(ordinals[mid] + ed->Base));
        if (c < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return nullptr;
}

void* PeImage::find_export_ordinal(u16 ordinal) const {
    const IMAGE_DATA_DIRECTORY& d = dir(IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!d.Size)
        return nullptr;
    IMAGE_EXPORT_DIRECTORY* ed = at_rva<IMAGE_EXPORT_DIRECTORY>(d.VirtualAddress);
    u32 index = (u32)ordinal - ed->Base;
    if (index >= ed->NumberOfFunctions)
        return nullptr;
    u32 rva = at_rva<u32>(ed->AddressOfFunctions)[index];
    if (!rva)
        return nullptr;
    if (rva >= d.VirtualAddress && rva < d.VirtualAddress + d.Size) {
        // Forwarder string ("DLL.Function"). Not expected in title DLLs; flag it loudly.
        LOG_W("loader", "%s: export ordinal %u is a forwarder (%s) — not supported yet", name, ordinal,
              at_rva<const char>(rva));
        return nullptr;
    }
    return base + rva;
}
