#include "plugin.h"
#include "common.h"
#include "CMenuManager.h"
#include "CMenuSystem.h"
#include "CText.h"
#include "CRadar.h"
#include "CSprite2d.h"
#include "CTxdStore.h"
#include "CPickups.h"
#include "CEntity.h"
#include "CStuntJumpManager.h"
#include "CGame.h"
#include "CollectibleCoords.h"
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include <windows.h>
// aap's debugmenu, the menu SilentPatch and SkyGfx publish their settings into. The header
// is his, verbatim, under the MIT licence in debugmenu_LICENSE.txt; it resolves the menu at
// runtime and does nothing at all when debugmenu.dll is not installed.
#include "debugmenu_public.h"

DebugMenuAPI gDebugMenuAPI;

using namespace plugin;

enum eCollectibleMode {
    MODE_OFF, MODE_TAGS, MODE_SNAPSHOTS, MODE_HORSESHOES, MODE_OYSTERS, MODE_JUMPS, MODE_ALL, MODE_COUNT
};

enum eRevealMode {
    REVEAL_OFF, REVEAL_ARMOR, REVEAL_WEAPONS, REVEAL_BRIBES, REVEAL_ALL, REVEAL_COUNT
};

enum eExtraRow {
    ROW_ASSETS, ROW_COLLECT, ROW_PICKUPS, ROW_KIND_COUNT
};

static const int FIRST_EXTRA_ROW = 5;
static const int MAX_MENU_ROWS = 12;

static const char KEY_LABEL[]  = "FED_BL6";
static const char TEXT_LABEL[] = "Collectibles";

static const unsigned int REBUILD_INTERVAL_MS = 1000;
static const unsigned int SETTINGS_POLL_MS = 2000;

static const char *MENU = "Map Collectibles";

static const char *KEY_MODE[MODE_COUNT] = {
    "COLM_OFF", "COLM_TAG", "COLM_SNP", "COLM_HRS", "COLM_OYS", "COLM_USJ", "COLM_ALL"
};
static const char *TEXT_MODE[MODE_COUNT] = {
    "OFF", "Gang Tags", "Snapshots", "Horseshoes", "Oysters", "Stunt Jumps", "All"
};
static const char *TEX_MODE[MODE_COUNT] = {
    NULL, "graffiti", "snapshot", "horseshoe", "oyster", "usj", NULL
};

static const char KEY_REVEAL_LABEL[]  = "FED_BL7";
static const char TEXT_REVEAL_LABEL[] = "Pickups";

static const char *KEY_REVEAL[REVEAL_COUNT] = {
    "PKUP_OFF", "PKUP_ARM", "PKUP_WPN", "PKUP_BRB", "PKUP_ALL"
};
static const char *TEXT_REVEAL[REVEAL_COUNT] = {
    "OFF", "Armor", "Weapons", "Bribes", "All"
};
static const char *TEX_REVEAL[REVEAL_COUNT] = {
    NULL, "armor", "weapon", "bribe", NULL
};

static const char KEY_ASSET_LABEL[]  = "FED_BL8";
static const char TEXT_ASSET_LABEL[] = "Assets";
static const char TEX_ASSET[]        = "asset";

static const char KEY_ON[]  = "FEM_ON";
static const char KEY_OFF[] = "FEM_OFF";

static const uintptr_t CALL_INSERT_COLUMN_LABELS = 0x57893F;
static const uintptr_t CALL_INSERT_COLUMN_STATES = 0x578A2E;
static const uintptr_t CALL_INSERT_COLUMN_UPDATE = 0x578B4A;
static const uintptr_t CALL_CHECK_FOR_ACCEPT     = 0x578A44;
static const uintptr_t CALL_DRAW_MAP_SECTION     = 0x5756E0;
static const uintptr_t CALL_DRAW_MAP_SPRITE      = 0x5759C9;
static const uintptr_t CALL_DRAW_MAP_BLIPS       = 0x575B44;
static const uintptr_t CALL_DRAW_RADAR_MAP       = 0x58A38A;
static const uintptr_t BYTE_LAST_ROW_INDEX       = 0x578B5E;

static const float WORLD_HALF_SIZE = 3000.0f;
static const float INTERIOR_Z = 900.0f;
static const float MAP_TILES_PER_AXIS = 12.0f;

static const uintptr_t CALL_TEXT_GET[] = { 0x5810BD, 0x581122, 0x581191, 0x58143E };

