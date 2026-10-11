#include "features/visuals/engine.hpp"
#include "detail/draw.hpp"

#include <algorithm>
#include <cmath>

namespace visuals::detail
{
    namespace
    {
        float bounded(float value, float minimum, float maximum, float fallback)
        {
            return std::isfinite(value) ? std::clamp(value, minimum, maximum) : fallback;
        }

        bool valid_point(const screen_point& value)
        {
            return value.valid && std::isfinite(value.x) && std::isfinite(value.y)
                && std::abs(value.x) < 100000.0f && std::abs(value.y) < 100000.0f;
        }

        float font_size() { return bounded(g_settings.text_size, 8.0f, 32.0f, 13.0f); }

        ImVec2 text_extent(const std::string& text)
        {
            return ImGui::GetFont()->CalcTextSizeA(font_size(), FLT_MAX, 0.0f, text.c_str());
        }

        void draw_text(ImDrawList* draw, ImVec2 position, const std::string& text, ImU32 color, text_style style)
        {
            const auto size = font_size();
            const auto shadow = IM_COL32(0, 0, 0, (color >> IM_COL32_A_SHIFT) & 0xff);
            if (style == text_style::shadow)
                draw->AddText(ImGui::GetFont(), size, { position.x + 1.0f, position.y + 1.0f }, shadow, text.c_str());
            else if (style == text_style::outline)
            {
                for (int x = -1; x <= 1; ++x)
                    for (int y = -1; y <= 1; ++y)
                        if (x || y)
                            draw->AddText(ImGui::GetFont(), size, { position.x + x, position.y + y }, shadow, text.c_str());
            }
            draw->AddText(ImGui::GetFont(), size, position, color, text.c_str());
        }

        void centered_text(ImDrawList* draw, float center_x, float y, const std::string& text, ImU32 color, text_style style)
        {
            draw_text(draw, { center_x - text_extent(text).x * 0.5f, y }, text, color, style);
        }

        void corner_box(ImDrawList* draw, const screen_rect& box, ImU32 color, float thickness)
        {
            const auto fraction = bounded(g_settings.corner_fraction, 0.08f, 0.5f, 0.28f);
            const auto x = (box.right - box.left) * fraction;
            const auto y = (box.bottom - box.top) * fraction;
            draw->AddLine({ box.left, box.top }, { box.left + x, box.top }, color, thickness);
            draw->AddLine({ box.left, box.top }, { box.left, box.top + y }, color, thickness);
            draw->AddLine({ box.right - x, box.top }, { box.right, box.top }, color, thickness);
            draw->AddLine({ box.right, box.top }, { box.right, box.top + y }, color, thickness);
            draw->AddLine({ box.left, box.bottom - y }, { box.left, box.bottom }, color, thickness);
            draw->AddLine({ box.left, box.bottom }, { box.left + x, box.bottom }, color, thickness);
            draw->AddLine({ box.right - x, box.bottom }, { box.right, box.bottom }, color, thickness);
            draw->AddLine({ box.right, box.bottom - y }, { box.right, box.bottom }, color, thickness);
        }

        void draw_box(ImDrawList* draw, const screen_rect& box, ImU32 color, float thickness, float rounding)
        {
            if (g_settings.boxes == box_style::corners) corner_box(draw, box, color, thickness);
            else draw->AddRect({ box.left, box.top }, { box.right, box.bottom }, color, rounding, ImDrawCornerFlags_All, thickness);
        }

        ImU32 shade(ImU32 color, float factor)
        {
            const auto channel = [color, factor](int shift) {
                return static_cast<int>(std::clamp(((color >> shift) & 255) * factor, 0.0f, 255.0f));
            };
            return IM_COL32(channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT), channel(IM_COL32_B_SHIFT), (color >> IM_COL32_A_SHIFT) & 255);
        }

