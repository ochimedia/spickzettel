# Compiles assets/ui_strings.json into a C++ header of string constants, at
# configure time - so every word the app shows can be edited, reviewed and
# proofread in one file, without the exe gaining a data file it has to find
# at runtime. Same shape as the embedded About text (cmake/EmbedFile.cmake);
# the only new part is that this one has to be parsed rather than copied.
#
# Parsed by CMake itself (string(JSON ...), available since 3.19; this
# project requires 3.21), deliberately: scripts/gen_icons.py is kept out of
# the build precisely so that building needs no Python, and a catalog that
# people edit often is the last thing that should reintroduce a toolchain
# dependency.
#
# Keys are dotted paths, grouped by where the text appears, and become
# PascalCase identifiers with a k prefix:
#     "appearance.showItemBorders"  ->  kAppearanceShowItemBorders
# The dots are for the human reading the JSON; nothing in the build cares
# about the grouping. A key starting with an underscore is a note to
# whoever is reading the file and produces nothing, JSON having no comments
# of its own.
#
# Usage: spickzettel_generate_ui_strings(<input-json> <output-header>
#                                         <namespace> [EDITABLE])
#
# EDITABLE is the string editor's build (SPICKZETTEL_STRING_EDITOR, see
# docs/STRING_EDITOR.md): each string is then a char array with room to
# grow, which the editor rewrites in place, and the constant a pointer to
# it - still a constant, so every table that holds one sees the edit. And
# a list of them all, by key, with the path of the file they came from.

# The delimiter for the raw string literals below. Raw literals are what
# make this safe: no escaping of quotes, backslashes or percent signs, so
# what is in the JSON is exactly what the program shows, and no character
# in the text can end the literal early - except this delimiter itself,
# which is why every value is checked for it.
set(SPICKZETTEL_UI_STRING_DELIM "SZTXT")

function(spickzettel_generate_ui_strings INPUT_JSON OUTPUT_HEADER NAMESPACE)
    cmake_parse_arguments(PARSE_ARGV 3 _arg "EDITABLE" "" "")
    file(READ "${INPUT_JSON}" _json)

    string(JSON _count ERROR_VARIABLE _err LENGTH "${_json}")
    if(_err)
        message(FATAL_ERROR "spickzettel: ${INPUT_JSON} is not a JSON object: ${_err}")
    endif()

    # A key written twice is no error to string(JSON): its parser keeps
    # the last value and drops the other without a word, so the text a
    # key was added with never shows, and the key it collided with may be
    # the one that changes. Found on 2026-10-03, when a new tutorial
    # step's strings took the keys of an existing one. Read off the text,
    # as the parsed object no longer has the twin: one key a line, as the
    # file is laid out.
    file(STRINGS "${INPUT_JSON}" _keyLines REGEX "^[ \t]*\"[^\"]+\"[ \t]*:")
    set(_written "")
    foreach(_line IN LISTS _keyLines)
        if(NOT _line MATCHES "^[ \t]*\"([^\"]+)\"[ \t]*:")
            continue()
        endif()
        if(CMAKE_MATCH_1 IN_LIST _written)
            message(FATAL_ERROR "spickzettel: ui string key '${CMAKE_MATCH_1}' is written twice in ${INPUT_JSON}")
        endif()
        list(APPEND _written "${CMAKE_MATCH_1}")
    endforeach()

    set(_body "")
    set(_editable "")
    set(_seen "")
    math(EXPR _last "${_count} - 1")
    foreach(_i RANGE 0 ${_last})
        string(JSON _key MEMBER "${_json}" ${_i})
        string(JSON _value ERROR_VARIABLE _err GET "${_json}" "${_key}")
        if(_err)
            message(FATAL_ERROR "spickzettel: ui string '${_key}' is not a string: ${_err}")
        endif()

        # JSON has no comments, and a file people are meant to edit needs
        # to be able to explain itself. A leading underscore means "this is
        # a note to the reader", and nothing is emitted for it.
        if(_key MATCHES "^_")
            continue()
        endif()

        # Dotted path to identifier. Only the first letter of each segment
        # is touched, so "showItemBorders" keeps its own humps.
        string(REPLACE "." ";" _parts "${_key}")
        set(_ident "k")
        foreach(_part IN LISTS _parts)
            if(NOT _part MATCHES "^[A-Za-z][A-Za-z0-9]*$")
                message(FATAL_ERROR
                    "spickzettel: ui string key '${_key}' has a segment ('${_part}') that is not a "
                    "plain identifier - keys are dotted lowerCamelCase words")
            endif()
            string(SUBSTRING "${_part}" 0 1 _head)
            string(SUBSTRING "${_part}" 1 -1 _tail)
            string(TOUPPER "${_head}" _head)
            string(APPEND _ident "${_head}${_tail}")
        endforeach()

        # Two different keys can mangle to one identifier ("a.bC" and
        # "a.b.c"), which would otherwise be a redefinition error pointing
        # at a generated file rather than at the mistake.
        if(_ident IN_LIST _seen)
            message(FATAL_ERROR
                "spickzettel: ui string keys collide - '${_key}' produces ${_ident}, which another "
                "key already produced")
        endif()
        list(APPEND _seen "${_ident}")

        string(REPLACE "\r\n" "\n" _value "${_value}")
        string(FIND "${_value}" ")${SPICKZETTEL_UI_STRING_DELIM}\"" _closes)
        if(NOT _closes EQUAL -1)
            message(FATAL_ERROR
                "spickzettel: ui string '${_key}' contains the raw-literal delimiter "
                "')${SPICKZETTEL_UI_STRING_DELIM}\"', which would end its literal early")
        endif()

        if(_arg_EDITABLE)
            # Twice its length and a little more: room for an edit to be
            # shown as it is typed. A longer one is saved all the same, and
            # shown once built.
            string(LENGTH "${_value}" _length)
            math(EXPR _room "${_length} * 2 + 64")
            string(APPEND _body
                "inline char ${_ident}_text[${_room}] =\n"
                "    R\"${SPICKZETTEL_UI_STRING_DELIM}(${_value})${SPICKZETTEL_UI_STRING_DELIM}\";\n"
                "inline constexpr const char* ${_ident} = ${_ident}_text;\n")
            string(APPEND _editable "    {\"${_key}\", ${_ident}_text, sizeof(${_ident}_text)},\n")
        else()
            string(APPEND _body
                "inline constexpr const char* ${_ident} =\n"
                "    R\"${SPICKZETTEL_UI_STRING_DELIM}(${_value})${SPICKZETTEL_UI_STRING_DELIM}\";\n")
        endif()
    endforeach()

    if(_arg_EDITABLE)
        string(APPEND _body "
// The string editor's: every string by its key, where it lives and how much
// it holds, terminator included.
struct EditableString {
    const char* key;
    char* text;
    unsigned long long room;
};
inline EditableString kEditable[] = {
${_editable}};
inline constexpr const char* kCatalogFile = R\"${SPICKZETTEL_UI_STRING_DELIM}(${INPUT_JSON})${SPICKZETTEL_UI_STRING_DELIM}\";
")
    endif()

    file(WRITE "${OUTPUT_HEADER}"
"// Generated at configure time from ${INPUT_JSON} by cmake/UiStrings.cmake
// - do not edit. Edit the JSON; CMake re-runs on its own when you do.
// Lives under the build directory (build/<preset>/generated/), already
// covered by .gitignore's blanket build/ rule.
#pragma once

namespace ${NAMESPACE} {

${_body}
}  // namespace ${NAMESPACE}
")
endfunction()