struct tTagDesc {
    CEntity      *m_pEntity;
    unsigned char m_nAlpha;
    char          _pad[3];
};

static tTagDesc *const gTagDesc = (tTagDesc *)0xA9A8C0;
static int &gNumTags = *(int *)0xA9AD70;
static const unsigned char TAG_SPRAYED_ALPHA = 0xE4;

static int &gScreenWidth  = *(int *)0xC17044;
static bool &gMenuMapMode = *(bool *)0xBA67A1;
static CVector2D &gRadarOrigin = *(CVector2D *)0xBAA248;

static int  gMode = MODE_OFF;
static int  gRevealMode = REVEAL_OFF;
static bool gRevealRow = false;
static bool gAssetsRow = false;
static bool gAssetsShown = false;
static bool gFinderEnabled = false;
static int  gFinderRange = 250;
static int  gFinderMapAlpha = 120;
static int  gIconScale = 8;
static int  gPickupIconScale = 8;
static int  gAssetIconScale = 8;
static bool gDebugLog = false;
static char gIniPath[MAX_PATH] = "";
static unsigned int gLastSettingsPoll = 0;
static FILETIME gIniStamp = { 0, 0 };
static bool gIconsValid = false;
static unsigned int gLastRebuild = 0;
static bool gTexturesTried = false;

static float gMapLeft = 0.0f;
static float gMapTop = 0.0f;
static float gMapWidth = 0.0f;
static float gMapHeight = 0.0f;
static bool  gMapRectValid = false;

static CSprite2d gSprites[MODE_COUNT];

struct CollectibleIcon {
    CVector2D posn;
    float     z;
    int       type;
};

static CSprite2d gRevealSprites[REVEAL_COUNT];
static CSprite2d gAssetSprite;

static int gRowKind[MAX_MENU_ROWS];
static int gKindRow[ROW_KIND_COUNT];
static int gLastRow = FIRST_EXTRA_ROW;

static std::vector<CollectibleIcon> gCollected;
static std::vector<CollectibleIcon> gRemaining;
static std::vector<CollectibleIcon> gPickups;
static std::vector<CollectibleIcon> gAssets;

static void TakeIniStamp() {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExA(gIniPath, GetFileExInfoStandard, &data))
        gIniStamp = data.ftLastWriteTime;
}

static void LoadSettings() {
    gFinderEnabled = GetPrivateProfileIntA("Settings", "Finder", 1, gIniPath) != 0;
    gFinderRange = GetPrivateProfileIntA("Settings", "FinderRadarRange", 250, gIniPath);
    gFinderMapAlpha = GetPrivateProfileIntA("Settings", "MapFadeAlpha", 120, gIniPath);
    gIconScale = GetPrivateProfileIntA("Settings", "IconSize", 8, gIniPath);
    gPickupIconScale = GetPrivateProfileIntA("Settings", "PickupIconSize", 8, gIniPath);
    gAssetIconScale = GetPrivateProfileIntA("Settings", "AssetIconSize", 8, gIniPath);
    gDebugLog = GetPrivateProfileIntA("Settings", "DebugLog", 0, gIniPath) != 0;
    TakeIniStamp();
}

static void WriteIniInt(const char *section, const char *key, int value) {
    char buffer[16];
    wsprintfA(buffer, " %d", value);
    WritePrivateProfileStringA(section, key, buffer, gIniPath);
}

static int ReadState(const char *key, int lowest, int highest) {
    int value = GetPrivateProfileIntA("SaveState", key, lowest, gIniPath);
    if (value < lowest)
        value = lowest;
    if (value > highest)
        value = highest;
    return value;
}

static void LoadState() {
    gMode = ReadState("Collectibles", MODE_OFF, MODE_COUNT - 1);
    gRevealMode = ReadState("Pickups", REVEAL_OFF, REVEAL_COUNT - 1);
    gAssetsShown = ReadState("Assets", 0, 1) != 0;

    if (!gRevealRow)
        gRevealMode = REVEAL_OFF;
    if (!gAssetsRow)
        gAssetsShown = false;
}

static void SaveState() {
    WriteIniInt("SaveState", "Collectibles", gMode);
    WriteIniInt("SaveState", "Pickups", gRevealMode);
    WriteIniInt("SaveState", "Assets", gAssetsShown ? 1 : 0);
    TakeIniStamp();
}

