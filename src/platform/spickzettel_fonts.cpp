#include "platform/spickzettel_fonts.h"

#include "generated/font_manrope_medium.h"

namespace sz::platform {

void LoadSpickzettelFonts(ImGuiIO& io) {
    ImFontConfig config;
    // The embedded array is a compile-time constant, not a heap
    // allocation ImGui could take ownership of and later free - without
    // this, ImGui::DestroyContext (or the atlas being rebuilt) would call
    // free() on it and corrupt the binary's own .rodata.
    config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(embedded::kManropeMedium),
                                    static_cast<int>(embedded::kManropeMedium_size), 17.0f, &config);
}

}  // namespace sz::platform
