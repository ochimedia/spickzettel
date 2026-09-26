#include "ui/interaction/machine.h"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/config/app_config.h"
#include "core/session/session.h"
#include "core/session/settings.h"
#include "ui/editor.h"

namespace sz::ui {
namespace {

// An interaction that answers as it is told and writes down what was done
// to it - the routing of docs/INTERACTIONS.md, section 4.1, seen from
// inside.
class Toy : public Interaction {
public:
    using Answering = std::function<Answer(const Event&)>;
    Toy(Level level, std::string name, std::vector<std::string>& log, Answering answer = nullptr)
        : level_(level), name_(std::move(name)), log_(log), answer_(std::move(answer)) {}
    Level level() const override { return level_; }
    const char* Name() const override { return name_.c_str(); }
    void Begin(const Event& /*event*/, Editor& /*editor*/) override { log_.push_back(name_ + " begun"); }
    Answer Offer(const Event& event, Editor& /*editor*/) override {
        log_.push_back(name_ + " offered");
        return answer_ ? answer_(event) : Answer::Pass();
    }
    void Interrupt(Editor& /*editor*/) override { log_.push_back(name_ + " interrupted"); }
    void Cancel(Editor& /*editor*/) override { log_.push_back(name_ + " cancelled"); }

private:
    Level level_;
    std::string name_;
    std::vector<std::string>& log_;
    Answering answer_;
};

class MachineTest : public ::testing::Test {
protected:
    MachineTest() : settings_(AppConfig{}), editor_(settings_, session_) {
        editor_.Input().SetRoot(std::make_unique<Toy>(Level::Canvas, "Canvas", log_, [this](const Event&) {
            return rootAnswer_ ? rootAnswer_() : Answer::Claim();
        }));
    }
    Machine& Stack() { return editor_.Input(); }
    std::unique_ptr<Toy> Make(Level level, const char* name, Toy::Answering answer = nullptr) {
        return std::make_unique<Toy>(level, name, log_, std::move(answer));
    }
    static Event Key(int key) {
        Event event;
        event.kind = EventKind::KeyDown;
        event.key = key;
        return event;
    }

    Settings settings_;
    Session session_;
    Editor editor_;
    std::vector<std::string> log_;
    std::function<Answer()> rootAnswer_;
};

TEST_F(MachineTest, AnEventGoesDownTheStackUntilOneClaimsIt) {
    Stack().Push(Make(Level::Mode, "Mode"), Event{});
    Stack().Push(Make(Level::Gesture, "Gesture", [](const Event&) { return Answer::Claim(); }), Event{});
    log_.clear();
    Stack().Offer(Key('A'));
    EXPECT_EQ(log_, (std::vector<std::string>{"Gesture offered"}));

    Stack().End(Level::Gesture);
    log_.clear();
    Stack().Offer(Key('A'));
    EXPECT_EQ(log_, (std::vector<std::string>{"Mode offered", "Canvas offered"})) << "passed down";
    EXPECT_EQ(Stack().Describe(), "Canvas / Mode / - / - / - / -");
}

// Finish pops what answered, runs what it produced, and passes the event
// on unless it was used up; Cancel pops, tells it, and uses the event up.
TEST_F(MachineTest, FinishAndCancelPopWhatAnswered) {
    const ItemId a = session_.CreateItem(false, Rect{0, 0, 100, 100}, "A");
    editor_.SelectOnly(a);
    Stack().Push(Make(Level::Popup, "Popup",
                      [](const Event&) { return Answer::Finish(/*usedUp=*/false, Command{CommandId::Copy}); }),
                 Event{});
    log_.clear();
    Stack().Offer(Key('A'));
    EXPECT_EQ(log_, (std::vector<std::string>{"Popup offered", "Canvas offered"}));
    EXPECT_EQ(Stack().At(Level::Popup), nullptr);
    EXPECT_EQ(editor_.LastCommand(), CommandId::Copy) << "what it produced";

    Stack().Push(Make(Level::Gesture, "Gesture", [](const Event&) { return Answer::Cancel(); }), Event{});
    log_.clear();
    Stack().Offer(Key(platform::KeyCombo::kEscape));
    EXPECT_EQ(log_, (std::vector<std::string>{"Gesture offered", "Gesture cancelled"}));
    EXPECT_EQ(Stack().At(Level::Gesture), nullptr);
}

// Starting something ends what the starting level has above it, top down,
// and then begins the new interaction with the event - section 4.2.
TEST_F(MachineTest, StartingSomethingEndsWhatIsAboveIt) {
    Stack().Push(Make(Level::Popup, "Popup"), Event{});
    Stack().Push(Make(Level::Text, "Text"), Event{});
    rootAnswer_ = [this] { return Answer::Start(std::nullopt, Make(Level::Gesture, "Pending")); };
    log_.clear();
    Stack().Offer(Key('A'));
    EXPECT_EQ(log_, (std::vector<std::string>{"Text offered", "Popup offered", "Canvas offered", "Text interrupted",
                                              "Popup interrupted", "Pending begun"}));
    EXPECT_EQ(Stack().Describe(), "Canvas / - / - / - / - / Pending");

    // What takes a level ends what was there, and everything above it.
    Stack().Push(Make(Level::Text, "Text"), Event{});
    EXPECT_EQ(Stack().Describe(), "Canvas / - / - / - / Text / -");
}

// A command's scope says what it ends first: the hand's commands only the
// gesture, the canvas's the gesture, the text and the popup - never the
// mode or a panel.
TEST_F(MachineTest, AScopeEndsWhatItCovers) {
    const auto fill = [&] {
        for (const auto& [level, name] : {std::pair{Level::Mode, "Mode"}, std::pair{Level::Panel, "Panel"},
                                          std::pair{Level::Popup, "Popup"}, std::pair{Level::Text, "Text"},
                                          std::pair{Level::Gesture, "Gesture"}}) {
            if (Stack().At(level) == nullptr) {
                Stack().Push(Make(level, name), Event{});
            }
        }
    };
    fill();
    Stack().EndFor(Scope::Hand);
    EXPECT_EQ(Stack().Describe(), "Canvas / Mode / Panel / Popup / Text / -");
    fill();
    log_.clear();
    Stack().EndFor(Scope::Canvas);
    EXPECT_EQ(Stack().Describe(), "Canvas / Mode / Panel / - / - / -");
    EXPECT_EQ(log_, (std::vector<std::string>{"Gesture interrupted", "Text interrupted", "Popup interrupted"}))
        << "top down";

    // Dispatch ends a command's scope before it runs it.
    fill();
    ASSERT_TRUE(editor_.Dispatch(Command{CommandId::Undo}));
    EXPECT_EQ(Stack().At(Level::Gesture), nullptr);
    EXPECT_NE(Stack().At(Level::Text), nullptr);
}

}  // namespace
}  // namespace sz::ui
