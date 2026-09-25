include(FetchContent)

# QOI, a single-header lossless RGBA codec, for captured screenshots.
# Measured on this app's own captures against stb's PNG:
# decode 6-8x faster, encode 22x faster, files ~30% smaller. The repo's
# tags stop at the format freeze, so the pin is a commit.
FetchContent_Declare(
    qoi
    URL https://github.com/phoboslab/qoi/archive/97bacc86a9c4abf5a2d452102dc26546c4c670b9.tar.gz
    URL_HASH SHA256=32704988b24321c91114418e884a3fb61a7d0d0004f3ed07a91e73058022bc3b
)
FetchContent_MakeAvailable(qoi)

add_library(qoi INTERFACE)
target_include_directories(qoi SYSTEM INTERFACE ${qoi_SOURCE_DIR})
