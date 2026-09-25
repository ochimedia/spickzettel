#include "core/gpu/texture_cache.h"

namespace sz::core {

void TextureCache::AttachWindow(platform::IOverlayWindow* window) {
    ReleaseAll();
    window_ = window;
    generation_ = window_ != nullptr ? window_->TextureGeneration() : 0;
}

void TextureCache::BeginFrame() {
    CheckDevice();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.lastAsked >= frame_) {
            ++it;
            continue;
        }
        if (it->second.handle != 0) {
            window_->ReleaseTexture(it->second.handle);
        }
        it = entries_.erase(it);
    }
    ++frame_;
}

std::optional<uint64_t> TextureCache::Find(TextureKey key) {
    CheckDevice();
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return std::nullopt;
    }
    it->second.lastAsked = frame_;
    return it->second.handle;
}

uint64_t TextureCache::Put(TextureKey key, TexturePixels pixels, uint64_t revision) {
    CheckDevice();
    if (window_ == nullptr) {
        return 0;
    }
    Drop(key);
    return Make(key, pixels, revision).handle;
}

uint64_t TextureCache::Get(TextureKey key, uint64_t revision, TexturePixels pixels) {
    CheckDevice();
    if (window_ == nullptr) {
        return 0;
    }
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return Make(key, pixels, revision).handle;
    }
    Entry& entry = it->second;
    entry.lastAsked = frame_;
    if (entry.revision == revision) {
        return entry.handle;
    }
    // New pixels the same size go into the texture there is - a stroke
    // raster after each stroke - rather than a texture made and one let go
    // of every time.
    if (entry.handle != 0 && pixels.rgba != nullptr && pixels.width == entry.width &&
        pixels.height == entry.height &&
        window_->UpdateTextureRegion(entry.handle, pixels.rgba, pixels.width, 0, 0, pixels.width, pixels.height)) {
        entry.revision = revision;
        return entry.handle;
    }
    Drop(key);
    return Make(key, pixels, revision).handle;
}

void TextureCache::Drop(TextureKey key) {
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
        return;
    }
    if (it->second.handle != 0 && window_ != nullptr) {
        window_->ReleaseTexture(it->second.handle);
    }
    entries_.erase(it);
}

size_t TextureCache::Held() const {
    size_t held = 0;
    for (const auto& [key, entry] : entries_) {
        (void)key;
        held += entry.handle != 0 ? 1 : 0;
    }
    return held;
}

void TextureCache::CheckDevice() {
    if (window_ == nullptr || window_->TextureGeneration() == generation_) {
        return;
    }
    // Made on a device that is gone: they draw nothing and must not be
    // updated, but each is still given back.
    ReleaseAll();
    generation_ = window_->TextureGeneration();
}

void TextureCache::ReleaseAll() {
    if (window_ != nullptr) {
        for (const auto& [key, entry] : entries_) {
            (void)key;
            if (entry.handle != 0) {
                window_->ReleaseTexture(entry.handle);
            }
        }
    }
    entries_.clear();
}

TextureCache::Entry& TextureCache::Make(TextureKey key, TexturePixels pixels, uint64_t revision) {
    Entry entry;
    entry.revision = revision;
    entry.lastAsked = frame_;
    if (pixels.rgba != nullptr && pixels.width > 0 && pixels.height > 0) {
        entry.handle = window_->CreateTextureFromPixels(pixels.rgba, pixels.width, pixels.height);
        entry.width = pixels.width;
        entry.height = pixels.height;
    }
    return entries_.insert_or_assign(key, entry).first->second;
}

}  // namespace sz::core
