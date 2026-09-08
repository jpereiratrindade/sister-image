#pragma once
#include <tiffio.h>
namespace sister_image {
// Disable libtiff's default whole-file mmap and bound decoder allocations.
inline TIFF* open_tiff(const char* path, const char* mode) {
    auto* options = TIFFOpenOptionsAlloc();
    if (!options) return nullptr;
    TIFFOpenOptionsSetMaxSingleMemAlloc(options, 64 * 1024 * 1024);
    auto* image = TIFFOpenExt(path, mode, options);
    TIFFOpenOptionsFree(options);
    return image;
}
}
