# The string editor

A build for rewording the app where the words are seen: F2 over any text,
or F3 over anything with a tooltip, opens a small window on the string in
`assets/ui_strings.json` it came from, the edit shows on screen as it is typed, and Save writes it to the
file. Built on 2026-09-29 as an experiment, for a pass over the strings
before a release. Never handed out.

## Using it

- Build it with the `windows-msvc-strings` preset. It is the release
  build with the editor in, named "Spickzettel Strings.exe" and copied to
  `dist/`. Quit the everyday copy first: the two share their settings and
  library.
- In edit mode, point at any text and press F2. The window opens next to
  the text, which gets a ring.
  - One string: its key and a field with its text.
  - Several strings that read the same ("Copy" is four), or text that
    only fits with what the app fills in, or only in part: a list of them,
    the first chosen. Choose another from the list.
  - Nothing found, such as text built from pieces or the user's own: the
    text, and a search over keys and texts.
- For a tooltip, point at what it explains and press F3. There is no need
  to wait for the tooltip, and help tooltips switched off in Settings work
  too. A tooltip made of two strings lists both.
- Type; the app shows the edit as it goes. Save (or Ctrl+Enter) writes it
  to the file; Cancel (or Esc) puts the text back.
- Menus and the delete confirmation stay open while the window is up, so
  their words can be edited where they are.
- The next build of any preset compiles the edit in: CMake re-runs on its
  own when the file changes.

## Limits

- **Fields the code fills in** have to stay: the same `%s`, `%d` and so on
  in the same order, a percent sign as `%%`, and no `{name}` the string did
  not have - except in the tutorial's texts, where any `{key:<command>}`
  and `{trigger:...}` can be added. An edit that breaks one is not shown or
  saved, and says why.
- **Marked names,** `{ui:Show deleted}`, are no field: they can be added
  and removed, but only in the tutorial's texts and the help texts (keys
  ending in `Help`), the ones drawn with their names marked.
- **An edit much longer than the text** is saved but not shown until the
  next build: each string has room for twice its length and a little more.
- **Text made once and kept,** a string copied into a longer text when a
  screen opens, shows the edit only once that is made again.
- **A tooltip shows the edit** only while the pointer stays over what it
  explains.
- While the window is open, the app gets no input of its own; ImGui's
  widgets still take clicks.

## How it works

- **The strings are writable.** In this build `cmake/UiStrings.cmake`
  makes each string a char array with room to grow, and its constant a
  pointer to it: still a constant, so no code that uses one changes, and
  every table that holds one sees an edit. A list of all of them by key,
  and the file's path, come with them.
- **What was drawn where.** All text ImGui draws, a widget's or an
  `AddText` of the app's own, goes through `ImFont::RenderText`. This
  build compiles a copy of `imgui_draw.cpp` with one call added at its
  start (`cmake/FetchImGui.cmake`); the fetched source is left alone. The
  text ledger (`src/ui/string_editor/text_ledger.cpp`) keeps each frame's
  text with its rectangle, and once the frame is rendered, which draw list
  is on top of which.
- **Tooltips** all go through `HelpTooltip` and `InfoTooltip` in
  `src/ui/widgets.cpp`, each frame the pointer is over what one explains.
  In this build they hand the ledger the text, and the catalog string it
  was made from: the one passed for `"%s"`, or the format itself.
- **Text laid out a word at a time** (`WrappedSpans`: the tutorial's
  cards, the help boxes and help tooltips) is recorded as one text, not
  as its words, so it is matched as the string it is.
- **From text to key** (`string_match.cpp`): text drawn straight from a
  catalog string is that string. Otherwise it is matched by its words:
  exactly; with `%s`, `{program}` and the like as anything, and a
  `{ui:}` name as the name; or in part.
- **Saving** changes only that value's line in the file, so comments and
  layout stay as they are. Every value in the catalog is tested to write
  back byte for byte.
- F2 and F3 are taken before the input machine sees them, and the window is drawn
  over everything once the surfaces are stacked. While a popup is open,
  the window tells ImGui it sits within it (`ParentWindowInBeginStack`),
  so focusing it does not close a menu, and a modal leaves it the input.
  Nothing of this is in any other build.
