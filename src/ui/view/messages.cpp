#include "ui/view/messages.h"

#include <cstdio>
#include <utility>

#include "generated/ui_strings.h"
#include "ui/theme.h"

#include <imgui.h>

namespace sz::ui {

namespace {
// As long as a message kept for the next showing is up (see
// OnOverlayShown): a line to read, and this one names a path.
constexpr double kFailedWriteSeconds = 8.0;
}  // namespace

void Messages::Say(std::string text) {
    // ImGui::GetTime() dereferences the current context unconditionally, so
    // asking it with none set is a crash, not a graceful no-op.
    if (!ImGui::GetCurrentContext()) {
        return;
    }
    text_ = std::move(text);
    expiresAtSeconds_ = ImGui::GetTime() + 2.2;
}

bool Messages::Showing() const {
    return !text_.empty() && ImGui::GetCurrentContext() != nullptr && ImGui::GetTime() < expiresAtSeconds_;
}

bool Messages::Timed() const {
    return Showing() || (ImGui::GetCurrentContext() != nullptr && ImGui::GetTime() < failedWriteSaidUntilSeconds_);
}

void Messages::SayDeletedForGoodAtStart(size_t count, int days) {
    if (count == 0) {
        return;
    }
    char text[160];
    if (count == 1) {
        std::snprintf(text, sizeof(text), strings::kToastPurgedOne, days);
    } else {
        std::snprintf(text, sizeof(text), strings::kToastPurgedMany, count, days);
    }
    messageForNextShow_ = text;
}

void Messages::OnOverlayShown() {
    if (!messageForNextShow_.empty() && ImGui::GetCurrentContext() != nullptr) {
        text_ = std::move(messageForNextShow_);
        messageForNextShow_.clear();
        expiresAtSeconds_ = ImGui::GetTime() + 8.0;
    }
}

bool Messages::NoticeJustFinished() {
    if (noticeFinishedReported_ || Showing()) {
        return false;
    }
    noticeFinishedReported_ = true;
    return true;
}

void Messages::Draw() {
    DrawToast();
    // A write that failed since the last frame is said from now - see
    // PersistenceWarning.
    if (session_.FailedWrites() != failedWritesSaid_) {
        failedWritesSaid_ = session_.FailedWrites();
        failedWriteSaidUntilSeconds_ = ImGui::GetTime() + kFailedWriteSeconds;
    }
    DrawPersistenceWarning();
}

void Messages::DrawToast() {
    if (!Showing()) {
        return;
    }
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(text_.c_str());
    constexpr float kPaddingX = 16.0f;
    constexpr float kPaddingY = 9.0f;
    const ImVec2 boxSize(textSize.x + Px(kPaddingX) * 2.0f, textSize.y + Px(kPaddingY) * 2.0f);
    const ImVec2 boxMin((ImGui::GetIO().DisplaySize.x - boxSize.x) * 0.5f, Px(22.0f));
    const ImVec2 boxMax(boxMin.x + boxSize.x, boxMin.y + boxSize.y);
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(18, 20, 26, 235), 999.0f);
    drawList->AddText(ImVec2(boxMin.x + Px(kPaddingX), boxMin.y + Px(kPaddingY)), IM_COL32(240, 242, 245, 255),
                      text_.c_str());
}

std::string Messages::PersistenceWarning() const {
    std::string warning;
    char line[1024];
    const bool failed = session_.LastWriteFailed() || session_.FailedWrites() != failedWritesSaid_ ||
                        (ImGui::GetCurrentContext() != nullptr && ImGui::GetTime() < failedWriteSaidUntilSeconds_);
    if (failed && session_.Store() != nullptr) {
        std::snprintf(line, sizeof(line), strings::kStatusWriteFailed, session_.Store()->File().string().c_str());
        warning = line;
    }
    if (configWriteFailedPath_.has_value()) {
        std::snprintf(line, sizeof(line), strings::kStatusConfigWriteFailed, configWriteFailedPath_->c_str());
        if (!warning.empty()) {
            warning += "\n";
        }
        warning += line;
    }
    return warning;
}

void Messages::DrawPersistenceWarning() {
    const std::string warning = PersistenceWarning();
    if (warning.empty()) {
        return;
    }
    // Along the bottom, out from under the canvas bar's own reveal zone and
    // away from the toast at the top, in the toast's own colors but with a
    // warning tint behind the text: this one does not go away by itself.
    ImDrawList* drawList = ImGui::GetForegroundDrawList();
    const ImVec2 textSize = ImGui::CalcTextSize(warning.c_str());
    constexpr float kPaddingX = 16.0f;
    constexpr float kPaddingY = 9.0f;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 boxSize(textSize.x + Px(kPaddingX) * 2.0f, textSize.y + Px(kPaddingY) * 2.0f);
    const ImVec2 boxMin((display.x - boxSize.x) * 0.5f, display.y - boxSize.y - Px(64.0f));
    const ImVec2 boxMax(boxMin.x + boxSize.x, boxMin.y + boxSize.y);
    drawList->AddRectFilled(boxMin, boxMax, IM_COL32(92, 40, 20, 235), Px(8.0f));
    drawList->AddText(ImVec2(boxMin.x + Px(kPaddingX), boxMin.y + Px(kPaddingY)), IM_COL32(255, 232, 210, 255),
                      warning.c_str());
}

}  // namespace sz::ui
