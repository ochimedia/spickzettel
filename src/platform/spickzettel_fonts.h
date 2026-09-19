#pragma once

#include <imgui.h>

namespace sz::platform {

// Loads the app's UI font (Manrope, embedded at build time - see
// cmake/EmbedFile.cmake and assets/fonts/) into `io`, replacing ImGui's
// built-in bitmap font. Must be called after ImGui::CreateContext() but
// before the render backend's own Init(), which builds the font atlas from
// whatever is in io.Fonts at that point; there is no portable way to force
// a rebuild afterwards, which is why this lives in platform/ rather than
// in the UI. Each backend's Initialize() calls it at the right point.
void LoadSpickzettelFonts(ImGuiIO& io);

}  // namespace sz::platform
