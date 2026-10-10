#include "plugin.h"
#include "common.h"
#include "CPlayerPed.h"
#include "CWorld.h"
#include "CPickups.h"
#include "CTagManager.h"
#include "CAudioEngine.h"
#include "CMenuManager.h"
#include "CTimer.h"
#include "CRadar.h"
#include "CSprite2d.h"
#include "RenderWare.h"
#include "CStuntJumpManager.h"
#include "CHud.h"
#include "CCutsceneMgr.h"
#include "CollectibleCoords.h"
#include "debugmenu_public.h"
#include <vector>
#include <cmath>
#include <windows.h>

using namespace plugin;

enum CollectibleType {
    MODE_TAGS = 0,
    MODE_SNAPSHOTS = 1,
    MODE_HORSESHOES = 2,
    MODE_OYSTERS = 3,
    MODE_JUMPS = 4,
    MODE_COUNT = 5
};

struct CollectibleItem {
    CVector pos;
    CVector2D searchCenter; // Centro desfasado para la zona estilo RDR2
    int type;
    int id;
};

// Graffitis en memoria
struct tTagDesc {
    CEntity *m_pEntity;
    unsigned char m_nAlpha;
    char _pad[3];
};
static tTagDesc *const gTagDesc = (tTagDesc *)0xA9A8C0;
static int &gNumTags = *(int *)0xA9AD70;
static const unsigned char TAG_SPRAYED_ALPHA = 0xE4;

// Configuracion desde el INI
static bool gEnableHelpMessage = true;
static bool gEnableSearchZones = true;
static bool gShowExactIcons = false;       // Por defecto falso para buscar organico
static float gSearchRadius = 55.0f;       // Radio en metros de la zona (~1 cuadra)
static float gNotificationDist = 60.0f;   // Distancia para avisar con la cajita negra

// Estados y control anti-glitch
static std::vector<CollectibleItem> gRemaining;
static std::vector<CollectibleItem> gCollected;
static unsigned int gLastRebuild = 0;
static unsigned int gLastSettingsPoll = 0;
static unsigned int gLastScanTime = 0;
static int gCurrentNearZoneId = -1;
static unsigned int gLastHelpMessageTime = 0;

// Texturas originales de Flentric (para cuando quieras el Plan B)
static RwTexture *gCollectiblesTex = nullptr;
static RwTexture *gPickupsTex = nullptr;

// Calculo del centro desfasado determinista (Fijo, nunca tiembla ni vibra)
static CVector2D ComputeSearchZoneCenter(const CVector &pos, int id) {
    unsigned int hash = (static_cast<unsigned int>(id) * 2654435761u) ^
                        (static_cast<unsigned int>(pos.x * 10.0f) * 2246822519u) ^
                        (static_cast<unsigned int>(pos.y * 10.0f) * 3266489917u);

    float angle = (hash % 360) * (3.14159265f / 180.0f);
    float offsetDist = 18.0f + static_cast<float>((hash >> 8) % 11); // Desfase de 18 a 28 metros

    return CVector2D(pos.x + cosf(angle) * offsetDist,
                     pos.y + sinf(angle) * offsetDist);
}

static void PollSettings() {
    char iniPath[MAX_PATH];
    GetFullPathNameA("SA.MapCollectibles.ini", MAX_PATH, iniPath, nullptr);

    if (GetFileAttributesA(iniPath) != INVALID_FILE_ATTRIBUTES) {
        gEnableHelpMessage = GetPrivateProfileIntA("Settings", "EnableHelpMessage", 1, iniPath) != 0;
        gEnableSearchZones = GetPrivateProfileIntA("Settings", "EnableSearchZones", 1, iniPath) != 0;
        gShowExactIcons    = GetPrivateProfileIntA("Settings", "ShowExactIcons", 0, iniPath) != 0;

        int r = GetPrivateProfileIntA("Settings", "SearchZoneRadius", 55, iniPath);
        if (r > 10) gSearchRadius = static_cast<float>(r);

        int d = GetPrivateProfileIntA("Settings", "NotificationDistance", 60, iniPath);
        if (d > 10) gNotificationDist = static_cast<float>(d);
    }
}

static bool PickupStillPresent(const float coord[3]) {
    for (int i = 0; i < 620; ++i) {
        CPickup &p = CPickups::aPickUps[i];
        if (p.m_nPickupType == 0) continue;
        CVector pos = p.m_vecPos;
        float dx = pos.x - coord[0];
        float dy = pos.y - coord[1];
        float dz = pos.z - coord[2];
        if ((dx * dx + dy * dy + dz * dz) < 1.0f) {
            return true;
        }
    }
    return false;
}