static void SaveSettings() {
    WriteIniInt("Settings", "Finder", gFinderEnabled ? 1 : 0);
    WriteIniInt("Settings", "FinderRadarRange", gFinderRange);
    WriteIniInt("Settings", "MapFadeAlpha", gFinderMapAlpha);
    WriteIniInt("Settings", "IconSize", gIconScale);
    WriteIniInt("Settings", "PickupIconSize", gPickupIconScale);
    WriteIniInt("Settings", "AssetIconSize", gAssetIconScale);
    TakeIniStamp();
}

static void PollSettings() {
    unsigned int now = GetTickCount();
    if (now - gLastSettingsPoll < SETTINGS_POLL_MS)
        return;
    gLastSettingsPoll = now;

    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(gIniPath, GetFileExInfoStandard, &data))
        return;
    if (CompareFileTime(&data.ftLastWriteTime, &gIniStamp) == 0)
        return;

    LoadSettings();
    LoadState();
    gIconsValid = false;
}

static void CmdReloadIni() { LoadSettings(); }
static void CmdSaveIni() { SaveSettings(); }
static void OnModeChanged() {
    gIconsValid = false;
    SaveState();
}

static void InitDebugMenu() {
    static bool registered = false;
    if (registered || !DebugMenuLoad())
        return;
    registered = true;

    DebugMenuAddVar(MENU, "Show", &gMode, OnModeChanged, 1, MODE_OFF, MODE_COUNT - 1, TEXT_MODE);
    DebugMenuAddVar(MENU, "Reveal pickups", &gRevealMode, OnModeChanged, 1, REVEAL_OFF, REVEAL_COUNT - 1, TEXT_REVEAL);
    DebugMenuAddVarBool8(MENU, "Show assets", (int8_t *)&gAssetsShown, OnModeChanged);
    DebugMenuAddVarBool8(MENU, "Finder (uncollected)", (int8_t *)&gFinderEnabled, NULL);
    DebugMenuAddVar(MENU, "Finder radar range", &gFinderRange, NULL, 25, 0, 3000, NULL);
    DebugMenuAddVar(MENU, "Finder map fade", &gFinderMapAlpha, NULL, 10, 0, 255, NULL);
    DebugMenuAddVar(MENU, "Collectible size", &gIconScale, NULL, 1, 1, 60, NULL);
    DebugMenuAddVar(MENU, "Pickup size", &gPickupIconScale, NULL, 1, 1, 60, NULL);
    DebugMenuAddVar(MENU, "Asset size", &gAssetIconScale, NULL, 1, 1, 60, NULL);
    DebugMenuAddVarBool8(MENU, "Debug log", (int8_t *)&gDebugLog, NULL);
    DebugMenuAddCmd(MENU, "Reload INI", CmdReloadIni);
    DebugMenuAddCmd(MENU, "Save settings to INI", CmdSaveIni);
}

static void LoadTextures() {
    if (gTexturesTried)
        return;
    gTexturesTried = true;

    int slot = CTxdStore::FindTxdSlot("collectibles");
    if (slot < 0)
        slot = CTxdStore::AddTxdSlot("collectibles");
    if (slot < 0 || !CTxdStore::LoadTxd(slot, PLUGIN_PATH((char *)"MapCollectibles\\collectibles.txd")))
        return;

    CTxdStore::AddRef(slot);
    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(slot);
    for (int i = MODE_TAGS; i < MODE_ALL; i++)
        gSprites[i].SetTexture((char *)TEX_MODE[i]);
    CTxdStore::PopCurrentTxd();

    int revealSlot = CTxdStore::FindTxdSlot("mcpickups");
    if (revealSlot < 0)
        revealSlot = CTxdStore::AddTxdSlot("mcpickups");
    if (revealSlot < 0 || !CTxdStore::LoadTxd(revealSlot, PLUGIN_PATH((char *)"MapCollectibles\\pickups.txd")))
        return;

    CTxdStore::AddRef(revealSlot);
    CTxdStore::PushCurrentTxd();
    CTxdStore::SetCurrentTxd(revealSlot);
    for (int i = REVEAL_ARMOR; i < REVEAL_ALL; i++)
        gRevealSprites[i].SetTexture((char *)TEX_REVEAL[i]);
    gAssetSprite.SetTexture((char *)TEX_ASSET);
    CTxdStore::PopCurrentTxd();
}

