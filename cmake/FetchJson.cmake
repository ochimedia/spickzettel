include(FetchContent)

# nlohmann/json, for config.json and the library records: nested records
# with arbitrary user-entered names are what a real JSON parser is for.
# The release's own json.tar.xz, which the project publishes for exactly
# this: the headers and the CMake files, without the repository's 300 MB of
# test data and history.
FetchContent_Declare(
    json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
    URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)
