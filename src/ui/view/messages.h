#pragma once

// The messages - docs/VIEW_LAYER.md, section 7: the toast, a line for a
// moment at the top of the screen that says what a key or a button just
// did; and the persistence warning, a line along the bottom for as long as
// a save stays failed. Both are drawn on ImGui's foreground list, above
// every window, in edit mode and in the read-only modes alike. What it
// keeps of its own is the message and when it expires, the message for the
// next showing, the settings file that failed, and whether a notice's end
// was reported.

#include <cstddef>
#include <optional>
#include <string>

#include "core/session/session.h"

namespace sz::ui {

class Messages {
public:
    explicit Messages(const core::Session& session) : session_(session) {}

    // `text`, for a moment. Any command reaches this, through
    // EditorViews::Say - between frames too, and before the overlay has
    // ever been shown: a capture hotkey pressed first, while still hidden,
    // with no ImGui context yet to take the time from. Then nothing is said
    // - nothing could be seen anyway.
    void Say(std::string text);
    // Drops the current message without showing it, for the caller that
    // asked for something and then decided nothing may appear on screen
    // (see AppConfig::showToastsWhileHidden). Worth doing rather than
    // leaving it to expire: the clock a message expires on only runs while
    // frames do, so one set while hidden would otherwise still be waiting,
    // hours later, for whenever the overlay next comes up.
    void Dismiss() { text_.clear(); }
    // What the message currently says, empty for none.
    const std::string& Text() const { return text_; }
    // Whether a message is on screen - which keeps frames coming while it
    // fades.
    bool Showing() const;

    // At startup, when the retention period deleted `count` folders and
    // canvases for good: said the next time the overlay comes up, rather
    // than while nobody is looking at it.
    void SayDeletedForGoodAtStart(size_t count, int days);
    // The overlay has come up: what was kept for the next showing is said
    // now, and for long enough to be read.
    void OnOverlayShown();

    // A settings file that could not be written, while it stays unwritten -
    // carried on the same line as a library save that failed.
    void SetConfigWriteFailed(std::optional<std::string> path) { configWriteFailedPath_ = std::move(path); }
    // The warning as it would be drawn this frame, or empty when there is
    // nothing wrong.
    std::string PersistenceWarning() const;

    // A notice just entered: its end is reported when its own message fades.
    void NewNotice() { noticeFinishedReported_ = false; }
    // Whether a notice's message has just faded - or was never set at all,
    // which is the same condition the toast draws nothing on. True once, so
    // a notice that stays up (because the window could not be hidden, say)
    // does not report its end on every frame afterwards.
    bool NoticeJustFinished();

    // Stage 7: the toast, and the persistence warning.
    void Draw();

private:
    void DrawToast();
    void DrawPersistenceWarning();

    const core::Session& session_;
    // The message, and when it expires - on ImGui's clock.
    std::string text_;
    double expiresAtSeconds_ = 0.0;
    // See SayDeletedForGoodAtStart.
    std::string messageForNextShow_;
    // See SetConfigWriteFailed.
    std::optional<std::string> configWriteFailedPath_;
    // See NoticeJustFinished.
    bool noticeFinishedReported_ = false;
};

}  // namespace sz::ui