static bool PickupStillPresent(const float *coors) {
    for (unsigned int i = 0; i < MAX_NUM_PICKUPS; i++) {
        CPickup &pickup = CPickups::aPickUps[i];
        if (pickup.m_nPickupType == PICKUP_NONE)
            continue;

        float dx = pickup.m_vecPos.x * 0.125f - coors[0];
        float dy = pickup.m_vecPos.y * 0.125f - coors[1];
        float dz = pickup.m_vecPos.z * 0.125f - coors[2];
        if (dx * dx + dy * dy + dz * dz < 4.0f)
            return true;
    }
    return false;
}

static void Sort(bool collected, int type, float x, float y, float z) {
    CollectibleIcon icon;
    icon.posn = CVector2D(x, y);
    icon.z = z;
    icon.type = type;
    (collected ? gCollected : gRemaining).push_back(icon);
}

static void CollectFromTable(int type, const float table[][3], int count) {
    for (int i = 0; i < count; i++)
        Sort(!PickupStillPresent(table[i]), type, table[i][0], table[i][1], table[i][2]);
}

static void GatherType(int type) {
    switch (type) {
    case MODE_TAGS:
        for (int i = 0; i < gNumTags; i++) {
            CEntity *entity = gTagDesc[i].m_pEntity;
            if (entity) {
                const CVector &posn = entity->GetPosition();
                Sort(gTagDesc[i].m_nAlpha > TAG_SPRAYED_ALPHA, type, posn.x, posn.y, posn.z);
            }
        }
        break;
    case MODE_SNAPSHOTS:
        CollectFromTable(type, kSnapshots, sizeof(kSnapshots) / sizeof(kSnapshots[0]));
        break;
    case MODE_HORSESHOES:
        CollectFromTable(type, kHorseshoes, sizeof(kHorseshoes) / sizeof(kHorseshoes[0]));
        break;
    case MODE_OYSTERS:
        CollectFromTable(type, kOysters, sizeof(kOysters) / sizeof(kOysters[0]));
        break;
    case MODE_JUMPS: {
        CStuntJumpsPool *pool = CStuntJumpManager::mp_poolStuntJumps;
        if (!pool)
            break;
        for (int i = 0; i < pool->m_nSize; i++) {
            if (pool->m_byteMap[i].bEmpty)
                continue;
            CStuntJump &jump = pool->m_pObjects[i];
            Sort(jump.done, type, (jump.start.m_vecMin.x + jump.start.m_vecMax.x) * 0.5f,
                                  (jump.start.m_vecMin.y + jump.start.m_vecMax.y) * 0.5f,
                                  (jump.start.m_vecMin.z + jump.start.m_vecMax.z) * 0.5f);
        }
        break;
    }
    default:
        break;
    }
}

static bool IsPersistentPickup(unsigned char type) {
    return type == PICKUP_ON_STREET || type == PICKUP_ON_STREET_SLOW;
}

static int PickupCategory(short modelId) {
    if (modelId <= 0 || modelId >= 20000)
        return REVEAL_OFF;

    if (modelId == MODEL_BODYARMOUR)
        return REVEAL_ARMOR;
    if (modelId == MODEL_BRIBE)
        return REVEAL_BRIBES;
    if (modelId >= MODEL_INFO && modelId <= MODEL_KILLFRENZY)
        return REVEAL_OFF;
    if (CPickups::WeaponForModel(modelId) > 0)
        return REVEAL_WEAPONS;
    return REVEAL_OFF;
}

