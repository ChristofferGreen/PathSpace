#include "catch.hpp"


int main(int argc, char** argv) {
    //FSNG::Forge::CreateSingleton();
    //for(auto i = 0; i < argc; ++i)
        //LOG("{}", argv[i]);
    
    auto const ret = Catch::Session().run(argc, argv);
    //FSNG::Forge::DestroySingleton();
    return ret;
}