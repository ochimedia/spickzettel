include(FetchContent)

# SQLite, for the library: one file, and a change to it lands whole or not
# at all. The amalgamation - the whole library as one C file and its
# header - from sqlite.org's own release, 3.53.4. sqlite.org publishes a
# SHA3-256 beside each download; this one was checked against it
# (628a44cf...934e) before its SHA-256 was taken for the pin.
FetchContent_Declare(
    sqlite
    URL https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip
    URL_HASH SHA256=1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d
)
FetchContent_MakeAvailable(sqlite)

add_library(sqlite STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite SYSTEM PUBLIC ${sqlite_SOURCE_DIR})
# What the library is used for and nothing else: one thread, no extensions
# loaded at run time, no deprecated interfaces, and double-quoted strings
# treated as the identifiers they are in standard SQL rather than quietly
# as string literals. Without threads or extensions it also needs neither
# pthreads nor libdl on Linux.
target_compile_definitions(sqlite PRIVATE
    SQLITE_THREADSAFE=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_DEPRECATED
    SQLITE_DQS=0
    SQLITE_DEFAULT_MEMSTATUS=0
)