static void GatherPickups() {
    gPickups.clear();
    if (gRevealMode == REVEAL_OFF)
        return;

    bool playerInside = CGame::currArea != 0;
    FILE *log = NULL;

    if (gDebugLog) {
        log = fopen(PLUGIN_PATH((char *)"SA.MapCollectibles.log"), "a");
        if (log) {
            CVector player = FindPlayerCoors(0);
            fprintf(log, "\n[%u] area=%d player=%.1f,%.1f,%.1f mode=%d\n",
                    GetTickCount(), CGame::currArea, player.x, player.y, player.z, gRevealMode);
        }
    }

    for (unsigned int i = 0; i < MAX_NUM_PICKUPS; i++) {
        CPickup &pickup = CPickups::aPickUps[i];
        if (pickup.m_nPickupType == PICKUP_NONE)
            continue;

        int category = PickupCategory(pickup.m_nModelIndex);
        if (category == REVEAL_OFF)
            continue;

        float x = pickup.m_vecPos.x * 0.125f;
        float y = pickup.m_vecPos.y * 0.125f;
        float z = pickup.m_vecPos.z * 0.125f;

        const char *reject = NULL;
        if (!IsPersistentPickup(pickup.m_nPickupType))
            reject = "transient";
        else if (pickup.m_nFlags.bDisabled)
            reject = "disabled";
        else if ((z >= INTERIOR_Z) != playerInside)
            reject = "other-layer";
        else if (gRevealMode != REVEAL_ALL && gRevealMode != category)
            reject = "other-category";

        if (log) {
            fprintf(log, "  slot=%3u model=%5d type=%2u flags=%02X cat=%d pos=%.1f,%.1f,%.1f %s\n",
                    i, pickup.m_nModelIndex, pickup.m_nPickupType,
                    *(unsigned char *)&pickup.m_nFlags, category, x, y, z,
                    reject ? reject : "SHOWN");
        }

        if (reject)
            continue;

        CollectibleIcon icon;
        icon.posn = CVector2D(x, y);
        icon.z = z;
        icon.type = category;
        gPickups.push_back(icon);
    }

    if (log)
        fclose(log);
}

static void GatherAssets() {
    gAssets.clear();
    if (!gAssetsShown)
        return;

    bool playerInside = CGame::currArea != 0;

    for (unsigned int i = 0; i < MAX_NUM_PICKUPS; i++) {
        CPickup &pickup = CPickups::aPickUps[i];
        if (pickup.m_nPickupType != PICKUP_ASSET_REVENUE)
            continue;

        float z = pickup.m_vecPos.z * 0.125f;
        if ((z >= INTERIOR_Z) != playerInside)
            continue;

        CollectibleIcon icon;
        icon.posn = CVector2D(pickup.m_vecPos.x * 0.125f, pickup.m_vecPos.y * 0.125f);
        icon.z = z;
        icon.type = 0;
        gAssets.push_back(icon);
    }
}

static void RebuildIcons() {
    gCollected.clear();
    gRemaining.clear();
    gIconsValid = true;
    gLastRebuild = GetTickCount();

    if (gMode == MODE_ALL) {
        for (int type = MODE_TAGS; type < MODE_ALL; type++)
            GatherType(type);
    }
    else if (gMode != MODE_OFF) {
        GatherType(gMode);
    }

    GatherPickups();
    GatherAssets();
}

static void RebuildIconsIfStale() {
    if (!gIconsValid || GetTickCount() - gLastRebuild >= REBUILD_INTERVAL_MS)
        RebuildIcons();
}

static float IconHalfSize(int scale) {
    return gScreenWidth * (scale * 0.001f);
}

static float RadarEdgeLimit(float half) {
    CVector2D centre, edgeX, edgeY;
    CRadar::TransformRadarPointToScreenSpace(centre, CVector2D(0.0f, 0.0f));
    CRadar::TransformRadarPointToScreenSpace(edgeX, CVector2D(1.0f, 0.0f));
    CRadar::TransformRadarPointToScreenSpace(edgeY, CVector2D(0.0f, 1.0f));

    float radiusX = fabsf(edgeX.x - centre.x);
    float radiusY = fabsf(edgeY.y - centre.y);
    float marginX = radiusX > 0.0f ? half / radiusX : 0.0f;
    float marginY = radiusY > 0.0f ? half / radiusY : 0.0f;

    float limit = 1.0f - (marginX > marginY ? marginX : marginY);
    if (limit < 0.0f)
        limit = 0.0f;
    return limit * limit;
}

static void DrawMapIcon(CSprite2d &sprite, const CVector2D &posn, unsigned char alpha,
                        float scaleX, float scaleY, float half) {
    if (!sprite.m_pTexture)
        return;

    float x = gMapLeft + (posn.x + WORLD_HALF_SIZE) * scaleX;
    float y = gMapTop + (WORLD_HALF_SIZE - posn.y) * scaleY;

    if (x < gMapLeft || x > gMapLeft + gMapWidth || y < gMapTop || y > gMapTop + gMapHeight)
        return;

    sprite.Draw(CRect(x - half, y - half, x + half, y + half), CRGBA(255, 255, 255, alpha));
}

