# A generator for synthetic libraries to measure the overlay against - see
# main.cpp and docs/PERF.md. Links sz_core only: it needs the canvas model
# and the persistence layer, and nothing that draws or talks to an OS.
add_executable(perf_library main.cpp)
target_link_libraries(perf_library PRIVATE sz_core)
target_compile_features(perf_library PRIVATE cxx_std_20)
spickzettel_set_warnings(perf_library)
