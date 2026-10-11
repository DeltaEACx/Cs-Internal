#include "features/visuals/engine.hpp"
#include "detail/draw.hpp"
#include <algorithm>
#include <cmath>
#include <string>

namespace visuals::detail
{
    namespace
    {
        float bounded(float value, float fallback, float minimum, float maximum)
        {
            return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
        }

        bool visible_point(const screen_point& value)
        {
            return value.valid && value.on_screen && std::isfinite(value.x) && std::isfinite(value.y)
                && std::abs(value.x) < 1000000.0f && std::abs(value.y) < 1000000.0f;
        }

        float label_size() { return bounded(g_settings.text_size, 13.0f, 8.0f, 32.0f); }

        void styled_text(ImDrawList* draw, const ImVec2& position, const std::string& label, ImU32 tint, text_style style)
        {
            const auto font = ImGui::GetFont();
            const auto size = label_size();
            const auto shade = IM_COL32(5, 6, 10, (tint >> IM_COL32_A_SHIFT) & 0xff);
            if (style == text_style::shadow)
                draw->AddText(font, size, ImVec2(position.x + 1.0f, position.y + 1.0f), shade, label.c_str());
            else if (style == text_style::outline)
                for (int y = -1; y <= 1; ++y)
                    for (int x = -1; x <= 1; ++x)
                        if (x || y)
                            draw->AddText(font, size, ImVec2(position.x + x, position.y + y), shade, label.c_str());
            draw->AddText(font, size, position, tint, label.c_str());
        }

        void centered_label(ImDrawList* draw, const screen_point& position, const std::string& label, ImU32 tint,
            text_style style, float y_offset = 0.0f)
        {
            const auto size = ImGui::GetFont()->CalcTextSizeA(label_size(), FLT_MAX, 0.0f, label.c_str());
            styled_text(draw, ImVec2(position.x - size.x * 0.5f, position.y + y_offset), label, tint, style);
        }

        std::string timer_label(float seconds)
        {
            const auto safe = bounded(seconds, 0.0f, 0.0f, 1000000.0f);
            return std::to_string(static_cast<int>(safe + 0.5f)) + "s";
        }

        void trajectory_segment(ImDrawList* draw, const ImVec2& from, const ImVec2& to, ImU32 tint,
            float thickness, float& phase)
        {
            const auto dx = to.x - from.x;
            const auto dy = to.y - from.y;
            const auto length = std::hypot(dx, dy);
            if (!std::isfinite(length) || length <= 0.001f || length > 32768.0f)
                return;
            if (g_settings.trajectory_style != line_style::dashed)
            {
                draw->AddLine(from, to, tint, thickness);
                return;
            }
            constexpr float dash = 7.0f, period = 11.0f;
            // Preserve gaps even when the projected trajectory has very short segments.
            for (float start = -phase; start < length; start += period)
            {
                const auto low = std::max(0.0f, start);
                const auto high = std::min(length, start + dash);
                if (high > low)
                    draw->AddLine(ImVec2(from.x + dx * low / length, from.y + dy * low / length),
                        ImVec2(from.x + dx * high / length, from.y + dy * high / length), tint, thickness);
            }
            phase = std::fmod(phase + length, period);
        }

        void guide_circle(ImDrawList* draw, const ImVec2& center, float radius)
        {
            const auto tint = to_u32(g_settings.guide_color);
            const auto thickness = bounded(g_settings.fov_thickness, 1.0f, 0.5f, 5.0f);
            if (g_settings.fov_style != line_style::dashed)
            {
                draw->AddCircle(center, radius, tint, 64, thickness);
                return;
            }
            constexpr float tau = 6.28318530718f;
            const auto count = std::clamp(static_cast<int>(tau * radius / 13.0f), 12, 128);
            const auto step = tau / count;
            for (int i = 0; i < count; ++i)
            {
                ImVec2 arc[5];
                for (int j = 0; j < 5; ++j)
                {
                    const auto angle = step * (i + j * 0.15f);
                    arc[j] = ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
                }
                draw->AddPolyline(arc, 5, tint, false, thickness);
            }
        }

