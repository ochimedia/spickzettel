include(FetchContent)

# nlohmann/json, for config.json and the library records: nested records
# with arbitrary user-entered names are what a real JSON parser is for.
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)