        void draw_bar(ImDrawList* draw, ImVec2 minimum, ImVec2 maximum, float ratio, bool vertical, ImU32 color, bar_style style, bool health = false)
        {
            ratio = bounded(ratio, 0.0f, 1.0f, 0.0f);
            draw->AddRectFilled({ minimum.x - 1.0f, minimum.y - 1.0f }, { maximum.x + 1.0f, maximum.y + 1.0f }, IM_COL32(10, 11, 15, 215));
            if (ratio <= 0.0f) return;
            const auto length = vertical ? maximum.y - minimum.y : maximum.x - minimum.x;
            if (style == bar_style::segmented)
            {
                const auto cells = std::clamp(static_cast<int>(length / 4.0f), 1, 10);
                const auto step = length / cells;
                const auto gap = cells > 1 ? std::min(1.0f, step * 0.2f) : 0.0f;
                for (int i = 0; i < cells; ++i)
                {
                    const auto fill = std::clamp(ratio * cells - i, 0.0f, 1.0f);
                    if (fill <= 0.0f) break;
                    const auto start = step * i;
                    const auto end = start + (step - gap) * fill;
                    if (vertical) draw->AddRectFilled({ minimum.x, maximum.y - end }, { maximum.x, maximum.y - start }, color);
                    else draw->AddRectFilled({ minimum.x + start, minimum.y }, { minimum.x + end, maximum.y }, color);
                }
                return;
            }
            if (vertical) minimum.y = maximum.y - length * ratio;
            else maximum.x = minimum.x + length * ratio;
            if (style == bar_style::gradient)
            {
                const auto low = health ? health_color(0) : shade(color, 0.45f);
                const auto high = health ? health_color(static_cast<int>(ratio * 100.0f)) : shade(color, 1.2f);
                if (vertical) draw->AddRectFilledMultiColor(minimum, maximum, high, high, low, low);
                else draw->AddRectFilledMultiColor(minimum, maximum, low, high, high, low);
            }
            else draw->AddRectFilled(minimum, maximum, color);
        }

        void draw_line(ImDrawList* draw, ImVec2 from, ImVec2 to, ImU32 color, float thickness, line_style style)
        {
            if (style != line_style::dashed) { draw->AddLine(from, to, color, thickness); return; }
            const auto dx = to.x - from.x, dy = to.y - from.y;
            const auto length = std::hypot(dx, dy);
            if (length < 0.01f) return;
            const auto count = std::clamp(static_cast<int>(std::ceil(length / 11.0f)), 1, 512);
            const auto step = length / count;
            for (int i = 0; i < count; ++i)
            {
                const auto start = i * step / length;
                const auto end = std::min((i * step + step * 0.6f) / length, 1.0f);
                draw->AddLine({ from.x + dx * start, from.y + dy * start }, { from.x + dx * end, from.y + dy * end }, color, thickness);
            }
        }

        void draw_marker(ImDrawList* draw, ImVec2 position, ImU32 color, float size, float thickness, marker_style style)
        {
            switch (style)
            {
            case marker_style::dot: draw->AddCircleFilled(position, size, color, 20); break;
            case marker_style::square:
                draw->AddRect({ position.x - size, position.y - size }, { position.x + size, position.y + size }, color, 0.0f, ImDrawCornerFlags_All, thickness);
                break;
            case marker_style::diamond:
            {
                const ImVec2 vertices[] = { { position.x, position.y - size }, { position.x + size, position.y }, { position.x, position.y + size }, { position.x - size, position.y } };
                draw->AddPolyline(vertices, 4, color, true, thickness);
                break;
            }
            case marker_style::cross:
                draw->AddLine({ position.x - size, position.y }, { position.x + size, position.y }, color, thickness);
                draw->AddLine({ position.x, position.y - size }, { position.x, position.y + size }, color, thickness);
                break;
            default: draw->AddCircle(position, size, color, 24, thickness); break;
            }
        }

        void draw_offscreen(ImDrawList* draw, const player_snapshot& player, const frame_snapshot& frame, ImU32 color)
        {
            if (!g_settings.show_offscreen || !valid_point(player.offscreen_direction)) return;
            const ImVec2 center(frame.screen_width * 0.5f, frame.screen_height * 0.5f);
            const auto dx = player.offscreen_direction.x - center.x;
            const auto dy = player.offscreen_direction.y - center.y;
            const auto length = std::hypot(dx, dy);
            if (length < 0.001f) return;
            const auto ux = dx / length, uy = dy / length;
            const auto size = bounded(g_settings.offscreen_size, 5.0f, 32.0f, 14.0f);
            const auto radius = std::min(frame.screen_width, frame.screen_height) * bounded(g_settings.offscreen_radius, 0.1f, 0.47f, 0.39f);
            const ImVec2 tip(center.x + ux * radius, center.y + uy * radius);
            const ImVec2 base(tip.x - ux * size, tip.y - uy * size);
            const ImVec2 side(-uy * size * 0.5f, ux * size * 0.5f);
            if (g_settings.offscreen_style == arrow_style::dot) draw->AddCircleFilled(tip, size * 0.4f, color, 24);
            else if (g_settings.offscreen_style == arrow_style::chevron)
            {
                const ImVec2 vertices[] = { { base.x + side.x, base.y + side.y }, tip, { base.x - side.x, base.y - side.y } };
                draw->AddPolyline(vertices, 3, color, false, std::max(1.5f, size * 0.16f));
            }
            else draw->AddTriangleFilled(tip, { base.x + side.x, base.y + side.y }, { base.x - side.x, base.y - side.y }, color);
        }

