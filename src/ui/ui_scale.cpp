#include "ui/ui_scale.h"

namespace sz::ui {

namespace {
float gUiScale = 1.0f;
}

float UiScale() { return gUiScale; }

void SetUiScale(float scale) { gUiScale = scale; }

}  // namespace sz::ui
