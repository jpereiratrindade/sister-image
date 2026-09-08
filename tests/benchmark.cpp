#include "service.hpp"
#include <iostream>
int main(int argc,char** argv){
    if(argc!=3){std::cerr<<"image_benchmark INPUT_TIFF OUTPUT_DIRECTORY\n";return 2;}
    try{
        const auto dir=std::filesystem::absolute(argv[2]);
        if(std::filesystem::exists(dir))throw std::runtime_error("Use um diretorio de saida novo");
        std::filesystem::create_directories(dir);
        std::filesystem::create_symlink(std::filesystem::absolute(argv[1]),dir/"input.tif");
        auto report=sister_image::classify(dir,{{"window",512},{"classes",7},{"mode","classes"},{"homogeneity",0.85},{"min_valid",0.25},{"ignore_zero",true},{"nodata",sister_image::Json::array()}});
        std::cout<<report.dump(2)<<'\n';return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
