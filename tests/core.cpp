#include "service.hpp"
#include "sister_image/RasterWindowAnalysis.hpp"
#include "sister_image/vector_shape.hpp"
#include "sister_image/raster_clip.hpp"
#include <tiffio.h>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <unistd.h>
namespace fs = std::filesystem;
void check(bool v, const char* text) { if(!v) throw std::runtime_error(text); }
void fixture(const fs::path& p, bool tiled, unsigned bits=8) {
    auto* t=TIFFOpen(p.c_str(),"w");check(t,"create fixture");
    TIFFSetField(t,TIFFTAG_IMAGEWIDTH,64);TIFFSetField(t,TIFFTAG_IMAGELENGTH,32);
    TIFFSetField(t,TIFFTAG_SAMPLESPERPIXEL,1);TIFFSetField(t,TIFFTAG_BITSPERSAMPLE,bits);
    TIFFSetField(t,TIFFTAG_PHOTOMETRIC,PHOTOMETRIC_MINISBLACK);TIFFSetField(t,TIFFTAG_PLANARCONFIG,PLANARCONFIG_CONTIG);
    if(tiled){TIFFSetField(t,TIFFTAG_TILEWIDTH,16);TIFFSetField(t,TIFFTAG_TILELENGTH,16);
        std::vector<unsigned char> tile(256);
        for(unsigned y=0;y<32;y+=16)for(unsigned x=0;x<64;x+=16){
            for(unsigned dy=0;dy<16;++dy)for(unsigned dx=0;dx<16;++dx)tile[dy*16+dx]=(x+dx)<32?30+((x+dx)%2)*10:200+((x+dx)%2)*20;
            check(TIFFWriteTile(t,tile.data(),x,y,0,0)>0,"write tile");
        }
    }else{TIFFSetField(t,TIFFTAG_ROWSPERSTRIP,8);std::vector<unsigned char> row(64*(bits/8));
        for(unsigned x=0;x<64;++x)row[x]=x<32?30+(x%2)*10:200+(x%2)*20;
        for(unsigned y=0;y<32;++y)check(TIFFWriteScanline(t,row.data(),y)>=0,"write row");}
    TIFFClose(t);
}
int main(){
    const auto dir=fs::temp_directory_path()/("sister-image-core-"+std::to_string(getpid()));fs::create_directories(dir);
    try{
        fixture(dir/"strip.tif",false);fixture(dir/"tile.tif",true);fixture(dir/"bad.tif",false,16);
        sister_image::RasterWindowAnalysisConfig c;c.window_size_px=c.min_window_size_px=32;c.compute_anomalies=false;c.ignore_zero_pixels=false;
        c.raster_path=(dir/"strip.tif").string();const auto a=sister_image::analyzeRasterWindows(c);
        c.raster_path=(dir/"tile.tif").string();const auto b=sister_image::analyzeRasterWindows(c);
        check(a.windows.size()==2&&b.windows.size()==2,"two regions");
        for(unsigned i=0;i<2;++i){check(std::abs(a.windows[i].mean_intensity-b.windows[i].mean_intensity)<1e-12,"tile mean equivalence");check(std::abs(a.windows[i].mean_abs_dx-b.windows[i].mean_abs_dx)<1e-12,"tile seam texture equivalence");}
        check(a.windows[0].mean_intensity==35&&a.windows[1].mean_intensity==210,"known means");
        check(a.windows[0].mean_abs_dx==10&&a.windows[1].mean_abs_dx==20,"known texture");
        for(auto mode:{sister_image::RasterClassificationMapConfig::Mode::kFeatureClasses,sister_image::RasterClassificationMapConfig::Mode::kHomogeneousPatches}){
            sister_image::RasterClassificationMapConfig m;m.output_path=(dir/"map.tif").string();m.class_count=2;m.mode=mode;
            sister_image::writeRasterClassificationMap(a,m);
            auto* t=TIFFOpen(m.output_path.c_str(),"r");check(t,"map exists");std::vector<unsigned char> row(64);
            for(unsigned y=0;y<32;++y){check(TIFFReadScanline(t,row.data(),y)>=0,"read map");for(unsigned x=0;x<64;++x)check(row[x]==(x<32?32:255),"known classification raster");}TIFFClose(t);
        }
        c.raster_path=(dir/"bad.tif").string();bool rejected=false;try{(void)sister_image::analyzeRasterWindows(c);}catch(const std::exception&){rejected=true;}check(rejected,"reject 16 bit");
        c.raster_path=(dir/"strip.tif").string();c.nodata_values={35,210};auto masked=sister_image::analyzeRasterWindows(c);check(masked.valid_pixels==2048,"nodata acts on pixels, not window means");
        c.nodata_values={30,40,200,220};masked=sister_image::analyzeRasterWindows(c);check(masked.windows.empty()&&masked.ignored_pixels==2048,"nodata ignores all marked values");
        
        // Test VectorShape GeoJSON and RasterClip
        sister_image::VectorShape shape;
        sister_image::Polygon2D poly;
        poly.outer_ring = {{0,0}, {16,0}, {16,16}, {0,16}, {0,0}};
        poly.compute_bounds();
        shape.polygons.push_back(poly);
        shape.compute_bounds();
        check(shape.contains(8, 8), "point inside poly");
        check(!shape.contains(20, 20), "point outside poly");

        sister_image::ClipConfig clip_cfg;
        clip_cfg.input_tiff = dir / "strip.tif";
        clip_cfg.output_tiff = dir / "clipped.tif";
        clip_cfg.shape = shape;
        auto clip_res = sister_image::clip_raster(clip_cfg);
        check(clip_res.output_width == 17 && clip_res.output_height == 17, "clipped dimensions");

        fs::remove_all(dir);std::cout<<"core classification, strip/tile texture, vector shape and raster clip passed\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';fs::remove_all(dir);return 1;}
}
