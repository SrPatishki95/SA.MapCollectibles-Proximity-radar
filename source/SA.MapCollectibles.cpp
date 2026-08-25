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
#include "CollectibleCoords.h"
#include <string.h>
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

static const int COLLECT_ROW = 5;

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
static bool gFinderEnabled = false;
static int  gFinderRange = 250;
static int  gFinderMapAlpha = 120;
static int  gIconScale = 10;
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
    int       type;
};

static std::vector<CollectibleIcon> gCollected;
static std::vector<CollectibleIcon> gRemaining;

static void TakeIniStamp() {
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (GetFileAttributesExA(gIniPath, GetFileExInfoStandard, &data))
        gIniStamp = data.ftLastWriteTime;
}

static void LoadSettings() {
    gFinderEnabled = GetPrivateProfileIntA("Settings", "Finder", 1, gIniPath) != 0;
    gFinderRange = GetPrivateProfileIntA("Settings", "FinderRadarRange", 250, gIniPath);
    gFinderMapAlpha = GetPrivateProfileIntA("Settings", "MapFadeAlpha", 120, gIniPath);
    gIconScale = GetPrivateProfileIntA("Settings", "IconSize", 10, gIniPath);
    TakeIniStamp();
}

static void WriteIniInt(const char *key, int value) {
    char buffer[16];
    wsprintfA(buffer, "%d", value);
    WritePrivateProfileStringA("Settings", key, buffer, gIniPath);
}

static void SaveSettings() {
    WriteIniInt("Finder", gFinderEnabled ? 1 : 0);
    WriteIniInt("FinderRadarRange", gFinderRange);
    WriteIniInt("MapFadeAlpha", gFinderMapAlpha);
    WriteIniInt("IconSize", gIconScale);
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
}

static void CmdReloadIni() { LoadSettings(); }
static void CmdSaveIni() { SaveSettings(); }
static void OnModeChanged() { gIconsValid = false; }