static void GatherType(int type) {
    int idCounter = type * 1000;
    switch (type) {
    case MODE_TAGS: {
        for (int i = 0; i < gNumTags; ++i) {
            if (!gTagDesc[i].m_pEntity) continue;
            CVector pos = gTagDesc[i].m_pEntity->GetPosition();
            CVector2D center = ComputeSearchZoneCenter(pos, idCounter + i);
            if (gTagDesc[i].m_nAlpha <= TAG_SPRAYED_ALPHA) {
                gRemaining.push_back({ pos, center, MODE_TAGS, idCounter + i });
            } else {
                gCollected.push_back({ pos, center, MODE_TAGS, idCounter + i });
            }
        }
        break;
    }
    case MODE_SNAPSHOTS: {
        for (int i = 0; i < 50; ++i) {
            CVector pos(kSnapshots[i][0], kSnapshots[i][1], kSnapshots[i][2]);
            CVector2D center = ComputeSearchZoneCenter(pos, idCounter + i);
            if (PickupStillPresent(kSnapshots[i])) {
                gRemaining.push_back({ pos, center, MODE_SNAPSHOTS, idCounter + i });
            } else {
                gCollected.push_back({ pos, center, MODE_SNAPSHOTS, idCounter + i });
            }
        }
        break;
    }
    case MODE_HORSESHOES: {
        for (int i = 0; i < 50; ++i) {
            CVector pos(kHorseshoes[i][0], kHorseshoes[i][1], kHorseshoes[i][2]);
            CVector2D center = ComputeSearchZoneCenter(pos, idCounter + i);
            if (PickupStillPresent(kHorseshoes[i])) {
                gRemaining.push_back({ pos, center, MODE_HORSESHOES, idCounter + i });
            } else {
                gCollected.push_back({ pos, center, MODE_HORSESHOES, idCounter + i });
            }
        }
        break;
    }
    case MODE_OYSTERS: {
        for (int i = 0; i < 50; ++i) {
            CVector pos(kOysters[i][0], kOysters[i][1], kOysters[i][2]);
            CVector2D center = ComputeSearchZoneCenter(pos, idCounter + i);
            if (PickupStillPresent(kOysters[i])) {
                gRemaining.push_back({ pos, center, MODE_OYSTERS, idCounter + i });
            } else {
                gCollected.push_back({ pos, center, MODE_OYSTERS, idCounter + i });
            }
        }
        break;
    }
    case MODE_JUMPS: {
        if (CStuntJumpManager::mp_poolStuntJumps) {
            for (int i = 0; i < CStuntJumpManager::mp_poolStuntJumps->m_nSize; ++i) {
                CStuntJump *jump = CStuntJumpManager::mp_poolStuntJumps->GetAt(i);
                if (jump) {
                    CVector center = (jump->start.m_vecMin + jump->start.m_vecMax) * 0.5f;
                    CVector2D sCenter = ComputeSearchZoneCenter(center, idCounter + i);
                    if (!jump->done) {
                        gRemaining.push_back({ center, sCenter, MODE_JUMPS, idCounter + i });
                    } else {
                        gCollected.push_back({ center, sCenter, MODE_JUMPS, idCounter + i });
                    }
                }
            }
        }
        break;
    }
    }
}

static void RebuildIcons() {
    gRemaining.clear();
    gCollected.clear();
    for (int t = 0; t < MODE_COUNT; ++t) {
        GatherType(t);
    }
}

static void RebuildIconsIfStale() {
    unsigned int now = GetTickCount();
    if (now - gLastRebuild >= 1000) {
        RebuildIcons();
        gLastRebuild = now;
    }
}

// Dibujado del circulo con borde difuso estilo RDR2 (Degradado por hardware)
static void DrawSoftSearchCircle(float screenX, float screenY, float radius, unsigned char r, unsigned char g, unsigned char b, unsigned char centerAlpha) {
    const int NUM_SEGS = 24;
    RwIm2DVertex vertices[NUM_SEGS + 2];

    // Vertice central: opacidad suave
    RwIm2DVertexSetScreenX(&vertices[0], screenX);
    RwIm2DVertexSetScreenY(&vertices[0], screenY);
    RwIm2DVertexSetScreenZ(&vertices[0], 0.0f);
    RwIm2DVertexSetRecipCameraZ(&vertices[0], 1.0f);
    RwIm2DVertexSetIntRGBA(&vertices[0], r, g, b, centerAlpha);

    // Vertices del borde: Alpha 0 (borde completamente desvanecido)
    for (int i = 0; i <= NUM_SEGS; ++i) {
        float angle = (i % NUM_SEGS) * (6.2831853f / static_cast<float>(NUM_SEGS));
        float vx = screenX + cosf(angle) * radius;
        float vy = screenY + sinf(angle) * radius;

        RwIm2DVertexSetScreenX(&vertices[i + 1], vx);
        RwIm2DVertexSetScreenY(&vertices[i + 1], vy);
        RwIm2DVertexSetScreenZ(&vertices[i + 1], 0.0f);
        RwIm2DVertexSetRecipCameraZ(&vertices[i + 1], 1.0f);
        RwIm2DVertexSetIntRGBA(&vertices[i + 1], r, g, b, 0);
    }

    RwRenderStateSet(rwRENDERSTATETEXTURERASTER, (void *)nullptr);
    RwRenderStateSet(rwRENDERSTATEZTESTENABLE, (void *)FALSE);
    RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void *)FALSE);
    RwRenderStateSet(rwRENDERSTATEVERTEXALPHAENABLE, (void *)TRUE);
    RwRenderStateSet(rwRENDERSTATESRCBLEND, (void *)rwBLENDSRCALPHA);
    RwRenderStateSet(rwRENDERSTATEDESTBLEND, (void *)rwBLENDINVSRCALPHA);

    RwIm2DVertexPrimitive(rwPRIMTYPETRIFAN, vertices, NUM_SEGS + 2);
}