        ImU32 player_color(const player_snapshot& player)
        {
            if (player.dormant) return to_u32(g_settings.dormant);
            if (!player.enemy) return to_u32(g_settings.teammate);
            return to_u32(player.visible ? g_settings.enemy_visible : g_settings.enemy_hidden);
        }
    }

    void draw_players(ImDrawList* draw, const frame_snapshot& frame)
    {
        if (!draw || !std::isfinite(frame.screen_width) || !std::isfinite(frame.screen_height)
            || frame.screen_width <= 20.0f || frame.screen_height <= 20.0f
            || frame.screen_width > 65536.0f || frame.screen_height > 65536.0f) return;
        const auto screen_bottom = frame.screen_height - 8.0f;
        const auto text_size = font_size();
        const auto health_width = bounded(g_settings.health_width, 1.0f, 8.0f, 3.0f);
        const auto armor_width = bounded(g_settings.armor_width, 1.0f, 8.0f, 3.0f);
        const auto ammo_width = bounded(g_settings.ammo_width, 1.0f, 8.0f, 3.0f);
        for (const auto& player : frame.players)
        {
            if (!player.alive || (!g_settings.show_teammates && player.team == frame.local_team) || (player.dormant && !g_settings.show_dormant)) continue;
            if (!std::isfinite(player.distance_meters) || player.distance_meters < 0.0f
                || player.distance_meters > bounded(g_settings.max_distance_meters, 1.0f, 10000.0f, 250.0f)) continue;
            const auto color = player_color(player);
            const auto& box = player.bounds;
            if (!box.valid || !std::isfinite(box.left) || !std::isfinite(box.top) || !std::isfinite(box.right) || !std::isfinite(box.bottom)
                || std::abs(box.left) > 100000.0f || std::abs(box.right) > 100000.0f || std::abs(box.top) > 100000.0f || std::abs(box.bottom) > 100000.0f
                || box.right <= box.left || box.bottom <= box.top)
            {
                draw_offscreen(draw, player, frame, color);
                continue;
            }
            const auto width = box.right - box.left;
            const auto height = box.bottom - box.top;
            const auto center_x = (box.left + box.right) * 0.5f;
            const auto rounding = g_settings.boxes == box_style::rounded ? std::min(bounded(g_settings.box_rounding, 0.0f, 24.0f, 6.0f), std::min(width, height) * 0.5f) : 0.0f;
            if (g_settings.player_box)
            {
                const auto thickness = bounded(g_settings.box_thickness, 0.5f, 6.0f, 1.5f);
                if (g_settings.box_fill) draw->AddRectFilled({ box.left, box.top }, { box.right, box.bottom }, to_u32(g_settings.box_fill_color), rounding);
                if (g_settings.box_outline) draw_box(draw, box, IM_COL32(0, 0, 0, 190), thickness + 2.0f, rounding);
                draw_box(draw, box, color, thickness, rounding);
            }

            if (g_settings.health_bar)
            {
                const auto ratio = std::clamp(player.health / 100.0f, 0.0f, 1.0f);
                draw_bar(draw, { box.left - health_width - 3.0f, box.top }, { box.left - 3.0f, box.bottom }, ratio, true, health_color(player.health), g_settings.health_style, true);
                if (g_settings.health_value)
                {
                    const auto text = std::to_string(std::max(0, player.health));
                    draw_text(draw, { box.left - health_width - 6.0f - text_extent(text).x, std::clamp(box.bottom - height * ratio - text_size * 0.5f, box.top, std::max(box.top, box.bottom - text_size)) }, text, IM_COL32(245, 245, 250, 255), text_style::outline);
                }
            }
            float armor_label_width = 0.0f;
            if (g_settings.armor_bar)
            {
                const auto ratio = std::clamp(player.armor / 100.0f, 0.0f, 1.0f);
                draw_bar(draw, { box.right + 3.0f, box.top }, { box.right + 3.0f + armor_width, box.bottom }, ratio, true, to_u32(g_settings.armor_color), g_settings.armor_style);
                if (g_settings.armor_value)
                {
                    const auto text = std::to_string(std::max(0, player.armor));
                    armor_label_width = text_extent(text).x + 3.0f;
                    draw_text(draw, { box.right + armor_width + 6.0f, std::clamp(box.bottom - height * ratio - text_size * 0.5f, box.top, std::max(box.top, box.bottom - text_size)) }, text, to_u32(g_settings.armor_color), text_style::outline);
                }
            }

            if (g_settings.player_name && !player.name.empty())
                centered_text(draw, center_x, box.top - text_size - 4.0f, player.name, to_u32(g_settings.name_color), g_settings.name_style);

            auto lower_y = box.bottom + 3.0f;
            if (g_settings.ammo_bar && player.ammo >= 0 && player.max_ammo > 0)
            {
                const auto fraction = std::clamp(static_cast<float>(player.ammo) / player.max_ammo, 0.0f, 1.0f);
                draw_bar(draw, { box.left, lower_y }, { box.right, lower_y + ammo_width }, fraction, false, to_u32(g_settings.ammo_color), g_settings.ammo_style);
                if (g_settings.ammo_value)
                    draw_text(draw, { box.right + 5.0f, lower_y - (text_size - ammo_width) * 0.5f }, std::to_string(player.ammo), to_u32(g_settings.ammo_color), text_style::outline);
                lower_y += ammo_width + 4.0f;
            }
            if (g_settings.weapon_name && !player.weapon.empty())
            {
                centered_text(draw, center_x, lower_y, player.weapon, to_u32(g_settings.weapon_color), g_settings.weapon_style);
                lower_y += text_size + 1.0f;
            }
            if (g_settings.distance)
                centered_text(draw, center_x, lower_y, std::to_string(static_cast<int>(player.distance_meters)) + " m", to_u32(g_settings.distance_color), g_settings.distance_style);

            if (g_settings.player_flags)
            {
                auto y = box.top;
                const auto x = box.right + 5.0f + (g_settings.armor_bar ? armor_width + 3.0f + armor_label_width : 0.0f);
                for (const auto& flag : player.flags)
                {
                    if (flag.empty()) continue;
                    if (g_settings.flags_style == flag_style::badges)
                    {
                        const auto extent = text_extent(flag);
                        draw->AddRectFilled({ x, y }, { x + extent.x + 10.0f, y + text_size + 5.0f }, IM_COL32(15, 16, 22, 210), 3.0f);
                        draw->AddRectFilled({ x, y + 2.0f }, { x + 2.0f, y + text_size + 3.0f }, to_u32(g_settings.flag_color));
                        draw_text(draw, { x + 5.0f, y + 2.0f }, flag, to_u32(g_settings.flag_color), text_style::plain);
                        y += text_size + 8.0f;
                    }
                    else
                    {
                        draw_text(draw, { x, y }, flag, to_u32(g_settings.flag_color), text_style::plain);
                        y += text_size + 1.0f;
                    }
                }
            }

            if (g_settings.skeleton)
            {
                const auto skeleton_color = to_u32(g_settings.skeleton_color);
                const auto thickness = bounded(g_settings.skeleton_thickness, 0.5f, 5.0f, 1.0f);
                std::vector<ImVec2> joints;
                for (const auto& bone : player.skeleton)
                {
                    if (!valid_point(bone.from) || !valid_point(bone.to) || !bone.from.on_screen || !bone.to.on_screen) continue;
                    draw->AddLine(point(bone.from), point(bone.to), skeleton_color, thickness);
                    if (g_settings.skeleton_render == skeleton_style::joints)
                    {
                        for (const auto& position : { point(bone.from), point(bone.to) })
                        {
                            if (std::none_of(joints.begin(), joints.end(), [&](const ImVec2& previous) { return std::abs(previous.x - position.x) < 0.25f && std::abs(previous.y - position.y) < 0.25f; }))
                                joints.push_back(position);
                        }
                    }
                }
                for (const auto& joint : joints)
                {
                    draw->AddCircleFilled(joint, thickness + 1.6f, IM_COL32(10, 11, 15, 230), 12);
                    draw->AddCircleFilled(joint, thickness + 0.7f, skeleton_color, 12);
                }
            }
            if (g_settings.head_marker && valid_point(player.head) && player.head.on_screen)
                draw_marker(draw, point(player.head), color, bounded(g_settings.head_size, 2.0f, 16.0f, 5.0f), bounded(g_settings.head_thickness, 0.5f, 5.0f, 1.25f), g_settings.head_style);
            if (g_settings.snapline && valid_point(player.feet))
            {
                const auto origin_y = screen_bottom - (screen_bottom - 8.0f) * bounded(g_settings.snapline_origin, 0.0f, 1.0f, 0.5f);
                draw_line(draw, { frame.screen_width * 0.5f, origin_y }, point(player.feet), color, bounded(g_settings.snapline_thickness, 0.5f, 5.0f, 1.0f), g_settings.snapline_style);
            }
        }
    }
}