        void projectile_marker(ImDrawList* draw, const ImVec2& position, ImU32 tint)
        {
            const auto size = bounded(g_settings.projectile_size, 3.0f, 1.0f, 16.0f);
            switch (g_settings.projectile_marker)
            {
            case marker_style::circle:
                draw->AddCircle(position, size, tint, 24, 1.25f);
                break;
            case marker_style::square:
                draw->AddRect(ImVec2(position.x - size, position.y - size), ImVec2(position.x + size, position.y + size), tint);
                break;
            case marker_style::diamond:
            {
                const ImVec2 vertices[] = { {position.x, position.y - size}, {position.x + size, position.y},
                    {position.x, position.y + size}, {position.x - size, position.y} };
                draw->AddPolyline(vertices, 4, tint, true, 1.25f);
                break;
            }
            case marker_style::cross:
                draw->AddLine(ImVec2(position.x - size, position.y), ImVec2(position.x + size, position.y), tint, 1.25f);
                draw->AddLine(ImVec2(position.x, position.y - size), ImVec2(position.x, position.y + size), tint, 1.25f);
                break;
            default:
                draw->AddCircleFilled(position, size, tint, 12);
                break;
            }
        }

        void draw_widgets(ImDrawList* draw, const frame_snapshot& frame)
        {
            if (!std::isfinite(frame.screen_width) || !std::isfinite(frame.screen_height)
                || frame.screen_width <= 0.0f || frame.screen_height <= 0.0f
                || frame.screen_width > 32768.0f || frame.screen_height > 32768.0f)
                return;
            if (g_settings.fov_guide && frame.show_fov_guide && std::isfinite(frame.fov_radius)
                && frame.fov_radius > 0.0f && frame.fov_radius <= 32768.0f)
                guide_circle(draw, ImVec2(frame.screen_width * 0.5f, frame.screen_height * 0.5f), frame.fov_radius);

            if (g_settings.speed_widget)
            {
                const auto speed = std::to_string(static_cast<int>(bounded(frame.local_speed, 0.0f, 0.0f, 1000000.0f))) + " u/s";
                styled_text(draw, ImVec2(18.0f, frame.screen_height - 42.0f), speed,
                    to_u32(g_settings.speed_color), g_settings.speed_text_style);
            }
            if (g_settings.bomb_widget && frame.bomb_planted)
            {
                const auto label = std::string("OBJECTIVE  ") + timer_label(frame.bomb_time_remaining);
                const auto size = ImGui::GetFont()->CalcTextSizeA(label_size(), FLT_MAX, 0.0f, label.c_str());
                const ImVec2 pos(frame.screen_width * 0.5f - size.x * 0.5f, 24.0f);
                draw->AddRectFilled(ImVec2(pos.x - 9.0f, pos.y - 5.0f), ImVec2(pos.x + size.x + 9.0f, pos.y + size.y + 5.0f), IM_COL32(18, 20, 25, 200), 4.0f);
                styled_text(draw, pos, label, IM_COL32(255, 205, 105, 255), g_settings.item_style);
            }
            if (g_settings.hit_marker && frame.show_hit_marker && std::isfinite(frame.hit_marker_time) && frame.hit_marker_time > 0.0f)
            {
                auto tint = g_settings.hit_color;
                tint.a = static_cast<std::uint8_t>(std::clamp(frame.hit_marker_time / 0.25f, 0.0f, 1.0f) * tint.a);
                const auto color = to_u32(tint);
                const auto cx = frame.screen_width * 0.5f;
                const auto cy = frame.screen_height * 0.5f;
                const auto size = bounded(g_settings.hit_marker_size, 8.0f, 2.0f, 32.0f);
                const auto thickness = bounded(g_settings.hit_marker_thickness, 1.5f, 0.5f, 5.0f);
                const auto gap = size * 0.375f;
                if (g_settings.hit_marker_style == hit_style::dot)
                    draw->AddCircleFilled(ImVec2(cx, cy), size * 0.35f, color, 24);
                else if (g_settings.hit_marker_style == hit_style::plus)
                {
                    draw->AddLine(ImVec2(cx - size, cy), ImVec2(cx - gap, cy), color, thickness);
                    draw->AddLine(ImVec2(cx + size, cy), ImVec2(cx + gap, cy), color, thickness);
                    draw->AddLine(ImVec2(cx, cy - size), ImVec2(cx, cy - gap), color, thickness);
                    draw->AddLine(ImVec2(cx, cy + size), ImVec2(cx, cy + gap), color, thickness);
                }
                else
                {
                    draw->AddLine(ImVec2(cx - size, cy - size), ImVec2(cx - gap, cy - gap), color, thickness);
                    draw->AddLine(ImVec2(cx + size, cy - size), ImVec2(cx + gap, cy - gap), color, thickness);
                    draw->AddLine(ImVec2(cx - size, cy + size), ImVec2(cx - gap, cy + gap), color, thickness);
                    draw->AddLine(ImVec2(cx + size, cy + size), ImVec2(cx + gap, cy + gap), color, thickness);
                }
            }
        }
    }