// Verificacion y disparo de la cajita negra (Con Histeresis Anti-Spam)
static void UpdateProximityLogic() {
    if (!gEnableHelpMessage) return;

    // Bloquear si el juego esta en pausa, cinematica o interior
    if (FrontEndMenuManager.m_bMenuActive || CTimer::m_UserPause || CCutsceneMgr::ms_running) return;

    CPlayerPed *player = FindPlayerPed();
    if (!player || player->m_fHealth <= 0.0f || player->m_nAreaCode != 0) return;

    // Escanear cada 200ms para 0% de lag
    unsigned int now = CTimer::m_snTimeInMilliseconds;
    if (now - gLastScanTime < 200) return;
    gLastScanTime = now;

    CVector playerPos = player->GetPosition();
    float minDistanceSq = 9999999.0f;
    int closestId = -1;

    for (const auto &item : gRemaining) {
        float dx = playerPos.x - item.pos.x;
        float dy = playerPos.y - item.pos.y;
        float dz = playerPos.z - item.pos.z;
        float dSq = dx * dx + dy * dy + dz * dz;
        if (dSq < minDistanceSq) {
            minDistanceSq = dSq;
            closestId = item.id;
        }
    }

    float notifyThresholdSq = gNotificationDist * gNotificationDist;
    float resetThresholdSq  = (gNotificationDist + 30.0f) * (gNotificationDist + 30.0f); // 90 metros para resetear

    if (closestId != -1 && minDistanceSq <= notifyThresholdSq) {
        // Entro a una zona nueva
        if (gCurrentNearZoneId != closestId && (now - gLastHelpMessageTime > 8000)) {
            gCurrentNearZoneId = closestId;
            gLastHelpMessageTime = now;
            // Cajita negra nativa oficial con el sonido original del juego
            CHud::SetHelpMessage("Zona de busqueda: Hay un coleccionable en las inmediaciones.", true, false, false);
        }
    } else if (minDistanceSq > resetThresholdSq) {
        // Se alejo lo suficiente, reactivar para cuando vuelva
        gCurrentNearZoneId = -1;
    }
}

// Dibujado de las zonas de busqueda en el radar
static void DrawRadarSearchZones() {
    if (!gEnableSearchZones) return;

    CPlayerPed *player = FindPlayerPed();
    if (!player) return;
    CVector playerPos = player->GetPosition();

    float radarMaxRange = *reinterpret_cast<float *>(0xBA8314);
    if (radarMaxRange <= 1.0f) radarMaxRange = 150.0f;

    for (const auto &item : gRemaining) {
        float dx = playerPos.x - item.searchCenter.x;
        float dy = playerPos.y - item.searchCenter.y;
        if ((dx * dx + dy * dy) > (radarMaxRange * radarMaxRange * 1.5f)) continue;

        CVector2D radarPoint;
        CRadar::TransformRealWorldPointToRadarSpace(radarPoint, item.searchCenter);
        if (radarPoint.Magnitude() > 1.3f) continue;

        CVector2D screenPos;
        CRadar::TransformRadarPointToScreenSpace(screenPos, radarPoint);

        float radarRadiusPixels = (RsGlobal.maximumWidth * 0.075f);
        float screenRadius = (gSearchRadius / radarMaxRange) * radarRadiusPixels;
        if (screenRadius < 10.0f) screenRadius = 10.0f;

        // Color ambar difuso estilo RDR2
        DrawSoftSearchCircle(screenPos.x, screenPos.y, screenRadius, 245, 205, 50, 75);
    }
}

class SearchCollectiblesPlugin {
public:
    SearchCollectiblesPlugin() {
        Events::initGameEvent += [] {
            PollSettings();
            RebuildIcons();
        };

        Events::gameProcessEvent += [] {
            unsigned int now = GetTickCount();
            if (now - gLastSettingsPoll >= 2000) {
                PollSettings();
                gLastSettingsPoll = now;
            }
            RebuildIconsIfStale();
            UpdateProximityLogic();
        };

        Events::drawRadarOverlayEvent += [] {
            DrawRadarSearchZones();
        };
    }
} gSearchPlugin;