static void DrawCollectibles() {
    PollSettings();

    if (!gMapRectValid || (gMode == MODE_OFF && gRevealMode == REVEAL_OFF && !gAssetsShown))
        return;

    LoadTextures();
    RebuildIconsIfStale();

    float half = IconHalfSize(gIconScale);
    float pickupHalf = IconHalfSize(gPickupIconScale);
    float scaleX = gMapWidth / (WORLD_HALF_SIZE * 2.0f);
    float scaleY = gMapHeight / (WORLD_HALF_SIZE * 2.0f);

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0 && !gFinderEnabled)
            continue;

        const std::vector<CollectibleIcon> &icons = pass == 0 ? gRemaining : gCollected;
        unsigned char alpha = pass == 0 ? (unsigned char)gFinderMapAlpha : 255;

        for (std::vector<CollectibleIcon>::const_iterator it = icons.begin(); it != icons.end(); ++it)
            DrawMapIcon(gSprites[it->type], it->posn, alpha, scaleX, scaleY, half);
    }

    for (std::vector<CollectibleIcon>::const_iterator it = gPickups.begin(); it != gPickups.end(); ++it)
        DrawMapIcon(gRevealSprites[it->type], it->posn, 255, scaleX, scaleY, pickupHalf);

    float assetHalf = IconHalfSize(gAssetIconScale);
    for (std::vector<CollectibleIcon>::const_iterator it = gAssets.begin(); it != gAssets.end(); ++it)
        DrawMapIcon(gAssetSprite, it->posn, 255, scaleX, scaleY, assetHalf);
}

static void DrawRadarIcon(const CollectibleIcon &icon, CSprite2d &sprite, float playerZ, float half, float limit) {
    if (!sprite.m_pTexture)
        return;

    float dx = icon.posn.x - gRadarOrigin.x;
    float dy = icon.posn.y - gRadarOrigin.y;
    float dz = icon.z - playerZ;
    if (dx * dx + dy * dy + dz * dz > (float)gFinderRange * (float)gFinderRange)
        return;

    CVector2D radarSpace, screen;
    CRadar::TransformRealWorldPointToRadarSpace(radarSpace, icon.posn);

    if (radarSpace.x * radarSpace.x + radarSpace.y * radarSpace.y > limit)
        return;

    CRadar::TransformRadarPointToScreenSpace(screen, radarSpace);
    sprite.Draw(CRect(screen.x - half, screen.y - half, screen.x + half, screen.y + half),
                CRGBA(255, 255, 255, 255));
}

static void DrawRadarCollectibles() {
    PollSettings();

    if (gMenuMapMode || FrontEndMenuManager.m_bMenuActive)
        return;

    bool showFinder = gMode != MODE_OFF && gFinderEnabled;
    bool showPickups = gRevealMode != REVEAL_OFF;
    if (!showFinder && !showPickups && !gAssetsShown)
        return;

    LoadTextures();
    RebuildIconsIfStale();
    float playerZ = FindPlayerCoors(0).z;

    float half = IconHalfSize(gIconScale);
    float pickupHalf = IconHalfSize(gPickupIconScale);
    float limit = RadarEdgeLimit(half);
    float pickupLimit = RadarEdgeLimit(pickupHalf);

    if (showFinder) {
        for (std::vector<CollectibleIcon>::const_iterator it = gRemaining.begin(); it != gRemaining.end(); ++it)
            DrawRadarIcon(*it, gSprites[it->type], playerZ, half, limit);
    }

    if (showPickups) {
        for (std::vector<CollectibleIcon>::const_iterator it = gPickups.begin(); it != gPickups.end(); ++it)
            DrawRadarIcon(*it, gRevealSprites[it->type], playerZ, pickupHalf, pickupLimit);
    }

    if (gAssetsShown) {
        float assetHalf = IconHalfSize(gAssetIconScale);
        float assetLimit = RadarEdgeLimit(assetHalf);
        for (std::vector<CollectibleIcon>::const_iterator it = gAssets.begin(); it != gAssets.end(); ++it)
            DrawRadarIcon(*it, gAssetSprite, playerZ, assetHalf, assetLimit);
    }
}

typedef void (__cdecl *DrawRadarMap_t)();
static const DrawRadarMap_t RadarDrawMap = (DrawRadarMap_t)0x586B00;

static void __cdecl DrawRadarMap_Hook() {
    RadarDrawMap();
    DrawRadarCollectibles();
}

static void SetMapRect(float x1, float y1, float x2, float y2) {
    float width = fabsf(x2 - x1);
    float height = fabsf(y2 - y1);
    if (width <= 0.0f || height <= 0.0f)
        return;

    gMapLeft = x1 < x2 ? x1 : x2;
    gMapTop = y1 < y2 ? y1 : y2;
    gMapWidth = width;
    gMapHeight = height;
    gMapRectValid = true;
}

