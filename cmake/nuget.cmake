# Fetches a NuGet package (a zip archive) at configure time and exposes its root directory as
# <name>_SOURCE_DIR. Pinned by version and SHA-256.
function(sludge_fetch_nuget name package version sha256)
    FetchContent_Declare(${name}
        URL https://www.nuget.org/api/v2/package/${package}/${version}
        URL_HASH SHA256=${sha256}
        DOWNLOAD_NAME ${package}.${version}.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(${name})
    set(${name}_SOURCE_DIR ${${name}_SOURCE_DIR} PARENT_SCOPE)
endfunction()
