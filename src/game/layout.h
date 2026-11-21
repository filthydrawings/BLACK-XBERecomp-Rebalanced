#pragma once
// Guest objects that several hook files read
#include <cstdint>

#include "xdk/hle.h"

namespace layout {

// float seconds, the frame time the game was made for
constexpr uint32_t kGameFrameTime = 0x2F3A38;

// game object
constexpr uint32_t kGame = 0x2D45F0;
constexpr uint32_t kGameState = 0x21070;
constexpr uint32_t kGameClockStep = 0x2013C;  // float: its clock's step
constexpr uint32_t kGameDifficulty = 0x2014C;  // 0 to 3
constexpr uint32_t kFrontEnd = 0x20220;
constexpr uint32_t kFilmPlayer = 0x4C;      // in the front end
constexpr uint32_t kCurrentFilm = 0x2042C;  // the front end's +0x20C

// film table
constexpr uint32_t kFilmTable = 0x29D9C0;
constexpr uint32_t kFilmEntry = 12;
constexpr uint32_t kFilmBehindMenu = 8;  // byte: the film runs behind a menu
constexpr uint32_t kFilms = 27;
constexpr uint32_t kLogoFilm = 1;  // LO_, the EA intro

// timer: whole steps and parts of a step over the divisor, the step's dt and the time
constexpr uint32_t kTimerWholeSteps = 0x0C;
constexpr uint32_t kTimerPartSteps = 0x10;
constexpr uint32_t kTimerDivisor = 0x18;
constexpr uint32_t kTimerDt = 0x1C;
constexpr uint32_t kTimerTime = 0x20;

// world container: its tick reads as a timer
constexpr uint32_t kWorld = 0x2D45D4;
constexpr uint32_t kWorldPlayer = 0x30;
constexpr uint32_t kWorldLevel = 0x5AEC;  // byte: the level loaded, story levels below kStoryLevels
constexpr uint32_t kStoryLevels = 0x32;
// bytes the level load sets (sub_001EDC90): the game finished on this difficulty or a harder one, or Black Ops. It
// picks the level's silver weapon data and gives infinite ammo
constexpr uint32_t kWorldNewGamePlus = 0x5CB0;
// Black Ops finished, or played: the player starts with the M16A2 (sub_001DEC70)
constexpr uint32_t kWorldStartWeapon = 0x5CB1;

// player
constexpr uint32_t kPlayerWeapon = 0x29C;  // the weapon in hand
constexpr uint32_t kCharacterAnimation = 0x328;  // its animation controller

// animation controller
constexpr uint32_t kAnimOwner = 0;
constexpr uint32_t kAnimClip = 0x54;
constexpr uint32_t kAnimName = 0x5C;
constexpr uint32_t kAnimFrom = 0x90;
// animation clip, the controller's kAnimClip: a track that plays one animation after another
constexpr uint32_t kAnimClipFrame = 0x14;  // float: the frame it is at, 60 a second at speed 1
constexpr uint32_t kAnimClipSpeed = 0x9C8;  // float: its playback speed, 1 (or 0) the normal one (sub_00097F00)

// the player, in the world container
inline uint32_t Player() { return kernel::R32(kWorld) + kWorldPlayer; }

// display: +0 width
constexpr uint32_t kDisplay = 0x2D67F4;
constexpr uint32_t kDisplayWidescreen = 0x0C;
constexpr uint32_t kDisplayWidth = 0xD428;
constexpr uint32_t kDisplayHeight = 0xD42C;
constexpr uint32_t kDisplayViewWidth = 0xD4C0;
constexpr uint32_t kDisplayAspect = 0xD4C4;
constexpr uint32_t kDisplayDrawLists = 0xCA6C;
constexpr uint32_t kGunDrawList = 5;  // the first person models
// draw list
constexpr uint32_t kDrawListSize = 0x18;
constexpr uint32_t kDrawListRoom = 0;
constexpr uint32_t kDrawListCap = 0xC;
constexpr uint32_t kStageX = 0x295DA4;  // float: the 640 Flash stage sits this far in on a widescreen

// weapon and its data
constexpr uint32_t kWeaponState = 0xD8;  // its state machine, sub_001EEFD0: 1 firing, kWeaponReloading
constexpr uint32_t kWeaponReloading = 4;  // the state a reload starts in
constexpr uint32_t kWeaponWait = 0xDC;   // float: the countdown to its next shot, from the fire interval
constexpr uint32_t kWeaponData = 0xEC;
constexpr uint32_t kWeaponOwner = 0xF0;  // the character holding it
constexpr uint32_t kWeaponMagazine = 0xF4;
constexpr uint32_t kWeaponSpareAmmo = 0xFC;  // -> uint16 per ammo type (kDataAmmoType): the rounds left to load
// magazine
constexpr uint32_t kMagazineRounds = 0x18;  // uint16: the rounds in the clip
constexpr uint32_t kDataAimZoom = 0xB4;  // float: 1 for a weapon that does not aim

// character type: characters of a type share its record, the types sit in one array by id
constexpr uint32_t kCharacterType = 0x320;  // in the character: its type record
constexpr uint32_t kTypeSize = 0x40;
constexpr uint32_t kTypeId = 0x04;
constexpr uint32_t kTypeHits = 0x3C;  // a pointer to a pointer to its hits table, an entry per weapon record
// hits table entry: float share of health a hit takes from the front, from the back, then two instant kill flag bytes
constexpr uint32_t kHitsEntrySize = 12;

// weapon records, the weapon data blocks, in one array
constexpr uint32_t kWeaponRecords = 16;
constexpr uint32_t kWeaponRecordSize = 0x1E0;
constexpr uint32_t kDataClip = 0x60;  // int: the clip size
constexpr uint32_t kDataAmmoType = 0x64;  // int: the ammo pool it shares
constexpr uint32_t kDataRange = 0x14;  // float: its range, unconfirmed
// float: the firing interval, seconds between shots, confirmed; changing it desyncs the animations, never written
constexpr uint32_t kDataFireGap = 0x20;
constexpr uint32_t kDataRecoil = 0x68;  // float: the recoil kick of a shot, confirmed by test (uzi 5.5, sniper 1)
constexpr uint32_t kWeaponRecordsAt = 0x8227D330;  // record 0, in GlobData.bin, mapped at 0x820BE000
// the records in order, as balance.toml names them
inline constexpr const char* kWeaponNames[kWeaponRecords] = {
    "dc3elite", "spas12", "sniper", "uzi", "g36c", "ak47", "rpg", "launcher",
    "mac10", "p90", "m249", "magnum", "m16", "rem870", "glock", "mp5"};
inline uint32_t WeaponRecord(uint32_t weapon) { return kWeaponRecordsAt + weapon * kWeaponRecordSize; }
// the place in the array of the record at data, kWeaponRecords for none
inline uint32_t WeaponRecordIndex(uint32_t data) {
    if (data < kWeaponRecordsAt) return kWeaponRecords;
    uint32_t at = data - kWeaponRecordsAt;
    return at % kWeaponRecordSize || at / kWeaponRecordSize >= kWeaponRecords ? kWeaponRecords
                                                                              : at / kWeaponRecordSize;
}
// GlobData.bin is in: record 0 has the words every record has
inline bool WeaponRecordsMapped() {
    return kernel::R32(WeaponRecord(0) + 4) == 1 && kernel::G<float>(WeaponRecord(0) + 0x1C) == 1.0f;
}

// UI object, the front end the menu hooks get in ebp
constexpr uint32_t kUi = 0x2D199C;
constexpr uint32_t kCallWaits = 0x374;  // a call to the game waits, the page script's FEFunctionTrigger
constexpr uint32_t kMenuInput = 0x44C;  // the menu input of this tick
constexpr uint32_t kWaitsForStart = 0x450;
constexpr uint32_t kInputOn = 0x454;
constexpr uint32_t kInputHeldOff = 0x455;
constexpr uint32_t kStageScaleX = 0x3710;  // Flash stage to screen
constexpr uint32_t kStageScaleY = 0x3714;
constexpr uint32_t kPageReady = 0x381C;
constexpr uint32_t kTargetPage = 0x3820;  // the page asked for
constexpr uint32_t kPageName = 0x3840;
constexpr uint32_t kPageAsked = 0x3884;    // 1 once a page is asked for
constexpr uint32_t kPageLeaving = 0x388C;  // 1 as it goes
constexpr uint32_t kUiReady = 0x390F;
constexpr uint32_t kPageRunning = 2;  // kPageReady once the page is ready

// font: its glyphs by code & 0x7F
constexpr uint32_t kFontScale = 0x08;
constexpr uint32_t kFontSpacing = 0x10;
constexpr uint32_t kFontLastGlyph = 0x1C;
constexpr uint32_t kFontGlyphs = 0x20;
// glyph
constexpr uint32_t kGlyphSize = 0x28;
constexpr uint32_t kGlyphWidth = 0x08;
constexpr uint32_t kGlyphLeft = 0x10;
constexpr uint32_t kGlyphAdvance = 0x18;
constexpr uint32_t kGlyphCode = 0x1C;
constexpr uint32_t kGlyphSpaced = 0x20;

enum MenuInput : uint32_t { kNoInput, kInputUp, kInputDown, kInputLeft, kInputRight, kInputConfirm, kInputBack };

// the page takes the menu input of this tick
inline bool InputTaken(uint32_t ui) {
    using kernel::G;
    return G<uint8_t>(ui + kUiReady) && G<uint8_t>(ui + kInputOn) && !G<uint8_t>(ui + kInputHeldOff);
}

}  // namespace layout
