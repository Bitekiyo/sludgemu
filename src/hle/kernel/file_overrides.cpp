// File-system functions as seen by the title: console paths are translated to host paths (see
// core/vfs.h). Opens are logged at debug level and failures at info, so missing files show up.

#include <windows.h>
#include <string.h>

#include "common/log.h"
#include "common/str.h"
#include "core/memory.h"
#include "core/vfs.h"
#include "hle/kernel/overrides.h"

#define PATH_CAP (MAX_PATH * 2)

// Translates `path` into `buf`; returns the path to hand to the host.
static const wchar_t* host_path(const wchar_t* path, wchar_t* buf) {
    if (!path)
        return nullptr;
    if (!vfs_translate(path, buf, PATH_CAP)) {
        LOG_W("file", "path too long: %ls", path);
        return path;
    }
    return buf;
}

// Logs an access without disturbing the error the title is about to read.
static void log_access(const char* fn, const wchar_t* path, bool ok) {
    DWORD error = GetLastError();
    if (ok)
        LOG_D("file", "%s(%ls)", fn, path);
    else
        LOG_I("file", "%s(%ls) failed (%lu)", fn, path, error);
    SetLastError(error);
}

static HANDLE WINAPI hk_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                    DWORD disposition, DWORD flags, HANDLE template_file) {
    wchar_t buf[PATH_CAP];
    const wchar_t* p = host_path(path, buf);
    HANDLE h = CreateFileW(p, access, share, sa, disposition, flags, template_file);
    log_access("CreateFileW", p, h != INVALID_HANDLE_VALUE);
    return h;
}

static HANDLE WINAPI hk_CreateFile2(LPCWSTR path, DWORD access, DWORD share, DWORD disposition,
                                    LPCREATEFILE2_EXTENDED_PARAMETERS params) {
    wchar_t buf[PATH_CAP];
    const wchar_t* p = host_path(path, buf);
    HANDLE h = CreateFile2(p, access, share, disposition, params);
    log_access("CreateFile2", p, h != INVALID_HANDLE_VALUE);
    return h;
}

static DWORD WINAPI hk_GetFileAttributesW(LPCWSTR path) {
    wchar_t buf[PATH_CAP];
    const wchar_t* p = host_path(path, buf);
    DWORD attributes = GetFileAttributesW(p);
    log_access("GetFileAttributesW", p, attributes != INVALID_FILE_ATTRIBUTES);
    return attributes;
}

static DWORD WINAPI hk_GetFileAttributesA(LPCSTR path) {
    if (!path)
        return GetFileAttributesA(path);
    wchar_t wide[PATH_CAP];
    str_to_wide(wide, PATH_CAP, path);
    return hk_GetFileAttributesW(wide);
}

static BOOL WINAPI hk_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID info) {
    wchar_t buf[PATH_CAP];
    const wchar_t* p = host_path(path, buf);
    BOOL ok = GetFileAttributesExW(p, level, info);
    log_access("GetFileAttributesExW", p, ok != FALSE);
    return ok;
}

static HANDLE WINAPI hk_FindFirstFileW(LPCWSTR pattern, LPWIN32_FIND_DATAW data) {
    wchar_t buf[PATH_CAP];
    const wchar_t* p = host_path(pattern, buf);
    HANDLE h = FindFirstFileW(p, data);
    log_access("FindFirstFileW", p, h != INVALID_HANDLE_VALUE);
    return h;
}

static BOOL WINAPI hk_CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES sa) {
    wchar_t buf[PATH_CAP];
    return CreateDirectoryW(host_path(path, buf), sa);
}

static BOOL WINAPI hk_RemoveDirectoryW(LPCWSTR path) {
    wchar_t buf[PATH_CAP];
    return RemoveDirectoryW(host_path(path, buf));
}

static BOOL WINAPI hk_DeleteFileW(LPCWSTR path) {
    wchar_t buf[PATH_CAP];
    return DeleteFileW(host_path(path, buf));
}

static BOOL WINAPI hk_SetFileAttributesW(LPCWSTR path, DWORD attributes) {
    wchar_t buf[PATH_CAP];
    return SetFileAttributesW(host_path(path, buf), attributes);
}

static BOOL WINAPI hk_MoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags) {
    wchar_t from_buf[PATH_CAP];
    wchar_t to_buf[PATH_CAP];
    return MoveFileExW(host_path(from, from_buf), host_path(to, to_buf), flags);
}

