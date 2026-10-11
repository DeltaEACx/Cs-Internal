#pragma once
#include "frame_data.hpp"

namespace visuals::spread_prediction {
void Frame(bool inWorld);
void Render(const frame_snapshot& frame);
}
