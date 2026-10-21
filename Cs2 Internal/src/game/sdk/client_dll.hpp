// Curated schema offsets verified in the installed client on 2026-10-21.
// All 39 skin fields were verified against the supported client build.
#pragma once
#include <cstddef>

namespace cs2_dumper::schemas::client_dll
{
    namespace C_AttributeContainer
    {
        constexpr std::ptrdiff_t m_Item = 0x50; // C_EconItemView
    }
    namespace C_BaseEntity
    {
        constexpr std::ptrdiff_t m_hOwnerEntity = 0x520;   // CHandle<C_BaseEntity>
        constexpr std::ptrdiff_t m_iHealth = 0x34C;        // int32
        constexpr std::ptrdiff_t m_iTeamNum = 0x3E7;       // uint8
        constexpr std::ptrdiff_t m_lifeState = 0x354;      // uint8
        constexpr std::ptrdiff_t m_nSubclassID = 0x380;    // CUtlStringToken
        constexpr std::ptrdiff_t m_pGameSceneNode = 0x330; // CGameSceneNode*
    } // namespace C_BaseEntity
    namespace C_BasePlayerPawn
    {
        constexpr std::ptrdiff_t m_pWeaponServices = 0x12F0; // CPlayer_WeaponServices*
    }
    namespace C_CSPlayerPawn
    {
        constexpr std::ptrdiff_t m_bNeedToReApplyGloves = 0x176D; // bool
        constexpr std::ptrdiff_t m_EconGloves = 0x1770;           // C_EconItemView
        constexpr std::ptrdiff_t m_hHudModelArms = 0x1DA4;        // CHandle<C_CS2HudModelArms>
    } // namespace C_CSPlayerPawn
    namespace C_EconEntity
    {
        constexpr std::ptrdiff_t m_AttributeManager = 0x1290;     // C_AttributeContainer
        constexpr std::ptrdiff_t m_flFallbackWear = 0x18B0;       // float32
        constexpr std::ptrdiff_t m_nFallbackPaintKit = 0x18A8;    // int32
        constexpr std::ptrdiff_t m_nFallbackSeed = 0x18AC;        // int32
        constexpr std::ptrdiff_t m_nFallbackStatTrak = 0x18B4;    // int32
        constexpr std::ptrdiff_t m_OriginalOwnerXuidLow = 0x18A0; // uint32
    } // namespace C_EconEntity
    namespace C_EconItemView
    {
        constexpr std::ptrdiff_t m_AttributeList = 0x208;                       // CAttributeList
        constexpr std::ptrdiff_t m_bDisallowSOC = 0x1E9;                        // bool
        constexpr std::ptrdiff_t m_bInitialized = 0x1E8;                        // bool
        constexpr std::ptrdiff_t m_bRestoreCustomMaterialAfterPrecache = 0x1B8; // bool
        constexpr std::ptrdiff_t m_iAccountID = 0x1D8;                          // uint32
        constexpr std::ptrdiff_t m_iEntityQuality = 0x1BC;                      // int32
        constexpr std::ptrdiff_t m_iItemDefinitionIndex = 0x1BA;                // uint16
        constexpr std::ptrdiff_t m_iItemID = 0x1C8;                             // uint64
        constexpr std::ptrdiff_t m_iItemIDHigh = 0x1D0;                         // uint32
        constexpr std::ptrdiff_t m_iItemIDLow = 0x1D4;                          // uint32
    } // namespace C_EconItemView
    namespace CBasePlayerController
    {
        constexpr std::ptrdiff_t m_steamID = 0x788; // uint64
    }
    namespace CCSPlayerController
    {
        constexpr std::ptrdiff_t m_hPlayerPawn = 0x92C; // CHandle<C_CSPlayerPawn>
    }
    namespace CEconItemAttribute
    {
        constexpr std::ptrdiff_t m_flInitialValue = 0x38;            // float32
        constexpr std::ptrdiff_t m_flValue = 0x34;                   // float32
        constexpr std::ptrdiff_t m_iAttributeDefinitionIndex = 0x30; // uint16
    } // namespace CEconItemAttribute
    namespace CGameSceneNode
    {
        constexpr std::ptrdiff_t m_pChild = 0x40;       // CGameSceneNode*
        constexpr std::ptrdiff_t m_pNextSibling = 0x48; // CGameSceneNode*
        constexpr std::ptrdiff_t m_pOwner = 0x30;       // CEntityInstance*
    } // namespace CGameSceneNode
    namespace CModelState
    {
        constexpr std::ptrdiff_t m_hModel = 0xA0;         // CStrongHandle<InfoForResourceTypeCModel>
        constexpr std::ptrdiff_t m_MeshGroupMask = 0x208; // uint64
    } // namespace CModelState
    namespace CPlayer_WeaponServices
    {
        constexpr std::ptrdiff_t m_hActiveWeapon = 0x60; // CHandle<C_BasePlayerWeapon>
    }
    namespace CSkeletonInstance
    {
        constexpr std::ptrdiff_t m_modelState = 0x140; // CModelState
    }
} // namespace cs2_dumper::schemas::client_dll
