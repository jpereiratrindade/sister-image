#pragma once
#include <tiffio.h>
namespace sister_image {

inline void register_geotiff_tags(TIFF* tif) {
    if (!tif) return;
    TIFFFieldInfo geo_fields[] = {
        {33550, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("ModelPixelScaleTag")},
        {33922, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("ModelTiepointTag")},
        {34264, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("ModelTransformationTag")},
        {34735, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_SHORT,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("GeoKeyDirectoryTag")},
        {34736, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_DOUBLE,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("GeoDoubleParamsTag")},
        {34737, TIFF_VARIABLE2, TIFF_VARIABLE2, TIFF_ASCII,
         FIELD_CUSTOM, 1, 1, const_cast<char*>("GeoAsciiParamsTag")},
    };
    TIFFMergeFieldInfo(tif, geo_fields,
                       static_cast<std::uint32_t>(sizeof(geo_fields) / sizeof(geo_fields[0])));
}

// Disable libtiff's default whole-file mmap and bound decoder allocations.
inline TIFF* open_tiff(const char* path, const char* mode) {
    auto* options = TIFFOpenOptionsAlloc();
    if (!options) return nullptr;
    TIFFOpenOptionsSetMaxSingleMemAlloc(options, 64 * 1024 * 1024);
    auto* image = TIFFOpenExt(path, mode, options);
    TIFFOpenOptionsFree(options);
    if (image) {
        register_geotiff_tags(image);
    }
    return image;
}
}