typedef void (__cdecl *DrawRadarSectionMap_t)(int, int, float, float, float, float);
static const DrawRadarSectionMap_t DrawRadarSectionMap = (DrawRadarSectionMap_t)0x586520;

static void __cdecl DrawRadarSectionMap_Hook(int tileX, int tileY, float left, float bottom, float right, float top) {
    float tileWidth = fabsf(right - left);
    float tileHeight = fabsf(bottom - top);

    if (tileWidth > 0.0f && tileHeight > 0.0f) {
        float x1 = (left < right ? left : right) - tileX * tileWidth;
        float y1 = (top < bottom ? top : bottom) - tileY * tileHeight;
        SetMapRect(x1, y1, x1 + tileWidth * MAP_TILES_PER_AXIS, y1 + tileHeight * MAP_TILES_PER_AXIS);
    }

    DrawRadarSectionMap(tileX, tileY, left, bottom, right, top);
}

typedef void (__thiscall *Sprite2dDraw_t)(CSprite2d *, CRect const &, CRGBA const &);
static const Sprite2dDraw_t Sprite2dDraw = (Sprite2dDraw_t)0x728350;

static void __fastcall MapSprite_Hook(CSprite2d *self, void *, CRect const &rect, CRGBA const &colour) {
    const float *bounds = &rect.left;
    SetMapRect(bounds[0], bounds[1], bounds[2], bounds[3]);
    Sprite2dDraw(self, rect, colour);
}

static void __cdecl DrawBlips_Hook() {
    CRadar::DrawBlips();
    DrawCollectibles();
}

static char *RowLabelKey(int kind) {
    if (kind == ROW_ASSETS)
        return (char *)KEY_ASSET_LABEL;
    if (kind == ROW_COLLECT)
        return (char *)KEY_LABEL;
    return (char *)KEY_REVEAL_LABEL;
}

static char *RowValueKey(int kind) {
    if (kind == ROW_ASSETS)
        return (char *)(gAssetsShown ? KEY_ON : KEY_OFF);
    if (kind == ROW_COLLECT)
        return (char *)KEY_MODE[gMode];
    return (char *)KEY_REVEAL[gRevealMode];
}

static void BuildRowLayout() {
    for (int i = 0; i < MAX_MENU_ROWS; i++)
        gRowKind[i] = -1;
    for (int i = 0; i < ROW_KIND_COUNT; i++)
        gKindRow[i] = -1;

    int row = FIRST_EXTRA_ROW;
    if (gAssetsRow) {
        gKindRow[ROW_ASSETS] = row;
        gRowKind[row++] = ROW_ASSETS;
    }
    gKindRow[ROW_COLLECT] = row;
    gRowKind[row++] = ROW_COLLECT;
    if (gRevealRow) {
        gKindRow[ROW_PICKUPS] = row;
        gRowKind[row++] = ROW_PICKUPS;
    }
    gLastRow = row - 1;
}

static tMenuPanel ** __cdecl InsertMenu_Hook(unsigned char panelId, unsigned char columnId, char *title,
                                             char *s1, char *s2, char *s3, char *s4, char *s5, char *s6,
                                             char *s7, char *s8, char *s9, char *s10, char *s11, char *s12) {
    char *slots[MAX_MENU_ROWS] = { s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12 };

    if (columnId == 0 || columnId == 1) {
        for (int kind = 0; kind < ROW_KIND_COUNT; kind++) {
            int row = gKindRow[kind];
            if (row >= 0)
                slots[row] = columnId == 0 ? RowLabelKey(kind) : RowValueKey(kind);
        }
    }

    if (columnId == 0) {
        gIconsValid = false;

        tMenuPanel *panel = MenuNumber[panelId];
        if (panel) {
            float wanted = panel->m_afColumnWidth[0] * 1.6f;
            if (panel->m_afColumnWidth[1] < wanted)
                panel->m_afColumnWidth[1] = wanted;
        }
    }

    return CMenuSystem::InsertMenu(panelId, columnId, title, slots[0], slots[1], slots[2], slots[3],
                                   slots[4], slots[5], slots[6], slots[7], slots[8], slots[9],
                                   slots[10], slots[11]);
}