    void draw_world(ImDrawList* draw, const frame_snapshot& frame)
    {
        if (!draw)
            return;
        const auto text_size = label_size();
        for (const auto& entity : frame.entities)
        {
            if ((entity.kind == world_entity_kind::weapon && !g_settings.dropped_weapons)
                || (entity.kind == world_entity_kind::grenade && !g_settings.dropped_grenades)
                || (entity.kind == world_entity_kind::objective && !g_settings.dropped_objectives)
                || (entity.kind == world_entity_kind::other && !g_settings.other_entities))
                continue;
            if (!visible_point(entity.position))
                continue;
            if (!std::isfinite(entity.distance_meters) || entity.distance_meters < 0.0f
                || entity.distance_meters > bounded(g_settings.max_distance_meters, 250.0f, 0.0f, 1000000.0f))
                continue;
            const auto color = entity.kind == world_entity_kind::objective
                ? IM_COL32(255, 195, 90, 255) : to_u32(g_settings.item_color);
            if (g_settings.item_names && !entity.label.empty())
                centered_label(draw, entity.position, entity.label, color, g_settings.item_style);
            if (g_settings.item_distance)
                centered_label(draw, entity.position, std::to_string(static_cast<int>(entity.distance_meters)) + " m",
                    IM_COL32(210, 215, 225, 245), g_settings.item_style, text_size);
            if (g_settings.objective_timer && entity.has_timer)
                centered_label(draw, entity.position, timer_label(entity.timer_seconds), IM_COL32(255, 215, 140, 255),
                    g_settings.item_style, text_size * 2.0f);
            if (!entity.detail.empty())
                centered_label(draw, entity.position, entity.detail, IM_COL32(190, 195, 205, 255), g_settings.item_style, -text_size);
        }
        for (const auto& projectile : frame.projectiles)
        {
            if (g_settings.projectile_path && projectile.trajectory.size() > 1)
            {
                float phase = 0.0f;
                const auto thickness = bounded(g_settings.trajectory_thickness, 1.5f, 0.5f, 5.0f);
                for (std::size_t i = 1; i < projectile.trajectory.size(); ++i)
                {
                    const auto& from = projectile.trajectory[i - 1];
                    const auto& to = projectile.trajectory[i];
                    if (visible_point(from) && visible_point(to))
                        trajectory_segment(draw, point(from), point(to), to_u32(g_settings.projectile_color), thickness, phase);
                    else
                        phase = 0.0f;
                }
            }
            if (!visible_point(projectile.position))
                continue;
            projectile_marker(draw, point(projectile.position), to_u32(g_settings.projectile_color));
            if (g_settings.projectile_names && !projectile.label.empty())
                centered_label(draw, projectile.position, projectile.label, to_u32(g_settings.projectile_color),
                    g_settings.projectile_text_style, -text_size - bounded(g_settings.projectile_size, 3.0f, 1.0f, 16.0f));
            if (g_settings.projectile_timer && projectile.has_timer)
                centered_label(draw, projectile.position, timer_label(projectile.time_remaining), IM_COL32(235, 235, 245, 255),
                    g_settings.projectile_text_style, bounded(g_settings.projectile_size, 3.0f, 1.0f, 16.0f) + 2.0f);
        }
        draw_widgets(draw, frame);
    }
}
