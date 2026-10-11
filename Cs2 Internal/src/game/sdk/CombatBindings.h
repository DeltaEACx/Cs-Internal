#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
// Input payload and native bullet-trace layout adapted from Spectral.win.
// Bindings are enabled only for the client hash in SkinBindings.h.
namespace combat_bindings
{
    inline constexpr std::size_t kMessagePayload = 0x10;
    inline constexpr std::size_t kCommandProto = 0x20;
    inline constexpr std::size_t kCommandSequence = 0x8;
    inline constexpr std::size_t kCommandType = 0x94;
    inline constexpr std::size_t kCommandBase = 0x40;
    inline constexpr std::size_t kCommandButtons = 0x58;
    inline constexpr std::size_t kInputPitch = 1672, kInputYaw = 1676;
    inline constexpr std::size_t kInputWeaponSelection = 0x694;
    inline constexpr std::uint64_t kAttack = 1;
    struct RepeatedField { std::uintptr_t arena; int size, capacity; std::uintptr_t entries; };
    struct BaseCommand
    {
        std::uint32_t bits, cachedSize;
        RepeatedField subticks;
        std::uintptr_t moveCrc, buttons, angles, notes;
        std::int32_t commandNumber, clientTick;
        float forward, left, up;
        std::int32_t impulse, weaponSelect, seed, mouseX, mouseY;
        std::uint32_t prediction, consumedAngles;
        std::int32_t flags;
        std::uint32_t pawnHandle;
    };
    struct ButtonState { std::uintptr_t vtable; std::uint64_t value, changed, scroll; };
    struct ButtonPayload { std::uint32_t bits, cachedSize; std::uint64_t value, changed, scroll; };
    struct AnglePayload { std::uint32_t bits, cachedSize; float pitch, yaw, roll; };
    struct CommandProto
    {
        std::uint32_t bits, cachedSize;
        RepeatedField history;
        std::uintptr_t base;
        bool leftHand, predictingBody, predictingHead, predictingRagdoll;
        std::int32_t attack1History, attack2History;
    };
    struct HistoryHeader { std::uint32_t bits, cachedSize; std::uintptr_t angles; };
    // Offsets are relative to the protobuf implementation at raw message +0x10.
    // Spectral input_history_entry preserves these native shot-time fields.
    inline constexpr std::size_t kHistoryPlayerTick = 0x58;
    inline constexpr std::size_t kHistoryPlayerFraction = 0x5c;
    inline constexpr std::uint32_t kHistoryPlayerTickBit = 0x800;
    inline constexpr std::uint32_t kHistoryPlayerFractionBit = 0x1000;
    struct HistoryShotTiming { std::int32_t tick; float fraction; };
    static_assert(sizeof(HistoryShotTiming) == 8);
    // Enemy render time is independent of player firing/seed time.
    inline constexpr std::size_t kHistoryRenderTick = 0x50;
    inline constexpr std::size_t kHistoryTargetIndex = 0x64;
    inline constexpr std::uint32_t kHistoryRenderBits = 0x600;
    struct BacktrackTiming { HistoryShotTiming render{}; int targetIndex{-1}; };
    struct Interpolation { std::uint32_t bits{}, cachedSize{}; float fraction{}; int source{}, destination{}; };
    struct SubtickPayload
    {
        std::uint32_t bits, cachedSize;
        std::uint64_t button;
        bool pressed;
        float when, forward, left, pitch, yaw;
    };
    static_assert(offsetof(BaseCommand,moveCrc)==0x20);
    static_assert(offsetof(BaseCommand,buttons)==0x28);
    static_assert(offsetof(BaseCommand,clientTick)==0x44);
    static_assert(sizeof(ButtonState)==0x20 && sizeof(ButtonPayload)==0x20);
    static_assert(offsetof(CommandProto, history)==0x8 && offsetof(CommandProto, base)==0x20);
    static_assert(offsetof(CommandProto, attack1History)==0x2c);
    static_assert(offsetof(SubtickPayload, forward)==0x18 && offsetof(SubtickPayload, left)==0x1c);
    namespace patterns
    {
    inline constexpr std::string_view handle_view_angles = "client.dll:FFFFFFFF488D05*????????48890D????????+40~";
    inline constexpr std::string_view set_view_angles = "client.dll:85D275??486381";
    inline constexpr std::string_view create_move = "client.dll:FFFFFFFF488D05*????????48890D????????+28~";
    inline constexpr std::string_view process_input_event = "client.dll:CCCC48895C2408574883EC20C6410800488D05*????????488901488BD9+20~";
    inline constexpr std::string_view csgo_input = "client.dll:84C0740C488D0D*????????E8????????";
    inline constexpr std::string_view get_usercmd_base = "client.dll:4883EC28>E8????????8B8010590000";
    inline constexpr std::string_view get_usercmd = "client.dll:40534883EC208BDAE8????????4C8BC0";
    inline constexpr std::string_view get_view_angles = "client.dll:8B0D????????8BD3>E8????????F20F1000";
    inline constexpr std::string_view button_state_alloc = "client.dll:488B54CA088D4101894708EB16488B0F>E8????????488BD0488BCF-80";
    inline constexpr std::string_view serialize_move_crc = "client.dll:48895C24??5556574883EC30498BC0488BFA488BF1488B09F6C103";
    inline constexpr std::string_view trace_filter_init = "client.dll:48895C24??48897424??574883EC??0FB641??33FF24";
    inline constexpr std::string_view trace_bullet_data_init = "client.dll:48895C240848896C2410488974241857415641574883EC40F20F10024D8D7108F20F1181F81C0000";
    inline constexpr std::string_view trace_bullet_data_reset = "client.dll:48895C24084889742410574883EC2033F6488DB9381C000089B1201C0000";
    inline constexpr std::string_view trace_bullet = "client.dll:40535741564883EC508B8424";
    // Current WeaponServices slot32, Vec3*(services, Vec3*); the old source slot29 is obsolete.
    // Native shooting helper subtracts interpolation and resolves a time-matched eye.
    inline constexpr std::string_view get_matched_shoot_position = "client.dll:48895C241848896C2420565741564883EC60448B92BC140000";
    inline constexpr std::string_view get_shoot_position = "client.dll:40534883EC20488B4938488BDA488B01FF9068050000";
    inline constexpr std::string_view get_aim_punch = "client.dll:48895C2408555657488D6C24B94881EC90000000833D????????FF488BF2488BF9";
    inline constexpr std::string_view get_spread = "client.dll:486391001A0000488B818803000085D278104883FA02730AF30F10849050070000";
    inline constexpr std::string_view get_inaccuracy = "client.dll:48895C24??5556574881EC????????440F298424";
    inline constexpr std::string_view weapon_calculate_spread = "client.dll:28F3440F11442420>E8????????488D85B0000000";
    // uint32_t(__fastcall*)(unused_self, const Vec3*, int shot_tick), client RVA D22BA0.
    // Native hashing uses normalized, half-degree pitch/yaw and the shot tick;
    // roll is deliberately absent from the seed input.
    inline constexpr std::string_view get_tick_view_angles = "client.dll:48895C2408574881ECF0000000F30F100A488D8C2410010000418BD8488BFAE8????????F30F104F04";
    }
}
