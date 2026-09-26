#pragma once

// The input machine of docs/INTERACTIONS.md, sections 4 and 4.1: a stack
// of interactions, one per level, innermost on top, which every event is
// offered to from the top down. Each interaction answers with one of five
// answers; what it does with the event is its own, and what the answer
// does to the stack is the routing below - a dozen lines, the same for
// every interaction. No ImGui: events in, editor changes out.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "ui/interaction/command.h"
#include "ui/interaction/event.h"

namespace sz::ui {

class Editor;

// Bottom to top - the order is fixed, and each level holds at most one
// interaction (section 4).
enum class Level {
    Canvas,   // the canvas and its selection - always there
    Mode,     // drawing mode on a snippet; a creation tool in hand
    Panel,    // the Overview; the cheat sheet
    Popup,    // a context menu, the Properties popover, the color chooser, a delete confirmation
    Text,     // a note being typed, a name being edited, a key being captured
    Gesture,  // what a held button or key is doing
};
inline constexpr size_t kLevelCount = 6;

class Interaction;

// What an interaction answers an event with - section 4.1.
struct Answer {
    enum class Kind {
        Claim,   // mine; handled; stop
        Pass,    // not mine; offer it to the level below
        Finish,  // done, as meant to end: popped - the event used up or passed on
        Cancel,  // done, leaving no trace: popped, and the event used up
        Start,   // this begins something: a command, a new interaction, or both
    };
    Kind kind = Kind::Pass;
    // For Finish: whether the event goes no further.
    bool usedUp = true;
    // For Start, and what a Finish produced: run once the answer has done
    // what it does to the stack, through Editor::Dispatch - which ends what
    // its scope covers first.
    std::optional<Command> command;
    // For Start: pushed after the command, ending what is on its level and
    // above it first (see Machine::Push).
    std::unique_ptr<Interaction> push;

    static Answer Claim() { return Answer{Kind::Claim}; }
    static Answer Pass() { return Answer{Kind::Pass}; }
    static Answer Finish(bool usedUp = true, std::optional<Command> produced = std::nullopt) {
        return Answer{Kind::Finish, usedUp, std::move(produced)};
    }
    static Answer Cancel() { return Answer{Kind::Cancel}; }
    static Answer Start(std::optional<Command> command, std::unique_ptr<Interaction> push = nullptr) {
        return Answer{Kind::Start, true, std::move(command), std::move(push)};
    }
};

// Anything that lasts from a beginning to an end and wants a say in the
// events that arrive meanwhile - section 2.
//
// None of these may change the stack itself - push, end or replace
// anything - since the machine is in the middle of routing through it when
// they are called: that is what the answers are for. An editor change that
// is not the stack's (a stroke's points, the selection) is theirs to make.
class Interaction {
public:
    virtual ~Interaction() = default;
    virtual Level level() const = 0;
    // A stable name, for tests and logs.
    virtual const char* Name() const = 0;
    // Which interaction this is, of every one made while the app runs - for
    // a test or a log to tell one from the next of the same kind, which may
    // well be made where the last one was.
    uint64_t Serial() const { return serial_; }
    // Once pushed, with the event that began it.
    virtual void Begin(const Event& /*event*/, Editor& /*editor*/) {}
    // The event, and what it means here: a switch over the event kinds
    // with no default, so that every pair of state and event has an
    // answer the compiler holds - section 4.3.
    virtual Answer Offer(const Event& event, Editor& editor) = 0;
    // Ended from outside - a command's scope, something started below it:
    // keep what is done, make nothing new.
    virtual void Interrupt(Editor& editor) = 0;
    // Ended by the user, through a Cancel answer: leave no trace.
    virtual void Cancel(Editor& editor) = 0;

private:
    static uint64_t NextSerial() {
        static uint64_t next = 0;
        return ++next;
    }
    const uint64_t serial_ = NextSerial();
};

class Machine {
public:
    explicit Machine(Editor& editor) : editor_(editor) {}

    // The Canvas level's interaction, which is always there.
    void SetRoot(std::unique_ptr<Interaction> root);

    // Routes `event` from the top of the stack down - section 4.1. An
    // event nobody claims is dropped: that is the Canvas level's answer to
    // anything it has no use for.
    void Offer(const Event& event);

    // Puts `interaction` on its level, ending what is there and everything
    // above it first, top down (interrupted), and begins it with `cause`.
    // For a command's Run - drawing mode entered, a popup opened - and the
    // routing's own Start.
    void Push(std::unique_ptr<Interaction> interaction, const Event& cause);
    // Ends what a command's scope covers, top down, interrupted - section
    // 4.2.
    void EndFor(Scope scope);
    // Ends the interaction at `level` (and nothing else), interrupted - for
    // a view whose widget closed it.
    void End(Level level);

    Interaction* At(Level level) const { return stack_[static_cast<size_t>(level)].get(); }
    // The interaction at `level`, if it is a T.
    template <class T>
    T* As(Level level) const {
        return dynamic_cast<T*>(At(level));
    }
    // The overlay has just come up: whatever was held when it went away
    // has been let go of since, wherever that release went. The Gesture
    // level is cleared (interrupted) and the buttons are forgotten - see
    // docs/INTERACTIONS.md, section 9, "Shown".
    void Forget();
    // The gesture buttons down, as the events have said (ButtonBit).
    uint8_t HeldButtons() const { return held_; }

    // The stack, bottom to top, as names - "Canvas" and "-" for an empty
    // level: "Canvas / - / - / - / - / Pending". For tests and logs.
    std::string Describe() const;

private:
    // Offers the event down the stack - the routing proper.
    void Route(const Event& event);
    // Takes the interaction at `index` off the stack and interrupts it.
    void InterruptAt(size_t index);
    // A button held with nothing on the Gesture level has had its say: the
    // rest of its press is Spent (section 6.3). Asked after every change,
    // which is what makes a gesture ended while its button is down -
    // cancelled, interrupted, a hold that acted, a double-click acted on at
    // its press - leave the rest of the drag doing nothing, whatever ended
    // it.
    void LeaveSpentIfHeld();
    // Section 4: every interaction is on its own level. Checked after
    // every change in a debug build.
    void CheckLevels() const;

    Editor& editor_;
    std::array<std::unique_ptr<Interaction>, kLevelCount> stack_;
    uint8_t held_ = 0;
    platform::MouseButton lastPressed_ = platform::MouseButton::Left;
};

}  // namespace sz::ui