static void InitDebugMenu() {
    static bool registered = false;
    if (registered || !DebugMenuLoad())
        return;
    registered = true;

    DebugMenuAddVar(MENU, "Show", &gMode, OnModeChanged, 1, MODE_OFF, MODE_COUNT - 1, TEXT_MODE);
    DebugMenuAddVarBool8(MENU, "Finder (uncollected)", (int8_t *)&gFinderEnabled, NULL);
    DebugMenuAddVar(MENU, "Finder radar range", &gFinderRange, NULL, 25, 0, 3000, NULL);
    DebugMenuAddVar(MENU, "Finder map fade", &gFinderMapAlpha, NULL, 10, 0, 255, NULL);
    DebugMenuAddVar(MENU, "Icon size (1/1000 screen)", &gIconScale, NULL, 1, 1, 60, NULL);
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

static void Sort(bool collected, int type, float x, float y) {
    CollectibleIcon icon;
    icon.posn = CVector2D(x, y);
    icon.type = type;
    (collected ? gCollected : gRemaining).push_back(icon);
}

static void CollectFromTable(int type, const float table[][3], int count) {
    for (int i = 0; i < count; i++)
        Sort(!PickupStillPresent(table[i]), type, table[i][0], table[i][1]);
}

static void GatherType(int type) {
    switch (type) {
    case MODE_TAGS:
        for (int i = 0; i < gNumTags; i++) {
            CEntity *entity = gTagDesc[i].m_pEntity;
            if (entity) {
                const CVector &posn = entity->GetPosition();
                Sort(gTagDesc[i].m_nAlpha > TAG_SPRAYED_ALPHA, type, posn.x, posn.y);
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
                                  (jump.start.m_vecMin.y + jump.start.m_vecMax.y) * 0.5f);
        }
        break;
    }
    default:
        break;
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
}

static void RebuildIconsIfStale() {
    if (!gIconsValid || GetTickCount() - gLastRebuild >= REBUILD_INTERVAL_MS)
        RebuildIcons();
}

static float IconHalfSize() {
    return gScreenWidth * (gIconScale * 0.001f);
}

static void DrawCollectibles() {
    PollSettings();

    if (gMode == MODE_OFF || !gMapRectValid)
        return;

    LoadTextures();
    RebuildIconsIfStale();

    float half = IconHalfSize();
    float scaleX = gMapWidth / (WORLD_HALF_SIZE * 2.0f);
    float scaleY = gMapHeight / (WORLD_HALF_SIZE * 2.0f);

    for (int pass = 0; pass < 2; pass++) {
        if (pass == 0 && !gFinderEnabled)
            continue;

        const std::vector<CollectibleIcon> &icons = pass == 0 ? gRemaining : gCollected;
        unsigned char alpha = pass == 0 ? (unsigned char)gFinderMapAlpha : 255;

        for (std::vector<CollectibleIcon>::const_iterator it = icons.begin(); it != icons.end(); ++it) {
            if (!gSprites[it->type].m_pTexture)
                continue;

            float x = gMapLeft + (it->posn.x + WORLD_HALF_SIZE) * scaleX;
            float y = gMapTop + (WORLD_HALF_SIZE - it->posn.y) * scaleY;

            if (x < gMapLeft || x > gMapLeft + gMapWidth || y < gMapTop || y > gMapTop + gMapHeight)
                continue;

            gSprites[it->type].Draw(CRect(x - half, y - half, x + half, y + half), CRGBA(255, 255, 255, alpha));
        }
    }
}

static void DrawRadarCollectibles() {
    PollSettings();

    if (gMode == MODE_OFF || !gFinderEnabled)
        return;

    if (gMenuMapMode || FrontEndMenuManager.m_bMenuActive)
        return;

    LoadTextures();
    RebuildIconsIfStale();
    float half = IconHalfSize();

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
    limit *= limit;

    for (std::vector<CollectibleIcon>::const_iterator it = gRemaining.begin(); it != gRemaining.end(); ++it) {
        if (!gSprites[it->type].m_pTexture)
            continue;

        float dx = it->posn.x - gRadarOrigin.x;
        float dy = it->posn.y - gRadarOrigin.y;
        if (dx * dx + dy * dy > (float)gFinderRange * (float)gFinderRange)
            continue;

        CVector2D radarSpace, screen;
        CRadar::TransformRealWorldPointToRadarSpace(radarSpace, it->posn);

        if (radarSpace.x * radarSpace.x + radarSpace.y * radarSpace.y > limit)
            continue;

        CRadar::TransformRadarPointToScreenSpace(screen, radarSpace);
        gSprites[it->type].Draw(CRect(screen.x - half, screen.y - half, screen.x + half, screen.y + half),
                                CRGBA(255, 255, 255, 255));
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

static tMenuPanel ** __cdecl InsertMenu_Hook(unsigned char panelId, unsigned char columnId, char *title,
                                             char *s1, char *s2, char *s3, char *s4, char *s5, char *s6,
                                             char *s7, char *s8, char *s9, char *s10, char *s11, char *s12) {
    if (columnId == 0) {
        s6 = (char *)KEY_LABEL;
        gIconsValid = false;
        if (MenuNumber[panelId])
            MenuNumber[panelId]->m_afColumnWidth[1] = MenuNumber[panelId]->m_afColumnWidth[0] * 1.6f;
    }
    else if (columnId == 1) {
        s6 = (char *)KEY_MODE[gMode];
    }

    return CMenuSystem::InsertMenu(panelId, columnId, title, s1, s2, s3, s4, s5, s6, s7, s8, s9, s10, s11, s12);
}

static char __cdecl CheckForAccept_Hook(unsigned char panelId) {
    char row = CMenuSystem::CheckForAccept(panelId);
    if (row != COLLECT_ROW)
        return row;

    gMode = (gMode + 1) % MODE_COUNT;
    gIconsValid = false;
    CMenuSystem::InsertOneMenuItem(panelId, 1, COLLECT_ROW, (char *)KEY_MODE[gMode]);

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

        Events::initGameEvent += [] { InitDebugMenu(); };

        patch::RedirectCall(CALL_INSERT_COLUMN_LABELS, InsertMenu_Hook);
        patch::RedirectCall(CALL_INSERT_COLUMN_STATES, InsertMenu_Hook);
        patch::RedirectCall(CALL_INSERT_COLUMN_UPDATE, InsertMenu_Hook);
        patch::RedirectCall(CALL_CHECK_FOR_ACCEPT, CheckForAccept_Hook);
        patch::RedirectCall(CALL_DRAW_MAP_SECTION, DrawRadarSectionMap_Hook);
        patch::RedirectCall(CALL_DRAW_MAP_SPRITE, MapSprite_Hook);
        patch::RedirectCall(CALL_DRAW_MAP_BLIPS, DrawBlips_Hook);
        patch::RedirectCall(CALL_DRAW_RADAR_MAP, DrawRadarMap_Hook);
        patch::SetUChar(BYTE_LAST_ROW_INDEX, COLLECT_ROW);

        for (uintptr_t address : CALL_TEXT_GET) {
            if (*(unsigned char *)address == 0xE8)
                patch::RedirectCall(address, TextGet_Hook);
        }
    }
} mapCollectiblesPlugin;