// Whether reads on `file` complete before ReadFile returns: handles opened without
// FILE_FLAG_OVERLAPPED, also when the caller passes an OVERLAPPED just for the file offset.
static bool synchronous_handle(HANDLE file) {
    struct IoStatus {
        LONG_PTR status;
        ULONG_PTR information;
    };
    typedef LONG(WINAPI * QueryFn)(HANDLE, IoStatus*, void*, ULONG, int);
    const int FILE_MODE_INFORMATION_CLASS = 16;
    const ULONG SYNCHRONOUS_IO = 0x00000010 | 0x00000020; // FILE_SYNCHRONOUS_IO_ALERT, _NONALERT
    static QueryFn query = (QueryFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationFile");
    IoStatus io;
    ULONG mode = 0;
    return query && query(file, &io, &mode, sizeof(mode), FILE_MODE_INFORMATION_CLASS) >= 0 &&
           (mode & SYNCHRONOUS_IO);
}

// The kernel reads and writes title buffers without faulting them in, so their pages are backed
// first (see mem_prepare). Title memory may also be write-protected (write watches, store traps),
// which the kernel's writes fail on rather than fault: reads that complete right away go through a
// buffer of ours and are copied in, faulting as usual. Asynchronous reads complete later, so their
// memory is unprotected instead.
static BOOL WINAPI hk_ReadFile(HANDLE file, LPVOID buffer, DWORD size, LPDWORD read,
                               LPOVERLAPPED overlapped) {
    BOOL ok = FALSE;
    bool done = false;
    if (size && mem_is_tracked(buffer) && (!overlapped || synchronous_handle(file))) {
        // Page aligned, as unbuffered reads need sector-aligned buffers.
        void* bounce = VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (bounce) {
            DWORD got = 0;
            ok = ReadFile(file, bounce, size, &got, overlapped); // the offset, if any, comes along
            DWORD error = GetLastError();
            memcpy(buffer, bounce, got);
            VirtualFree(bounce, 0, MEM_RELEASE);
            if (read)
                *read = got;
            SetLastError(error);
            done = true;
        }
    }
    if (!done) {
        mem_end_write_watches(buffer, size);
        mem_prepare(buffer, size);
        ok = ReadFile(file, buffer, size, read, overlapped);
    }
    DWORD error = GetLastError();
    if (!ok && error != ERROR_IO_PENDING && error != ERROR_HANDLE_EOF) {
        static volatile long logged = 0;
        if (InterlockedIncrement(&logged) <= 16)
            LOG_W("file", "ReadFile(%p, %u bytes into %p%s) failed (%lu)", file, size, buffer,
                  overlapped ? ", overlapped" : "", error);
        SetLastError(error);
    }
    return ok;
}

static BOOL WINAPI hk_WriteFile(HANDLE file, LPCVOID buffer, DWORD size, LPDWORD written,
                                LPOVERLAPPED overlapped) {
    mem_prepare(buffer, size);
    return WriteFile(file, buffer, size, written, overlapped);
}

const HleExport g_file_overrides[] = {
    HLE_EXPORT_AS(hk_ReadFile, "ReadFile"),
    HLE_EXPORT_AS(hk_WriteFile, "WriteFile"),
    HLE_EXPORT_AS(hk_CreateFileW, "CreateFileW"),
    HLE_EXPORT_AS(hk_CreateFile2, "CreateFile2"),
    HLE_EXPORT_AS(hk_GetFileAttributesW, "GetFileAttributesW"),
    HLE_EXPORT_AS(hk_GetFileAttributesA, "GetFileAttributesA"),
    HLE_EXPORT_AS(hk_GetFileAttributesExW, "GetFileAttributesExW"),
    HLE_EXPORT_AS(hk_FindFirstFileW, "FindFirstFileW"),
    HLE_EXPORT_AS(hk_CreateDirectoryW, "CreateDirectoryW"),
    HLE_EXPORT_AS(hk_RemoveDirectoryW, "RemoveDirectoryW"),
    HLE_EXPORT_AS(hk_DeleteFileW, "DeleteFileW"),
    HLE_EXPORT_AS(hk_SetFileAttributesW, "SetFileAttributesW"),
    HLE_EXPORT_AS(hk_MoveFileExW, "MoveFileExW"),
};
const u32 g_file_override_count = ARRAY_COUNT(g_file_overrides);
