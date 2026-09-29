#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>

#include "platform/i_overlay_window.h"

namespace sz::core {

// What a texture shows, which is also the name it is kept under: nothing
// outside TextureCache holds a handle from one frame to the next, only this.
struct TextureKey {
    enum class Kind : uint8_t {
        Picture,       // a snippet's picture, full size
        Thumbnail,     // the same scaled down, for a preview of a canvas
        FrozenScreen,  // the screen held while the overlay is up
    };
    Kind kind = Kind::Picture;
    uint64_t id = 0;  // the snippet's; 0 for the frozen screen
    bool operator==(const TextureKey&) const = default;
};

// Pixels to make a texture from, borrowed for the call: width*height RGBA8,
// row-major, no row padding - the layout platform::CaptureResult uses. No
// pixels at all (`rgba` null) stands for "there are none to show".
struct TexturePixels {
    const uint8_t* rgba = nullptr;
    int width = 0;
    int height = 0;
};

// Every GPU texture the app makes, and the one place that holds their
// handles - see ARCHITECTURE.md, "Textures". A texture is asked for by what
// it shows (a TextureKey) whenever it is about to be drawn, and made from
// its pixels when there is none; a handle is good for the frame it was
// asked for in and is never kept past it. That is what makes the three ways
// a handle goes bad this class's alone to handle:
//
// - The device is replaced (see IOverlayWindow::TextureGeneration): every
//   texture is let go of before any is handed out again, and each is made
//   again the next time it is asked for.
// - Nobody draws it any more: a texture not asked for through a whole frame
//   is released at the start of the next (see BeginFrame). What leaves the
//   screen - a canvas switched away from, a snippet deleted, a panel closed
//   - gives its textures back without anyone saying so.
// - It is released while a frame still draws it: the renderer holds such a
//   release until the frame is drawn (see Win32Dx11Renderer::ReleaseTexture).
//
// A texture that could not be made - no pixels to make it from, or an upload
// that failed - is kept as a handle of 0, so that a picture that cannot be
// read is not read again on every frame; it is tried again once it has gone
// unasked for a frame, or the device has been replaced.
//
// Without a window nothing is made, and every answer is "none". Everything
// held is given back when the cache goes, so the window must outlive it.
class TextureCache {
public:
    TextureCache() = default;
    ~TextureCache() { ReleaseAll(); }
    TextureCache(const TextureCache&) = delete;
    TextureCache& operator=(const TextureCache&) = delete;

    // Where textures are made. Everything made on the window before - if
    // any - is released first. Null detaches.
    void AttachWindow(platform::IOverlayWindow* window);

    // Once a frame, before anything draws: releases every texture no one
    // asked for during the frame before.
    void BeginFrame();

    // The texture for `key` as it stands: its handle - 0 for one that could
    // not be made - or nullopt when there is none, made or tried. Counts as
    // asked for.
    std::optional<uint64_t> Find(TextureKey key);
    // Makes the texture for `key` from `pixels`, in place of any there was,
    // and returns its handle: 0, kept as such, when there are no pixels or
    // the upload failed. `revision` is what Get compares.
    uint64_t Put(TextureKey key, TexturePixels pixels, uint64_t revision = 0);
    // The texture for `key`, from pixels at hand that change: as it is when
    // it was made from `revision`, and otherwise made again from `pixels` -
    // uploaded into the one there was when they are the same size.
    uint64_t Get(TextureKey key, uint64_t revision, TexturePixels pixels);
    // Releases `key`'s texture now rather than a frame after it was last
    // drawn: for one too big to leave waiting while nothing is drawn, like
    // the frozen screen when the overlay hides.
    void Drop(TextureKey key);

    // How many textures are held, failed ones included - for the tests.
    size_t Size() const { return entries_.size(); }
    // How many of those are textures, failed ones left out - what the
    // window should hold, and no more.
    size_t Held() const;

private:
    struct KeyHash {
        size_t operator()(const TextureKey& key) const {
            return std::hash<uint64_t>{}(key.id * 4u + static_cast<uint64_t>(key.kind));
        }
    };
    struct Entry {
        uint64_t handle = 0;
        uint64_t revision = 0;
        int width = 0;
        int height = 0;
        uint64_t lastAsked = 0;
    };

    // Lets go of everything, if the device the textures were made on has
    // been replaced since. Before every answer, so none is ever a handle
    // from a device that is gone.
    void CheckDevice();
    void ReleaseAll();
    Entry& Make(TextureKey key, TexturePixels pixels, uint64_t revision);

    platform::IOverlayWindow* window_ = nullptr;
    uint64_t generation_ = 0;
    // The frame being drawn - see BeginFrame.
    uint64_t frame_ = 0;
    std::unordered_map<TextureKey, Entry, KeyHash> entries_;
};

}  // namespace sz::core
