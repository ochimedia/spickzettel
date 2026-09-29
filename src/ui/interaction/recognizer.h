#pragma once

// What a press on the canvas means - docs/INTERACTIONS.md, section 6. One
// function whose rules are tried in order; the first that matches decides
// what the press does at once and which interaction it starts. The rules,
// in the order they are tried:
//
//  #  | Press                                            | At once           | Click       | Drag          | Hold          | Double
//  1  | any, over an ImGui window                        | -                 | -           | (ImGui's)     | -             | -
//  2  | left, on a bar button; right, on the pen's or the eraser's | -       | -           | Bar button    | its menu      | -
//  3  | left, on a selected snippet's handle             | -                 | -           | Resize        | -             | -
//  4  | left, on the drawing snippet, drawing mode       | -                 | -           | Stroke / Text | -             | -
//  5  | left, elsewhere, drawing mode                    | leave drawing mode| -           | -             | drawing mode there, or fullscreen | -
//  6  | left, a creation tool in hand                    | -                 | fullscreen  | Frame         | -             | -
//  7  | left, on a snippet, Shift                        | add or take away  | -           | -             | -             | -
//  8  | left, on a snippet                               | select (and raise)| -           | Move          | drawing mode  | drawing mode
//  9  | left, on empty canvas, Shift                     | -                 | -           | Box select    | -             | -
//  10 | left, on empty canvas, a trigger held            | clear selection   | -           | Frame         | fullscreen    | fullscreen
//  11 | left, on empty canvas                            | clear selection   | -           | -             | -             | -
//  12 | right, on the drawing snippet, drawing mode      | -                 | leave drawing mode | Erase  | -             | -
//  13 | right, on a snippet                              | select (and raise)| context menu| Resize from the nearest edge | - | -
//  14 | right, on empty canvas                           | -                 | empty canvas menu | -       | -             | -
//  15 | middle, X1, X2                                   | the bindings'     |             |               |               |
//
// "Drawing mode" in 4, 5 and 12 is with no Alt held: Alt picks a snippet
// up whatever the tool, as rules 8 and 13 do. What a press does at once is
// only what every meaning it can still have shares (6.1); what it still
// may mean is a Pending interaction (6.2). A press that has had its say
// leaves the rest of it Spent (6.3) - which the machine sees to.

#include "ui/interaction/machine.h"

namespace sz::ui {

// The Canvas level's answer to a left or right press, as the rules above
// say.
Answer RecognizePress(const Event& press, Editor& editor);

}  // namespace sz::ui
