# Embeds files into the binary as generated headers, at configure time, so
# Spickzettel.exe stays one self-contained file with no "asset not found"
# failure mode. No external tool: CMake's own file(READ ... HEX) does the
# conversion, and at ~0.2s for a 100 KB font it is fast enough.

# A binary file (a font) as a byte array:
#   inline constexpr unsigned char <name>[] = {...};
#   inline constexpr unsigned long <name>_size = sizeof(<name>);
function(spickzettel_embed_binary_file INPUT_FILE OUTPUT_HEADER VAR_NAME)
    file(READ "${INPUT_FILE}" _hex_content HEX)
    string(REGEX REPLACE "(..)" "0x\\1," _array_content "${_hex_content}")
    file(WRITE "${OUTPUT_HEADER}"
"// Generated at configure time from ${INPUT_FILE} by cmake/EmbedFile.cmake - do not edit.
#pragma once

namespace sz::platform::embedded {
inline constexpr unsigned char ${VAR_NAME}[] = {${_array_content}};
inline constexpr unsigned long ${VAR_NAME}_size = sizeof(${VAR_NAME});
}  // namespace sz::platform::embedded
")
endfunction()

# A text file as a std::string_view over a byte array, in <namespace>:
#   inline constexpr std::string_view <name>{...};
#
# Three things differ from the binary version. Line endings are normalized
# to LF *before* the hex conversion (a CRLF checkout would otherwise show a
# stray glyph per line, and "0d0a" can straddle a byte boundary in the hex
# string, so replacing it there would corrupt content). The text goes
# through hex rather than a raw string literal, because a raw literal needs
# a delimiter the text never contains and configure_file would expand @VAR@
# inside the prose. And it is not NUL-terminated: the string_view carries
# the length, so a NUL in the file cannot truncate it.
function(spickzettel_embed_text_file INPUT_FILE OUTPUT_HEADER VAR_NAME NAMESPACE)
    file(READ "${INPUT_FILE}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")
    string(HEX "${_text}" _hex_content)
    string(REGEX REPLACE "(..)" "0x\\1," _array_content "${_hex_content}")
    file(WRITE "${OUTPUT_HEADER}"
"// Generated at configure time from ${INPUT_FILE} by cmake/EmbedFile.cmake - do not edit.
#pragma once

#include <string_view>

namespace ${NAMESPACE} {
inline constexpr char ${VAR_NAME}_bytes[] = {${_array_content}};
inline constexpr std::string_view ${VAR_NAME}{${VAR_NAME}_bytes, sizeof(${VAR_NAME}_bytes)};
}  // namespace ${NAMESPACE}
")
endfunction()
