#pragma once
#include "engine.hpp"
#include "settings.hpp"
#include "game/image_finish_presets.h"
namespace visuals::gun_images {
struct Configuration {bool enabled{},followSkybox{true};std::array<char,200> texture{};color tint{255,255,255,255};};
Configuration Get();void Set(Configuration value);
std::string Source(std::string_view path,bool cube);
bool SelectPreset(Configuration &value, std::size_t index);
bool CanUpdate(const Configuration &value, bool skyboxEnabled);
enum class MenuMode {Images,Camo};
void Update(bool inWorld);void Reset();void Menu(MenuMode mode=MenuMode::Images);
std::uintptr_t Material();color Tint();
}
