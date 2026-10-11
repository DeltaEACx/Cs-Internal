#pragma once
namespace hvh {
bool Install();
const char* Status();
bool Stop();
void SetInputState(bool menuOpen, float width, float height);
void DrawMenu();
void DrawAntiAimMenu();
}
