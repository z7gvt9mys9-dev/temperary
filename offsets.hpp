#pragma once

#include <cstdint>

// CS2 Windows offsets. Global addresses are relative to client.dll.
// Schema offsets are relative to the corresponding game object.
namespace offsets {

inline constexpr std::uintptr_t dwEntityList = 0x2577BE0;
inline constexpr std::uintptr_t dwLocalPlayerPawn = 0x23CCC08;
inline constexpr std::uintptr_t dwViewMatrix = 0x23D21F0;
inline constexpr std::uintptr_t highestEntityIndex = 0x2090;

inline constexpr std::uintptr_t m_pGameSceneNode = 0x330;
inline constexpr std::uintptr_t m_pCollision = 0x340;
inline constexpr std::uintptr_t m_iHealth = 0x34C;
inline constexpr std::uintptr_t m_iTeamNum = 0x3E7;
inline constexpr std::uintptr_t m_hPlayerPawn = 0x914;
inline constexpr std::uintptr_t m_hController = 0x13D0;
inline constexpr std::uintptr_t m_sSanitizedPlayerName = 0x868;
inline constexpr std::uintptr_t m_steamID = 0x780;
inline constexpr std::uintptr_t m_pInGameMoneyServices = 0x810;
inline constexpr std::uintptr_t m_iAccount = 0x40;
inline constexpr std::uintptr_t m_pWeaponServices = 0x1208;
inline constexpr std::uintptr_t m_pBulletServices = 0x1490;
inline constexpr std::uintptr_t m_hActiveWeapon = 0x60;
inline constexpr std::uintptr_t m_hMyWeapons = 0x48;
inline constexpr std::uintptr_t m_nSubclassID = 0x380;
inline constexpr std::uintptr_t m_szWeaponName = 0x720;
inline constexpr std::uintptr_t m_WeaponType = 0x520;
inline constexpr std::uintptr_t m_vecViewOffset = 0xE78;
inline constexpr std::uintptr_t m_iShotsFired = 0x1C8C;
inline constexpr std::uintptr_t m_angEyeAngles = 0x3350;
inline constexpr std::uintptr_t m_iIDEntIndex = 0x342C;
inline constexpr std::uintptr_t m_AttributeManager = 0x11A8;
inline constexpr std::uintptr_t m_Item = 0x50;
inline constexpr std::uintptr_t m_iItemDefinitionIndex = 0x1BA;
inline constexpr std::uintptr_t m_vecOrigin = 0x80;
inline constexpr std::uintptr_t m_vecMins = 0x40;
inline constexpr std::uintptr_t m_vecMaxs = 0x4C;

// CCSPlayer_BulletServices contains a client-only CUtlVector<bullet_data>
// starting at the same address as the schema-visible hit counter.
inline constexpr std::uintptr_t m_bulletData = 0x48;

inline constexpr std::uintptr_t entityChunkTable = 0x10;
inline constexpr std::uintptr_t entityIdentityStride = 0x70;
inline constexpr std::uint32_t entityIndexMask = 0x7FFF;
inline constexpr std::uint32_t entityChunkMask = 0x1FF;
inline constexpr unsigned entityChunkShift = 9;

} // namespace offsets