static char __cdecl CheckForAccept_Hook(unsigned char panelId) {
    char row = CMenuSystem::CheckForAccept(panelId);
    if (row < FIRST_EXTRA_ROW || row >= MAX_MENU_ROWS || gRowKind[row] < 0)
        return row;

    int kind = gRowKind[row];
    if (kind == ROW_COLLECT)
        gMode = (gMode + 1) % MODE_COUNT;
    else if (kind == ROW_PICKUPS)
        gRevealMode = (gRevealMode + 1) % REVEAL_COUNT;
    else
        gAssetsShown = !gAssetsShown;

    OnModeChanged();
    CMenuSystem::InsertOneMenuItem(panelId, 1, (unsigned char)row, RowValueKey(kind));

    if (MenuNumber[panelId])
        MenuNumber[panelId]->m_nAcceptedRow = -99;
    return -1;
}

static const char * __fastcall TextGet_Hook(CText *self, void *, const char *key) {
    if (key) {
        if (!strcmp(key, KEY_LABEL))
            return TEXT_LABEL;
        for (int i = 0; i < MODE_COUNT; i++) {
            if (!strcmp(key, KEY_MODE[i]))
                return TEXT_MODE[i];
        }
        if (!strcmp(key, KEY_REVEAL_LABEL))
            return TEXT_REVEAL_LABEL;
        if (!strcmp(key, KEY_ASSET_LABEL))
            return TEXT_ASSET_LABEL;
        for (int i = 0; i < REVEAL_COUNT; i++) {
            if (!strcmp(key, KEY_REVEAL[i]))
                return TEXT_REVEAL[i];
        }
    }
    return self->Get(key);
}

class MapCollectiblesPlugin {
public:
    MapCollectiblesPlugin() {
        if (*(unsigned char *)CALL_INSERT_COLUMN_LABELS != 0xE8 ||
            *(unsigned char *)CALL_INSERT_COLUMN_STATES != 0xE8 ||
            *(unsigned char *)CALL_INSERT_COLUMN_UPDATE != 0xE8 ||
            *(unsigned char *)CALL_CHECK_FOR_ACCEPT != 0xE8 ||
            *(unsigned char *)CALL_DRAW_MAP_SECTION != 0xE8 ||
            *(unsigned char *)CALL_DRAW_MAP_SPRITE != 0xE8 ||
            *(unsigned char *)CALL_DRAW_MAP_BLIPS != 0xE8 ||
            *(unsigned char *)CALL_DRAW_RADAR_MAP != 0xE8 ||
            *(unsigned char *)(BYTE_LAST_ROW_INDEX - 1) != 0x3C ||
            *(unsigned char *)BYTE_LAST_ROW_INDEX != 0x04)
            return;

        lstrcpynA(gIniPath, PLUGIN_PATH((char *)"SA.MapCollectibles.ini"), MAX_PATH);
        gLastSettingsPoll = GetTickCount();
        LoadSettings();
        gRevealRow = GetPrivateProfileIntA("Settings", "Pickups", 0, gIniPath) != 0;
        gAssetsRow = GetPrivateProfileIntA("Settings", "Assets", 0, gIniPath) != 0;
        BuildRowLayout();
        LoadState();

        Events::initGameEvent += [] { InitDebugMenu(); };

        patch::RedirectCall(CALL_INSERT_COLUMN_LABELS, InsertMenu_Hook);
        patch::RedirectCall(CALL_INSERT_COLUMN_STATES, InsertMenu_Hook);
        patch::RedirectCall(CALL_INSERT_COLUMN_UPDATE, InsertMenu_Hook);
        patch::RedirectCall(CALL_CHECK_FOR_ACCEPT, CheckForAccept_Hook);
        patch::RedirectCall(CALL_DRAW_MAP_SECTION, DrawRadarSectionMap_Hook);
        patch::RedirectCall(CALL_DRAW_MAP_SPRITE, MapSprite_Hook);
        patch::RedirectCall(CALL_DRAW_MAP_BLIPS, DrawBlips_Hook);
        patch::RedirectCall(CALL_DRAW_RADAR_MAP, DrawRadarMap_Hook);
        patch::SetUChar(BYTE_LAST_ROW_INDEX, (unsigned char)gLastRow);

        for (uintptr_t address : CALL_TEXT_GET) {
            if (*(unsigned char *)address == 0xE8)
                patch::RedirectCall(address, TextGet_Hook);
        }
    }
} mapCollectiblesPlugin;
