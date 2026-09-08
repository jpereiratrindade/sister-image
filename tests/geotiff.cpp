#include "sister_image/RasterWindowAnalysis.hpp"
#include <tiffio.h>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <iostream>
#include <unistd.h>
void check(bool v,const char* s){if(!v)throw std::runtime_error(s);}
void register_tags(TIFF* t){
 TIFFFieldInfo fields[]={
 {33550,TIFF_VARIABLE2,TIFF_VARIABLE2,TIFF_DOUBLE,FIELD_CUSTOM,1,1,const_cast<char*>("ModelPixelScaleTag")},
 {33922,TIFF_VARIABLE2,TIFF_VARIABLE2,TIFF_DOUBLE,FIELD_CUSTOM,1,1,const_cast<char*>("ModelTiepointTag")},
 {34735,TIFF_VARIABLE2,TIFF_VARIABLE2,TIFF_SHORT,FIELD_CUSTOM,1,1,const_cast<char*>("GeoKeyDirectoryTag")}};
 TIFFMergeFieldInfo(t,fields,3);
}
int main(){namespace fs=std::filesystem;auto dir=fs::temp_directory_path()/("image-geo-"+std::to_string(getpid()));fs::create_directories(dir);
 try{
 auto* t=TIFFOpen((dir/"input.tif").c_str(),"w");check(t,"create");register_tags(t);
 TIFFSetField(t,TIFFTAG_IMAGEWIDTH,32);TIFFSetField(t,TIFFTAG_IMAGELENGTH,16);TIFFSetField(t,TIFFTAG_SAMPLESPERPIXEL,2);TIFFSetField(t,TIFFTAG_BITSPERSAMPLE,8);TIFFSetField(t,TIFFTAG_PHOTOMETRIC,PHOTOMETRIC_MINISBLACK);TIFFSetField(t,TIFFTAG_PLANARCONFIG,PLANARCONFIG_CONTIG);
 uint16_t alpha=EXTRASAMPLE_UNASSALPHA;TIFFSetField(t,TIFFTAG_EXTRASAMPLES,1,&alpha);
 double scale[]={0.25,0.25,0},tie[]={0,0,0,500000,7000000,0};uint16_t keys[]={1,1,0,1,1024,0,1,1};
 TIFFSetField(t,33550,3,scale);TIFFSetField(t,33922,6,tie);TIFFSetField(t,34735,8,keys);
 std::vector<unsigned char> row(64);for(unsigned x=0;x<32;++x){row[x*2]=x<16?40:200;row[x*2+1]=x<8?0:255;}
 for(unsigned y=0;y<16;++y)check(TIFFWriteScanline(t,row.data(),y)>=0,"write");TIFFClose(t);
 sister_image::RasterWindowAnalysisConfig c;c.raster_path=(dir/"input.tif").string();c.min_window_size_px=c.window_size_px=16;c.compute_anomalies=false;
 auto result=sister_image::analyzeRasterWindows(c);check(result.ignored_pixels==128,"gray alpha transparency");check(result.windows.size()==2,"alpha valid fraction");
 sister_image::RasterClassificationMapConfig m;m.output_path=(dir/"output.tif").string();m.class_count=2;sister_image::writeRasterClassificationMap(result,m);
 t=TIFFOpen(m.output_path.c_str(),"r");check(t,"output");register_tags(t);
 uint32_t count=0;double* values=nullptr;uint16_t* shorts=nullptr;
 check(TIFFGetField(t,33550,&count,&values)==1&&count==3&&values[0]==.25&&values[1]==.25,"pixel scale preserved");
 check(TIFFGetField(t,33922,&count,&values)==1&&count==6&&values[3]==500000&&values[4]==7000000,"tiepoint preserved");
 check(TIFFGetField(t,34735,&count,&shorts)==1&&count==8&&shorts[4]==1024,"geokeys preserved");
 TIFFClose(t);fs::remove_all(dir);std::cout<<"GeoTIFF tags and grayscale alpha passed\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';fs::remove_all(dir);return 1;}}
