#include "plugin.h"
#include "common.h"
#include "CPlayerPed.h"
#include "CWorld.h"
#include "CPickups.h"
#include "CAudioEngine.h"
#include "CMenuManager.h"
#include "CTimer.h"
#include "CRadar.h"
#include "CSprite2d.h"
#include "CStuntJumpManager.h"
#include "CHud.h"
#include "CCutsceneMgr.h"
#include "CollectibleCoords.h"
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
    CVector2D searchCenter;
    int type;
    int id;
};

// Graffitis en memoria
struct MyTagDesc {
    CEntity *m_pEntity;
    unsigned char m_nAlpha;
    char _pad[3];
};
static MyTagDesc *const gTagDesc = (MyTagDesc *)0xA9A8C0;
static int &gNumTags = *(int *)0xA9AD70;
static const unsigned char TAG_SPRAYED_ALPHA = 0xE4;

// Configuracion
static bool gEnableHelpMessage = true;
static bool gEnableSearchZones = true;
static bool gShowExactIcons = false;       // Se conmuta con F12
static float gSearchRadius = 55.0f;       // Radio en metros (~1 cuadra)
static float gNotificationDist = 60.0f;   // Distancia de la cajita negra

// Estados
static std::vector<CollectibleItem> gRemaining;
static std::vector<CollectibleItem> gCollected;
static unsigned int gLastRebuild = 0;
static unsigned int gLastScanTime = 0;
static int gCurrentNearZoneId = -1;
static unsigned int gLastHelpMessageTime = 0;
static bool gF12WasPressed = false;

// Calculo del centro desfasado determinista (Fijo, nunca tiembla)
static CVector2D ComputeSearchZoneCenter(const CVector &pos, int id) {
    unsigned int hash = (static_cast<unsigned int>(id) * 2654435761u) ^
                        (static_cast<unsigned int>(pos.x * 10.0f) * 2246822519u) ^
                        (static_cast<unsigned int>(pos.y * 10.0f) * 3266489917u);

    float angle = (hash % 360) * (3.14159265f / 180.0f);
    float offsetDist = 18.0f + static_cast<float>((hash >> 8) % 11);

    return CVector2D(pos.x + cosf(angle) * offsetDist,
                     pos.y + sinf(angle) * offsetDist);
}

static bool PickupStillPresent(const float coord[3]) {
    for (int i = 0; i < 620; ++i) {
        CPickup &p = CPickups::aPickUps[i];
        if (p.m_nPickupType == 0) continue;
        float px = static_cast<float>(p.m_vecPos.x) / 8.0f;
        float py = static_cast<float>(p.m_vecPos.y) / 8.0f;
        float pz = static_cast<float>(p.m_vecPos.z) / 8.0f;
        float dx = px - coord[0];
        float dy = py - coord[1];
        float dz = pz - coord[2];
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

// Atajo F12
static void CheckF12Hotkey() {
    bool isDown = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (isDown && !gF12WasPressed) {
        gF12WasPressed = true;
        gShowExactIcons = !gShowExactIcons;

        if (gShowExactIcons) {
            CHud::SetHelpMessage("Iconos de coleccionables: ACTIVADOS", true, false, false);
        } else {
            CHud::SetHelpMessage("Iconos de coleccionables: DESACTIVADOS", true, false, false);
        }
    } else if (!isDown) {
        gF12WasPressed = false;
    }
}

// Logica de proximidad (Cajita negra con sonido)
static void UpdateProximityLogic() {
    CheckF12Hotkey();

    if (!gEnableHelpMessage) return;
    if (FrontEndMenuManager.m_bMenuActive || CTimer::m_UserPause || CCutsceneMgr::ms_running) return;

    CPlayerPed *player = FindPlayerPed();
    if (!player || player->m_fHealth <= 0.0f || player->m_nAreaCode != 0) return;

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
    float resetThresholdSq  = (gNotificationDist + 30.0f) * (gNotificationDist + 30.0f);

    if (closestId != -1 && minDistanceSq <= notifyThresholdSq) {
        if (gCurrentNearZoneId != closestId && (now - gLastHelpMessageTime > 8000)) {
            gCurrentNearZoneId = closestId;
            gLastHelpMessageTime = now;
            CHud::SetHelpMessage("Zona de busqueda: Hay un coleccionable en las inmediaciones.", true, false, false);
        }
    } else if (minDistanceSq > resetThresholdSq) {
        gCurrentNearZoneId = -1;
    }
}

// Dibujado en el Radar oficial 100% nativo de GTA SA
static void DrawRadarElements() {
    CPlayerPed *player = FindPlayerPed();
    if (!player) return;
    CVector playerPos = player->GetPosition();

    for (const auto &item : gRemaining) {
        float dx = playerPos.x - item.searchCenter.x;
        float dy = playerPos.y - item.searchCenter.y;

        // Solo procesar si esta a menos de 250 metros
        if ((dx * dx + dy * dy) <= (250.0f * 250.0f)) {

            // 1. Zona difusa estilo RDR2 usando CRect empaquetado
            if (gEnableSearchZones) {
                CRect zoneRect(
                    item.searchCenter.x - gSearchRadius,
                    item.searchCenter.y - gSearchRadius,
                    item.searchCenter.x + gSearchRadius,
                    item.searchCenter.y + gSearchRadius
                );
                CRadar::DrawAreaOnRadar(zoneRect, CRGBA(255, 215, 0, 75), true);
            }

            // 2. Iconos exactos al apretar F12
            if (gShowExactIcons) {
                CRGBA col(255, 255, 255, 255);
                switch (item.type) {
                case MODE_TAGS:       col = CRGBA(50, 205, 50, 255); break;  // Verde
                case MODE_SNAPSHOTS:  col = CRGBA(30, 144, 255, 255); break; // Azul
                case MODE_HORSESHOES: col = CRGBA(255, 140, 0, 255); break; // Naranja
                case MODE_OYSTERS:    col = CRGBA(0, 255, 255, 255); break;  // Cyan
                case MODE_JUMPS:      col = CRGBA(255, 215, 0, 255); break;  // Amarillo
                }

                // Marcador visible en el radar
                CRect markerRect(
                    item.pos.x - 3.0f,
                    item.pos.y - 3.0f,
                    item.pos.x + 3.0f,
                    item.pos.y + 3.0f
                );
                CRadar::DrawAreaOnRadar(markerRect, col, true);
            }
        }
    }
}

class SearchCollectiblesPlugin {
public:
    SearchCollectiblesPlugin() {
        Events::initGameEvent += [] {
            RebuildIcons();
        };

        Events::gameProcessEvent += [] {
            RebuildIconsIfStale();
            UpdateProximityLogic();
        };

        // Evento nativo de Plugin-SDK blindado contra CLEO+
        Events::drawRadarOverlayEvent += [] {
            DrawRadarElements();
        };
    }
} gSearchPlugin;
