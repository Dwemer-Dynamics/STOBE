#include "Functions.h"
#include "AudioPlayback.h"
#include "AutonomyController.h"
#include "Comm.h"
#include "Context.h"
#include "Globals.h"
#include "KenshiBuildingCompat.h"
#include "Utils.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <core/Functions.h>
#include <kenshi/Appearance.h>
#include <kenshi/AppearanceManager.h>
#include <kenshi/Character.h>
#include <kenshi/CharMovement.h>
#include <kenshi/CharStats.h>
#include <kenshi/Dialogue.h>
#include <kenshi/Enums.h>
#include <kenshi/Faction.h>
#include <kenshi/FactionRelations.h>
#include <kenshi/GameData.h>
#include <kenshi/GameDataManager.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Inventory.h>
#include <kenshi/Item.h>
#include <kenshi/MedicalSystem.h>
#include <kenshi/Platoon.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/RaceData.h>
#include <kenshi/RootObject.h>
#include <kenshi/RootObjectBase.h>
#include <kenshi/RootObjectFactory.h>
#include <kenshi/SensoryData.h>
#include <kenshi/util/YesNoMaybe.h>
#include <kenshi/util/hand.h>
#include <mygui/MyGUI_Colour.h>
#include <mygui/MyGUI_EditBox.h>
#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_TextBox.h>
#include <ogre/OgreColourValue.h>
#include <map>
#include <sstream>
#include <vector>

std::string TrimCopySimple(const std::string &value);
std::string ToLowerCopy(const std::string &value);
void ApplyKnockoutPulse(Character *npc);
int ResolveCurrentGameTimestampSeconds(GameWorld *world);
std::string SafeBuildingName(Building *building);

const int kDrunkLevelDurationSeconds = 5 * 60 * 60;
const int kDrunkPassoutDurationSeconds = 2 * 60 * 60;
const DWORD kDrunkKnockoutPulseMs = 1000;
const int kSustainedKnockoutDurationSeconds = kDrunkPassoutDurationSeconds;
const DWORD kSustainedKnockoutPulseMs = kDrunkKnockoutPulseMs;
const int kDrugHighDurationSeconds = 5 * 60 * 60;
const float kDrugHungerMultiplier = 1.5f;
const float kDrugExtraHungerMultiplier = kDrugHungerMultiplier - 1.0f;
const float kNpcCloseActionRangeUnits = 25.0f;
const DWORD kNpcCloseActionApproachTimeoutMs = 10000;

namespace {
const char *kShekHornBodyKey = "bone_horns_body_short";
const char *kShekHornUpperKey = "bone_horns_top_short";
const char *kShekHornLowerKey = "bone_horns_bottom_short";
const char *kShekHornCurvedKey = "bone_horns_curved";
const char *kShekHornThinKey = "bone_horns_thin";
const char *kShekHornThickKey = "bone_horns_thick";
const float kHornCutOffThreshold = 0.999f;

float ClampHornSlider01(float value) {
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    if (value <= 100.0f) {
      value /= 100.0f;
    } else {
      value = 1.0f;
    }
  }
  if (value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

bool TryReadGameDataNumberExact(GameData *data, const char *key, float &valueOut) {
  if (!data || !key) {
    return false;
  }

  auto fit = data->fdata.find(key);
  if (fit != data->fdata.end()) {
    valueOut = fit->second;
    return true;
  }

  auto iit = data->idata.find(key);
  if (iit != data->idata.end()) {
    valueOut = (float)iit->second;
    return true;
  }

  auto sit = data->sdata.find(key);
  if (sit != data->sdata.end()) {
    valueOut = (float)atof(sit->second.c_str());
    return true;
  }

  return false;
}

bool TryGetCharacterAppearanceData(Character *npc, AppearanceBase *&appearanceOut,
                                   GameData *&appearanceDataOut,
                                   std::string &reasonOut) {
  appearanceOut = nullptr;
  appearanceDataOut = nullptr;
  reasonOut.clear();
  if (!npc || (uintptr_t)npc <= 0x1000) {
    reasonOut = "target not found";
    return false;
  }

  try {
    appearanceOut = npc->getAppearance();
  } catch (...) {
    appearanceOut = nullptr;
  }
  if (!appearanceOut || (uintptr_t)appearanceOut <= 0x1000) {
    reasonOut = "target has no appearance data";
    return false;
  }

  try {
    appearanceDataOut = appearanceOut->getAppearanceData();
  } catch (...) {
    appearanceDataOut = nullptr;
  }
  if (!appearanceDataOut || (uintptr_t)appearanceDataOut <= 0x1000) {
    reasonOut = "target has no appearance sliders";
    return false;
  }

  return true;
}

GameDataCopyStandalone *TryGetCharacterAppearanceSourceData(Character *npc) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return nullptr;
  }
  try {
    return npc->getAppearanceData();
  } catch (...) {
    return nullptr;
  }
}

bool TryGetCharacterHornAverageInternal(Character *npc, float &averageOut,
                                        std::string &reasonOut) {
  averageOut = 0.0f;
  reasonOut.clear();
  if (!IsCharacterShekRace(npc)) {
    reasonOut = "target is not Shek";
    return false;
  }

  AppearanceBase *appearance = nullptr;
  GameData *appearanceData = nullptr;
  if (!TryGetCharacterAppearanceData(npc, appearance, appearanceData, reasonOut)) {
    return false;
  }

  const char *kHornKeys[] = {
      kShekHornBodyKey,
      kShekHornUpperKey,
      kShekHornLowerKey,
  };

  float sum = 0.0f;
  int count = 0;
  for (size_t i = 0; i < sizeof(kHornKeys) / sizeof(kHornKeys[0]); ++i) {
    float value = 0.0f;
    if (!TryReadGameDataNumberExact(appearanceData, kHornKeys[i], value)) {
      continue;
    }
    sum += ClampHornSlider01(value);
    ++count;
  }

  if (count <= 0) {
    reasonOut = "target horn sliders are unavailable";
    return false;
  }

  averageOut = sum / (float)count;
  return true;
}

bool RefreshCharacterAppearance(Character *npc) {
  AppearanceBase *appearance = nullptr;
  try {
    appearance = npc ? npc->getAppearance() : nullptr;
  } catch (...) {
    appearance = nullptr;
  }
  if (!appearance || (uintptr_t)appearance <= 0x1000) {
    return false;
  }

  bool refreshed = false;
  try {
    appearance->notifyDirty();
    refreshed = true;
  } catch (...) {
  }
  try {
    appearance->updateAppearance();
    refreshed = true;
  } catch (...) {
  }
  try {
    appearance->updatePortrait();
    refreshed = true;
  } catch (...) {
  }
  return refreshed;
}

bool HardReloadCharacterAppearance(Character *npc) {
  AppearanceBase *appearance = nullptr;
  try {
    appearance = npc ? npc->getAppearance() : nullptr;
  } catch (...) {
    appearance = nullptr;
  }
  if (!appearance || (uintptr_t)appearance <= 0x1000) {
    return false;
  }

  bool refreshed = false;
  try {
    appearance->notifyDirty();
    refreshed = true;
  } catch (...) {
  }
  try {
    appearance->reload();
    refreshed = true;
  } catch (...) {
  }
  try {
    appearance->updateAppearance();
    refreshed = true;
  } catch (...) {
  }
  try {
    appearance->updatePortrait();
    refreshed = true;
  } catch (...) {
  }
  return refreshed;
}

std::string DescribeHornPersistenceState(Character *npc) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return "npc=invalid";
  }

  bool isPlayerCharacter = false;
  bool withPlayer = false;
  bool persistentPlatoon = false;
  ActivePlatoon *activePlatoon = nullptr;
  Platoon *platoon = nullptr;
  bool persistentSquad = false;

  try {
    isPlayerCharacter = npc->isPlayerCharacter();
  } catch (...) {
  }
  try {
    withPlayer = npc->isWithThePlayer();
  } catch (...) {
  }
  try {
    persistentPlatoon = npc->isInAPersistentPlatoon();
  } catch (...) {
  }
  try {
    activePlatoon = npc->getPlatoon();
  } catch (...) {
    activePlatoon = nullptr;
  }
  if (activePlatoon && (uintptr_t)activePlatoon > 0x1000) {
    try {
      platoon = activePlatoon->me;
    } catch (...) {
      platoon = nullptr;
    }
  }
  if (platoon && (uintptr_t)platoon > 0x1000) {
    try {
      persistentSquad = platoon->isPersistentSquad();
    } catch (...) {
      persistentSquad = false;
    }
  }

  return "player=" + std::string(isPlayerCharacter ? "1" : "0") +
         " with_player=" + std::string(withPlayer ? "1" : "0") +
         " persistent_platoon=" + std::string(persistentPlatoon ? "1" : "0") +
         " active_platoon=" +
         std::string((activePlatoon && (uintptr_t)activePlatoon > 0x1000) ? "1"
                                                                           : "0") +
         " platoon=" +
         std::string((platoon && (uintptr_t)platoon > 0x1000) ? "1" : "0") +
         " persistent_squad=" + std::string(persistentSquad ? "1" : "0");
}

bool PromoteCharacterHornPersistence(Character *npc, std::string &reasonOut) {
  reasonOut.clear();
  if (!npc || (uintptr_t)npc <= 0x1000) {
    reasonOut = "target_invalid";
    return false;
  }

  bool alreadyPersistent = false;
  try {
    alreadyPersistent = npc->isInAPersistentPlatoon();
  } catch (...) {
    alreadyPersistent = false;
  }

  ActivePlatoon *activePlatoon = nullptr;
  Platoon *platoon = nullptr;
  try {
    activePlatoon = npc->getPlatoon();
  } catch (...) {
    activePlatoon = nullptr;
  }
  if (activePlatoon && (uintptr_t)activePlatoon > 0x1000) {
    try {
      platoon = activePlatoon->me;
    } catch (...) {
      platoon = nullptr;
    }
  }

  if (!platoon || (uintptr_t)platoon <= 0x1000) {
    reasonOut = "platoon_unavailable";
    return false;
  }

  bool persistentSquad = false;
  try {
    persistentSquad = platoon->isPersistentSquad();
  } catch (...) {
    persistentSquad = false;
  }

  if (!persistentSquad) {
    try {
      platoon->setPersistentSquad(true);
      persistentSquad = true;
    } catch (...) {
      reasonOut = "set_persistent_failed";
      return false;
    }
  }

  bool saved = false;
  if (activePlatoon && (uintptr_t)activePlatoon > 0x1000) {
    try {
      activePlatoon->serialiseEverythingToDisk(false);
      saved = true;
    } catch (...) {
      saved = false;
    }
  }

  bool persistentAfter = false;
  try {
    persistentAfter = npc->isInAPersistentPlatoon();
  } catch (...) {
    persistentAfter = false;
  }

  if (persistentAfter || persistentSquad) {
    reasonOut = alreadyPersistent ? "already_persistent"
                                  : (saved ? "promoted_and_saved"
                                           : "promoted_unsaved");
    return true;
  }

  reasonOut = saved ? "saved_without_persistence" : "promotion_unconfirmed";
  return false;
}

GameDataCopyStandalone *CloneAppearanceDataForCharacter(Character *npc,
                                                        GameData *sourceData,
                                                        std::string &reasonOut) {
  reasonOut.clear();
  if (!npc || (uintptr_t)npc <= 0x1000) {
    reasonOut = "target_invalid";
    return nullptr;
  }
  if (!sourceData || (uintptr_t)sourceData <= 0x1000) {
    reasonOut = "source_invalid";
    return nullptr;
  }

  GameData *raceData = nullptr;
  try {
    raceData = (GameData *)npc->getRace();
  } catch (...) {
    raceData = nullptr;
  }
  if (!raceData || (uintptr_t)raceData <= 0x1000) {
    reasonOut = "race_invalid";
    return nullptr;
  }

  AppearanceManager *appearanceManager = nullptr;
  try {
    appearanceManager = AppearanceManager::getInstance();
  } catch (...) {
    appearanceManager = nullptr;
  }
  if (!appearanceManager || (uintptr_t)appearanceManager <= 0x1000) {
    reasonOut = "manager_invalid";
    return nullptr;
  }

  GameDataCopyStandalone *clone = nullptr;
  try {
    clone = appearanceManager->createAppearanceData(raceData);
  } catch (...) {
    clone = nullptr;
  }
  if (!clone || (uintptr_t)clone <= 0x1000) {
    reasonOut = "create_failed";
    return nullptr;
  }

  try {
    clone->updateFrom(sourceData, false);
    clone->readOnly = false;
    clone->isStandalone = true;
    clone->type = CHARACTER_APPEARANCE;
    clone->validity = sourceData->validity;
    clone->name = sourceData->name;
    clone->stringID = sourceData->stringID;
  } catch (...) {
    reasonOut = "copy_failed";
    return nullptr;
  }

  if (reasonOut.empty()) {
    reasonOut = "ok";
  }
  return clone;
}

GameDataCopyStandalone *AssignClonedAppearanceData(Character *npc,
                                                   AppearanceBase *appearance,
                                                   GameData *sourceData,
                                                   std::string &reasonOut) {
  reasonOut.clear();
  if (!npc || (uintptr_t)npc <= 0x1000) {
    reasonOut = "target_invalid";
    return nullptr;
  }
  if (!appearance || (uintptr_t)appearance <= 0x1000) {
    reasonOut = "appearance_invalid";
    return nullptr;
  }

  GameDataCopyStandalone *clone =
      CloneAppearanceDataForCharacter(npc, sourceData, reasonOut);
  if (!clone || (uintptr_t)clone <= 0x1000) {
    return nullptr;
  }

  try {
    npc->setAppearanceData(clone);
    appearance->setAppearanceData(clone);
    appearance->updatedAppearanceData = true;
    appearance->updateBody = true;
    appearance->notifyDirty();
  } catch (...) {
    reasonOut = "assign_failed";
    return nullptr;
  }

  reasonOut = "ok";
  return clone;
}

void ApplyHornCutOffValues(GameData *data) {
  if (!data) {
    return;
  }
  data->fdata[kShekHornBodyKey] = 1.0f;
  data->fdata[kShekHornUpperKey] = 1.0f;
  data->fdata[kShekHornLowerKey] = 1.0f;
  data->fdata[kShekHornCurvedKey] = 0.0f;
  data->fdata[kShekHornThinKey] = 0.0f;
  data->fdata[kShekHornThickKey] = 0.0f;
  data->sdata[kShekHornBodyKey] = "1";
  data->sdata[kShekHornUpperKey] = "1";
  data->sdata[kShekHornLowerKey] = "1";
  data->sdata[kShekHornCurvedKey] = "0";
  data->sdata[kShekHornThinKey] = "0";
  data->sdata[kShekHornThickKey] = "0";
  data->activeValues[kShekHornBodyKey] = true;
  data->activeValues[kShekHornUpperKey] = true;
  data->activeValues[kShekHornLowerKey] = true;
  data->activeValues[kShekHornCurvedKey] = true;
  data->activeValues[kShekHornThinKey] = true;
  data->activeValues[kShekHornThickKey] = true;
}

void ValidateAppearanceDataForCharacter(GameData *data, Character *npc,
                                        AppearanceBase *appearance) {
  if (!data || !npc || (uintptr_t)npc <= 0x1000) {
    return;
  }

  AppearanceManager *manager = nullptr;
  try {
    manager = AppearanceManager::getInstance();
  } catch (...) {
    manager = nullptr;
  }
  if (!manager || (uintptr_t)manager <= 0x1000) {
    return;
  }

  try {
    manager->cleanValidateAppearanceData(data);
  } catch (...) {
  }

  GameData *raceData = nullptr;
  if (appearance) {
    try {
      raceData = appearance->getRace();
    } catch (...) {
      raceData = nullptr;
    }
  }
  if (!raceData || (uintptr_t)raceData <= 0x1000) {
    return;
  }

  try {
    AppearanceManager::Gender gender(data);
    manager->updateModifiers(data, raceData, gender);
  } catch (...) {
  }
}

void PushImmediateHornContextSnapshot(Character *npc, const std::string &reason) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return;
  }

  std::string contextType = "npc";
  std::string npcName = "Unknown";
  try {
    if (npc->isPlayerCharacter()) {
      contextType = "player";
    }
    npcName = npc->getName();
  } catch (...) {
  }

  std::string contextJson = BuildNpcContextEnvelope(npc, contextType);
  if (contextJson.empty() || contextJson.front() != '{' ||
      contextJson.back() != '}') {
    Log("CONTEXT_PUSH: skipped immediate horn snapshot reason=" + reason +
        " name=" + npcName);
    return;
  }

  AsyncPostToStobe(L"/context", contextJson);
  Log("CONTEXT_PUSH: sent immediate horn snapshot reason=" + reason +
      " name=" + npcName + " type=" + contextType +
      " len=" + ToString((int)contextJson.length()));
}
} // namespace

struct NpcDrunkState {
  int level;
  int levelExpiresGameTs;
  int passedOutUntilGameTs;
  DWORD nextKnockoutPulseTick;

  NpcDrunkState()
      : level(0), levelExpiresGameTs(0), passedOutUntilGameTs(0),
        nextKnockoutPulseTick(0) {}
};

struct NpcSustainedKnockoutState {
  int untilGameTs;
  DWORD nextPulseTick;

  NpcSustainedKnockoutState() : untilGameTs(0), nextPulseTick(0) {}
};

struct NpcDrugState {
  int highUntilGameTs;
  int lastObservedGameTs;
  float lastObservedHunger;
  bool hasHungerSnapshot;

  NpcDrugState()
      : highUntilGameTs(0), lastObservedGameTs(0), lastObservedHunger(0.0f),
        hasHungerSnapshot(false) {}
};

static std::map<unsigned int, NpcDrunkState> g_npcDrunkStates;
static std::map<unsigned int, NpcSustainedKnockoutState>
    g_npcSustainedKnockoutStates;
static std::map<unsigned int, NpcDrugState> g_npcDrugStates;
struct PendingHornCutReapplyState {
  hand target;
  unsigned int serial;
  DWORD nextReapplyTick;
  DWORD expireTick;
  int remainingAttempts;

  PendingHornCutReapplyState()
      : serial(0), nextReapplyTick(0), expireTick(0), remainingAttempts(0) {}
};
static std::map<unsigned int, PendingHornCutReapplyState> g_pendingHornCutReapplies;
struct PendingHornCutDismissState {
  hand target;
  unsigned int serial;
  std::string originFactionToken;
  DWORD nextCheckTick;
  DWORD expireTick;
  int remainingChecks;

  PendingHornCutDismissState()
      : serial(0), nextCheckTick(0), expireTick(0), remainingChecks(0) {}
};
static std::map<unsigned int, PendingHornCutDismissState>
    g_pendingHornCutDismissals;
static MyGUI::EditBox *g_narratorTimedPopupBackdrop = nullptr;
static MyGUI::TextBox *g_narratorTimedPopupText = nullptr;
static bool g_narratorTimedPopupVisible = false;
static DWORD g_narratorTimedPopupShownTick = 0;
static DWORD g_narratorTimedPopupDurationMs = 0;

static bool TrySetNarratorTimedPopupVisible(bool visible) {
  if (!g_narratorTimedPopupBackdrop && !g_narratorTimedPopupText) {
    return false;
  }
  try {
    if (g_narratorTimedPopupBackdrop) {
      g_narratorTimedPopupBackdrop->setVisible(visible);
    }
    if (g_narratorTimedPopupText) {
      g_narratorTimedPopupText->setVisible(visible);
    }
    return true;
  } catch (...) {
    g_narratorTimedPopupBackdrop = nullptr;
    g_narratorTimedPopupText = nullptr;
    return false;
  }
}

static bool TryShowNarratorTimedPopupText(const wchar_t *caption) {
  if (!g_narratorTimedPopupText || !caption) {
    return false;
  }
  try {
    g_narratorTimedPopupText->setCaption(caption);
    if (g_narratorTimedPopupBackdrop) {
      g_narratorTimedPopupBackdrop->setVisible(true);
    }
    g_narratorTimedPopupText->setVisible(true);
    return true;
  } catch (...) {
    g_narratorTimedPopupBackdrop = nullptr;
    g_narratorTimedPopupText = nullptr;
    return false;
  }
}

void PerformLeaveSquad(Character *npc, GameWorld *world,
                       const std::string &originFaction) {
  if (!npc || !world)
    return;

  std::string factionPart = originFaction;
  std::string platoonPart = "";
  size_t pipePos = originFaction.find('|');
  if (pipePos != std::string::npos) {
    factionPart = originFaction.substr(0, pipePos);
    platoonPart = originFaction.substr(pipePos + 1);
  }

  Log("ACTION_EXEC: Dismissing " + npc->getName() + " (Target Faction: " +
      factionPart + ", Target Platoon: " + platoonPart + ")");

  if (world->player) {
    world->player->unselectPlayerCharacter(npc);
    lektor<Character *> &pc = world->player->playerCharacters;
    for (uint32_t i = 0; i < pc.size(); ++i) {
      if (pc.stuff[i] == npc) {
        for (uint32_t j = i; j < pc.size() - 1; ++j)
          pc.stuff[j] = pc.stuff[j + 1];
        pc.count--;
        Log("ACTION_EXEC: Removed " + npc->getName() +
            " from playerCharacters list.");
        break;
      }
    }
  }

  if (world->factionMgr) {
    FactionManager *fm = world->factionMgr;

    std::string targetFactionName = "Drifters";
    if (!factionPart.empty() && factionPart != "Unknown") {
      targetFactionName = factionPart;
    } else if (g_originFactions.count(npc->getHandle().serial)) {
      targetFactionName = g_originFactions[npc->getHandle().serial];
    }

    Faction *targetFaction = fm->getFactionByName(targetFactionName);

    // If Drifters requested or origin is missing, try to find the character's
    // original faction but exclude the player faction.
    if ((factionPart.empty() || factionPart == "Unknown" ||
         targetFactionName == "Drifters") &&
        npc->getGameData()) {
      GameData *characterData = npc->getGameData();
      // Try to find the original faction link in the character's template data
      const Ogre::vector<GameDataReference>::type *refs =
          characterData->getReferenceListIfExists("faction");
      if (refs && !refs->empty()) {
        Faction *refFaction = fm->getFactionByStringID(refs->at(0).sid);
        if (refFaction && !refFaction->isThePlayer()) {
          targetFaction = refFaction;
          targetFactionName = targetFaction->getName();
        }
      }
    }

    // Give fallback if origin doesn't exist (e.g. invalid string)
    if (!targetFaction || targetFaction->isThePlayer() ||
        targetFaction->isNotARealFaction()) {
      targetFactionName = "Drifters";
      targetFaction = fm->getFactionByName("Drifters");
    }

    if (!targetFaction || targetFaction->isThePlayer()) {
      targetFaction = NULL;
      const lektor<Faction *> *all = fm->getAllFactions();
      if (all) {
        for (uint32_t i = 0; i < all->count; ++i) {
          Faction *f = all->stuff[i];
          if (f && !f->isThePlayer() && !f->isNotARealFaction()) {
            targetFaction = f;
            if (f->getName() == targetFactionName)
              break;
          }
        }
      }
    }

    if (targetFaction) {
      Log("ACTION_EXEC: Moving character to target faction: " +
          targetFaction->getName());

      ActivePlatoon *ap = NULL;

      // Attempt to find existing platoon if requested
      if (!platoonPart.empty()) {
        const lektor<Platoon *> *activePlats =
            targetFaction->getActivePlatoons();
        if (activePlats) {
          for (uint32_t i = 0; i < activePlats->count; ++i) {
            Platoon *p = activePlats->stuff[i];
            if (p && (p->stringID == platoonPart ||
                      p->getPlatoonStringID() == platoonPart)) {
              ap = p->getActivePlatoon();
              if (ap) {
                Log("ACTION_EXEC: Found existing active platoon: " +
                    platoonPart);
                break;
              }
            }
          }
        }
      }

      // Fallback: Create a new platoon if no existing one found/active
      if (!ap) {
        Platoon *newPlat = targetFaction->createNewEmptyActivePlatoon(
            NULL, true, npc->getPosition());
        if (newPlat) {
          ap = newPlat->getActivePlatoon();
          Log("ACTION_EXEC: Created new platoon for dismissal.");
        }
      }

      if (ap) {
        npc->setFaction(targetFaction, ap);

        // Ensure the platoon has a leader if it was just created
        if (ap->getSquadSize() == 1 || !ap->getSquadLeader()) {
          ap->setSquadLeader(npc);
        }

        // --- RESTORE NPC DATA PACKAGES ---
        // Restore standard NPC AI systems (was using Player AI)
        npc->setupAI();
        npc->setupPlatoonAI();

        npc->reThinkCurrentAIAction();
      } else {
        Log("ACTION_EXEC: ERROR: Could not create or find a platoon for "
            "dismissal!");
      }
    }
  }
}

DWORD EstimateSpeechDurationMs(const std::string &text) {
  if (text.empty()) {
    int fallbackSeconds = g_dialogueSpeedSeconds > 0 ? g_dialogueSpeedSeconds : 2;
    return static_cast<DWORD>(fallbackSeconds * 1000);
  }

  int words = 0;
  bool inWord = false;
  int punctuationPauseMs = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    unsigned char ch = static_cast<unsigned char>(text[i]);
    bool wordChar = std::isalnum(ch) != 0;
    if (wordChar && !inWord) {
      ++words;
      inWord = true;
    } else if (!wordChar) {
      inWord = false;
    }
    if (ch == '.' || ch == '!' || ch == '?') {
      punctuationPauseMs += 260;
    } else if (ch == ',' || ch == ';' || ch == ':') {
      punctuationPauseMs += 120;
    } else if (ch == '\n' || ch == '\r') {
      punctuationPauseMs += 180;
    }
  }

  int chars = static_cast<int>(text.length());
  int baseMs = 0;
  if (words > 0) {
    baseMs = words * 360;
  } else {
    baseMs = chars * 55;
  }
  baseMs += punctuationPauseMs + 220;

  if (baseMs < 900) {
    baseMs = 900;
  } else if (baseMs > 45000) {
    baseMs = 45000;
  }
  return static_cast<DWORD>(baseMs);
}

DWORD ResolveSpeechQueueDelayMs(const QueuedAction &act) {
  DWORD delayMs = 0;
  if (g_ttsEnabled && !act.ttsHash.empty() && act.taskValue > 0) {
    delayMs = static_cast<DWORD>(act.taskValue) + 120;
  } else {
    delayMs = EstimateSpeechDurationMs(act.message) + 120;
  }
  if (delayMs < 250) {
    delayMs = 250;
  } else if (delayMs > 600000) {
    delayMs = 600000;
  }
  return delayMs;
}

DWORD ResolveSpeechQueueRemainingRealMs(double remainingScaledMs,
                                        float speedMultiplier) {
  if (remainingScaledMs <= 0.0) {
    return 0;
  }
  if (!(speedMultiplier > 0.0f)) {
    speedMultiplier = 1.0f;
  }
  double remainingReal =
      remainingScaledMs / static_cast<double>(speedMultiplier);
  if (remainingReal <= 1.0) {
    return 1;
  }
  if (remainingReal >= 600000.0) {
    return 600000;
  }
  return static_cast<DWORD>(remainingReal + 0.999);
}

static bool IsNarratorTimedPopupMessage(const std::string &message) {
  const std::string trimmed = TrimCopySimple(message);
  if (trimmed.empty()) {
    return false;
  }
  const std::string lowered = ToLowerCopy(trimmed);
  return lowered.find("the narrator:") == 0 || lowered.find("narrator:") == 0;
}

static void HideNarratorTimedPopup() {
  if (g_narratorTimedPopupVisible) {
    Log("ACTION_TIMING: narrator popup hidden");
  }
  TrySetNarratorTimedPopupVisible(false);
  g_narratorTimedPopupVisible = false;
  g_narratorTimedPopupShownTick = 0;
  g_narratorTimedPopupDurationMs = 0;
}

static bool EnsureNarratorTimedPopupWidget() {
  if (g_narratorTimedPopupBackdrop && g_narratorTimedPopupText) {
    return true;
  }
  MyGUI::Gui *gui = MyGUI::Gui::getInstancePtr();
  if (!gui) {
    return false;
  }

  try {
    const float kNarratorPopupTextX = 0.135f;
    const float kNarratorPopupTextY = 0.044f;
    const float kNarratorPopupTextW = 0.73f;
    const float kNarratorPopupTextH = 0.078f;
    const float kNarratorPopupPadX = 0.007f;
    const float kNarratorPopupPadY = 0.006f;
    const float kNarratorPopupBackdropX = kNarratorPopupTextX - kNarratorPopupPadX;
    const float kNarratorPopupBackdropY = kNarratorPopupTextY - kNarratorPopupPadY;
    const float kNarratorPopupBackdropW =
        kNarratorPopupTextW + (kNarratorPopupPadX * 2.0f);
    const float kNarratorPopupBackdropH =
        kNarratorPopupTextH + (kNarratorPopupPadY * 2.0f);

    g_narratorTimedPopupBackdrop = gui->createWidgetReal<MyGUI::EditBox>(
        "Kenshi_EditBox", kNarratorPopupBackdropX, kNarratorPopupBackdropY,
        kNarratorPopupBackdropW, kNarratorPopupBackdropH,
        MyGUI::Align::Top | MyGUI::Align::HCenter, "Popup",
        "Stobe_NarratorTimedPopupBackdrop");
    if (!g_narratorTimedPopupBackdrop) {
      return false;
    }
    g_narratorTimedPopupBackdrop->setEnabled(false);
    g_narratorTimedPopupBackdrop->setVisible(false);

    g_narratorTimedPopupText = gui->createWidgetReal<MyGUI::TextBox>(
        "Kenshi_TextboxStandardText", kNarratorPopupTextX, kNarratorPopupTextY,
        kNarratorPopupTextW, kNarratorPopupTextH,
        MyGUI::Align::Top | MyGUI::Align::HCenter, "Popup",
        "Stobe_NarratorTimedPopupText");
    if (!g_narratorTimedPopupText) {
      g_narratorTimedPopupBackdrop = nullptr;
      return false;
    }
    g_narratorTimedPopupText->setTextAlign(MyGUI::Align::Center);
    g_narratorTimedPopupText->setTextColour(MyGUI::Colour(1.0f, 0.91f, 0.56f));
    g_narratorTimedPopupText->setVisible(false);
  } catch (...) {
    g_narratorTimedPopupBackdrop = nullptr;
    g_narratorTimedPopupText = nullptr;
    return false;
  }

  return g_narratorTimedPopupBackdrop != nullptr &&
         g_narratorTimedPopupText != nullptr;
}

static void UpdateNarratorTimedPopupLifecycle() {
  if (!g_narratorTimedPopupVisible) {
    return;
  }
  if (!g_narratorTimedPopupBackdrop || !g_narratorTimedPopupText ||
      !MyGUI::Gui::getInstancePtr()) {
    g_narratorTimedPopupBackdrop = nullptr;
    g_narratorTimedPopupText = nullptr;
    g_narratorTimedPopupVisible = false;
    g_narratorTimedPopupShownTick = 0;
    g_narratorTimedPopupDurationMs = 0;
    return;
  }
  if ((DWORD)(GetTickCount() - g_narratorTimedPopupShownTick) >=
      g_narratorTimedPopupDurationMs) {
    HideNarratorTimedPopup();
  }
}

static bool ShowNarratorTimedPopup(const std::string &message, DWORD durationMs) {
  if (!EnsureNarratorTimedPopupWidget()) {
    return false;
  }
  if (!g_narratorTimedPopupText) {
    return false;
  }
  if (durationMs < 250) {
    durationMs = 250;
  } else if (durationMs > 600000) {
    durationMs = 600000;
  }

  const std::wstring wideMessage = WideFromUtf8(message);
  if (!TryShowNarratorTimedPopupText(wideMessage.c_str())) {
    g_narratorTimedPopupBackdrop = nullptr;
    g_narratorTimedPopupText = nullptr;
    g_narratorTimedPopupVisible = false;
    g_narratorTimedPopupShownTick = 0;
    g_narratorTimedPopupDurationMs = 0;
    return false;
  }

  g_narratorTimedPopupVisible = true;
  g_narratorTimedPopupShownTick = GetTickCount();
  g_narratorTimedPopupDurationMs = durationMs;
  Log("ACTION_TIMING: narrator popup shown dur_ms=" + ToString((int)durationMs) +
      " text_len=" + ToString((int)message.length()));
  return true;
}

int ClampTtsVolumePercent(int volumePercent) {
  if (volumePercent < 0) {
    return 0;
  }
  if (volumePercent > 100) {
    return 100;
  }
  return volumePercent;
}

bool IsCharacterLoadedForTtsPlayback(Character *character) {
  if (!character || (uintptr_t)character <= 0x1000) {
    return false;
  }
  bool isVisibleNear = false;
  bool isOnScreen = false;
  try {
    isVisibleNear = character->isVisibleAndNear;
    isOnScreen = character->isOnScreen;
  } catch (...) {
    return false;
  }
  return isVisibleNear || isOnScreen;
}

float ResolveCameraDistanceToCharacter(GameWorld *world, Character *character) {
  if (!world || !character || (uintptr_t)character <= 0x1000) {
    return -1.0f;
  }
  try {
    const Ogre::Vector3 cameraPos = world->getCameraPos();
    const Ogre::Vector3 characterPos = character->getPosition();
    float distance = cameraPos.distance(characterPos);
    if (distance >= 0.0f) {
      return distance;
    }
  } catch (...) {
  }
  return -1.0f;
}

int ResolveTtsPlaybackVolumePercent(GameWorld *world, Character *speaker,
                                    float &cameraDistanceOut,
                                    bool &speakerLoadedOut,
                                    bool &attenuatedOut) {
  cameraDistanceOut = -1.0f;
  speakerLoadedOut = false;
  attenuatedOut = false;

  int baseVolumePercent = ClampTtsVolumePercent(g_ttsVolumePercent);
  if (baseVolumePercent <= 0) {
    return 0;
  }
  if (!speaker || (uintptr_t)speaker <= 0x1000) {
    return 0;
  }

  speakerLoadedOut = IsCharacterLoadedForTtsPlayback(speaker);
  if (!speakerLoadedOut) {
    return 0;
  }

  cameraDistanceOut = ResolveCameraDistanceToCharacter(world, speaker);
  if (cameraDistanceOut < 0.0f) {
    return baseVolumePercent;
  }

  // Fade out by camera distance, and fully mute beyond far range.
  const float kPlaybackRangeMultiplier = 5.0f;
  const float kNearDistanceUnits = 40.0f;
  float farDistanceUnits = g_shoutRadius;
  // Keep TTS audible for squad chatter even when shout radius is configured low.
  if (farDistanceUnits < 350.0f) {
    farDistanceUnits = 350.0f;
  } else if (farDistanceUnits > 1200.0f) {
    farDistanceUnits = 1200.0f;
  }
  farDistanceUnits *= kPlaybackRangeMultiplier;

  if (cameraDistanceOut <= kNearDistanceUnits) {
    return baseVolumePercent;
  }
  if (cameraDistanceOut >= farDistanceUnits) {
    attenuatedOut = true;
    return 0;
  }

  float linearGain =
      (farDistanceUnits - cameraDistanceOut) /
      (farDistanceUnits - kNearDistanceUnits);
  if (linearGain < 0.0f) {
    linearGain = 0.0f;
  } else if (linearGain > 1.0f) {
    linearGain = 1.0f;
  }
  float gain = linearGain * linearGain;
  if (gain <= 0.001f) {
    attenuatedOut = true;
    return 0;
  }

  int resolvedVolumePercent = ClampTtsVolumePercent(
      static_cast<int>(static_cast<float>(baseVolumePercent) * gain + 0.5f));
  if (resolvedVolumePercent < baseVolumePercent) {
    attenuatedOut = true;
  }
  return resolvedVolumePercent;
}

static bool ParseTravelLocationPayload(const std::string &rawPayload, float &xOut,
                                       float &yOut, float &zOut,
                                       std::string &labelOut) {
  std::string payload = TrimCopySimple(rawPayload);
  if (payload.empty()) {
    return false;
  }

  char separator = '|';
  if (payload.find('|') == std::string::npos &&
      payload.find(';') != std::string::npos) {
    separator = ';';
  }

  size_t sep1 = payload.find(separator);
  size_t sep2 =
      (sep1 == std::string::npos) ? std::string::npos
                                  : payload.find(separator, sep1 + 1);
  if (sep1 == std::string::npos || sep2 == std::string::npos) {
    return false;
  }
  size_t sep3 = payload.find(separator, sep2 + 1);

  std::string xToken = TrimCopySimple(payload.substr(0, sep1));
  std::string yToken =
      TrimCopySimple(payload.substr(sep1 + 1, sep2 - sep1 - 1));
  std::string zToken =
      (sep3 == std::string::npos)
          ? TrimCopySimple(payload.substr(sep2 + 1))
          : TrimCopySimple(payload.substr(sep2 + 1, sep3 - sep2 - 1));

  auto parseFloatToken = [](const std::string &token, float &valueOut) -> bool {
    if (token.empty()) {
      return false;
    }
    char *endPtr = NULL;
    float parsed = (float)strtod(token.c_str(), &endPtr);
    if (endPtr == token.c_str()) {
      return false;
    }
    while (endPtr && *endPtr != '\0') {
      if (!isspace((unsigned char)*endPtr)) {
        return false;
      }
      ++endPtr;
    }
    if (parsed < -10000000.0f || parsed > 10000000.0f) {
      return false;
    }
    valueOut = parsed;
    return true;
  };

  if (!parseFloatToken(xToken, xOut) || !parseFloatToken(yToken, yOut) ||
      !parseFloatToken(zToken, zOut)) {
    return false;
  }

  labelOut = (sep3 == std::string::npos)
                 ? ""
                 : TrimCopySimple(payload.substr(sep3 + 1));
  if (!labelOut.empty()) {
    std::replace(labelOut.begin(), labelOut.end(), '@', ' ');
    std::replace(labelOut.begin(), labelOut.end(), '|', ' ');
    std::replace(labelOut.begin(), labelOut.end(), ';', ' ');
    labelOut = TrimCopySimple(labelOut);
  }

  return true;
}

Character *ResolveLiveCharacter(GameWorld *world, const hand &characterHandle) {
  if (!world || !characterHandle.isValid() || characterHandle.serial == 0) {
    return nullptr;
  }
  const auto &chars = world->getCharacterUpdateList();
  for (auto it = chars.begin(); it != chars.end(); ++it) {
    Character *candidate = *it;
    if (!candidate || (uintptr_t)candidate <= 0x1000) {
      continue;
    }
    if (candidate->getHandle().serial == characterHandle.serial) {
      return candidate;
    }
  }
  return nullptr;
}

Character *ResolveCharacterByTargetToken(GameWorld *world,
                                         const std::string &rawTarget,
                                         Character *actorToExclude) {
  if (!world) {
    return nullptr;
  }
  std::string token = TrimCopySimple(rawTarget);
  if (token.empty()) {
    return nullptr;
  }

  auto trimPunctuation = [](std::string value) -> std::string {
    while (!value.empty()) {
      char c = value.front();
      if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == '(' ||
          c == '[' || c == '{') {
        value.erase(0, 1);
        continue;
      }
      break;
    }
    while (!value.empty()) {
      char c = value.back();
      if (c == ' ' || c == '\t' || c == '"' || c == '\'' || c == ')' ||
          c == ']' || c == '}' || c == '.' || c == ',' || c == '!' ||
          c == '?' || c == ':') {
        value.erase(value.size() - 1, 1);
        continue;
      }
      break;
    }
    return value;
  };
  token = trimPunctuation(token);
  if (token.size() >= 2 &&
      ((token.front() == '"' && token.back() == '"') ||
       (token.front() == '\'' && token.back() == '\''))) {
    token = TrimCopySimple(token.substr(1, token.size() - 2));
  }
  if (token.empty()) {
    return nullptr;
  }

  auto tryParseSerialDigits = [](const std::string &value,
                                 unsigned int &serialOut) -> bool {
    serialOut = 0;
    std::string digits = TrimCopySimple(value);
    if (digits.empty()) {
      return false;
    }
    for (size_t i = 0; i < digits.size(); ++i) {
      unsigned char ch = (unsigned char)digits[i];
      if (ch < '0' || ch > '9') {
        return false;
      }
    }
    serialOut = (unsigned int)strtoul(digits.c_str(), NULL, 10);
    return serialOut > 0;
  };

  unsigned int wantedSerial = 0;
  bool hasSerial = false;
  std::string tokenLow = token;
  std::transform(tokenLow.begin(), tokenLow.end(), tokenLow.begin(), ::tolower);
  if (tokenLow.find("serial:") == 0) {
    hasSerial = tryParseSerialDigits(token.substr(7), wantedSerial);
    token.clear();
  } else if (tokenLow.find("id:") == 0) {
    hasSerial = tryParseSerialDigits(token.substr(3), wantedSerial);
    token.clear();
  }

  size_t pipePos = token.find('|');
  if (pipePos != std::string::npos) {
    std::string serialPart = TrimCopySimple(token.substr(pipePos + 1));
    token = TrimCopySimple(token.substr(0, pipePos));
    if (!serialPart.empty() && !hasSerial) {
      hasSerial = tryParseSerialDigits(serialPart, wantedSerial);
    }
  }
  if (!hasSerial && !token.empty()) {
    if (tryParseSerialDigits(token, wantedSerial)) {
      hasSerial = true;
      token.clear();
    }
  }

  tokenLow = token;
  std::transform(tokenLow.begin(), tokenLow.end(), tokenLow.begin(), ::tolower);
  if (tokenLow == "the player") {
    tokenLow = "player";
  } else if (tokenLow.find("the ") == 0) {
    tokenLow = TrimCopySimple(tokenLow.substr(4));
  } else if (tokenLow.find("a ") == 0) {
    tokenLow = TrimCopySimple(tokenLow.substr(2));
  } else if (tokenLow.find("an ") == 0) {
    tokenLow = TrimCopySimple(tokenLow.substr(3));
  }

  Character *bestMatch = nullptr;
  int bestScore = 0;
  unsigned int excludeSerial = 0;
  Ogre::Vector3 anchorPosition = Ogre::Vector3::ZERO;
  bool anchorPositionValid = false;
  hand anchorIndoorsHandle;
  bool anchorIsIndoors = false;
  int anchorFloor = 0;
  if (actorToExclude && (uintptr_t)actorToExclude > 0x1000) {
    try {
      excludeSerial = actorToExclude->getHandle().serial;
    } catch (...) {
      excludeSerial = 0;
    }
    try {
      anchorPosition = actorToExclude->getPosition();
      anchorPositionValid = true;
    } catch (...) {
      anchorPositionValid = false;
    }
    try {
      anchorIndoorsHandle = actorToExclude->isIndoors();
      anchorIsIndoors =
          anchorIndoorsHandle.isValid() && !anchorIndoorsHandle.isNull();
    } catch (...) {
      anchorIsIndoors = false;
    }
    try {
      anchorFloor = actorToExclude->getFloor();
    } catch (...) {
      anchorFloor = 0;
    }
  }

  const auto &chars = world->getCharacterUpdateList();
  for (auto it = chars.begin(); it != chars.end(); ++it) {
    Character *candidate = *it;
    if (!candidate || (uintptr_t)candidate <= 0x1000) {
      continue;
    }

    unsigned int candidateSerial = 0;
    try {
      candidateSerial = candidate->getHandle().serial;
    } catch (...) {
      candidateSerial = 0;
    }
    if (excludeSerial != 0 && candidateSerial == excludeSerial) {
      continue;
    }
    if (excludeSerial == 0 && actorToExclude &&
        (uintptr_t)actorToExclude > 0x1000 && candidate == actorToExclude) {
      continue;
    }

    int score = 0;
    if (hasSerial && candidateSerial == wantedSerial) {
      score = 1000;
    } else if (!tokenLow.empty()) {
      std::string candidateName = "";
      try {
        candidateName = candidate->getName();
      } catch (...) {
        candidateName = "";
      }
      std::string candidateLow = candidateName;
      std::transform(candidateLow.begin(), candidateLow.end(), candidateLow.begin(),
                     ::tolower);
      if (candidateLow == tokenLow) {
        score = 500;
      } else if (candidateLow.find(tokenLow) == 0) {
        score = 320;
      } else if (candidateLow.find(tokenLow) != std::string::npos) {
        score = 180;
      } else if (!candidate->displayName.empty()) {
        std::string displayLow = candidate->displayName;
        std::transform(displayLow.begin(), displayLow.end(), displayLow.begin(),
                       ::tolower);
        if (displayLow == tokenLow) {
          score = 260;
        } else if (displayLow.find(tokenLow) != std::string::npos) {
          score = 140;
        }
      }
    }

    if (score > 0 && anchorPositionValid) {
      float distance = -1.0f;
      try {
        distance = candidate->getPosition().distance(anchorPosition);
      } catch (...) {
        distance = -1.0f;
      }
      if (distance >= 0.0f) {
        if (distance <= 8.0f) {
          score += 260;
        } else if (distance <= 25.0f) {
          score += 180;
        } else if (distance <= 60.0f) {
          score += 110;
        } else if (distance <= 120.0f) {
          score += 40;
        } else if (!hasSerial && distance >= 300.0f) {
          score -= 100;
        }
      }

      bool candidateIsIndoors = false;
      hand candidateIndoorsHandle;
      try {
        candidateIndoorsHandle = candidate->isIndoors();
        candidateIsIndoors =
            candidateIndoorsHandle.isValid() && !candidateIndoorsHandle.isNull();
      } catch (...) {
        candidateIsIndoors = false;
      }
      if (candidateIsIndoors == anchorIsIndoors) {
        score += 24;
      }
      if (candidateIsIndoors && anchorIsIndoors &&
          candidateIndoorsHandle.serial == anchorIndoorsHandle.serial) {
        score += 90;
      } else if (candidateIsIndoors != anchorIsIndoors) {
        score -= 30;
      }

      int candidateFloor = 0;
      bool candidateFloorValid = true;
      try {
        candidateFloor = candidate->getFloor();
      } catch (...) {
        candidateFloorValid = false;
      }
      if (candidateFloorValid) {
        if (candidateFloor == anchorFloor) {
          score += 20;
        } else {
          score -= 6;
        }
      }
    }

    if (score > bestScore) {
      bestScore = score;
      bestMatch = candidate;
      if (score == 1000) {
        break;
      }
    }
  }

  // Fallback: nearby sphere query catches dead/KO actors that may not be in
  // the regular update set.
  if (!bestMatch || bestScore < 1000) {
    Character *anchorCharacter = actorToExclude;
    if ((!anchorCharacter || (uintptr_t)anchorCharacter <= 0x1000) &&
        world->player && world->player->playerCharacters.size() > 0 &&
        world->player->playerCharacters[0]) {
      anchorCharacter = world->player->playerCharacters[0];
    }
    if (anchorCharacter && (uintptr_t)anchorCharacter > 0x1000) {
      Ogre::Vector3 anchorPos = Ogre::Vector3::ZERO;
      bool anchorPosValid = false;
      try {
        anchorPos = anchorCharacter->getPosition();
        anchorPosValid = true;
      } catch (...) {
        anchorPosValid = false;
      }
      if (anchorPosValid) {
        lektor<RootObject *> nearby;
        try {
          world->getCharactersWithinSphere(nearby, anchorPos, 600.0f, 0.0f, 0.0f,
                                           16, 0, anchorCharacter);
        } catch (...) {
          nearby.clear();
        }

        for (uint32_t idx = 0; idx < nearby.size(); ++idx) {
          Character *candidate = (Character *)nearby.stuff[idx];
          if (!candidate || (uintptr_t)candidate <= 0x1000) {
            continue;
          }

          unsigned int candidateSerial = 0;
          try {
            candidateSerial = candidate->getHandle().serial;
          } catch (...) {
            candidateSerial = 0;
          }
          if (excludeSerial != 0 && candidateSerial == excludeSerial) {
            continue;
          }
          if (excludeSerial == 0 && actorToExclude &&
              (uintptr_t)actorToExclude > 0x1000 &&
              candidate == actorToExclude) {
            continue;
          }

          int score = 0;
          if (hasSerial && candidateSerial == wantedSerial) {
            score = 1200;
          } else if (!tokenLow.empty()) {
            std::string candidateName = "";
            try {
              candidateName = candidate->getName();
            } catch (...) {
              candidateName = "";
            }
            std::string candidateLow = candidateName;
            std::transform(candidateLow.begin(), candidateLow.end(),
                           candidateLow.begin(), ::tolower);
            if (candidateLow == tokenLow) {
              score = 520;
            } else if (candidateLow.find(tokenLow) == 0) {
              score = 340;
            } else if (candidateLow.find(tokenLow) != std::string::npos) {
              score = 200;
            } else if (!candidate->displayName.empty()) {
              std::string displayLow = candidate->displayName;
              std::transform(displayLow.begin(), displayLow.end(),
                             displayLow.begin(), ::tolower);
              if (displayLow == tokenLow) {
                score = 280;
              } else if (displayLow.find(tokenLow) != std::string::npos) {
                score = 160;
              }
            }
          }
          if (score <= 0) {
            continue;
          }

          try {
            float distance = candidate->getPosition().distance(anchorPos);
            if (distance <= 8.0f) {
              score += 320;
            } else if (distance <= 25.0f) {
              score += 220;
            } else if (distance <= 60.0f) {
              score += 130;
            } else if (distance <= 120.0f) {
              score += 55;
            } else if (!hasSerial && distance >= 300.0f) {
              score -= 120;
            }
          } catch (...) {
          }

          if (score > bestScore) {
            bestScore = score;
            bestMatch = candidate;
            if (score >= 1200) {
              break;
            }
          }
        }
      }
    }
  }

  if (bestMatch && bestScore > 0) {
    return bestMatch;
  }
  return nullptr;
}

Character *ResolveLiveCharacterBySerial(GameWorld *world, unsigned int serial) {
  if (!world || serial == 0) {
    return nullptr;
  }
  const auto &chars = world->getCharacterUpdateList();
  for (auto it = chars.begin(); it != chars.end(); ++it) {
    Character *candidate = *it;
    if (!candidate || (uintptr_t)candidate <= 0x1000) {
      continue;
    }
    unsigned int candidateSerial = 0;
    try {
      candidateSerial = candidate->getHandle().serial;
    } catch (...) {
      candidateSerial = 0;
    }
    if (candidateSerial == serial) {
      return candidate;
    }
  }
  return nullptr;
}

void UpdateNpcDrunkStates(GameWorld *world) {
  if (!world) {
    return;
  }

  int gameTs = ResolveCurrentGameTimestampSeconds(world);
  DWORD nowTick = GetTickCount();
  std::vector<unsigned int> knockoutSerials;

  EnterCriticalSection(&g_stateMutex);
  for (auto it = g_npcDrunkStates.begin(); it != g_npcDrunkStates.end();) {
    NpcDrunkState &state = it->second;
    if (state.level > 0 && state.levelExpiresGameTs > 0 &&
        gameTs >= state.levelExpiresGameTs) {
      state.level = 0;
      state.levelExpiresGameTs = 0;
    }

    if (state.passedOutUntilGameTs > 0) {
      if (gameTs >= state.passedOutUntilGameTs) {
        state.passedOutUntilGameTs = 0;
        state.nextKnockoutPulseTick = 0;
      } else if (state.nextKnockoutPulseTick == 0 ||
                 nowTick >= state.nextKnockoutPulseTick) {
        knockoutSerials.push_back(it->first);
        state.nextKnockoutPulseTick = nowTick + kDrunkKnockoutPulseMs;
      }
    }

    if (state.level <= 0 && state.levelExpiresGameTs <= 0 &&
        state.passedOutUntilGameTs <= 0) {
      it = g_npcDrunkStates.erase(it);
    } else {
      ++it;
    }
  }
  LeaveCriticalSection(&g_stateMutex);

  for (size_t i = 0; i < knockoutSerials.size(); ++i) {
    Character *target = ResolveLiveCharacterBySerial(world, knockoutSerials[i]);
    if (!target || (uintptr_t)target <= 0x1000) {
      continue;
    }
    bool dead = false;
    try {
      dead = target->isDead();
    } catch (...) {
      dead = false;
    }
    if (dead) {
      continue;
    }
    ApplyKnockoutPulse(target);
  }
}

void UpdateNpcSustainedKnockoutStates(GameWorld *world) {
  if (!world) {
    return;
  }

  int gameTs = ResolveCurrentGameTimestampSeconds(world);
  DWORD nowTick = GetTickCount();
  std::vector<unsigned int> knockoutSerials;

  EnterCriticalSection(&g_stateMutex);
  for (auto it = g_npcSustainedKnockoutStates.begin();
       it != g_npcSustainedKnockoutStates.end();) {
    NpcSustainedKnockoutState &state = it->second;
    if (state.untilGameTs > 0) {
      if (gameTs >= state.untilGameTs) {
        state.untilGameTs = 0;
        state.nextPulseTick = 0;
      } else if (state.nextPulseTick == 0 || nowTick >= state.nextPulseTick) {
        knockoutSerials.push_back(it->first);
        state.nextPulseTick = nowTick + kSustainedKnockoutPulseMs;
      }
    }

    if (state.untilGameTs <= 0) {
      it = g_npcSustainedKnockoutStates.erase(it);
    } else {
      ++it;
    }
  }
  LeaveCriticalSection(&g_stateMutex);

  for (size_t i = 0; i < knockoutSerials.size(); ++i) {
    Character *target = ResolveLiveCharacterBySerial(world, knockoutSerials[i]);
    if (!target || (uintptr_t)target <= 0x1000) {
      continue;
    }
    bool dead = false;
    try {
      dead = target->isDead();
    } catch (...) {
      dead = false;
    }
    if (dead) {
      continue;
    }
    ApplyKnockoutPulse(target);
  }
}

void UpdateNpcDrugStates(GameWorld *world) {
  if (!world) {
    return;
  }

  int gameTs = ResolveCurrentGameTimestampSeconds(world);
  std::vector<unsigned int> activeSerials;

  EnterCriticalSection(&g_stateMutex);
  for (auto it = g_npcDrugStates.begin(); it != g_npcDrugStates.end();) {
    NpcDrugState &state = it->second;
    if (state.highUntilGameTs > 0 && gameTs >= state.highUntilGameTs) {
      state.highUntilGameTs = 0;
      state.lastObservedGameTs = 0;
      state.lastObservedHunger = 0.0f;
      state.hasHungerSnapshot = false;
    }

    if (state.highUntilGameTs <= 0) {
      it = g_npcDrugStates.erase(it);
      continue;
    }

    activeSerials.push_back(it->first);
    ++it;
  }
  LeaveCriticalSection(&g_stateMutex);

  for (size_t i = 0; i < activeSerials.size(); ++i) {
    unsigned int serial = activeSerials[i];
    Character *target = ResolveLiveCharacterBySerial(world, serial);
    if (!target || (uintptr_t)target <= 0x1000) {
      continue;
    }

    bool dead = false;
    try {
      dead = target->isDead();
    } catch (...) {
      dead = false;
    }
    if (dead) {
      continue;
    }

    MedicalSystem *medical = nullptr;
    try {
      medical = target->getMedical();
    } catch (...) {
      medical = nullptr;
    }
    if (!medical || (uintptr_t)medical <= 0x1000) {
      continue;
    }

    float currentHunger = 0.0f;
    try {
      currentHunger = medical->hunger;
    } catch (...) {
      currentHunger = 0.0f;
    }
    if (currentHunger < 0.0f) {
      currentHunger = 0.0f;
    }

    float extraHungerToApply = 0.0f;
    EnterCriticalSection(&g_stateMutex);
    auto stateIt = g_npcDrugStates.find(serial);
    if (stateIt != g_npcDrugStates.end()) {
      NpcDrugState &state = stateIt->second;
      if (state.highUntilGameTs > gameTs) {
        if (!state.hasHungerSnapshot) {
          state.hasHungerSnapshot = true;
          state.lastObservedGameTs = gameTs;
          state.lastObservedHunger = currentHunger;
        } else {
          int gameDelta = gameTs - state.lastObservedGameTs;
          if (gameDelta > 0) {
            float naturalHungerDelta = currentHunger - state.lastObservedHunger;
            state.lastObservedGameTs = gameTs;
            state.lastObservedHunger = currentHunger;
            if (naturalHungerDelta > 0.001f) {
              extraHungerToApply = naturalHungerDelta * kDrugExtraHungerMultiplier;
            }
          } else {
            state.lastObservedHunger = currentHunger;
          }
        }
      }
    }
    LeaveCriticalSection(&g_stateMutex);

    if (extraHungerToApply > 0.001f) {
      float postHunger = currentHunger;
      try {
        medical->hunger = currentHunger + extraHungerToApply;
        medical->validateHealthValues();
        postHunger = medical->hunger;
      } catch (...) {
        postHunger = currentHunger;
      }
      EnterCriticalSection(&g_stateMutex);
      auto updateIt = g_npcDrugStates.find(serial);
      if (updateIt != g_npcDrugStates.end()) {
        updateIt->second.lastObservedHunger = postHunger;
        updateIt->second.lastObservedGameTs = gameTs;
        updateIt->second.hasHungerSnapshot = true;
      }
      LeaveCriticalSection(&g_stateMutex);
    }
  }
}

bool IsCharacterUnavailableForDialogue(Character *character) {
  if (!character || (uintptr_t)character <= 0x1000) {
    return true;
  }
  try {
    return character->isDead() || character->isUnconcious();
  } catch (...) {
    return true;
  }
}

static bool IsCharacterStandingForSpeechFacing(Character *character) {
  if (IsCharacterUnavailableForDialogue(character)) {
    return false;
  }

  bool isDown = false;
  try {
    isDown = character->isDown();
  } catch (...) {
    isDown = true;
  }
  if (isDown) {
    return false;
  }

  ProneState proneState = PS_KO;
  try {
    proneState = character->getProneState();
  } catch (...) {
    proneState = PS_KO;
  }
  if (proneState != PS_NORMAL) {
    return false;
  }

  try {
    if (character->inSomething != IN_NOTHING) {
      return false;
    }
  } catch (...) {
    return false;
  }

  return true;
}

static bool IsCharacterStandingStillForSpeechFacing(Character *character) {
  if (!IsCharacterStandingForSpeechFacing(character)) {
    return false;
  }

  float currentSpeed = 0.0f;
  bool hasSpeed = false;
  try {
    CharMovement *movement = character->getMovement();
    if (movement && (uintptr_t)movement > 0x1000) {
      currentSpeed = movement->getCurrentSpeed();
      hasSpeed = true;
    }
  } catch (...) {
  }

  if (!hasSpeed) {
    try {
      currentSpeed = character->getMovementSpeed();
      hasSpeed = true;
    } catch (...) {
      hasSpeed = false;
    }
  }

  if (!hasSpeed) {
    return false;
  }

  return currentSpeed <= 0.35f;
}

static Character *ResolveSpeechListenerForFacing(Character *speaker) {
  if (!speaker || (uintptr_t)speaker <= 0x1000) {
    return nullptr;
  }
  Dialogue *dialogue = nullptr;
  try {
    dialogue = speaker->dialogue;
  } catch (...) {
    dialogue = nullptr;
  }
  if (!dialogue || (uintptr_t)dialogue <= 0x1000) {
    return nullptr;
  }

  auto tryResolve = [&](const hand &candidateHandle) -> Character * {
    if (!candidateHandle.isValid() || candidateHandle.serial == 0) {
      return nullptr;
    }
    Character *candidate = nullptr;
    try {
      candidate = candidateHandle.getCharacter();
    } catch (...) {
      candidate = nullptr;
    }
    if (!candidate || (uintptr_t)candidate <= 0x1000 || candidate == speaker) {
      return nullptr;
    }
    return candidate;
  };

  try {
    Character *target = tryResolve(dialogue->getConversationTarget());
    if (target) {
      return target;
    }
  } catch (...) {
  }
  try {
    Character *target = tryResolve(dialogue->conversationTarget);
    if (target) {
      return target;
    }
  } catch (...) {
  }
  try {
    Character *target = tryResolve(dialogue->waitingForReplyFrom);
    if (target) {
      return target;
    }
  } catch (...) {
  }
  try {
    Character *target = tryResolve(dialogue->conversationMaster);
    if (target) {
      return target;
    }
  } catch (...) {
  }

  return nullptr;
}

static bool TryFaceSpeakerTowardListenerForSpeech(Character *speaker,
                                                  Character *listener) {
  if (!speaker || !listener || (uintptr_t)speaker <= 0x1000 ||
      (uintptr_t)listener <= 0x1000 || speaker == listener) {
    return false;
  }

  if (!IsCharacterStandingStillForSpeechFacing(speaker) ||
      !IsCharacterStandingForSpeechFacing(listener)) {
    return false;
  }

  Ogre::Vector3 speakerPos = Ogre::Vector3::ZERO;
  Ogre::Vector3 listenerPos = Ogre::Vector3::ZERO;
  try {
    speakerPos = speaker->getPosition();
    listenerPos = listener->getPosition();
  } catch (...) {
    return false;
  }

  Ogre::Vector3 direction = listenerPos - speakerPos;
  direction.y = 0.0f;
  if (direction.squaredLength() < 0.001f) {
    return false;
  }

  bool faced = false;
  try {
    CharMovement *movement = speaker->getMovement();
    if (movement && (uintptr_t)movement > 0x1000) {
      Ogre::Vector3 faceDir = direction;
      faceDir.normalise();
      movement->faceDirection(faceDir);
      movement->lookatPosition(listenerPos);
      faced = true;
    }
  } catch (...) {
  }

  try {
    speaker->lookatPosition(listenerPos, true);
    faced = true;
  } catch (...) {
  }

  return faced;
}

static bool IsActionIndoorsHandleValid(const hand &indoorsHandle) {
  return indoorsHandle.isValid() && !indoorsHandle.isNull();
}

static bool TryGetActionSpatialState(Character *character, bool &hasBuilding,
                                     unsigned int &buildingSerial,
                                     int &floorValue) {
  hasBuilding = false;
  buildingSerial = 0;
  floorValue = 0;
  if (!character || (uintptr_t)character <= 0x1000) {
    return false;
  }
#if defined(_MSC_VER)
  __try {
#endif
    const hand &indoorsHandle = character->isIndoors();
    hasBuilding = IsActionIndoorsHandleValid(indoorsHandle);
    buildingSerial = hasBuilding ? indoorsHandle.serial : 0;
    floorValue = character->getFloor();
    return true;
#if defined(_MSC_VER)
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
#endif
}

static bool IsActionAreaCompatible(Character *actor, Character *target) {
  if (!actor || !target || (uintptr_t)actor <= 0x1000 ||
      (uintptr_t)target <= 0x1000) {
    return false;
  }

  bool actorHasBuilding = false;
  bool targetHasBuilding = false;
  unsigned int actorBuildingSerial = 0;
  unsigned int targetBuildingSerial = 0;
  int actorFloor = 0;
  int targetFloor = 0;
  if (!TryGetActionSpatialState(actor, actorHasBuilding, actorBuildingSerial,
                                actorFloor)) {
    return false;
  }
  if (!TryGetActionSpatialState(target, targetHasBuilding, targetBuildingSerial,
                                targetFloor)) {
    return false;
  }

  if (actorHasBuilding) {
    if (!targetHasBuilding) {
      return false;
    }
    if (actorBuildingSerial == 0 || targetBuildingSerial == 0 ||
        actorBuildingSerial != targetBuildingSerial) {
      return false;
    }
    int floorDelta = actorFloor - targetFloor;
    if (floorDelta < 0) {
      floorDelta = -floorDelta;
    }
    return floorDelta <= 1;
  }

  if (targetHasBuilding) {
    return false;
  }
  if (targetFloor > actorFloor + 1) {
    return false;
  }
  return true;
}

static bool ValidateNpcCloseActionRange(Character *actor, Character *target,
                                        float allowedRange, float &distanceOut,
                                        std::string &reasonOut) {
  distanceOut = -1.0f;
  reasonOut = "";
  if (!actor || !target || (uintptr_t)actor <= 0x1000 ||
      (uintptr_t)target <= 0x1000) {
    reasonOut = "invalid_handles";
    return false;
  }
  if (!IsActionAreaCompatible(actor, target)) {
    reasonOut = "area_mismatch";
    return false;
  }
  try {
    distanceOut = actor->getPosition().distance(target->getPosition());
  } catch (...) {
    reasonOut = "distance_failed";
    return false;
  }
  if (distanceOut > allowedRange) {
    reasonOut = "out_of_range";
    return false;
  }
  return true;
}

bool IsQueuedActorReferenceValid(Character *npc, const hand &queuedActor,
                                 unsigned int &liveSerialOut) {
  liveSerialOut = 0;
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }
  if (!queuedActor.isValid() || queuedActor.serial == 0) {
    return false;
  }
  try {
    liveSerialOut = npc->getHandle().serial;
  } catch (...) {
    liveSerialOut = 0;
  }
  if (liveSerialOut == 0) {
    return false;
  }
  return liveSerialOut == queuedActor.serial;
}

void ClearCharacterSpeechBubble(Character *character) {
  if (!character || (uintptr_t)character <= 0x1000) {
    return;
  }
  try {
    if (!character->dialogue || (uintptr_t)character->dialogue <= 0x1000) {
      return;
    }
    character->dialogue->speechTextTimer = 0.0f;
    character->dialogue->speechTextTimer_forced = 0.0f;
  } catch (...) {
  }
}

Character *GetPrimaryPlayerCharacter(GameWorld *world) {
  if (!world || !world->player || world->player->playerCharacters.size() == 0) {
    return nullptr;
  }
  Character *player = world->player->playerCharacters[0];
  if (!player || (uintptr_t)player <= 0x1000) {
    return nullptr;
  }
  return player;
}

Character *GetActivePlayerCharacter(GameWorld *world) {
  if (!world || !world->player) {
    return nullptr;
  }
  if (world->player->selectedCharacter.isValid() &&
      world->player->selectedCharacter.serial != 0) {
    Character *selected =
        ResolveLiveCharacter(world, world->player->selectedCharacter);
    if (selected && (uintptr_t)selected > 0x1000 &&
        selected->isPlayerCharacter()) {
      return selected;
    }
  }
  return GetPrimaryPlayerCharacter(world);
}

bool IsInPlayerFactionSafe(Character *npc) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }
  try {
    Faction *faction = npc->getFaction();
    return faction && faction->isThePlayer();
  } catch (...) {
    return false;
  }
}

bool IsInPlayerRoster(GameWorld *world, Character *npc) {
  if (!world || !world->player || !npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }
  unsigned int serial = 0;
  try {
    serial = npc->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0) {
    return false;
  }
  const lektor<Character *> &roster = world->player->playerCharacters;
  for (uint32_t i = 0; i < roster.size(); ++i) {
    Character *member = roster.stuff[i];
    if (!member || (uintptr_t)member <= 0x1000) {
      continue;
    }
    try {
      if (member->getHandle().serial == serial) {
        return true;
      }
    } catch (...) {
    }
  }
  return false;
}

ActivePlatoon *ResolvePlayerJoinPlatoon(GameWorld *world, Character *npc) {
  if (!world || !world->player) {
    return nullptr;
  }

  Character *anchor = GetActivePlayerCharacter(world);
  if (!anchor && world->player->playerCharacters.size() > 0) {
    anchor = world->player->playerCharacters[0];
  }
  if (anchor && (uintptr_t)anchor > 0x1000) {
    try {
      ActivePlatoon *platoon = anchor->getPlatoon();
      if (platoon && (uintptr_t)platoon > 0x1000) {
        return platoon;
      }
    } catch (...) {
    }
  }

  Faction *playerFaction = nullptr;
  try {
    playerFaction = world->player->getFaction();
  } catch (...) {
    playerFaction = nullptr;
  }
  if ((!playerFaction || !playerFaction->isThePlayer()) &&
      world->player->playerCharacters.size() > 0 &&
      world->player->playerCharacters[0]) {
    try {
      playerFaction = world->player->playerCharacters[0]->getFaction();
    } catch (...) {
      playerFaction = nullptr;
    }
  }
  if (!playerFaction) {
    return nullptr;
  }

  try {
    ActivePlatoon *chosen = playerFaction->choosePlatoon();
    if (chosen && (uintptr_t)chosen > 0x1000) {
      return chosen;
    }
  } catch (...) {
  }

  try {
    const lektor<Platoon *> *activePlatoons = playerFaction->getActivePlatoons();
    if (activePlatoons) {
      for (uint32_t i = 0; i < activePlatoons->count; ++i) {
        Platoon *p = activePlatoons->stuff[i];
        if (!p) {
          continue;
        }
        ActivePlatoon *ap = p->getActivePlatoon();
        if (ap && (uintptr_t)ap > 0x1000) {
          return ap;
        }
      }
    }
  } catch (...) {
  }

  Ogre::Vector3 spawnPos = Ogre::Vector3::ZERO;
  bool spawnPosResolved = false;
  if (npc && (uintptr_t)npc > 0x1000) {
    try {
      spawnPos = npc->getPosition();
      spawnPosResolved = true;
    } catch (...) {
      spawnPosResolved = false;
    }
  }
  if (!spawnPosResolved && anchor && (uintptr_t)anchor > 0x1000) {
    try {
      spawnPos = anchor->getPosition();
      spawnPosResolved = true;
    } catch (...) {
      spawnPosResolved = false;
    }
  }
  if (!spawnPosResolved && world->player->playerCharacters.size() > 0 &&
      world->player->playerCharacters[0]) {
    try {
      spawnPos = world->player->playerCharacters[0]->getPosition();
      spawnPosResolved = true;
    } catch (...) {
      spawnPosResolved = false;
    }
  }
  try {
    Platoon *created = playerFaction->createNewEmptyActivePlatoon(NULL, true, spawnPos);
    if (created) {
      ActivePlatoon *ap = created->getActivePlatoon();
      if (ap && (uintptr_t)ap > 0x1000) {
        return ap;
      }
    }
  } catch (...) {
  }
  return nullptr;
}

bool ForceJoinPlayerSquad(GameWorld *world, Character *npc) {
  if (!world || !world->player || !npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }

  Faction *playerFaction = nullptr;
  try {
    playerFaction = world->player->getFaction();
  } catch (...) {
    playerFaction = nullptr;
  }
  if ((!playerFaction || !playerFaction->isThePlayer()) &&
      world->player->playerCharacters.size() > 0 &&
      world->player->playerCharacters[0]) {
    try {
      playerFaction = world->player->playerCharacters[0]->getFaction();
    } catch (...) {
      playerFaction = nullptr;
    }
  }
  if (!playerFaction || !playerFaction->isThePlayer()) {
    Log("ACTION_EXEC: JOIN_PARTY fallback failed: no player faction");
    return false;
  }

  ActivePlatoon *platoon = ResolvePlayerJoinPlatoon(world, npc);
  if (!platoon) {
    Log("ACTION_EXEC: JOIN_PARTY fallback failed: no player platoon");
    return false;
  }

  bool setFactionOk = false;
  try {
    npc->setFaction(playerFaction, platoon);
    setFactionOk = true;
  } catch (...) {
    setFactionOk = false;
  }
  if (!setFactionOk) {
    Log("ACTION_EXEC: JOIN_PARTY fallback failed: setFaction exception");
    return false;
  }

  try {
    if (platoon->getSquadSize() == 1 || !platoon->getSquadLeader()) {
      platoon->setSquadLeader(npc);
    }
  } catch (...) {
  }

  try {
    npc->clearPermajobs();
    npc->clearAllAIGoals();
  } catch (...) {
  }
  try {
    npc->setupAI();
    npc->setupPlatoonAI();
  } catch (...) {
  }
  try {
    npc->reThinkCurrentAIAction();
  } catch (...) {
  }

  bool recruitNormalOk = false;
  try {
    recruitNormalOk = world->player->recruit(npc, false);
  } catch (...) {
    recruitNormalOk = false;
  }

  try {
    world->player->setCharacterEditMode(false);
  } catch (...) {
  }

  bool joinedFaction = IsInPlayerFactionSafe(npc);
  bool joinedRoster = IsInPlayerRoster(world, npc);
  Log("ACTION_EXEC: JOIN_PARTY fallback setFaction=1 recruit_normal=" +
      std::string(recruitNormalOk ? "1" : "0") + " joined_faction=" +
      std::string(joinedFaction ? "1" : "0") + " joined_roster=" +
      std::string(joinedRoster ? "1" : "0"));
  return joinedFaction && joinedRoster;
}

std::string BuildFactionPlatoonOriginToken(Character *npc) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return "";
  }

  std::string factionName = "";
  std::string platoonName = "";
  try {
    Faction *faction = npc->getFaction();
    if (faction && (uintptr_t)faction > 0x1000 && !faction->isThePlayer()) {
      factionName = faction->getName();
      if (factionName.empty() && faction->data) {
        factionName = faction->data->name;
        if (factionName.empty()) {
          factionName = faction->data->stringID;
        }
      }
    }
  } catch (...) {
  }
  if (factionName.empty()) {
    try {
      unsigned int serial = npc->getHandle().serial;
      auto it = g_originFactions.find(serial);
      if (it != g_originFactions.end()) {
        factionName = it->second;
      }
    } catch (...) {
    }
  }

  try {
    ActivePlatoon *activePlatoon = npc->getPlatoon();
    if (activePlatoon && (uintptr_t)activePlatoon > 0x1000 && activePlatoon->me &&
        (uintptr_t)activePlatoon->me > 0x1000) {
      Platoon *platoon = activePlatoon->me;
      platoonName = platoon->stringID;
      if (platoonName.empty()) {
        platoonName = platoon->getPlatoonStringID();
      }
    }
  } catch (...) {
  }

  if (factionName.empty()) {
    return "";
  }
  if (platoonName.empty()) {
    return factionName;
  }
  return factionName + "|" + platoonName;
}

bool TryInternalJoinPlayerSquad(GameWorld *world, Character *npc,
                                std::string &reasonOut) {
  reasonOut.clear();
  if (!world || !world->player || !npc || (uintptr_t)npc <= 0x1000) {
    reasonOut = "invalid_join_target";
    return false;
  }

  bool recruitNormalOk = false;
  try {
    recruitNormalOk = world->player->recruit(npc, false);
  } catch (...) {
    recruitNormalOk = false;
  }

  bool joinedFaction = IsInPlayerFactionSafe(npc);
  bool joinedRoster = IsInPlayerRoster(world, npc);
  bool fallbackJoinOk = false;
  if (!(joinedFaction && joinedRoster)) {
    fallbackJoinOk = ForceJoinPlayerSquad(world, npc);
    joinedFaction = IsInPlayerFactionSafe(npc);
    joinedRoster = IsInPlayerRoster(world, npc);
  }

  bool canTakeOrdersAfter = false;
  try {
    canTakeOrdersAfter = npc->canTakePlayerOrdersAtThisTime();
  } catch (...) {
    canTakeOrdersAfter = false;
  }

  bool joined = joinedFaction || joinedRoster || canTakeOrdersAfter;
  if (joined && !joinedRoster) {
    try {
      world->player->recruit(npc, false);
    } catch (...) {
    }
    joinedRoster = IsInPlayerRoster(world, npc);
    joined = joinedFaction || joinedRoster || canTakeOrdersAfter;
  }
  try {
    world->player->setCharacterEditMode(false);
  } catch (...) {
  }

  reasonOut = "recruit_normal=" + std::string(recruitNormalOk ? "1" : "0") +
              " fallback=" + std::string(fallbackJoinOk ? "1" : "0") +
              " joined_faction=" + std::string(joinedFaction ? "1" : "0") +
              " joined_roster=" + std::string(joinedRoster ? "1" : "0") +
              " can_take_orders=" +
              std::string(canTakeOrdersAfter ? "1" : "0");
  return joined;
}

void PostLeaveSquadCleanup(Character *npc) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return;
  }
  try {
    if (npc->dialogue && (uintptr_t)npc->dialogue > 0x1000) {
      npc->dialogue->endDialogue(true);
      npc->dialogue->setInDialog(false);
    }
  } catch (...) {
  }
  try {
    npc->clearPermajobs();
    npc->clearAllAIGoals();
  } catch (...) {
  }
  try {
    npc->setStandingOrder((MessageForB::StandingOrder)13, false);
    npc->setStandingOrder((MessageForB::StandingOrder)12, false);
  } catch (...) {
  }
}

bool TryDismissCharacterToOrigin(GameWorld *world, Character *npc,
                                 const std::string &originFactionToken,
                                 std::string &reasonOut) {
  reasonOut.clear();
  if (!world || !npc || (uintptr_t)npc <= 0x1000) {
    reasonOut = "invalid_dismiss_target";
    return false;
  }

  PostLeaveSquadCleanup(npc);
  PerformLeaveSquad(npc, world, originFactionToken);

  try {
    if (npc->getPermajobCount() == 0) {
      TownBase *town = npc->getCurrentTownLocation();
      if (town) {
        npc->addJob(WANDER_TOWN, (RootObject *)town, false, false,
                    npc->getPosition());
        npc->addGoal(WANDER_TOWN, (RootObjectBase *)town);
      } else {
        npc->addJob(WANDERER, NULL, false, false, npc->getPosition());
        npc->addGoal(WANDERER, NULL);
      }
    }
  } catch (...) {
  }
  try {
    npc->reThinkCurrentAIAction();
  } catch (...) {
  }

  const bool stillInPlayerFaction = IsInPlayerFactionSafe(npc);
  const bool stillInRoster = IsInPlayerRoster(world, npc);
  reasonOut = "in_player_faction=" +
              std::string(stillInPlayerFaction ? "1" : "0") +
              " in_player_roster=" + std::string(stillInRoster ? "1" : "0");
  return !stillInPlayerFaction && !stillInRoster;
}

std::string ToLowerCopy(const std::string &value) {
  std::string lowered = value;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return lowered;
}

std::string TrimCopySimple(const std::string &value) {
  size_t first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  size_t last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::string ToUpperCopy(const std::string &value) {
  std::string upper = value;
  std::transform(upper.begin(), upper.end(), upper.begin(),
                 [](unsigned char c) { return (char)std::toupper(c); });
  return upper;
}

std::string SafeCharacterName(Character *npc) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return "Unknown";
  }
  try {
    return npc->getName();
  } catch (...) {
    return "Unknown";
  }
}

std::string SafeRootObjectName(const hand &targetHandle) {
  if (!targetHandle.isValid()) {
    return "";
  }
  try {
    Character *targetCharacter = targetHandle.getCharacter();
    if (targetCharacter && (uintptr_t)targetCharacter > 0x1000) {
      std::string name = targetCharacter->getName();
      if (name.empty() && !targetCharacter->displayName.empty()) {
        name = targetCharacter->displayName;
      }
      return name;
    }
  } catch (...) {
  }
  try {
    RootObjectBase *targetBase = targetHandle.getRootObjectBase();
    if (targetBase && (uintptr_t)targetBase > 0x1000) {
      return targetBase->getName();
    }
  } catch (...) {
  }
  return "";
}

std::string NormalizeCarryTargetToken(const std::string &value) {
  std::string normalized = TrimCopySimple(value);
  while (!normalized.empty()) {
    char first = normalized.front();
    if (first == '"' || first == '\'' || first == '(' || first == '[' ||
        first == '{' || first == ' ') {
      normalized.erase(0, 1);
      continue;
    }
    break;
  }
  while (!normalized.empty()) {
    char last = normalized.back();
    if (last == '"' || last == '\'' || last == ')' || last == ']' ||
        last == '}' || last == '.' || last == ',' || last == '!' ||
        last == '?' || last == ':' || last == ' ') {
      normalized.erase(normalized.size() - 1, 1);
      continue;
    }
    break;
  }
  normalized = ToLowerCopy(TrimCopySimple(normalized));
  if (normalized.find("the ") == 0) {
    normalized = TrimCopySimple(normalized.substr(4));
  } else if (normalized.find("a ") == 0) {
    normalized = TrimCopySimple(normalized.substr(2));
  } else if (normalized.find("an ") == 0) {
    normalized = TrimCopySimple(normalized.substr(3));
  }
  return normalized;
}

enum CloseActionApproachResult {
  CLOSE_ACTION_APPROACH_NOT_APPLICABLE = 0,
  CLOSE_ACTION_APPROACH_DEFERRED = 1,
  CLOSE_ACTION_APPROACH_TIMED_OUT = 2,
};

static bool IsCloseActionApproachRetryableReason(const std::string &reason) {
  return reason == "out_of_range" || reason == "area_mismatch";
}

static std::string DescribeCloseActionRangeReasonForUser(
    const std::string &reason) {
  if (reason == "area_mismatch") {
    return "not in the same area";
  }
  if (reason == "distance_failed" || reason == "invalid_handles") {
    return "not reachable";
  }
  return "too far away";
}

static void ResetCloseActionApproachState(Character *actor, QueuedAction &act) {
  if (actor && (uintptr_t)actor > 0x1000) {
    unsigned int actorSerial = 0;
    try {
      actorSerial = actor->getHandle().serial;
    } catch (...) {
      actorSerial = 0;
    }
    if (actorSerial != 0) {
      try {
        ClearFollowTarget(actorSerial);
      } catch (...) {
      }
      try {
        ClearTravelTarget(actorSerial);
      } catch (...) {
      }
    }
  }
  act.proximityStartTick = 0;
  act.proximityMoveIssued = false;
}

static CloseActionApproachResult TryDeferCloseActionUntilInRange(
    GameWorld *world, Character *actor, Character *target, QueuedAction &act,
    const std::string &actionLabel, const std::string &targetDisplayName,
    float currentDistance, const std::string &rangeReason,
    float allowedRangeUnits) {
  if (!world || !actor || !target || (uintptr_t)actor <= 0x1000 ||
      (uintptr_t)target <= 0x1000) {
    return CLOSE_ACTION_APPROACH_NOT_APPLICABLE;
  }
  if (!IsCloseActionApproachRetryableReason(rangeReason)) {
    return CLOSE_ACTION_APPROACH_NOT_APPLICABLE;
  }

  DWORD nowTick = GetTickCount();
  if (act.proximityStartTick == 0 || nowTick < act.proximityStartTick) {
    act.proximityStartTick = nowTick;
    act.proximityMoveIssued = false;
  }
  DWORD elapsedMs = nowTick - act.proximityStartTick;
  if (elapsedMs >= kNpcCloseActionApproachTimeoutMs) {
    ResetCloseActionApproachState(actor, act);
    return CLOSE_ACTION_APPROACH_TIMED_OUT;
  }

  unsigned int actorSerial = 0;
  try {
    actorSerial = actor->getHandle().serial;
  } catch (...) {
    actorSerial = 0;
  }
  hand targetHandle;
  try {
    targetHandle = target->getHandle();
  } catch (...) {
    targetHandle = hand();
  }
  if (actorSerial != 0 && targetHandle.isValid() && targetHandle.serial != 0) {
    try {
      SetFollowTarget(actorSerial, targetHandle);
    } catch (...) {
    }
    try {
      ClearTravelTarget(actorSerial);
    } catch (...) {
    }
  }

  try {
    if (actor->dialogue && (uintptr_t)actor->dialogue > 0x1000) {
      actor->dialogue->endDialogue(true);
      actor->dialogue->setInDialog(false);
    }
  } catch (...) {
  }
  try {
    actor->setStandingOrder((MessageForB::StandingOrder)13 /* PASSIVE */, false);
    actor->setStandingOrder((MessageForB::StandingOrder)12 /* HOLD */, false);
  } catch (...) {
  }

  bool destinationQueued = false;
  bool goalQueued = false;
  bool jobQueued = false;
  try {
    actor->setDestination(target->getPosition(), false);
    destinationQueued = true;
  } catch (...) {
    destinationQueued = false;
  }

  if (!act.proximityMoveIssued) {
    try {
      actor->addGoal(STAY_CLOSE_TO_TARGET, (RootObjectBase *)target);
      goalQueued = true;
    } catch (...) {
      goalQueued = false;
    }
    try {
      actor->addJob(STAY_CLOSE_TO_TARGET, (RootObject *)target, true, false,
                    actor->getPosition());
      jobQueued = true;
    } catch (...) {
      jobQueued = false;
    }
  }

  try {
    CharMovement *movement = actor->getMovement();
    if (movement && (uintptr_t)movement > 0x1000) {
      movement->setDesiredSpeedOrders(RUN);
      movement->setDesiredSpeed(RUN);
      if (!destinationQueued) {
        movement->setDestination(target, HIGH_PRIORITY);
        destinationQueued = true;
      }
    }
  } catch (...) {
  }

  try {
    actor->reThinkCurrentAIAction();
  } catch (...) {
  }

  try {
    hand targetHandle = target->getHandle();
    if (targetHandle.isValid() && targetHandle.serial != 0) {
      act.target = targetHandle;
    }
  } catch (...) {
  }

  std::string actorName = SafeCharacterName(actor);
  std::string targetName = TrimCopySimple(targetDisplayName);
  if (targetName.empty()) {
    targetName = SafeCharacterName(target);
  }
  if (targetName.empty()) {
    targetName = "the target";
  }
  if (!act.proximityMoveIssued) {
    world->showPlayerAMessage_withLog(
        actorName + " moves toward " + targetName + " to " + actionLabel + ".",
        true);
  }

  Log("ACTION_EXEC: CLOSE_ACTION_APPROACH actor=" + actorName +
      " action=" + actionLabel + " target=" + targetName +
      " reason=" + rangeReason + " dist=" + ToString(currentDistance) +
      " max_dist=" + ToString(allowedRangeUnits) +
      " elapsed_ms=" + ToString((int)elapsedMs) +
      " destination_queued=" + std::string(destinationQueued ? "1" : "0") +
      " goal_queued=" + std::string(goalQueued ? "1" : "0") +
      " job_queued=" + std::string(jobQueued ? "1" : "0"));

  act.proximityMoveIssued = true;
  return CLOSE_ACTION_APPROACH_DEFERRED;
}

bool StringContainsAsciiInsensitive(const std::string &haystack,
                                    const std::string &needle) {
  if (needle.empty()) {
    return false;
  }
  return ToLowerCopy(haystack).find(ToLowerCopy(needle)) != std::string::npos;
}

int ResolveCurrentGameTimestampSeconds(GameWorld *world) {
  if (!world) {
    return 0;
  }
  try {
    TimeOfDay tod = world->getTimeStamp_inGameHours();
    int ts = (int)tod.getTotalSeconds();
    return ts > 0 ? ts : 0;
  } catch (...) {
    return 0;
  }
}

std::string NormalizeInventoryMatchToken(const std::string &value) {
  std::string lowered = ToLowerCopy(TrimCopySimple(value));
  std::string out;
  out.reserve(lowered.size());
  bool wroteSpace = false;
  for (size_t i = 0; i < lowered.size(); ++i) {
    unsigned char ch = (unsigned char)lowered[i];
    if (std::isalnum(ch)) {
      out.push_back((char)ch);
      wroteSpace = false;
      continue;
    }
    if (std::isspace(ch) || ch == '_' || ch == '-') {
      if (!wroteSpace && !out.empty()) {
        out.push_back(' ');
        wroteSpace = true;
      }
    }
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string CollapseInventoryTokenNoSpace(const std::string &value) {
  std::string compact;
  compact.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    unsigned char ch = (unsigned char)value[i];
    if (std::isalnum(ch)) {
      compact.push_back((char)std::tolower(ch));
    }
  }
  return compact;
}

bool InventoryTokensMatch(const std::string &itemToken,
                          const std::string &queryToken) {
  if (itemToken.empty() || queryToken.empty()) {
    return false;
  }
  if (itemToken.find(queryToken) != std::string::npos ||
      queryToken.find(itemToken) != std::string::npos) {
    return true;
  }
  const std::string itemCompact = CollapseInventoryTokenNoSpace(itemToken);
  const std::string queryCompact = CollapseInventoryTokenNoSpace(queryToken);
  if (itemCompact.empty() || queryCompact.empty()) {
    return false;
  }
  return itemCompact.find(queryCompact) != std::string::npos ||
         queryCompact.find(itemCompact) != std::string::npos;
}

bool IsLikelyTraderStorageBuilding(Building *building) {
  if (!building || (uintptr_t)building <= 0x1000) {
    return false;
  }

  BuildingFunction functionType = BF_ANY;
  BuildingClassType classType = BCTYPE_FLUFF;
  try {
    functionType = building->_NV_getSpecialFunction();
  } catch (...) {
    functionType = BF_ANY;
  }
  try {
    classType = building->_NV_getBuildingClass();
  } catch (...) {
    classType = BCTYPE_FLUFF;
  }

  std::string buildingNameLower = ToLowerCopy(SafeBuildingName(building));
  bool likelyStorageByName =
      buildingNameLower.find("shop") != std::string::npos ||
      buildingNameLower.find("counter") != std::string::npos ||
      buildingNameLower.find("barrel") != std::string::npos ||
      buildingNameLower.find("chest") != std::string::npos ||
      buildingNameLower.find("storage") != std::string::npos ||
      buildingNameLower.find("cabinet") != std::string::npos ||
      buildingNameLower.find("shelf") != std::string::npos ||
      buildingNameLower.find("basket") != std::string::npos;

  return functionType == BF_SHOP || functionType == BF_GENERAL_STORAGE ||
         functionType == BF_RESOURCE_STORAGE || classType == BCTYPE_STORAGE ||
         classType == BCTYPE_PRODUCTION || classType == BCTYPE_CRAFTING ||
         classType == BCTYPE_USABLE || likelyStorageByName;
}

bool TryTransferItemFromInventoryByQuery(Inventory *sourceInventory,
                                         Character *recipient,
                                         const std::string &queryToken,
                                         int maxQuantity,
                                         int &quantityTransferredOut,
                                         std::string &itemNameOut) {
  quantityTransferredOut = 0;
  itemNameOut.clear();
  if (!sourceInventory || (uintptr_t)sourceInventory <= 0x1000 || !recipient ||
      (uintptr_t)recipient <= 0x1000 || queryToken.empty() ||
      maxQuantity <= 0) {
    return false;
  }

  std::vector<Item *> items;
  try {
    GetAllInventoryItemsFromInventory(sourceInventory, items);
  } catch (...) {
    items.clear();
  }
  if (items.size() > 600) {
    items.resize(600);
  }

  for (size_t i = 0; i < items.size(); ++i) {
    Item *item = items[i];
    if (!item || (uintptr_t)item <= 0x1000) {
      continue;
    }

    std::string itemName = "";
    try {
      itemName = item->getName();
    } catch (...) {
      itemName = "";
    }
    if (itemName.empty()) {
      continue;
    }

    std::string itemToken = NormalizeInventoryMatchToken(itemName);
    if (itemToken.empty()) {
      continue;
    }
    bool queryMatches = InventoryTokensMatch(itemToken, queryToken);
    if (!queryMatches) {
      continue;
    }

    int stackQuantity = 1;
    try {
      stackQuantity = item->quantity;
    } catch (...) {
      stackQuantity = 1;
    }
    if (stackQuantity < 1) {
      stackQuantity = 1;
    }
    int transferQuantity = stackQuantity;
    if (transferQuantity > maxQuantity) {
      transferQuantity = maxQuantity;
    }

    Inventory *itemInventory = nullptr;
    try {
      itemInventory = item->getInventory();
    } catch (...) {
      itemInventory = nullptr;
    }
    if (!itemInventory) {
      itemInventory = sourceInventory;
    }

    Item *detached = nullptr;
    try {
      detached = itemInventory ? itemInventory->removeItemDontDestroy_returnsItem(
                                    item, transferQuantity, false)
                              : nullptr;
    } catch (...) {
      detached = nullptr;
    }
    if (!detached || (uintptr_t)detached <= 0x1000) {
      continue;
    }

    int detachedQuantity = transferQuantity;
    try {
      if (detached->quantity > 0) {
        detachedQuantity = detached->quantity;
      }
    } catch (...) {
      detachedQuantity = transferQuantity;
    }
    try {
      recipient->giveItem(detached, true, false);
    } catch (...) {
      return false;
    }

    itemNameOut = itemName;
    quantityTransferredOut = detachedQuantity;
    return true;
  }

  return false;
}

bool TryTransferItemFromNearbyTraderStorage(Character *npc, Character *recipient,
                                            const std::string &rawQuery,
                                            int maxQuantity,
                                            int &quantityTransferredOut,
                                            std::string &itemNameOut,
                                            std::string &sourceNameOut) {
  quantityTransferredOut = 0;
  itemNameOut.clear();
  sourceNameOut.clear();
  if (!npc || (uintptr_t)npc <= 0x1000 || !recipient ||
      (uintptr_t)recipient <= 0x1000 || maxQuantity <= 0) {
    return false;
  }

  std::string queryToken = NormalizeInventoryMatchToken(rawQuery);
  if (queryToken.empty()) {
    return false;
  }

  GameWorld *world = GetWorldSafe();
  if (!world || (uintptr_t)world <= 0x1000) {
    return false;
  }

  bool npcIsIndoors = false;
  unsigned int npcIndoorSerial = 0;
  try {
    const hand &indoorsHandle = npc->isIndoors();
    npcIsIndoors = indoorsHandle.isValid();
    npcIndoorSerial = npcIsIndoors ? indoorsHandle.serial : 0;
  } catch (...) {
    npcIsIndoors = false;
    npcIndoorSerial = 0;
  }

  lektor<RootObject *> nearbyBuildings;
  try {
    world->getObjectsWithinSphere(nearbyBuildings, npc->getPosition(), 120.0f,
                                  BUILDING, 96, (RootObject *)npc);
  } catch (...) {
    nearbyBuildings.clear();
  }

  std::map<unsigned int, bool> seenSerials;
  for (uint32_t i = 0; i < nearbyBuildings.size(); ++i) {
    Building *building = (Building *)nearbyBuildings.stuff[i];
    if (!building || (uintptr_t)building <= 0x1000) {
      continue;
    }

    unsigned int buildingSerial = 0;
    try {
      buildingSerial = building->getHandle().serial;
    } catch (...) {
      buildingSerial = 0;
    }
    if (buildingSerial == 0 || seenSerials.count(buildingSerial) > 0) {
      continue;
    }
    seenSerials[buildingSerial] = true;

    if (npcIsIndoors) {
      bool sameIndoorShell = (buildingSerial == npcIndoorSerial);
      try {
        const hand &buildingIndoors = building->isIndoors();
        if (!sameIndoorShell && buildingIndoors.isValid() &&
            buildingIndoors.serial == npcIndoorSerial) {
          sameIndoorShell = true;
        }
      } catch (...) {
      }
      if (!sameIndoorShell) {
        continue;
      }
    }

    if (!IsLikelyTraderStorageBuilding(building)) {
      continue;
    }

    Inventory *buildingInventory = nullptr;
    try {
      buildingInventory = building->getInventory();
    } catch (...) {
      buildingInventory = nullptr;
    }
    if (!buildingInventory || (uintptr_t)buildingInventory <= 0x1000) {
      continue;
    }

    if (TryTransferItemFromInventoryByQuery(
            buildingInventory, recipient, queryToken, maxQuantity,
            quantityTransferredOut, itemNameOut)) {
      sourceNameOut = SafeBuildingName(building);
      return true;
    }
  }

  return false;
}

std::string CanonicalDrinkLabelFromToken(const std::string &token) {
  std::string key = token;
  key.erase(std::remove(key.begin(), key.end(), ' '), key.end());
  if (key == "bloodrum") {
    return "Bloodrum";
  }
  if (key == "cactusrum") {
    return "Cactus Rum";
  }
  if (key == "grog") {
    return "Grog";
  }
  if (key == "sake") {
    return "Sake";
  }
  return "";
}

bool IsAllowedDrinkItemName(const std::string &itemName,
                            std::string &canonicalLabelOut) {
  canonicalLabelOut.clear();
  std::string token = NormalizeInventoryMatchToken(itemName);
  if (token.empty()) {
    return false;
  }
  canonicalLabelOut = CanonicalDrinkLabelFromToken(token);
  return !canonicalLabelOut.empty();
}

std::string CanonicalDrugLabelFromToken(const std::string &token) {
  std::string key = token;
  key.erase(std::remove(key.begin(), key.end(), ' '), key.end());
  if (key == "hashish") {
    return "Hashish";
  }
  return "";
}

bool IsAllowedDrugItemName(const std::string &itemName,
                           std::string &canonicalLabelOut) {
  canonicalLabelOut.clear();
  std::string token = NormalizeInventoryMatchToken(itemName);
  if (token.empty()) {
    return false;
  }
  canonicalLabelOut = CanonicalDrugLabelFromToken(token);
  return !canonicalLabelOut.empty();
}

bool TryResolveDrugItemForActor(Character *npc, const std::string &rawQuery,
                                Item *&itemOut, std::string &itemNameOut,
                                std::string &canonicalLabelOut) {
  itemOut = nullptr;
  itemNameOut.clear();
  canonicalLabelOut.clear();
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }

  std::string queryToken = NormalizeInventoryMatchToken(rawQuery);
  if (queryToken.empty()) {
    return false;
  }

  std::vector<Item *> items;
  try {
    GetAllCharacterItems(npc, items);
  } catch (...) {
    items.clear();
  }

  for (size_t i = 0; i < items.size(); ++i) {
    Item *item = items[i];
    if (!item || (uintptr_t)item <= 0x1000) {
      continue;
    }
    std::string itemName = "";
    try {
      itemName = item->getName();
    } catch (...) {
      itemName = "";
    }
    if (itemName.empty()) {
      continue;
    }

    std::string canonicalLabel = "";
    if (!IsAllowedDrugItemName(itemName, canonicalLabel)) {
      continue;
    }

    std::string itemToken = NormalizeInventoryMatchToken(itemName);
    std::string canonicalToken = NormalizeInventoryMatchToken(canonicalLabel);
    bool queryMatches =
        itemToken.find(queryToken) != std::string::npos ||
        canonicalToken.find(queryToken) != std::string::npos ||
        queryToken.find(itemToken) != std::string::npos ||
        queryToken.find(canonicalToken) != std::string::npos;
    if (!queryMatches) {
      continue;
    }

    itemOut = item;
    itemNameOut = itemName;
    canonicalLabelOut = canonicalLabel;
    return true;
  }

  return false;
}

bool TryResolveDrinkItemForActor(Character *npc, const std::string &rawQuery,
                                 Item *&itemOut, std::string &itemNameOut,
                                 std::string &canonicalLabelOut) {
  itemOut = nullptr;
  itemNameOut.clear();
  canonicalLabelOut.clear();
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }

  std::string queryToken = NormalizeInventoryMatchToken(rawQuery);
  if (queryToken.empty()) {
    return false;
  }

  std::vector<Item *> items;
  try {
    GetAllCharacterItems(npc, items);
  } catch (...) {
    items.clear();
  }

  for (size_t i = 0; i < items.size(); ++i) {
    Item *item = items[i];
    if (!item || (uintptr_t)item <= 0x1000) {
      continue;
    }
    std::string itemName = "";
    try {
      itemName = item->getName();
    } catch (...) {
      itemName = "";
    }
    if (itemName.empty()) {
      continue;
    }

    std::string canonicalLabel = "";
    if (!IsAllowedDrinkItemName(itemName, canonicalLabel)) {
      continue;
    }

    std::string itemToken = NormalizeInventoryMatchToken(itemName);
    std::string canonicalToken = NormalizeInventoryMatchToken(canonicalLabel);
    bool queryMatches =
        itemToken.find(queryToken) != std::string::npos ||
        canonicalToken.find(queryToken) != std::string::npos ||
        queryToken.find(itemToken) != std::string::npos ||
        queryToken.find(canonicalToken) != std::string::npos;
    if (!queryMatches) {
      continue;
    }

    itemOut = item;
    itemNameOut = itemName;
    canonicalLabelOut = canonicalLabel;
    return true;
  }

  return false;
}

bool ConsumeSingleItemFromActor(Character *npc, Item *item) {
  if (!npc || !item || (uintptr_t)npc <= 0x1000 || (uintptr_t)item <= 0x1000) {
    return false;
  }

  try {
    if (item->isEquipped) {
      npc->unequipItem(item->inventorySection, item);
    }
  } catch (...) {
  }

  Inventory *inventory = nullptr;
  try {
    inventory = item->getInventory();
  } catch (...) {
    inventory = nullptr;
  }
  if (!inventory) {
    try {
      inventory = npc->getInventory();
    } catch (...) {
      inventory = nullptr;
    }
  }
  if (!inventory || (uintptr_t)inventory <= 0x1000) {
    return false;
  }

  Item *removed = nullptr;
  try {
    removed = inventory->removeItemDontDestroy_returnsItem(item, 1, false);
  } catch (...) {
    removed = nullptr;
  }
  if (removed && (uintptr_t)removed > 0x1000) {
    return true;
  }

  int stackCount = 0;
  try {
    stackCount = item->quantity;
  } catch (...) {
    stackCount = 0;
  }
  if (stackCount > 1) {
    try {
      item->quantity = stackCount - 1;
      return true;
    } catch (...) {
    }
  }
  return false;
}

void BuildDrunkPromptStateFromSnapshot(const NpcDrunkState &state,
                                       int gameTs, int &levelOut,
                                       bool &isDrunkOut, std::string &statusOut,
                                       int &secondsRemainingOut) {
  levelOut = 0;
  isDrunkOut = false;
  statusOut = "sober";
  secondsRemainingOut = 0;

  if (state.passedOutUntilGameTs > gameTs) {
    levelOut = 3;
    isDrunkOut = true;
    statusOut = "passed_out";
    secondsRemainingOut = state.passedOutUntilGameTs - gameTs;
    if (secondsRemainingOut < 0) {
      secondsRemainingOut = 0;
    }
    return;
  }

  if (state.level > 0 && state.levelExpiresGameTs > gameTs) {
    levelOut = state.level;
    isDrunkOut = true;
    statusOut = state.level >= 2 ? "very_drunk" : "drunk";
    secondsRemainingOut = state.levelExpiresGameTs - gameTs;
    if (secondsRemainingOut < 0) {
      secondsRemainingOut = 0;
    }
  }
}

void BuildDrugPromptStateFromSnapshot(const NpcDrugState &state, int gameTs,
                                      bool &isHighOut, std::string &statusOut,
                                      int &secondsRemainingOut,
                                      float &hungerRateMultiplierOut) {
  isHighOut = false;
  statusOut = "sober";
  secondsRemainingOut = 0;
  hungerRateMultiplierOut = 1.0f;
  if (state.highUntilGameTs > gameTs) {
    isHighOut = true;
    statusOut = "high";
    secondsRemainingOut = state.highUntilGameTs - gameTs;
    if (secondsRemainingOut < 0) {
      secondsRemainingOut = 0;
    }
    hungerRateMultiplierOut = kDrugHungerMultiplier;
  }
}

bool ActivateNpcDrugHighState(GameWorld *world, Character *npc,
                              int &secondsRemainingOut) {
  secondsRemainingOut = 0;
  if (!world || !npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }

  unsigned int serial = 0;
  try {
    serial = npc->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0) {
    return false;
  }

  int gameTs = ResolveCurrentGameTimestampSeconds(world);
  EnterCriticalSection(&g_stateMutex);
  NpcDrugState &state = g_npcDrugStates[serial];
  state.highUntilGameTs = gameTs + kDrugHighDurationSeconds;
  state.lastObservedGameTs = 0;
  state.lastObservedHunger = 0.0f;
  state.hasHungerSnapshot = false;
  LeaveCriticalSection(&g_stateMutex);

  secondsRemainingOut = kDrugHighDurationSeconds;
  return true;
}

void ApplyKnockoutPulse(Character *npc) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return;
  }
  MedicalSystem *medical = nullptr;
  try {
    medical = npc->getMedical();
  } catch (...) {
    medical = nullptr;
  }
  if (!medical || (uintptr_t)medical <= 0x1000) {
    return;
  }
  try {
    // Force an immediate KO state and let the caller decide whether to sustain it.
    medical->knockout(100.0f);
  } catch (...) {
  }
  try {
    medical->knockoutForceTimer(8.0f);
  } catch (...) {
  }
  try {
    medical->startKnockoutTimer();
  } catch (...) {
  }
}

bool AdvanceNpcDrunkLevel(GameWorld *world, Character *npc, int &newLevelOut,
                          int &secondsRemainingOut, bool &passedOutOut) {
  newLevelOut = 0;
  secondsRemainingOut = 0;
  passedOutOut = false;
  if (!world || !npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }

  unsigned int serial = 0;
  try {
    serial = npc->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0) {
    return false;
  }

  int gameTs = ResolveCurrentGameTimestampSeconds(world);
  EnterCriticalSection(&g_stateMutex);
  NpcDrunkState &state = g_npcDrunkStates[serial];

  if (state.passedOutUntilGameTs > gameTs) {
    LeaveCriticalSection(&g_stateMutex);
    return false;
  }
  if (state.level > 0 && state.levelExpiresGameTs > 0 &&
      gameTs >= state.levelExpiresGameTs) {
    state.level = 0;
    state.levelExpiresGameTs = 0;
  }

  int nextLevel = state.level + 1;
  if (nextLevel >= 3) {
    state.level = 0;
    state.levelExpiresGameTs = 0;
    state.passedOutUntilGameTs = gameTs + kDrunkPassoutDurationSeconds;
    state.nextKnockoutPulseTick = 0;
    newLevelOut = 3;
    secondsRemainingOut = kDrunkPassoutDurationSeconds;
    passedOutOut = true;
  } else {
    state.level = nextLevel;
    state.levelExpiresGameTs = gameTs + kDrunkLevelDurationSeconds;
    state.passedOutUntilGameTs = 0;
    state.nextKnockoutPulseTick = 0;
    newLevelOut = nextLevel;
    secondsRemainingOut = kDrunkLevelDurationSeconds;
    passedOutOut = false;
  }
  LeaveCriticalSection(&g_stateMutex);

  if (passedOutOut) {
    ApplyKnockoutPulse(npc);
  }
  return true;
}

bool BeginNpcSustainedKnockout(GameWorld *world, Character *npc,
                               int durationSeconds,
                               int &secondsRemainingOut) {
  secondsRemainingOut = 0;
  if (!world || !npc || (uintptr_t)npc <= 0x1000 || durationSeconds <= 0) {
    return false;
  }

  unsigned int serial = 0;
  try {
    serial = npc->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0) {
    return false;
  }

  int gameTs = ResolveCurrentGameTimestampSeconds(world);
  int desiredUntilGameTs = gameTs + durationSeconds;
  EnterCriticalSection(&g_stateMutex);
  NpcSustainedKnockoutState &state = g_npcSustainedKnockoutStates[serial];
  if (desiredUntilGameTs > state.untilGameTs) {
    state.untilGameTs = desiredUntilGameTs;
  }
  state.nextPulseTick = 0;
  secondsRemainingOut = state.untilGameTs - gameTs;
  LeaveCriticalSection(&g_stateMutex);

  if (secondsRemainingOut < 0) {
    secondsRemainingOut = 0;
  }

  ApplyKnockoutPulse(npc);
  return true;
}

bool ResolveRaceNameSafe(Character *npc, std::string &raceNameOut) {
  raceNameOut.clear();
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }
  RaceData *race = nullptr;
  try {
    race = npc->getRace() ? npc->getRace() : npc->myRace;
  } catch (...) {
    race = nullptr;
  }
  if (!race || (uintptr_t)race <= 0x1000) {
    return false;
  }
  try {
    if (race->data && (uintptr_t)race->data > 0x1000) {
      if (!race->data->name.empty()) {
        raceNameOut = race->data->name;
      } else if (!race->data->stringID.empty()) {
        raceNameOut = race->data->stringID;
      }
    }
  } catch (...) {
    raceNameOut.clear();
  }
  return !raceNameOut.empty();
}

bool IsCharacterSkeletonRace(Character *npc) {
  std::string raceName = "";
  if (!ResolveRaceNameSafe(npc, raceName)) {
    return false;
  }
  std::string token = NormalizeInventoryMatchToken(raceName);
  return token.find("skeleton") != std::string::npos;
}

bool IsCharacterShekRace(Character *npc) {
  std::string raceName = "";
  if (!ResolveRaceNameSafe(npc, raceName)) {
    return false;
  }
  std::string token = NormalizeInventoryMatchToken(raceName);
  return token.find("shek") != std::string::npos;
}

bool TryGetCharacterHornAverage(Character *npc, float &averageOut) {
  std::string reason = "";
  return TryGetCharacterHornAverageInternal(npc, averageOut, reason);
}

bool ResolveCharacterDrinkItemMatch(Character *npc, const std::string &rawQuery,
                                    std::string &matchedNameOut) {
  matchedNameOut.clear();
  Item *item = nullptr;
  std::string itemName = "";
  std::string canonicalLabel = "";
  if (!TryResolveDrinkItemForActor(npc, rawQuery, item, itemName, canonicalLabel)) {
    return false;
  }
  matchedNameOut = canonicalLabel.empty() ? itemName : canonicalLabel;
  return !matchedNameOut.empty();
}

bool ResolveCharacterDrugItemMatch(Character *npc, const std::string &rawQuery,
                                   std::string &matchedNameOut) {
  matchedNameOut.clear();
  Item *item = nullptr;
  std::string itemName = "";
  std::string canonicalLabel = "";
  if (!TryResolveDrugItemForActor(npc, rawQuery, item, itemName, canonicalLabel)) {
    return false;
  }
  matchedNameOut = canonicalLabel.empty() ? itemName : canonicalLabel;
  return !matchedNameOut.empty();
}

bool GetCharacterDrunkPromptState(Character *npc, int &levelOut,
                                  bool &isDrunkOut, std::string &statusOut,
                                  int &secondsRemainingOut) {
  levelOut = 0;
  isDrunkOut = false;
  statusOut = "sober";
  secondsRemainingOut = 0;
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }

  unsigned int serial = 0;
  try {
    serial = npc->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0) {
    return false;
  }

  GameWorld *world = GetWorldSafe();
  int gameTs = ResolveCurrentGameTimestampSeconds(world);
  EnterCriticalSection(&g_stateMutex);
  auto it = g_npcDrunkStates.find(serial);
  if (it != g_npcDrunkStates.end()) {
    BuildDrunkPromptStateFromSnapshot(it->second, gameTs, levelOut, isDrunkOut,
                                      statusOut,
                                      secondsRemainingOut);
  }
  LeaveCriticalSection(&g_stateMutex);
  return isDrunkOut;
}

bool GetCharacterDrugPromptState(Character *npc, bool &isHighOut,
                                 std::string &statusOut,
                                 int &secondsRemainingOut,
                                 float &hungerRateMultiplierOut) {
  isHighOut = false;
  statusOut = "sober";
  secondsRemainingOut = 0;
  hungerRateMultiplierOut = 1.0f;
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }

  unsigned int serial = 0;
  try {
    serial = npc->getHandle().serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0) {
    return false;
  }

  GameWorld *world = GetWorldSafe();
  int gameTs = ResolveCurrentGameTimestampSeconds(world);
  EnterCriticalSection(&g_stateMutex);
  auto it = g_npcDrugStates.find(serial);
  if (it != g_npcDrugStates.end()) {
    BuildDrugPromptStateFromSnapshot(it->second, gameTs, isHighOut, statusOut,
                                     secondsRemainingOut,
                                     hungerRateMultiplierOut);
  }
  LeaveCriticalSection(&g_stateMutex);
  return isHighOut;
}

bool CharacterHasHacksaw(Character *npc) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    return false;
  }
  std::vector<Item *> items;
  try {
    GetAllCharacterItems(npc, items);
  } catch (...) {
    items.clear();
  }
  for (size_t i = 0; i < items.size(); ++i) {
    Item *item = items[i];
    if (!item || (uintptr_t)item <= 0x1000) {
      continue;
    }
    std::string itemName = "";
    try {
      itemName = item->getName();
    } catch (...) {
      itemName = "";
    }
    if (itemName.empty()) {
      continue;
    }
    if (StringContainsAsciiInsensitive(itemName, "hacksaw") ||
        StringContainsAsciiInsensitive(itemName, "hack saw")) {
      return true;
    }
  }
  return false;
}

bool ResolveRobotLimbFromCode(int limbCode, RobotLimbs::Limb &limbOut,
                              std::string &displayNameOut) {
  switch (limbCode) {
  case (int)RobotLimbs::LEFT_ARM:
    limbOut = RobotLimbs::LEFT_ARM;
    displayNameOut = "left arm";
    return true;
  case (int)RobotLimbs::RIGHT_ARM:
    limbOut = RobotLimbs::RIGHT_ARM;
    displayNameOut = "right arm";
    return true;
  case (int)RobotLimbs::LEFT_LEG:
    limbOut = RobotLimbs::LEFT_LEG;
    displayNameOut = "left leg";
    return true;
  case (int)RobotLimbs::RIGHT_LEG:
    limbOut = RobotLimbs::RIGHT_LEG;
    displayNameOut = "right leg";
    return true;
  default:
    limbOut = RobotLimbs::NULL_LIMB;
    displayNameOut = "limb";
    return false;
  }
}

MedicalSystem::HealthPartStatus *
ResolveHealthPartForLimb(MedicalSystem *medical, RobotLimbs::Limb limb) {
  if (!medical || (uintptr_t)medical <= 0x1000) {
    return nullptr;
  }

  MedicalSystem::HealthPartStatus *part = nullptr;
  try {
    part = medical->getPart(limb);
  } catch (...) {
    part = nullptr;
  }
  if (part && (uintptr_t)part > 0x1000) {
    return part;
  }

  switch (limb) {
  case RobotLimbs::LEFT_ARM:
    return medical->leftArm;
  case RobotLimbs::RIGHT_ARM:
    return medical->rightArm;
  case RobotLimbs::LEFT_LEG:
    return medical->leftLeg;
  case RobotLimbs::RIGHT_LEG:
    return medical->rightLeg;
  default:
    return nullptr;
  }
}

void ForcePostAmputationKnockout(MedicalSystem *medical, RobotLimbs::Limb limb,
                                 bool &limbHealthForcedOut,
                                 bool &knockoutForcedOut) {
  limbHealthForcedOut = false;
  knockoutForcedOut = false;
  if (!medical || (uintptr_t)medical <= 0x1000) {
    return;
  }

  MedicalSystem::HealthPartStatus *part = ResolveHealthPartForLimb(medical, limb);
  if (part && (uintptr_t)part > 0x1000) {
    try {
      part->flesh = -100.0f;
      if (part->fleshStun > -100.0f) {
        part->fleshStun = -100.0f;
      }
      part->updateDerivedHealths();
      limbHealthForcedOut = true;
    } catch (...) {
      limbHealthForcedOut = false;
    }
  }

  try {
    medical->validateHealthValues();
  } catch (...) {
  }

  try {
    medical->knockoutForceTimer(8.0f);
    knockoutForcedOut = true;
  } catch (...) {
    knockoutForcedOut = false;
  }
  if (!knockoutForcedOut) {
    try {
      medical->startKnockoutTimer();
      knockoutForcedOut = true;
    } catch (...) {
      knockoutForcedOut = false;
    }
  }
}

static bool ForceImmediateCharacterKnockout(Character *target,
                                            bool &alreadyKnockedOutOut,
                                            bool &knockoutAppliedOut,
                                            bool &forceTimerAppliedOut,
                                            bool &medicalValidatedOut) {
  alreadyKnockedOutOut = false;
  knockoutAppliedOut = false;
  forceTimerAppliedOut = false;
  medicalValidatedOut = false;
  if (!target || (uintptr_t)target <= 0x1000) {
    return false;
  }

  bool isUnconscious = false;
  try {
    isUnconscious = target->isUnconcious();
  } catch (...) {
    isUnconscious = false;
  }
  bool isKnockedOut = false;
  try {
    isKnockedOut = target->isDown();
  } catch (...) {
    isKnockedOut = false;
  }
  alreadyKnockedOutOut = isUnconscious || isKnockedOut;

  MedicalSystem *medical = nullptr;
  try {
    medical = target->getMedical();
  } catch (...) {
    medical = nullptr;
  }
  if (medical && (uintptr_t)medical > 0x1000) {
    try {
      medical->knockout(100.0f);
      knockoutAppliedOut = true;
    } catch (...) {
      knockoutAppliedOut = false;
    }
    try {
      medical->knockoutForceTimer(8.0f);
      forceTimerAppliedOut = true;
    } catch (...) {
      forceTimerAppliedOut = false;
    }
    if (!forceTimerAppliedOut) {
      try {
        medical->startKnockoutTimer();
        forceTimerAppliedOut = true;
      } catch (...) {
        forceTimerAppliedOut = false;
      }
    }
    try {
      medical->validateHealthValues();
      medicalValidatedOut = true;
    } catch (...) {
      medicalValidatedOut = false;
    }
  }

  bool nowUnconscious = false;
  try {
    nowUnconscious = target->isUnconcious();
  } catch (...) {
    nowUnconscious = false;
  }
  bool nowKnockedOut = false;
  try {
    nowKnockedOut = target->isDown();
  } catch (...) {
    nowKnockedOut = false;
  }
  // Kenshi can apply the prone/KO state a short moment after the medical calls
  // succeed, so treat accepted knockout scheduling as success too.
  return alreadyKnockedOutOut || nowUnconscious || nowKnockedOut ||
         knockoutAppliedOut || forceTimerAppliedOut;
}

bool IsCharacterPrisoned(Character *target) {
  if (!target || (uintptr_t)target <= 0x1000) {
    return false;
  }
  bool inPrison = false;
  try {
    inPrison = (target->inSomething == IN_PRISON);
  } catch (...) {
    inPrison = false;
  }
  bool chained = false;
  try {
    // Prisoner poles/captive bindings typically set chained mode.
    chained = target->isChainedMode();
  } catch (...) {
    chained = false;
  }
  return inPrison || chained;
}

bool IsCharacterBeingCarried(GameWorld *world, Character *target,
                             std::string &carrierNameOut) {
  carrierNameOut.clear();
  if (!world || !target || (uintptr_t)target <= 0x1000) {
    return false;
  }

  unsigned int targetSerial = 0;
  try {
    targetSerial = target->getHandle().serial;
  } catch (...) {
    targetSerial = 0;
  }
  if (targetSerial == 0) {
    return false;
  }

  try {
    if (target->isKidnapped()) {
      carrierNameOut = "someone";
      return true;
    }
  } catch (...) {
  }

  const auto &chars = world->getCharacterUpdateList();
  for (auto it = chars.begin(); it != chars.end(); ++it) {
    Character *candidate = *it;
    if (!candidate || (uintptr_t)candidate <= 0x1000) {
      continue;
    }
    unsigned int candidateSerial = 0;
    try {
      candidateSerial = candidate->getHandle().serial;
    } catch (...) {
      candidateSerial = 0;
    }
    if (candidateSerial == 0 || candidateSerial == targetSerial) {
      continue;
    }
    bool carriesTarget = false;
    try {
      carriesTarget =
          candidate->isCarryingSomething && candidate->carryingObject.isValid() &&
          candidate->carryingObject.serial == targetSerial;
    } catch (...) {
      carriesTarget = false;
    }
    if (!carriesTarget) {
      continue;
    }
    carrierNameOut = SafeCharacterName(candidate);
    return true;
  }
  return false;
}

bool IsRemoveLimbTargetValid(GameWorld *world, Character *target,
                             std::string &reasonOut) {
  reasonOut.clear();
  if (!target || (uintptr_t)target <= 0x1000) {
    reasonOut = "target not found";
    return false;
  }

  bool isDead = false;
  try {
    isDead = target->isDead();
  } catch (...) {
    isDead = false;
  }
  if (isDead) {
    reasonOut = "target is dead";
    return false;
  }

  bool isUnconscious = false;
  try {
    isUnconscious = target->isUnconcious();
  } catch (...) {
    isUnconscious = false;
  }
  bool isKnockedOut = false;
  try {
    isKnockedOut = target->isDown();
  } catch (...) {
    isKnockedOut = false;
  }
  bool isPrisoned = IsCharacterPrisoned(target);
  std::string carrierName = "";
  bool isCarried = IsCharacterBeingCarried(world, target, carrierName);
  if (isUnconscious || isKnockedOut || isPrisoned || isCarried) {
    return true;
  }

  reasonOut = "target must be knocked out, unconscious, imprisoned, or carried";
  return false;
}

bool IsTakeItemLootTargetValid(GameWorld *world, Character *target,
                               std::string &reasonOut, bool &isDeadOut) {
  reasonOut.clear();
  isDeadOut = false;
  if (!target || (uintptr_t)target <= 0x1000) {
    reasonOut = "target not found";
    return false;
  }

  bool isDead = false;
  try {
    isDead = target->isDead();
  } catch (...) {
    isDead = false;
  }
  isDeadOut = isDead;
  if (isDead) {
    return true;
  }

  bool isUnconscious = false;
  try {
    isUnconscious = target->isUnconcious();
  } catch (...) {
    isUnconscious = false;
  }
  bool isKnockedOut = false;
  try {
    isKnockedOut = target->isDown();
  } catch (...) {
    isKnockedOut = false;
  }
  bool isPrisoned = IsCharacterPrisoned(target);
  std::string carrierName = "";
  bool isCarried = IsCharacterBeingCarried(world, target, carrierName);
  if (isUnconscious || isKnockedOut || isPrisoned || isCarried) {
    return true;
  }

  reasonOut =
      "target must be dead, knocked out, unconscious, imprisoned, or carried";
  return false;
}

bool IsPickupNpcTargetValid(GameWorld *world, Character *target,
                            std::string &reasonOut, bool &isDeadOut) {
  reasonOut.clear();
  isDeadOut = false;
  if (!target || (uintptr_t)target <= 0x1000) {
    reasonOut = "target not found";
    return false;
  }

  bool isDead = false;
  try {
    isDead = target->isDead();
  } catch (...) {
    isDead = false;
  }
  isDeadOut = isDead;

  bool isUnconscious = false;
  try {
    isUnconscious = target->isUnconcious();
  } catch (...) {
    isUnconscious = false;
  }
  bool isKnockedOut = false;
  try {
    isKnockedOut = target->isDown();
  } catch (...) {
    isKnockedOut = false;
  }
  bool isPrisoned = IsCharacterPrisoned(target);
  if (!(isDead || isUnconscious || isKnockedOut || isPrisoned)) {
    reasonOut = "target must be dead, knocked out, unconscious, or imprisoned";
    return false;
  }

  std::string carrierName = "";
  if (IsCharacterBeingCarried(world, target, carrierName)) {
    reasonOut = carrierName.empty()
                    ? "target is already being carried"
                    : ("target is already being carried by " + carrierName);
    return false;
  }

  return true;
}

struct UseObjectCandidate {
  Building *building;
  std::string name;
  unsigned int serial;
  float distance;
  BuildingClassType classType;
  BuildingFunction functionType;
  TaskType taskType;
  bool destroyed;
  bool broken;
  bool hasFreeSlot;
  bool slotAvailableEstimate;
  int occupiedEstimate;
  bool usableNow;

  UseObjectCandidate()
      : building(nullptr), name(""), serial(0), distance(0.0f),
        classType(BCTYPE_FLUFF), functionType(BF_ANY), taskType(NULL_TASK),
        destroyed(false), broken(false), hasFreeSlot(false),
        slotAvailableEstimate(false), occupiedEstimate(0),
        usableNow(false) {}
};

std::string SafeBuildingName(Building *building) {
  if (!building || (uintptr_t)building <= 0x1000) {
    return "";
  }
  std::string name = "";
  try {
    name = TrimCopySimple(building->getName());
  } catch (...) {
    name = "";
  }
  if (name.empty()) {
    name = "nearby object";
  }
  return name;
}

std::string NormalizeUseObjectToken(const std::string &value) {
  std::string normalized = TrimCopySimple(value);
  while (!normalized.empty()) {
    char first = normalized.front();
    if (first == '"' || first == '\'' || first == '(' || first == '[' ||
        first == '{' || first == ' ') {
      normalized.erase(0, 1);
      continue;
    }
    break;
  }
  while (!normalized.empty()) {
    char last = normalized.back();
    if (last == '"' || last == '\'' || last == ')' || last == ']' ||
        last == '}' || last == '.' || last == ',' || last == '!' ||
        last == '?' || last == ':' || last == ';' || last == ' ') {
      normalized.erase(normalized.size() - 1, 1);
      continue;
    }
    break;
  }
  normalized = ToLowerCopy(normalized);
  for (size_t i = 0; i < normalized.size(); ++i) {
    if (normalized[i] == '_' || normalized[i] == '-') {
      normalized[i] = ' ';
    }
  }
  normalized = TrimCopySimple(normalized);
  if (normalized.find("the ") == 0) {
    normalized = TrimCopySimple(normalized.substr(4));
  } else if (normalized.find("a ") == 0) {
    normalized = TrimCopySimple(normalized.substr(2));
  } else if (normalized.find("an ") == 0) {
    normalized = TrimCopySimple(normalized.substr(3));
  }
  return normalized;
}

bool ParseUseObjectSerialToken(const std::string &rawToken,
                               unsigned int &serialOut) {
  serialOut = 0;
  std::string token = ToLowerCopy(TrimCopySimple(rawToken));
  if (token.empty()) {
    return false;
  }
  if (token.find("hand_") == 0) {
    token = token.substr(5);
  }
  if (token.find("serial_") == 0) {
    token = token.substr(7);
  } else if (token.find("serial:") == 0) {
    token = token.substr(7);
  }
  token = TrimCopySimple(token);
  if (token.empty()) {
    return false;
  }
  for (size_t i = 0; i < token.size(); ++i) {
    unsigned char ch = (unsigned char)token[i];
    if (ch < '0' || ch > '9') {
      return false;
    }
  }
  unsigned long parsed = strtoul(token.c_str(), NULL, 10);
  if (parsed == 0 || parsed > 0xFFFFFFFFUL) {
    return false;
  }
  serialOut = (unsigned int)parsed;
  return true;
}

std::string UseObjectClassLabel(BuildingClassType classType) {
  switch (classType) {
  case BCTYPE_USABLE:
    return "usable";
  case BCTYPE_TURRET:
    return "turret";
  case BCTYPE_PRODUCTION:
    return "production";
  case BCTYPE_CRAFTING:
    return "crafting";
  case BCTYPE_RESEARCH:
    return "research";
  case BCTYPE_STORAGE:
    return "storage";
  case BCTYPE_FARM:
    return "farm";
  case BCTYPE_LIGHT:
    return "light";
  default:
    return "other";
  }
}

std::string UseObjectFunctionLabel(BuildingFunction functionType) {
  switch (functionType) {
  case BF_CHAIR:
    return "chair";
  case BF_TURRET:
    return "turret";
  case BF_BED:
    return "bed";
  case BF_SKELETON_BED:
    return "skeleton_bed";
  case BF_THRONE:
    return "throne";
  case BF_CRAFTING:
    return "crafting";
  case BF_RESEARCH:
    return "research";
  case BF_MINE:
    return "mine";
  case BF_MINE_NATURAL:
    return "mine_natural";
  case BF_REFINERY:
    return "refinery";
  case BF_GENERATOR:
    return "generator";
  case BF_ENGINE:
    return "engine";
  case BF_STEERING:
    return "steering";
  case BF_ITEM_FURNACE:
    return "item_furnace";
  case BF_CAGE:
    return "cage";
  case BF_TABLE:
    return "table";
  default:
    return "other";
  }
}

bool IsUseObjectFunctionCandidate(BuildingFunction functionType) {
  switch (functionType) {
  case BF_CHAIR:
  case BF_TURRET:
  case BF_BED:
  case BF_SKELETON_BED:
  case BF_THRONE:
  case BF_CRAFTING:
  case BF_RESEARCH:
  case BF_MINE:
  case BF_MINE_NATURAL:
  case BF_REFINERY:
  case BF_GENERATOR:
  case BF_ENGINE:
  case BF_STEERING:
  case BF_ITEM_FURNACE:
  case BF_CAGE:
  case BF_TABLE:
    return true;
  default:
    return false;
  }
}

bool IsUseObjectClassCandidate(BuildingClassType classType) {
  return classType == BCTYPE_USABLE || classType == BCTYPE_TURRET ||
         classType == BCTYPE_PRODUCTION || classType == BCTYPE_CRAFTING ||
         classType == BCTYPE_RESEARCH || classType == BCTYPE_FARM;
}

bool IsUseObjectTaskCandidate(TaskType taskType) {
  switch (taskType) {
  case USE_TURRET:
  case MAN_A_TURRET:
  case MAN_A_TURRET_ON_BUILDING:
  case MAN_A_TURRET_PLAYER_JOB:
  case USE_BED:
  case USE_BED_ORDER:
  case OPERATE_MACHINERY:
  case OPERATE_AUTOMATIC_MACHINERY:
  case USE_CAGE:
  case SIT_ON_THRONE:
  case SIT_AROUND:
  case USE_TRAINING_DUMMY:
  case REST:
  case JOB_KEEP_EVERYTHING_RUNNING:
    return true;
  default:
    return false;
  }
}

TaskType ResolveUseObjectTask(TaskType defaultTask,
                              BuildingFunction functionType) {
  if (defaultTask != NULL_TASK && IsUseObjectTaskCandidate(defaultTask)) {
    return defaultTask;
  }
  switch (functionType) {
  case BF_TURRET:
    return MAN_A_TURRET_ON_BUILDING;
  case BF_BED:
  case BF_SKELETON_BED:
    return USE_BED;
  case BF_CAGE:
    return USE_CAGE;
  case BF_THRONE:
    return SIT_ON_THRONE;
  case BF_CHAIR:
  case BF_TABLE:
    return SIT_AROUND;
  case BF_CRAFTING:
  case BF_RESEARCH:
  case BF_REFINERY:
  case BF_GENERATOR:
  case BF_ENGINE:
  case BF_STEERING:
  case BF_ITEM_FURNACE:
  case BF_MINE:
  case BF_MINE_NATURAL:
    return OPERATE_MACHINERY;
  default:
    return NULL_TASK;
  }
}

std::map<unsigned int, int> BuildUseObjectOccupancyMap(GameWorld *world,
                                                        Character *actor,
                                                        float searchRange) {
  std::map<unsigned int, int> occupancy;
  if (!world || !actor || (uintptr_t)actor <= 0x1000) {
    return occupancy;
  }

  lektor<RootObject *> nearbyCharacters;
  world->getObjectsWithinSphere(nearbyCharacters, actor->getPosition(),
                                searchRange, CHARACTER, 128,
                                (RootObject *)actor);
  for (uint32_t i = 0; i < nearbyCharacters.size(); ++i) {
    Character *candidate = (Character *)nearbyCharacters.stuff[i];
    if (!candidate || (uintptr_t)candidate <= 0x1000) {
      continue;
    }
    bool dead = false;
    try {
      dead = candidate->isDead();
    } catch (...) {
      dead = false;
    }
    if (dead) {
      continue;
    }

    unsigned int useSerial = 0;
    try {
      if (candidate->inSomething != IN_NOTHING && candidate->inWhat.isValid() &&
          candidate->inWhat.serial != 0) {
        useSerial = candidate->inWhat.serial;
      }
    } catch (...) {
      useSerial = 0;
    }

    unsigned int turretSerial = 0;
    try {
      if (candidate->isUsingTurret.isValid() &&
          candidate->isUsingTurret.serial != 0) {
        turretSerial = candidate->isUsingTurret.serial;
      }
    } catch (...) {
      turretSerial = 0;
    }

    if (useSerial != 0) {
      occupancy[useSerial] += 1;
    }
    if (turretSerial != 0 && turretSerial != useSerial) {
      occupancy[turretSerial] += 1;
    }
  }

  return occupancy;
}

void CollectUseObjectCandidates(GameWorld *world, Character *npc, float range,
                                std::vector<UseObjectCandidate> &out) {
  out.clear();
  if (!world || !npc || (uintptr_t)npc <= 0x1000) {
    return;
  }

  std::map<unsigned int, int> occupancy =
      BuildUseObjectOccupancyMap(world, npc, range + 4.0f);
  std::map<unsigned int, bool> seenSerials;
  lektor<RootObject *> nearbyBuildings;
  world->getObjectsWithinSphere(nearbyBuildings, npc->getPosition(), range,
                                BUILDING, 96, (RootObject *)npc);
  for (uint32_t i = 0; i < nearbyBuildings.size(); ++i) {
    Building *building = (Building *)nearbyBuildings.stuff[i];
    if (!building || (uintptr_t)building <= 0x1000) {
      continue;
    }

    unsigned int buildingSerial = 0;
    try {
      buildingSerial = building->getHandle().serial;
    } catch (...) {
      buildingSerial = 0;
    }
    if (buildingSerial != 0 && seenSerials.count(buildingSerial) > 0) {
      continue;
    }
    if (buildingSerial != 0) {
      seenSerials[buildingSerial] = true;
    }

    BuildingClassType classType = BCTYPE_FLUFF;
    BuildingFunction functionType = BF_ANY;
    TaskType defaultTask = NULL_TASK;
    bool destroyed = false;
    bool broken = false;
    bool hasFreeSlot = false;
    try {
      classType = building->_NV_getBuildingClass();
    } catch (...) {
      classType = BCTYPE_FLUFF;
    }
    try {
      functionType = building->_NV_getSpecialFunction();
    } catch (...) {
      functionType = BF_ANY;
    }
    try {
      defaultTask = building->_NV_getDefaultTask();
    } catch (...) {
      defaultTask = NULL_TASK;
    }
    try {
      destroyed = building->_NV_isDestroyed();
    } catch (...) {
      destroyed = false;
    }
    try {
      broken = building->_NV_isBroken();
    } catch (...) {
      broken = false;
    }
    try {
      building->forceValidUsageNodesValidation();
    } catch (...) {
    }
    try {
      hasFreeSlot = building->hasAnyGoodPositionMarkersLeft();
    } catch (...) {
      hasFreeSlot = false;
    }

    bool isCandidate = IsUseObjectFunctionCandidate(functionType) ||
                       IsUseObjectClassCandidate(classType) ||
                       IsUseObjectTaskCandidate(defaultTask);
    if (!isCandidate) {
      continue;
    }

    UseObjectCandidate candidate;
    candidate.building = building;
    candidate.name = SafeBuildingName(building);
    candidate.serial = buildingSerial;
    candidate.distance = npc->getPosition().distance(building->getPosition());
    candidate.classType = classType;
    candidate.functionType = functionType;
    candidate.taskType = ResolveUseObjectTask(defaultTask, functionType);
    candidate.destroyed = destroyed;
    candidate.broken = broken;
    candidate.hasFreeSlot = hasFreeSlot;
    candidate.occupiedEstimate =
        (buildingSerial != 0 && occupancy.count(buildingSerial) > 0)
            ? occupancy[buildingSerial]
            : 0;
    candidate.slotAvailableEstimate =
        hasFreeSlot || candidate.occupiedEstimate <= 0;
    candidate.usableNow = !destroyed && !broken &&
                          candidate.slotAvailableEstimate &&
                          candidate.taskType != NULL_TASK;
    out.push_back(candidate);
  }

  std::sort(out.begin(), out.end(),
            [](const UseObjectCandidate &a,
               const UseObjectCandidate &b) -> bool {
              if (a.distance == b.distance) {
                return a.serial < b.serial;
              }
              return a.distance < b.distance;
            });
}

int ScoreUseObjectCandidate(const UseObjectCandidate &candidate,
                            const std::string &normalizedToken,
                            unsigned int targetSerial) {
  if (targetSerial != 0 && candidate.serial == targetSerial) {
    return 10000;
  }
  if (normalizedToken.empty()) {
    return 1;
  }

  const std::string nameToken = NormalizeUseObjectToken(candidate.name);
  const std::string classToken = UseObjectClassLabel(candidate.classType);
  const std::string functionToken = UseObjectFunctionLabel(candidate.functionType);
  std::string keywordToken = classToken + " " + functionToken;
  if (candidate.functionType == BF_TURRET ||
      candidate.classType == BCTYPE_TURRET) {
    keywordToken += " gun";
  }
  if (candidate.functionType == BF_CHAIR || candidate.functionType == BF_THRONE ||
      candidate.functionType == BF_TABLE) {
    keywordToken += " seat";
  }
  if (candidate.classType == BCTYPE_PRODUCTION ||
      candidate.classType == BCTYPE_CRAFTING ||
      candidate.functionType == BF_CRAFTING ||
      candidate.functionType == BF_REFINERY ||
      candidate.functionType == BF_ENGINE ||
      candidate.functionType == BF_STEERING ||
      candidate.functionType == BF_GENERATOR) {
    keywordToken += " workspot workstation";
  }

  int score = 0;
  if (!nameToken.empty() && nameToken == normalizedToken) {
    score = 500;
  } else if (!nameToken.empty() &&
             nameToken.find(normalizedToken) != std::string::npos) {
    score = 400;
  } else if (!nameToken.empty() &&
             normalizedToken.find(nameToken) != std::string::npos) {
    score = 350;
  }
  if (score < 250 &&
      keywordToken.find(normalizedToken) != std::string::npos) {
    score = 250;
  }
  return score;
}

std::string ExplainUseObjectUnavailable(const UseObjectCandidate &candidate) {
  if (candidate.destroyed) {
    return "it is destroyed";
  }
  if (candidate.broken) {
    return "it is broken";
  }
  if (candidate.taskType == NULL_TASK) {
    return "it has no usable interaction task";
  }
  if (!candidate.hasFreeSlot) {
    if (candidate.occupiedEstimate > 0) {
      return "it appears to be in use";
    }
    return "slot availability could not be confirmed";
  }
  return "it is not usable right now";
}

bool TrySetStandingOrderSafe(Character *npc, MessageForB::StandingOrder order,
                             bool enabled, const std::string &label) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    Log("ACTION_EXEC: Toggle write skipped (" + label + "): invalid npc");
    return false;
  }
  auto tryReadOrder = [](Character *c, MessageForB::StandingOrder o,
                         bool fallback) -> bool {
    if (!c || (uintptr_t)c <= 0x1000) {
      return fallback;
    }
    try {
      return c->getStandingOrder(o);
    } catch (...) {
      return fallback;
    }
  };

  const bool beforeState = tryReadOrder(npc, order, false);
  try {
    npc->setStandingOrder(order, enabled);
    return true;
  } catch (...) {
    Log("ACTION_EXEC: Toggle write exception (" + label + ") order=" +
        ToString((int)order) + " enabled=" + (enabled ? "1" : "0"));
    // Fallback path: Character::setStandingOrder can throw for some NPC states.
    // RootObject::setStandingOrder toggles the flag in-place; we use it only
    // when the desired state differs from the current observed state.
    try {
      RootObject *root = (RootObject *)npc;
      if (root && (uintptr_t)root > 0x1000) {
        if (beforeState != enabled) {
          root->setStandingOrder(order);
        }
        bool afterState = tryReadOrder(npc, order, beforeState);
        bool success = (afterState == enabled);
        Log("ACTION_EXEC: Toggle fallback (" + label + ") order=" +
            ToString((int)order) + " before=" + (beforeState ? "1" : "0") +
            " after=" + (afterState ? "1" : "0") +
            " target=" + (enabled ? "1" : "0") +
            " success=" + (success ? "1" : "0"));
        if (success) {
          return true;
        }
      }
    } catch (...) {
      Log("ACTION_EXEC: Toggle fallback exception (" + label + ") order=" +
          ToString((int)order));
    }
    // Final fallback for non-party NPCs where standing-order API rejects
    // writes: directly update known CharStats mode flags.
    try {
      CharStats *stats = nullptr;
      try {
        stats = npc->getStats();
      } catch (...) {
        stats = nullptr;
        Log("ACTION_EXEC: Toggle stats fallback getStats exception (" + label +
            ") order=" + ToString((int)order));
      }
      if ((!stats || (uintptr_t)stats <= 0x1000) && npc->stats &&
          (uintptr_t)npc->stats > 0x1000) {
        stats = npc->stats;
        Log("ACTION_EXEC: Toggle stats fallback using direct npc->stats (" +
            label + ") order=" + ToString((int)order));
      }
      if (stats && (uintptr_t)stats > 0x1000) {
        bool *modeFlag = nullptr;
        if (order == MessageForB::M_SET_ORDER_DEF ||
            order == MessageForB::M_SET_ORDER_DEFENSIVE_COMBAT) {
          modeFlag = &stats->_defensiveMode;
        } else if ((int)order == 12) {
          modeFlag = &stats->_holdPositionMode;
          if (enabled) {
            stats->setHoldLocation(npc->getPosition());
          } else {
            stats->clearHoldLocation();
          }
        } else if ((int)order == 13) {
          modeFlag = &stats->passiveCombatMode;
        } else if (order == MessageForB::M_SET_ORDER_RANGED) {
          modeFlag = &stats->rangedMode;
        } else if (order == MessageForB::M_SET_ORDER_TAUNT) {
          modeFlag = &stats->tauntMode;
        }
        if (modeFlag) {
          bool before = *modeFlag;
          *modeFlag = enabled;
          bool after = *modeFlag;
          bool success = (after == enabled);
          Log("ACTION_EXEC: Toggle stats fallback (" + label + ") order=" +
              ToString((int)order) + " before=" + (before ? "1" : "0") +
              " after=" + (after ? "1" : "0") +
              " target=" + (enabled ? "1" : "0") +
              " success=" + (success ? "1" : "0"));
          return success;
        }
        Log("ACTION_EXEC: Toggle stats fallback unsupported order (" + label +
            ") order=" + ToString((int)order));
      } else {
        Log("ACTION_EXEC: Toggle stats fallback unavailable (" + label +
            ") order=" + ToString((int)order));
      }
    } catch (...) {
      Log("ACTION_EXEC: Toggle stats fallback exception (" + label +
          ") order=" + ToString((int)order));
    }
    return false;
  }
}

bool TrySetStealthModeSafe(Character *npc, bool enabled,
                           const std::string &label) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    Log("ACTION_EXEC: Toggle write skipped (" + label + "): invalid npc");
    return false;
  }
  try {
    npc->setStealthMode(enabled);
    return true;
  } catch (...) {
    Log("ACTION_EXEC: Toggle write exception (" + label +
        ") enabled=" + (enabled ? "1" : "0"));
    return false;
  }
}

void SetJobsEnabled(Character *npc, bool enabled);
void SetResourceMode(Character *npc, bool enabled);
void SetMedicMode(Character *npc, bool enabled);

bool TryApplyJobsToggleSafe(Character *npc, bool enabled,
                            const std::string &label) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    Log("ACTION_EXEC: Toggle write skipped (" + label + "): invalid npc");
    return false;
  }
  try {
    SetJobsEnabled(npc, enabled);
    return true;
  } catch (...) {
    Log("ACTION_EXEC: Toggle write exception (" + label +
        ") enabled=" + (enabled ? "1" : "0"));
    return false;
  }
}

bool TryApplyResourceToggleSafe(Character *npc, bool enabled,
                                const std::string &label) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    Log("ACTION_EXEC: Toggle write skipped (" + label + "): invalid npc");
    return false;
  }
  try {
    SetResourceMode(npc, enabled);
    return true;
  } catch (...) {
    Log("ACTION_EXEC: Toggle write exception (" + label +
        ") enabled=" + (enabled ? "1" : "0"));
    return false;
  }
}

bool TryApplyMedicToggleSafe(Character *npc, bool enabled,
                             const std::string &label) {
  if (!npc || (uintptr_t)npc <= 0x1000) {
    Log("ACTION_EXEC: Toggle write skipped (" + label + "): invalid npc");
    return false;
  }
  try {
    SetMedicMode(npc, enabled);
    return true;
  } catch (...) {
    Log("ACTION_EXEC: Toggle write exception (" + label +
        ") enabled=" + (enabled ? "1" : "0"));
    return false;
  }
}

bool HasPermaJob(Character *npc, TaskType task) {
  if (!npc) {
    return false;
  }
  int count = npc->getPermajobCount();
  for (int i = 0; i < count; ++i) {
    if (npc->getPermajob(i) == task) {
      return true;
    }
  }
  return false;
}

void RemovePermaJob(Character *npc, TaskType task) {
  if (!npc) {
    return;
  }
  for (int i = npc->getPermajobCount() - 1; i >= 0; --i) {
    if (npc->getPermajob(i) == task) {
      npc->removePermajob(i);
    }
  }
  npc->removeJob(task);
}

bool IsResourceTask(TaskType task) {
  return task == JOB_KEEP_EVERYTHING_RUNNING || task == DELIVER_RESOURCES ||
         task == COLLECT_OUTPUT_RESOURCE || task == FILL_MACHINE ||
         task == OPERATE_MACHINERY || task == OPERATE_AUTOMATIC_MACHINERY ||
         task == GET_RID_OF_RESOURCES_IN_MY_INVENTORY ||
         task == DITCH_ALL_RESOURCES || task == AUTO_LABOURING_MINES;
}

bool IsMedicTask(TaskType task) {
  return task == JOB_MEDIC || task == FIRST_AID_ORDER ||
         task == FIRST_AID_ROBOT || task == SPLINT_ORDER ||
         task == SPLINT_JOB || task == JOB_REPAIR_ROBOT;
}

bool HasMatchingPermaJob(Character *npc, bool (*predicate)(TaskType)) {
  if (!npc || !predicate) {
    return false;
  }
  int count = npc->getPermajobCount();
  for (int i = 0; i < count; ++i) {
    if (predicate(npc->getPermajob(i))) {
      return true;
    }
  }
  return false;
}

void RemoveMatchingPermaJobs(Character *npc, bool (*predicate)(TaskType)) {
  if (!npc || !predicate) {
    return;
  }
  for (int i = npc->getPermajobCount() - 1; i >= 0; --i) {
    TaskType task = npc->getPermajob(i);
    if (predicate(task)) {
      npc->removePermajob(i);
      npc->removeJob(task);
    }
  }
}

void EnsureIdleFallbackJob(Character *npc) {
  if (!npc || npc->getPermajobCount() > 0) {
    return;
  }
  npc->addJob(IDLE, NULL, true, false, npc->getPosition());
  npc->addGoal(IDLE, NULL);
}

void ClearAllJobsForFollow(Character *npc) {
  if (!npc) {
    return;
  }
  try {
    npc->clearPermajobs();
  } catch (...) {
  }
  static const TaskType kFollowCancelledJobs[] = {
      GO_HOMEBUILDING,
      STAND_AT_SHOPKEEPER_NODE,
      IDLE,
      WANDERER,
      WANDER_TOWN,
      PATROL_TOWN,
      STAND_AT_GUARD_NODE_HOMEBUILDING_IN_OUT,
      STAND_AT_GENERAL_NODE,
      STAND_AT_DEFENSIVE_NODE,
      STAND_AT_BUILDING_GUARD_NODE,
      STAND_AT_BUILDING_DEFENSIVE_NODE,
      STAND_AT_NODE,
      STAND_AT_GUARD_NODE_HOMETOWN_OUTSIDE,
      STAND_AT_GUARD_NODE_HOMEBUILDING_INDOORS_ONLY,
      STAY_IN_HOME,
      MAN_THE_GATE,
      OPEN_UP_SHOP_DOORS,
      RELAX_IN_TOWN_PACKAGE,
      TRAVEL_TO_TARGET_PACKAGE,
      TRAVEL_TO_TARGET_TOWN,
      TRAVEL_TO_TARGET_TOWN_FAST,
      RUN_AWAY,
      JOB_KEEP_EVERYTHING_RUNNING,
      DELIVER_RESOURCES,
      COLLECT_OUTPUT_RESOURCE,
      FILL_MACHINE,
      OPERATE_MACHINERY,
      OPERATE_AUTOMATIC_MACHINERY,
      GET_RID_OF_RESOURCES_IN_MY_INVENTORY,
      DITCH_ALL_RESOURCES,
      AUTO_LABOURING_MINES,
      JOB_MEDIC,
      FIRST_AID_ORDER,
      FIRST_AID_ROBOT,
      SPLINT_ORDER,
      SPLINT_JOB,
      JOB_REPAIR_ROBOT,
  };
  for (size_t i = 0; i < sizeof(kFollowCancelledJobs) / sizeof(kFollowCancelledJobs[0]); ++i) {
    try {
      npc->removeJob(kFollowCancelledJobs[i]);
    } catch (...) {
    }
  }
}

void SetJobsEnabled(Character *npc, bool enabled) {
  if (!npc) {
    return;
  }
  if (enabled) {
    EnsureIdleFallbackJob(npc);
    return;
  }
  npc->clearPermajobs();
  npc->clearAllAIGoals();
  EnsureIdleFallbackJob(npc);
}

void SetResourceMode(Character *npc, bool enabled) {
  if (!npc) {
    return;
  }
  if (enabled) {
    if (!HasMatchingPermaJob(npc, IsResourceTask)) {
      npc->addJob(JOB_KEEP_EVERYTHING_RUNNING, NULL, true, false,
                  npc->getPosition());
      npc->addGoal(JOB_KEEP_EVERYTHING_RUNNING, NULL);
    }
    return;
  }
  RemoveMatchingPermaJobs(npc, IsResourceTask);
  EnsureIdleFallbackJob(npc);
}

void SetMedicMode(Character *npc, bool enabled) {
  if (!npc) {
    return;
  }
  if (enabled) {
    if (!HasMatchingPermaJob(npc, IsMedicTask)) {
      npc->addJob(JOB_MEDIC, NULL, true, false, npc->getPosition());
      npc->addGoal(JOB_MEDIC, NULL);
    }
    return;
  }
  RemoveMatchingPermaJobs(npc, IsMedicTask);
  EnsureIdleFallbackJob(npc);
}

Character *ResolveCharacterBySerialFromWorld(GameWorld *world,
                                             unsigned int serial) {
  if (!world || serial == 0) {
    return nullptr;
  }
  try {
    const auto &chars = world->getCharacterUpdateList();
    for (auto it = chars.begin(); it != chars.end(); ++it) {
      Character *candidate = *it;
      if (!candidate || (uintptr_t)candidate <= 0x1000) {
        continue;
      }
      unsigned int candidateSerial = 0;
      try {
        candidateSerial = candidate->getHandle().serial;
      } catch (...) {
        candidateSerial = 0;
      }
      if (candidateSerial == serial) {
        return candidate;
      }
    }
  } catch (...) {
  }
  return nullptr;
}

void QueueHornCutReapply(Character *target) {
  if (!target || (uintptr_t)target <= 0x1000) {
    return;
  }

  unsigned int serial = 0;
  hand targetHand;
  try {
    targetHand = target->getHandle();
    serial = targetHand.serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0) {
    return;
  }

  DWORD nowTick = GetTickCount();
  PendingHornCutReapplyState state;
  state.target = targetHand;
  state.serial = serial;
  state.nextReapplyTick = nowTick + 250;
  state.expireTick = nowTick + 8000;
  state.remainingAttempts = 24;
  g_pendingHornCutReapplies[serial] = state;
}

void QueueHornCutDismiss(Character *target, const std::string &originFactionToken) {
  if (!target || (uintptr_t)target <= 0x1000 || originFactionToken.empty()) {
    return;
  }

  unsigned int serial = 0;
  hand targetHand;
  try {
    targetHand = target->getHandle();
    serial = targetHand.serial;
  } catch (...) {
    serial = 0;
  }
  if (serial == 0) {
    return;
  }

  DWORD nowTick = GetTickCount();
  PendingHornCutDismissState state;
  state.target = targetHand;
  state.serial = serial;
  state.originFactionToken = originFactionToken;
  state.nextCheckTick = nowTick + 1200;
  state.expireTick = nowTick + 12000;
  state.remainingChecks = 20;
  g_pendingHornCutDismissals[serial] = state;
}

void UpdatePendingHornCutReapplies(GameWorld *world) {
  DWORD nowTick = GetTickCount();
  for (auto it = g_pendingHornCutReapplies.begin();
       it != g_pendingHornCutReapplies.end();) {
    PendingHornCutReapplyState &state = it->second;
    if (state.serial == 0 || state.remainingAttempts <= 0 ||
        (state.expireTick != 0 && nowTick >= state.expireTick)) {
      it = g_pendingHornCutReapplies.erase(it);
      continue;
    }
    if (state.nextReapplyTick != 0 && nowTick < state.nextReapplyTick) {
      ++it;
      continue;
    }

    Character *target = nullptr;
    try {
      if (state.target.isValid()) {
        target = state.target.getCharacter();
      }
    } catch (...) {
      target = nullptr;
    }
    if ((!target || (uintptr_t)target <= 0x1000) && world) {
      target = ResolveCharacterBySerialFromWorld(world, state.serial);
    }
    if (!target || (uintptr_t)target <= 0x1000) {
      state.remainingAttempts = 0;
      it = g_pendingHornCutReapplies.erase(it);
      continue;
    }

    float average = 0.0f;
    std::string reason = "";
    if (TryGetCharacterHornAverageInternal(target, average, reason) &&
        average >= kHornCutOffThreshold) {
      it = g_pendingHornCutReapplies.erase(it);
      continue;
    }

    AppearanceBase *appearance = nullptr;
    GameData *appearanceData = nullptr;
    std::string appearanceReason = "";
    if (!TryGetCharacterAppearanceData(target, appearance, appearanceData,
                                       appearanceReason)) {
      state.nextReapplyTick = nowTick + 500;
      --state.remainingAttempts;
      ++it;
      continue;
    }

    std::string persistenceReason = "";
    const bool persistenceReady =
        PromoteCharacterHornPersistence(target, persistenceReason);
    try {
      std::string cloneReason = "";
      GameDataCopyStandalone *sourceData =
          AssignClonedAppearanceData(target, appearance, appearanceData,
                                     cloneReason);
      GameData *mutableData =
          (sourceData && (uintptr_t)sourceData > 0x1000)
              ? static_cast<GameData *>(sourceData)
              : appearanceData;
      ApplyHornCutOffValues(mutableData);
      ValidateAppearanceDataForCharacter(mutableData, target, appearance);
      appearance->updatedAppearanceData = true;
      appearance->updateBody = true;
      RefreshCharacterAppearance(target);
      if (persistenceReady) {
        std::string commitReason = "";
        PromoteCharacterHornPersistence(target, commitReason);
      }
    } catch (...) {
    }

    state.nextReapplyTick = nowTick + 350;
    --state.remainingAttempts;
    ++it;
  }
}

void UpdatePendingHornCutDismissals(GameWorld *world) {
  DWORD nowTick = GetTickCount();
  for (auto it = g_pendingHornCutDismissals.begin();
       it != g_pendingHornCutDismissals.end();) {
    PendingHornCutDismissState &state = it->second;
    if (state.serial == 0 || state.remainingChecks <= 0 ||
        (state.expireTick != 0 && nowTick >= state.expireTick)) {
      it = g_pendingHornCutDismissals.erase(it);
      continue;
    }
    if (state.nextCheckTick != 0 && nowTick < state.nextCheckTick) {
      ++it;
      continue;
    }

    Character *target = nullptr;
    try {
      if (state.target.isValid()) {
        target = state.target.getCharacter();
      }
    } catch (...) {
      target = nullptr;
    }
    if ((!target || (uintptr_t)target <= 0x1000) && world) {
      target = ResolveCharacterBySerialFromWorld(world, state.serial);
    }
    if (!target || (uintptr_t)target <= 0x1000) {
      it = g_pendingHornCutDismissals.erase(it);
      continue;
    }

    if (!IsInPlayerFactionSafe(target) && !IsInPlayerRoster(world, target)) {
      it = g_pendingHornCutDismissals.erase(it);
      continue;
    }

    float average = 0.0f;
    std::string averageReason = "";
    if (!TryGetCharacterHornAverageInternal(target, average, averageReason) ||
        average < kHornCutOffThreshold) {
      state.nextCheckTick = nowTick + 400;
      --state.remainingChecks;
      ++it;
      continue;
    }

    std::string dismissReason = "";
    const bool dismissed =
        TryDismissCharacterToOrigin(world, target, state.originFactionToken,
                                    dismissReason);
    if (dismissed) {
      PushImmediateHornContextSnapshot(target, "horn_cut_leave");
      it = g_pendingHornCutDismissals.erase(it);
      continue;
    }

    state.nextCheckTick = nowTick + 500;
    --state.remainingChecks;
    ++it;
  }
}

std::string ToggleActionLabel(const std::string &command) {
  if (command == "SET_BLOCK") {
    return "block";
  }
  if (command == "SET_HOLD") {
    return "hold";
  }
  if (command == "SET_PASSIVE") {
    return "passive";
  }
  if (command == "SET_RANGED") {
    return "ranged";
  }
  if (command == "SET_TAUNT") {
    return "taunt";
  }
  if (command == "SET_SNEAK") {
    return "sneak";
  }
  if (command == "SET_JOBS") {
    return "jobs";
  }
  if (command == "SET_RESOURCE") {
    return "resource";
  }
  if (command == "SET_MEDIC") {
    return "medic";
  }
  return "toggle";
}

void ExecuteQueuedActions(GameWorld *thisptr, int &inventoryTimer) {
  static bool holdForTtsPlayback = false;
  static hand activeSpeechTarget;
  static unsigned int activeSpeechTargetSerial = 0;
  static DWORD holdPlaybackLogTick = 0;
  static DWORD pausedQueueLogTick = 0;
  static double speechDelayRemainingScaledMs = 0.0;
  static DWORD speechDelayLastTick = 0;

  UpdateNarratorTimedPopupLifecycle();

  UpdateNpcDrunkStates(thisptr);
  UpdateNpcSustainedKnockoutStates(thisptr);
  UpdateNpcDrugStates(thisptr);
  UpdatePendingHornCutReapplies(thisptr);
  UpdatePendingHornCutDismissals(thisptr);

  if (TryEnterCriticalSection(&g_uiMutex)) {
    bool gamePaused = false;
    float gameSpeedMultiplier = ResolveDialogueGameSpeedMultiplier(thisptr);
    try {
      gamePaused = (thisptr && thisptr->isPaused());
    } catch (...) {
      gamePaused = false;
    }
    if (gamePaused) {
      gameSpeedMultiplier = 1.0f;
    }

    DWORD nowTick = GetTickCount();
    if (g_nextSpeechActionTick == 0) {
      speechDelayRemainingScaledMs = 0.0;
      speechDelayLastTick = 0;
    } else {
      if (speechDelayRemainingScaledMs <= 0.0) {
        if (g_nextSpeechActionTick > nowTick) {
          speechDelayRemainingScaledMs =
              static_cast<double>(g_nextSpeechActionTick - nowTick);
          speechDelayLastTick = nowTick;
        } else {
          g_nextSpeechActionTick = 0;
          speechDelayRemainingScaledMs = 0.0;
          speechDelayLastTick = 0;
        }
      }
      if (!gamePaused && g_nextSpeechActionTick != 0 &&
          speechDelayRemainingScaledMs > 0.0) {
        if (speechDelayLastTick == 0 || speechDelayLastTick > nowTick) {
          speechDelayLastTick = nowTick;
        }
        DWORD elapsedMs = nowTick - speechDelayLastTick;
        if (elapsedMs > 0) {
          speechDelayRemainingScaledMs -=
              static_cast<double>(elapsedMs) *
              static_cast<double>(gameSpeedMultiplier);
          speechDelayLastTick = nowTick;
        }
        if (speechDelayRemainingScaledMs <= 0.0) {
          g_nextSpeechActionTick = 0;
          speechDelayRemainingScaledMs = 0.0;
          speechDelayLastTick = 0;
        } else {
          g_nextSpeechActionTick =
              nowTick + ResolveSpeechQueueRemainingRealMs(
                            speechDelayRemainingScaledMs, gameSpeedMultiplier);
        }
      }
    }

    if (gamePaused) {
      if (!g_uiActionQueue.empty() && nowTick - pausedQueueLogTick >= 1500) {
        pausedQueueLogTick = nowTick;
        const QueuedAction &nextAction = g_uiActionQueue.front();
        Log("ACTION_QUEUE: paused; deferring queued actions size=" +
            ToString((int)g_uiActionQueue.size()) +
            " next_type=" + ToString((int)nextAction.type));
      }
      LeaveCriticalSection(&g_uiMutex);
      return;
    }

    if (holdForTtsPlayback) {
      Character *holdTarget = nullptr;
      if (activeSpeechTargetSerial != 0) {
        holdTarget = ResolveLiveCharacter(thisptr, activeSpeechTarget);
      }
      if (holdTarget && IsCharacterUnavailableForDialogue(holdTarget)) {
        Log("ACTION_TIMING: hold cancelled; target unavailable target=" +
            SafeCharacterName(holdTarget));
        ClearCharacterSpeechBubble(holdTarget);
        InterruptTtsPlayback();
      } else if (holdTarget && holdTarget->dialogue &&
                 (uintptr_t)holdTarget->dialogue > 0x1000) {
        float speed = ResolveDialogueGameSpeedMultiplier(thisptr);
        float keepAlive = 0.45f * speed;
        if (holdTarget->dialogue->speechTextTimer < keepAlive) {
          holdTarget->dialogue->speechTextTimer = keepAlive;
        }
        if (holdTarget->dialogue->speechTextTimer_forced < keepAlive) {
          holdTarget->dialogue->speechTextTimer_forced = keepAlive;
        }
      }
      if (IsTtsPlaybackActive()) {
        DWORD nowTick = GetTickCount();
        if (nowTick - holdPlaybackLogTick >= 1000) {
          holdPlaybackLogTick = nowTick;
          int remainingMs = GetTtsPlaybackRemainingMs();
          Log("ACTION_TIMING: hold active remaining_ms=" +
              ToString(remainingMs) + " target=" +
              (holdTarget ? holdTarget->getName() : "Unknown"));
        }
        LeaveCriticalSection(&g_uiMutex);
        return;
      }
      Log("ACTION_TIMING: hold released, playback is no longer active");
      holdForTtsPlayback = false;
      activeSpeechTarget = hand();
      activeSpeechTargetSerial = 0;
      g_nextSpeechActionTick = 0;
      speechDelayRemainingScaledMs = 0.0;
      speechDelayLastTick = 0;
    }

    while (!g_uiActionQueue.empty()) {
      DWORD nowTick = GetTickCount();
      const QueuedAction &nextAction = g_uiActionQueue.front();
      bool nextActionIsSpeech =
          nextAction.type == ACT_SAY || nextAction.type == ACT_PLAY_TTS;
      if (nextActionIsSpeech && g_nextSpeechActionTick != 0 &&
          nowTick < g_nextSpeechActionTick) {
        break;
      }

      QueuedAction act = g_uiActionQueue.front();
      g_uiActionQueue.pop_front();
      LeaveCriticalSection(&g_uiMutex);
      bool blockSpeechQueue = false;
      DWORD speechDelayMs = 0;
      bool lockReacquired = false;
      bool deferActionQueue = false;
      bool queueDeferredAction = false;
      QueuedAction deferredAction;

      Character *npc = ResolveLiveCharacter(thisptr, act.actor);
      Character *target = ResolveLiveCharacter(thisptr, act.target);
      if (!npc && act.type != ACT_NOTIFY && act.type != ACT_SAY &&
          act.type != ACT_PLAY_TTS) {
        Log("ACTION_EXEC: Failed to resolve actor serial=" +
            ToString((int)act.actor.serial) +
            " action_type=" + ToString((int)act.type));
      }

      if (act.type == ACT_NOTIFY) {
        bool narratorPopupShown = false;
        if (IsNarratorTimedPopupMessage(act.message)) {
          narratorPopupShown =
              ShowNarratorTimedPopup(act.message, ResolveSpeechQueueDelayMs(act));
        }
        if (!narratorPopupShown) {
          thisptr->showPlayerAMessage_withLog(act.message, true);
        }
        bool hasTtsClip = g_ttsEnabled && !act.ttsHash.empty();
        bool playbackQueued = false;
        if (hasTtsClip) {
          playbackQueued =
              QueueTtsPlayback(act.ttsHash, -1, 0,
                               ResolveDialogueGameSpeedMultiplier(thisptr));
        }
        if (hasTtsClip) {
          Log("ACTION_TIMING: NOTIFY tts_hash=" + act.ttsHash.substr(0, 8) +
              " tts_dur_ms=" + ToString(act.taskValue) +
              " playbackQueued=" + std::string(playbackQueued ? "1" : "0"));
        }
        if (playbackQueued) {
          blockSpeechQueue = true;
          holdForTtsPlayback = true;
          activeSpeechTarget = hand();
          activeSpeechTargetSerial = 0;
          speechDelayMs = 250;
        } else if (act.taskValue > 0) {
          blockSpeechQueue = true;
          speechDelayMs = static_cast<DWORD>(act.taskValue) + 120;
        }
      } else if (act.type == ACT_SAY && target &&
                 !IsCharacterUnavailableForDialogue(target)) {
        bool isPC = target->isPlayerCharacter();
        float appliedBubbleDuration = 0.0f;
        bool forcedSayFallback = false;
        bool speechExecuted = false;
        Character *listenerForFacing = nullptr;
        std::string explicitTalkTargetToken = TrimCopySimple(act.targetToken);
        if (!explicitTalkTargetToken.empty()) {
          listenerForFacing = ResolveCharacterByTargetToken(
              thisptr, explicitTalkTargetToken, target);
        }
        if (!listenerForFacing) {
          listenerForFacing = ResolveSpeechListenerForFacing(target);
        }
        Log("ACTION_EXEC: SAY [" + target->getName() + "]: " + act.message +
            (isPC ? " (PC)" : " (NPC)"));
        try {
          // If npc is in vanilla dialogue state, bubbles are often suppressed.
          // Force a reset if they seem stuck.
          if (!isPC && target->dialogue && (uintptr_t)target->dialogue > 0x1000) {
            target->dialogue->endDialogue(true);
            target->dialogue->setInDialog(false);
          }
          if (listenerForFacing) {
            TryFaceSpeakerTowardListenerForSpeech(target, listenerForFacing);
          }

          // Primary method: sayALine (supports multiple lines/delays)
          target->sayALine(act.message, !isPC);
          speechExecuted = true;
          if (listenerForFacing) {
            TryFaceSpeakerTowardListenerForSpeech(target, listenerForFacing);
          }
          // Force native floating text path as well. This is the most reliable
          // way to surface overhead speech bubbles across player and NPC actors.
          target->say(act.message);
          forcedSayFallback = true;

          // Keep bubble lifetime aligned to the dialogue line. Queue/audio
          // pacing is scaled separately by current game speed.
          if (target->dialogue && (uintptr_t)target->dialogue > 0x1000) {
            target->dialogue->npcReplyText = act.message;
            float baseDuration = g_speechBubbleLife;
            if (g_ttsEnabled && act.ttsHash.size() == 32 && act.taskValue > 0) {
              baseDuration = (float)act.taskValue / 1000.0f + 0.20f;
            } else {
              baseDuration =
                  (float)EstimateSpeechDurationMs(act.message) / 1000.0f + 0.20f;
            }
            if (baseDuration < 1.0f) {
              baseDuration = 1.0f;
            }
            float duration = baseDuration;
            target->dialogue->speechTextTimer = duration;
            target->dialogue->speechTextTimer_forced = duration;
            appliedBubbleDuration = duration;
            bool timerActive = target->dialogue->speechTextTimer > 0.05f ||
                               target->dialogue->speechTextTimer_forced > 0.05f;
            bool hasReplyText = !target->dialogue->npcReplyText.empty();
            if ((!timerActive || !hasReplyText) && !forcedSayFallback) {
              target->say(act.message);
              forcedSayFallback = true;
            }
          } else {
            // Secondary fallback: say (force floating text bubble)
            // ONLY if dialogue system failed to initialize for this character
            target->say(act.message);
            forcedSayFallback = true;
          }

        } catch (...) {
          Log("ACTION_EXEC: SAY (ERROR): Exception during sayALine/say");
        }

        bool hasTtsClip = g_ttsEnabled && !act.ttsHash.empty();
        bool playbackQueued = false;
        float ttsCameraDistance = -1.0f;
        bool ttsSpeakerLoaded = false;
        bool ttsAttenuated = false;
        int ttsVolumePercent = ClampTtsVolumePercent(g_ttsVolumePercent);
        std::string ttsSkipReason = "";
        if (hasTtsClip) {
          ttsVolumePercent =
              ResolveTtsPlaybackVolumePercent(thisptr, target, ttsCameraDistance,
                                              ttsSpeakerLoaded, ttsAttenuated);
          if (ttsVolumePercent > 0) {
            playbackQueued =
                QueueTtsPlayback(act.ttsHash, ttsVolumePercent,
                                 act.target.serial,
                                 ResolveDialogueGameSpeedMultiplier(thisptr));
          } else {
            ttsSkipReason =
                ttsSpeakerLoaded ? "camera_out_of_range" : "speaker_not_loaded";
            Log("TTS_PLAYBACK: skip queue hash=" + act.ttsHash.substr(0, 8) +
                " reason=" + ttsSkipReason +
                " target=" + SafeCharacterName(target) +
                " target_serial=" + ToString((int)act.target.serial) +
                " camera_dist=" + ToString(ttsCameraDistance));
          }
        }
        Log("ACTION_TIMING: SAY target=" + target->getName() +
            " msg_len=" + ToString((int)act.message.length()) +
            " tts_hash=" + (hasTtsClip ? act.ttsHash.substr(0, 8) : "") +
            " tts_dur_ms=" + ToString(act.taskValue) +
            " tts_vol_pct=" + ToString(ttsVolumePercent) +
            " tts_cam_dist=" + ToString(ttsCameraDistance) +
            " tts_speaker_loaded=" +
            std::string(ttsSpeakerLoaded ? "1" : "0") +
            " tts_attenuated=" + std::string(ttsAttenuated ? "1" : "0") +
            " tts_skip_reason=" + ttsSkipReason +
            " bubble_ms=" + ToString((int)(appliedBubbleDuration * 1000.0f)) +
            " est_ms=" + ToString((int)EstimateSpeechDurationMs(act.message)) +
            " tts_enabled=" + std::string(g_ttsEnabled ? "1" : "0") +
            " playbackQueued=" + std::string(playbackQueued ? "1" : "0") +
            " forced_say_fallback=" +
            std::string(forcedSayFallback ? "1" : "0"));
        blockSpeechQueue = true;
        if (playbackQueued) {
          holdForTtsPlayback = true;
          activeSpeechTarget = act.target;
          activeSpeechTargetSerial = act.target.serial;
          speechDelayMs = 250;
        } else {
          if (isPC && !hasTtsClip && act.taskValue < 0) {
            // PLAYER_TTS arrives asynchronously after PLAYER_SAY; keep queue
            // delay short so playback can start quickly once hash resolves.
            // Non-player squad speech should use normal pacing.
            speechDelayMs = 250;
          } else {
            speechDelayMs = ResolveSpeechQueueDelayMs(act);
          }
        }
        if (speechExecuted) {
          if (!act.utteranceId.empty()) {
            PostSpeechDeliveryState(act.utteranceId, "spoken");
          }
        } else if (!act.utteranceId.empty()) {
          PostSpeechDeliveryState(act.utteranceId, "cancelled");
        }
      } else if (act.type == ACT_PLAY_TTS && target &&
                 !IsCharacterUnavailableForDialogue(target)) {
        float appliedBubbleDuration = 0.0f;
        Character *listenerForFacing = nullptr;
        std::string explicitTalkTargetToken = TrimCopySimple(act.targetToken);
        if (!explicitTalkTargetToken.empty()) {
          listenerForFacing = ResolveCharacterByTargetToken(
              thisptr, explicitTalkTargetToken, target);
        }
        if (!listenerForFacing) {
          listenerForFacing = ResolveSpeechListenerForFacing(target);
        }
        if (listenerForFacing) {
          TryFaceSpeakerTowardListenerForSpeech(target, listenerForFacing);
        }
        if (target->dialogue && (uintptr_t)target->dialogue > 0x1000) {
          float baseDuration = g_speechBubbleLife;
          if (g_ttsEnabled && !act.ttsHash.empty() && act.taskValue > 0) {
            baseDuration = (float)act.taskValue / 1000.0f + 0.20f;
          } else {
            baseDuration =
                (float)EstimateSpeechDurationMs(act.message) / 1000.0f + 0.20f;
          }
          if (baseDuration < 1.0f) {
            baseDuration = 1.0f;
          }
          float duration = baseDuration;
          target->dialogue->speechTextTimer = duration;
          target->dialogue->speechTextTimer_forced = duration;
          appliedBubbleDuration = duration;
        }
        bool hasTtsClip = g_ttsEnabled && !act.ttsHash.empty();
        bool playbackQueued = false;
        float ttsCameraDistance = -1.0f;
        bool ttsSpeakerLoaded = false;
        bool ttsAttenuated = false;
        int ttsVolumePercent = ClampTtsVolumePercent(g_ttsVolumePercent);
        std::string ttsSkipReason = "";
        if (hasTtsClip) {
          ttsVolumePercent =
              ResolveTtsPlaybackVolumePercent(thisptr, target, ttsCameraDistance,
                                              ttsSpeakerLoaded, ttsAttenuated);
          if (ttsVolumePercent > 0) {
            playbackQueued =
                QueueTtsPlayback(act.ttsHash, ttsVolumePercent,
                                 act.target.serial,
                                 ResolveDialogueGameSpeedMultiplier(thisptr));
          } else {
            ttsSkipReason =
                ttsSpeakerLoaded ? "camera_out_of_range" : "speaker_not_loaded";
            Log("TTS_PLAYBACK: skip queue hash=" + act.ttsHash.substr(0, 8) +
                " reason=" + ttsSkipReason +
                " target=" + SafeCharacterName(target) +
                " target_serial=" + ToString((int)act.target.serial) +
                " camera_dist=" + ToString(ttsCameraDistance));
          }
        }
        Log("ACTION_TIMING: PLAY_TTS target=" + target->getName() +
            " tts_hash=" + (hasTtsClip ? act.ttsHash.substr(0, 8) : "") +
            " tts_dur_ms=" + ToString(act.taskValue) +
            " tts_vol_pct=" + ToString(ttsVolumePercent) +
            " tts_cam_dist=" + ToString(ttsCameraDistance) +
            " tts_speaker_loaded=" +
            std::string(ttsSpeakerLoaded ? "1" : "0") +
            " tts_attenuated=" + std::string(ttsAttenuated ? "1" : "0") +
            " tts_skip_reason=" + ttsSkipReason +
            " bubble_ms=" + ToString((int)(appliedBubbleDuration * 1000.0f)) +
            " est_ms=" + ToString((int)EstimateSpeechDurationMs(act.message)) +
            " tts_enabled=" + std::string(g_ttsEnabled ? "1" : "0") +
            " playbackQueued=" + std::string(playbackQueued ? "1" : "0"));
        blockSpeechQueue = true;
        if (playbackQueued) {
          holdForTtsPlayback = true;
          activeSpeechTarget = act.target;
          activeSpeechTargetSerial = act.target.serial;
          speechDelayMs = 250;
        } else {
          speechDelayMs = ResolveSpeechQueueDelayMs(act);
        }
      } else if ((act.type == ACT_SAY || act.type == ACT_PLAY_TTS) && target) {
        Log("ACTION_EXEC: Skipping speech for unavailable target " +
            SafeCharacterName(target) + " action_type=" + ToString((int)act.type));
        ClearCharacterSpeechBubble(target);
        if (act.type == ACT_SAY && !act.utteranceId.empty()) {
          PostSpeechDeliveryState(act.utteranceId, "cancelled");
        }
      } else if (act.type == ACT_SAY || act.type == ACT_PLAY_TTS) {
        Log("ACTION_EXEC: Skipping speech with unresolved target action_type=" +
            ToString((int)act.type));
        if (act.type == ACT_SAY && !act.utteranceId.empty()) {
          PostSpeechDeliveryState(act.utteranceId, "cancelled");
        }
      } else if (npc) {
        if (act.type != ACT_SUICIDE && IsCharacterUnavailableForDialogue(npc)) {
          Log("ACTION_EXEC: Skipping action for unavailable actor " +
              SafeCharacterName(npc) + " action_type=" + ToString((int)act.type));
        } else if (act.type == ACT_SUICIDE) {
          std::string npcName = SafeCharacterName(npc);
          LONG generation = BeginChatInterruptGeneration();
          ClearCharacterSpeechBubble(npc);
          bool alreadyDead = false;
          bool killed = false;
          try {
            alreadyDead = npc->isDead();
          } catch (...) {
            alreadyDead = false;
          }
          if (!alreadyDead) {
            try {
              npc->declareDead();
              killed = true;
            } catch (...) {
              killed = false;
            }
          }
          Log("ACTION_EXEC: SUICIDE npc=" + npcName +
              " generation=" + ToString((int)generation) +
              " killed=" + (killed ? "1" : "0") +
              " already_dead=" + (alreadyDead ? "1" : "0"));
          thisptr->showPlayerAMessage_withLog(
              npcName + (killed || alreadyDead ? " died." : " suicide failed."),
              true);
        } else if (act.type == ACT_ATTACK && target) {
          if (npc->getFaction() && npc->getFaction()->isThePlayer()) {
            bool targetInPlayerFaction = false;
            try {
              targetInPlayerFaction =
                  (target->getFaction() && target->getFaction()->isThePlayer());
            } catch (...) {
              targetInPlayerFaction = false;
            }

            // Only leave player squad when explicitly attacking someone
            // in the current player faction.
            if (targetInPlayerFaction) {
              PerformLeaveSquad(npc, thisptr, "");
            }

            // Clear existing goals so the attack command can take over.
            npc->clearAllAIGoals();
          }
          npc->attackTarget(target);
          npc->addGoal(MELEE_ATTACK, (RootObjectBase *)target);
          npc->reThinkCurrentAIAction();
          thisptr->showPlayerAMessage(npc->getName() + " is attacking!", false);
        } else if (act.type == ACT_JOIN_PARTY && thisptr->player) {
          std::string npcName = SafeCharacterName(npc);
          bool inPlayerFactionBefore = IsInPlayerFactionSafe(npc);
          bool inPlayerRosterBefore = IsInPlayerRoster(thisptr, npc);
          bool canTakeOrdersBefore = false;
          try {
            canTakeOrdersBefore = npc->canTakePlayerOrdersAtThisTime();
          } catch (...) {
            canTakeOrdersBefore = false;
          }
          Log("ACTION_EXEC: JOIN_PARTY request npc=" + npcName +
              " serial=" + ToString((int)act.actor.serial) +
              " in_faction_before=" +
              std::string(inPlayerFactionBefore ? "1" : "0") +
              " in_roster_before=" +
              std::string(inPlayerRosterBefore ? "1" : "0") +
              " can_take_orders_before=" +
              std::string(canTakeOrdersBefore ? "1" : "0"));

          if (inPlayerFactionBefore && inPlayerRosterBefore) {
            thisptr->showPlayerAMessage_withLog(
                npcName + " is already in your squad.", true);
          } else {
            bool recruitNormalOk = false;
            try {
              recruitNormalOk = thisptr->player->recruit(npc, false);
            } catch (...) {
              recruitNormalOk = false;
            }

            bool joinedFaction = IsInPlayerFactionSafe(npc);
            bool joinedRoster = IsInPlayerRoster(thisptr, npc);
            bool fallbackJoinOk = false;
            if (!(joinedFaction && joinedRoster)) {
              fallbackJoinOk = ForceJoinPlayerSquad(thisptr, npc);
              joinedFaction = IsInPlayerFactionSafe(npc);
              joinedRoster = IsInPlayerRoster(thisptr, npc);
            }

            bool canTakeOrdersAfter = false;
            try {
              canTakeOrdersAfter = npc->canTakePlayerOrdersAtThisTime();
            } catch (...) {
              canTakeOrdersAfter = false;
            }
            bool joined = joinedFaction || joinedRoster || canTakeOrdersAfter;
            if (joined && !joinedRoster) {
              try {
                thisptr->player->recruit(npc, false);
              } catch (...) {
              }
              joinedRoster = IsInPlayerRoster(thisptr, npc);
              joined = joinedFaction || joinedRoster || canTakeOrdersAfter;
            }
            try {
              thisptr->player->setCharacterEditMode(false);
            } catch (...) {
            }
            Log("ACTION_EXEC: JOIN_PARTY result npc=" + npcName +
                " serial=" + ToString((int)act.actor.serial) +
                " recruit_normal=" + (recruitNormalOk ? "1" : "0") +
                " fallback=" + (fallbackJoinOk ? "1" : "0") +
                " joined_faction=" + (joinedFaction ? "1" : "0") +
                " joined_roster=" + (joinedRoster ? "1" : "0") +
                " can_take_orders_after=" +
                (canTakeOrdersAfter ? "1" : "0"));

            if (joined) {
              thisptr->playNotification("ui_cat_change");
            }
            thisptr->showPlayerAMessage_withLog(
                npcName +
                    (joined ? " joined your squad." : " failed to join your squad."),
                true);
          }
        } else if (act.type == ACT_SET_NPC_TOGGLE) {
          const bool enabled = act.taskValue != 0;
          const std::string command = ToUpperCopy(TrimCopySimple(act.message));
          unsigned int liveActorSerial = 0;
          if (!IsQueuedActorReferenceValid(npc, act.actor, liveActorSerial)) {
            Log("ACTION_EXEC: Toggle skipped due stale actor reference command=" +
                command + " queued_serial=" +
                ToString((unsigned int)act.actor.serial) + " live_serial=" +
                ToString(liveActorSerial));
          } else {
            bool handled = true;
            bool applied = false;
            const std::string npcName = SafeCharacterName(npc);
            bool canTakeOrders = false;
            bool isPlayerCharacter = false;
            bool playerFaction = false;
            try {
              canTakeOrders = npc->canTakePlayerOrdersAtThisTime();
            } catch (...) {
              canTakeOrders = false;
            }
            try {
              isPlayerCharacter = npc->isPlayerCharacter();
            } catch (...) {
              isPlayerCharacter = false;
            }
            try {
              Faction *npcFaction = npc->getFaction();
              playerFaction = (npcFaction && npcFaction->isThePlayer());
            } catch (...) {
              playerFaction = false;
            }
            Log("ACTION_EXEC: Toggle request command=" + command +
                " enabled=" + (enabled ? "1" : "0") + " npc=" + npcName +
                " serial=" + ToString((unsigned int)act.actor.serial) +
                " can_take_orders=" + (canTakeOrders ? "1" : "0") +
                " is_player_char=" + (isPlayerCharacter ? "1" : "0") +
                " player_faction=" + (playerFaction ? "1" : "0"));
            if (command == "SET_BLOCK") {
              // Prefer classic DEF stance first; then defensive-combat toggle.
              // Some NPC states/mod stacks behave more safely with DEF.
              bool baseApplied = TrySetStandingOrderSafe(
                  npc, MessageForB::M_SET_ORDER_DEF, enabled, "SET_BLOCK_DEF");
              bool defensiveApplied = TrySetStandingOrderSafe(
                  npc, MessageForB::M_SET_ORDER_DEFENSIVE_COMBAT, enabled,
                  "SET_BLOCK_DEFENSIVE_COMBAT");
              applied = baseApplied || defensiveApplied;
            } else if (command == "SET_HOLD") {
              applied = TrySetStandingOrderSafe(
                  npc, (MessageForB::StandingOrder)12 /* HOLD */, enabled,
                  "SET_HOLD");
            } else if (command == "SET_PASSIVE") {
              applied = TrySetStandingOrderSafe(
                  npc, (MessageForB::StandingOrder)13 /* PASSIVE */, enabled,
                  "SET_PASSIVE");
            } else if (command == "SET_RANGED") {
              applied = TrySetStandingOrderSafe(
                  npc, MessageForB::M_SET_ORDER_RANGED, enabled, "SET_RANGED");
            } else if (command == "SET_TAUNT") {
              applied = TrySetStandingOrderSafe(
                  npc, MessageForB::M_SET_ORDER_TAUNT, enabled, "SET_TAUNT");
            } else if (command == "SET_SNEAK") {
              applied = TrySetStealthModeSafe(npc, enabled, "SET_SNEAK");
            } else if (command == "SET_JOBS") {
              applied = TryApplyJobsToggleSafe(npc, enabled, "SET_JOBS");
            } else if (command == "SET_RESOURCE") {
              applied = TryApplyResourceToggleSafe(npc, enabled, "SET_RESOURCE");
            } else if (command == "SET_MEDIC") {
              applied = TryApplyMedicToggleSafe(npc, enabled, "SET_MEDIC");
            } else {
              handled = false;
            }

            if (!handled) {
              Log("ACTION_EXEC: Unknown NPC toggle command: " + command);
            } else {
              if (applied) {
                try {
                  npc->reThinkCurrentAIAction();
                } catch (...) {
                  Log("ACTION_EXEC: reThinkCurrentAIAction exception after toggle "
                      "for " +
                      npcName);
                }
              } else {
                Log("ACTION_EXEC: Toggle apply failed command=" + command +
                    " npc=" + npcName + " serial=" +
                    ToString((unsigned int)act.actor.serial));
              }
              const std::string label = ToggleActionLabel(command);
              const std::string stateText = enabled ? "ON" : "OFF";
              Log("ACTION_EXEC: Toggle " + label + "=" + stateText + " for " +
                  npcName + " applied=" + (applied ? "1" : "0"));
              thisptr->showPlayerAMessage_withLog(
                  npcName + " set " + label + " " + stateText +
                      (applied ? "." : " (failed)."),
                  true);
            }
          }
        } else if (act.type == ACT_LEAVE) {
          bool inPlayerFaction = false;
          try {
            Faction *npcFaction = npc->getFaction();
            inPlayerFaction = (npcFaction && npcFaction->isThePlayer());
          } catch (...) {
            inPlayerFaction = false;
          }
          if (!inPlayerFaction) {
            thisptr->showPlayerAMessage_withLog(
                npc->getName() + " is not in your squad.", true);
          } else {
            // Clear dialogue state to ensure they don't stay frozen
            if (npc->dialogue && (uintptr_t)npc->dialogue > 0x1000) {
              npc->dialogue->endDialogue(true);
              npc->dialogue->setInDialog(false);
            }

            npc->clearPermajobs();
            npc->clearAllAIGoals();
            PerformLeaveSquad(npc, thisptr, act.message);

            // Clear limiting orders (Passive/Hold) that might prevent movement
            npc->setStandingOrder((MessageForB::StandingOrder)13 /* PASSIVE */,
                                  false);
            npc->setStandingOrder((MessageForB::StandingOrder)12 /* HOLD */,
                                  false);

            if (npc->getPermajobCount() == 0) {
              TownBase *town = npc->getCurrentTownLocation();
              if (town) {
                npc->addJob(WANDER_TOWN, (RootObject *)town, false, false,
                            npc->getPosition());
                npc->addGoal(WANDER_TOWN, (RootObjectBase *)town);
              } else {
                npc->addJob(WANDERER, NULL, false, false, npc->getPosition());
                npc->addGoal(WANDERER, NULL);
              }
            }
            npc->reThinkCurrentAIAction();
            thisptr->showPlayerAMessage_withLog(
                npc->getName() + " left your squad.", true);
          }

        } else if (act.type == ACT_START_FOLLOW) {
          unsigned int followerSerial = 0;
          try {
            followerSerial = npc->getHandle().serial;
          } catch (...) {
            followerSerial = 0;
          }
          if (target && followerSerial != 0) {
            // Hard-follow mode: clear patrol/package behavior so follow isn't
            // immediately overridden by background AI.
            ClearAllJobsForFollow(npc);
            try {
              npc->clearAllAIGoals();
            } catch (...) {
            }
            try {
              npc->addGoal(STAY_CLOSE_TO_TARGET, (RootObjectBase *)target);
            } catch (...) {
            }
            try {
              npc->setStandingOrder((MessageForB::StandingOrder)13 /* PASSIVE */,
                                    false);
              npc->setStandingOrder((MessageForB::StandingOrder)12 /* HOLD */,
                                    false);
            } catch (...) {
            }
            SetFollowTarget(followerSerial, act.target);
            ClearTravelTarget(followerSerial);
            try {
              npc->setDestination(target->getPosition(), false);
            } catch (...) {
            }
            try {
              CharMovement *movement = npc->getMovement();
              if (movement && (uintptr_t)movement > 0x1000) {
                movement->setDesiredSpeedOrders(RUN);
                movement->setDesiredSpeed(RUN);
              }
            } catch (...) {
            }
            npc->reThinkCurrentAIAction();
            thisptr->showPlayerAMessage_withLog(
                npc->getName() + " is following " + target->getName() + ".",
                true);
            Log("ACTION_EXEC: FOLLOW assigned " + npc->getName() + " -> " +
                target->getName());
          } else {
            thisptr->showPlayerAMessage_withLog(
                npc->getName() + " follow failed (target not found).", true);
            Log("ACTION_EXEC: FOLLOW failed for " + npc->getName() +
                " target missing");
          }

        } else if (act.type == ACT_STOP_FOLLOW) {
          try {
            ClearFollowTarget(npc->getHandle().serial);
          } catch (...) {
          }
          // Re-enable jobs behavior after follow lock is released.
          TryApplyJobsToggleSafe(npc, true, "STOP_FOLLOW_RESTORE_JOBS");
          try {
            CharMovement *movement = npc->getMovement();
            if (movement && (uintptr_t)movement > 0x1000) {
              movement->restoreDesiredSpeed();
            }
          } catch (...) {
          }
          npc->clearAllAIGoals();
          npc->reThinkCurrentAIAction();
          thisptr->showPlayerAMessage_withLog(
              npc->getName() + " stopped following.", true);
          Log("ACTION_EXEC: STOP_FOLLOW for " + npc->getName());

        } else if (act.type == ACT_TRAVEL_LOCATION) {
          float travelX = 0.0f;
          float travelY = 0.0f;
          float travelZ = 0.0f;
          std::string destinationLabel = "";
          bool payloadOk = ParseTravelLocationPayload(
              act.message, travelX, travelY, travelZ, destinationLabel);
          if (!payloadOk) {
            std::string requestedLabel = TrimCopySimple(act.message);
            if (requestedLabel.empty()) {
              requestedLabel = "that location";
            }
            thisptr->showPlayerAMessage_withLog(
                "Can not travel to " + requestedLabel +
                    " as you have not visited it yet",
                true);
            Log("ACTION_EXEC: TRAVEL_LOCATION invalid payload actor=" +
                npc->getName() + " payload='" + act.message + "'");
          } else {
            if (destinationLabel.empty()) {
              destinationLabel = "destination";
            }
            unsigned int actorSerial = 0;
            try {
              actorSerial = npc->getHandle().serial;
            } catch (...) {
              actorSerial = 0;
            }

            // If dialogue lock is still active, movement orders can be ignored.
            try {
              if (npc->dialogue && (uintptr_t)npc->dialogue > 0x1000) {
                npc->dialogue->endDialogue(true);
                npc->dialogue->setInDialog(false);
              }
            } catch (...) {
            }

            try {
              ClearFollowTarget(actorSerial);
            } catch (...) {
            }

            // Clear background job/task stacks so they do not immediately
            // override the travel destination.
            ClearAllJobsForFollow(npc);
            TryApplyJobsToggleSafe(npc, false, "TRAVEL_LOCATION_DISABLE_JOBS");
            if (actorSerial != 0) {
              SetTravelTarget(actorSerial, travelX, travelY, travelZ,
                              destinationLabel);
            }

            bool passiveCleared = TrySetStandingOrderSafe(
                npc, (MessageForB::StandingOrder)13 /* PASSIVE */, false,
                "TRAVEL_LOCATION_CLEAR_PASSIVE");
            bool holdCleared = TrySetStandingOrderSafe(
                npc, (MessageForB::StandingOrder)12 /* HOLD */, false,
                "TRAVEL_LOCATION_CLEAR_HOLD");

            try {
              npc->clearAllAIGoals();
            } catch (...) {
            }

            Ogre::Vector3 destination(travelX, travelY, travelZ);
            bool moved = false;
            CharMovement *movement = nullptr;
            try {
              movement = npc->getMovement();
              if (movement && (uintptr_t)movement > 0x1000) {
                movement->setDesiredSpeedOrders(RUN);
                movement->setDesiredSpeed(RUN);
              }
            } catch (...) {
              movement = nullptr;
            }
            try {
              if (movement && (uintptr_t)movement > 0x1000) {
                movement->setDestination(destination, HIGH_PRIORITY, false);
                moved = true;
              }
            } catch (...) {
              moved = false;
            }
            if (!moved) {
              try {
                if (movement && (uintptr_t)movement > 0x1000) {
                  movement->setDestination(destination, HIGH_PRIORITY, true);
                  moved = true;
                }
              } catch (...) {
                moved = false;
              }
            }
            if (!moved) {
              try {
                if (movement && (uintptr_t)movement > 0x1000) {
                  moved = movement->setRoadDestination(destination);
                }
              } catch (...) {
                moved = false;
              }
            }
            if (!moved) {
              try {
                npc->setDestination(destination, false);
                moved = true;
              } catch (...) {
                moved = false;
              }
            }
            if (!moved) {
              try {
                npc->setDestination(destination, true);
                moved = true;
              } catch (...) {
                moved = false;
              }
            }

            if (moved) {
              try {
                npc->reThinkCurrentAIAction();
              } catch (...) {
              }
              thisptr->showPlayerAMessage_withLog(
                  npc->getName() + " is traveling to " + destinationLabel + ".",
                  true);
              Log("ACTION_EXEC: TRAVEL_LOCATION actor=" + npc->getName() +
                  " destination='" + destinationLabel + "' x=" +
                  ToString((int)travelX) + " y=" + ToString((int)travelY) +
                  " z=" + ToString((int)travelZ) +
                  " serial=" + ToString(actorSerial) +
                  " hold_cleared=" + (holdCleared ? "1" : "0") +
                  " passive_cleared=" + (passiveCleared ? "1" : "0"));
            } else {
              ClearTravelTarget(actorSerial);
              thisptr->showPlayerAMessage_withLog(
                  npc->getName() + " could not start traveling to " +
                      destinationLabel + ".",
                  true);
              Log("ACTION_EXEC: TRAVEL_LOCATION setDestination failed actor=" +
                  npc->getName() + " destination='" + destinationLabel + "'");
            }
          }

        } else if (act.type == ACT_SET_TASK) {
          unsigned int liveActorSerial = 0;
          if (!IsQueuedActorReferenceValid(npc, act.actor, liveActorSerial)) {
            Log("ACTION_EXEC: SET_TASK skipped due stale actor reference task=" +
                ToString(act.taskValue) + " queued_serial=" +
                ToString((unsigned int)act.actor.serial) + " live_serial=" +
                ToString(liveActorSerial));
          } else {
            Log("ACTION_EXEC: Setting task for " + npc->getName() + ": " +
                ToString(act.taskValue) +
                (target ? " (Target: " + target->getName() + ")" : ""));

            // Force end dialogue if they were talking
            if (npc->dialogue && (uintptr_t)npc->dialogue > 0x1000) {
              npc->dialogue->endDialogue(true);
              npc->dialogue->setInDialog(false);
            }

            // Clear limiting orders (Passive/Hold) that might prevent task
            // execution Matches enum values in MessageForB::StandingOrder
            npc->setStandingOrder((MessageForB::StandingOrder)13 /* PASSIVE */,
                                  false);
            npc->setStandingOrder((MessageForB::StandingOrder)12 /* HOLD */,
                                  false);

            npc->clearAllAIGoals();

            TaskType tt = (TaskType)act.taskValue;
            RootObject *taskTarget = (RootObject *)target;
            std::string originalTaskText = ToString((int)tt);

            // SPECIAL HANDLING: If told to patrol/wander town, ensure use town
            // target not player target
            if (tt == PATROL_TOWN || tt == WANDER_TOWN) {
              TownBase *town = npc->getCurrentTownLocation();
              if (town) {
                taskTarget = (RootObject *)town;
              } else {
                // PATROL/WANDER town without a valid town target has been a
                // crash-prone path in live gameplay. Fall back to a safe
                // target-less wander task.
                Log("ACTION_EXEC: No current town for task " + originalTaskText +
                    " on " + npc->getName() + ", falling back to WANDERER");
                tt = WANDERER;
                taskTarget = NULL;
              }
            } else if (tt == IDLE || tt == WANDERER || tt == RUN_AWAY) {
              // These tasks shouldn't have the player as a target or they walk
              // into the player
              taskTarget = NULL;
            }
            if ((tt == STAY_CLOSE_TO_TARGET || tt == FOLLOW_PLAYER_ORDER ||
                 tt == FOLLOW_WHILE_TALKING) &&
                taskTarget == NULL) {
              Log("ACTION_EXEC: Follow-style task missing target for " +
                  npc->getName() + ", falling back to IDLE");
              tt = IDLE;
            }

            bool taskApplied = false;
            bool isFollowTask =
                (tt == STAY_CLOSE_TO_TARGET || tt == FOLLOW_PLAYER_ORDER ||
                 tt == FOLLOW_WHILE_TALKING);
            if (isFollowTask) {
              unsigned int followerSerial = 0;
              try {
                followerSerial = npc->getHandle().serial;
              } catch (...) {
                followerSerial = 0;
              }
              if (taskTarget && followerSerial != 0) {
                SetFollowTarget(followerSerial, act.target);
                try {
                  npc->setDestination(((Character *)taskTarget)->getPosition(),
                                      false);
                } catch (...) {
                }
                Log("ACTION_EXEC: Follow assignment enabled for " +
                    npc->getName() + " -> " + ((Character *)taskTarget)->getName());
                taskApplied = true;
              } else {
                Log("ACTION_EXEC: Follow assignment skipped for " + npc->getName() +
                    " (missing target/serial)");
              }
            } else {
              if (tt == IDLE) {
                try {
                  ClearFollowTarget(npc->getHandle().serial);
                } catch (...) {
                }
              }
              try {
                npc->addJob(tt, taskTarget, true, false, npc->getPosition());
                npc->addGoal(tt, (RootObjectBase *)taskTarget);
                taskApplied = true;
              } catch (...) {
                Log("ACTION_EXEC: Task dispatch exception for " + npc->getName() +
                    " task=" + ToString((int)tt));
              }
            }
            if (taskApplied) {
              npc->reThinkCurrentAIAction();
            }
            thisptr->showPlayerAMessage(npc->getName() + " updated their goal.",
                                        false);
            thisptr->showPlayerAMessage(npc->getName() + " updated their goal.",
                                        false);
          }
        } else if (act.type == ACT_DROP_ITEM) {
          std::vector<Item *> items;
          GetAllCharacterItems(npc, items);
          std::string targetName = act.message;
          // Cleanup quotes and whitespace
          size_t fnot = targetName.find_first_not_of(" \t\n\r\"'");
          if (fnot != std::string::npos) {
            targetName.erase(0, fnot);
            size_t lnot = targetName.find_last_not_of(" \t\n\r\"'");
            if (lnot != std::string::npos)
              targetName.erase(lnot + 1);
          }
          std::transform(targetName.begin(), targetName.end(),
                         targetName.begin(), ::tolower);

          for (uint32_t i = 0; i < items.size(); ++i) {
            std::string itemName = items[i]->getName();
            std::transform(itemName.begin(), itemName.end(), itemName.begin(),
                           ::tolower);
            if (itemName.find(targetName) != std::string::npos) {
              Log("ACTION_EXEC: Dropping item: " + items[i]->getName());
              npc->dropItem(items[i]);
              thisptr->showPlayerAMessage_withLog(
                  npc->getName() + " dropped " + items[i]->getName(), true);
              npc->reThinkCurrentAIAction();
              break;
            }
          }
        } else if (act.type == ACT_TAKE_ITEM) {
          Character *primaryPlayer =
              (thisptr->player && thisptr->player->playerCharacters.size() > 0)
                  ? thisptr->player->playerCharacters[0]
                  : nullptr;
          const std::string actorName = SafeCharacterName(npc);
          std::string explicitSourceToken = TrimCopySimple(act.targetToken);
          bool sourceTargetRequested =
              act.target.isValid() && act.target.serial != 0 &&
              (!act.actor.isValid() || act.target.serial != act.actor.serial);
          if (!sourceTargetRequested && !explicitSourceToken.empty()) {
            sourceTargetRequested = true;
          }
          bool resolvedSourceTarget =
              target && target != npc && (uintptr_t)target > 0x1000;
          bool explicitSourceTarget = sourceTargetRequested || resolvedSourceTarget;
          Character *sourceCharacter = nullptr;
          if (resolvedSourceTarget) {
            sourceCharacter = target;
          } else if (sourceTargetRequested && !explicitSourceToken.empty()) {
            Character *resolvedByToken =
                ResolveCharacterByTargetToken(thisptr, explicitSourceToken, npc);
            if (resolvedByToken && (uintptr_t)resolvedByToken > 0x1000) {
              sourceCharacter = resolvedByToken;
              explicitSourceTarget = true;
            }
          } else if (!sourceTargetRequested) {
            sourceCharacter = primaryPlayer;
          }
          std::string itemQueryRaw = TrimCopySimple(act.message);
          if (itemQueryRaw.empty() && explicitSourceTarget) {
            itemQueryRaw = "equipment";
          }
          size_t fnot = itemQueryRaw.find_first_not_of(" \t\n\r\"'");
          if (fnot != std::string::npos) {
            itemQueryRaw.erase(0, fnot);
            size_t lnot = itemQueryRaw.find_last_not_of(" \t\n\r\"'");
            if (lnot != std::string::npos) {
              itemQueryRaw.erase(lnot + 1);
            }
          } else {
            itemQueryRaw.clear();
          }

          std::string itemQueryToken = NormalizeInventoryMatchToken(itemQueryRaw);
          bool equipmentSweep =
              itemQueryToken == "equipment" || itemQueryToken == "equip" ||
              itemQueryToken == "gear" || itemQueryToken == "armor" ||
              itemQueryToken == "armour";
          bool allItemsSweep =
              itemQueryToken == "all" || itemQueryToken == "everything" ||
              itemQueryToken == "inventory" || itemQueryToken == "items" ||
              itemQueryToken == "loot";
          int requestedCount = act.taskValue > 0 ? act.taskValue : 1;
          if (requestedCount > 100) {
            requestedCount = 100;
          }

          bool inferredLootSource = false;
          if (!explicitSourceTarget && npc && (uintptr_t)npc > 0x1000 && thisptr) {
            Character *bestCandidate = nullptr;
            int bestScore = 0;
            const auto &chars = thisptr->getCharacterUpdateList();
            for (auto it = chars.begin(); it != chars.end(); ++it) {
              Character *candidate = *it;
              if (!candidate || (uintptr_t)candidate <= 0x1000 ||
                  candidate == npc || candidate == primaryPlayer) {
                continue;
              }

              float candidateDistance = -1.0f;
              std::string candidateRangeReason = "";
              if (!ValidateNpcCloseActionRange(npc, candidate,
                                               kNpcCloseActionRangeUnits,
                                               candidateDistance,
                                               candidateRangeReason)) {
                continue;
              }

              std::string candidateStateReason = "";
              bool candidateDead = false;
              if (!IsTakeItemLootTargetValid(thisptr, candidate, candidateStateReason,
                                             candidateDead)) {
                continue;
              }

              std::vector<Item *> candidateItems;
              try {
                GetAllCharacterItems(candidate, candidateItems);
              } catch (...) {
                candidateItems.clear();
              }
              if (candidateItems.empty()) {
                continue;
              }

              bool candidateMatches = false;
              bool candidateHasEquipped = false;
              for (size_t itemIdx = 0; itemIdx < candidateItems.size(); ++itemIdx) {
                Item *candidateItem = candidateItems[itemIdx];
                if (!candidateItem || (uintptr_t)candidateItem <= 0x1000) {
                  continue;
                }

                std::string candidateItemName = "";
                try {
                  candidateItemName = candidateItem->getName();
                } catch (...) {
                  candidateItemName = "";
                }
                if (candidateItemName.empty()) {
                  continue;
                }

                bool candidateItemEquipped = false;
                try {
                  candidateItemEquipped = candidateItem->isEquipped;
                } catch (...) {
                  candidateItemEquipped = false;
                }
                if (candidateItemEquipped) {
                  candidateHasEquipped = true;
                }

                if (allItemsSweep) {
                  candidateMatches = true;
                  break;
                }
                if (equipmentSweep) {
                  if (candidateItemEquipped) {
                    candidateMatches = true;
                    break;
                  }
                  continue;
                }
                if (!itemQueryToken.empty()) {
                  std::string candidateItemToken =
                      NormalizeInventoryMatchToken(candidateItemName);
                  if (InventoryTokensMatch(candidateItemToken, itemQueryToken)) {
                    candidateMatches = true;
                    break;
                  }
                }
              }

              if (!candidateMatches && equipmentSweep && candidateDead) {
                candidateMatches = true;
              }
              if (!candidateMatches) {
                continue;
              }

              int candidateScore = candidateDead ? 1000 : 600;
              if (candidateHasEquipped) {
                candidateScore += 120;
              }
              if (!itemQueryToken.empty() && !equipmentSweep && !allItemsSweep) {
                candidateScore += 200;
              }
              if (candidateDistance >= 0.0f) {
                float proximity = kNpcCloseActionRangeUnits - candidateDistance;
                if (proximity > 0.0f) {
                  candidateScore += (int)(proximity * 4.0f);
                }
              }

              if (!bestCandidate || candidateScore > bestScore) {
                bestCandidate = candidate;
                bestScore = candidateScore;
              }
            }

            if (bestCandidate && (uintptr_t)bestCandidate > 0x1000) {
              sourceCharacter = bestCandidate;
              explicitSourceTarget = true;
              inferredLootSource = true;
            }
          }

          if (itemQueryRaw.empty() && explicitSourceTarget) {
            itemQueryRaw = "equipment";
            itemQueryToken = NormalizeInventoryMatchToken(itemQueryRaw);
            equipmentSweep = true;
          }
          if (itemQueryToken.empty() && explicitSourceTarget) {
            equipmentSweep = true;
          }

          const std::string sourceName = SafeCharacterName(sourceCharacter);
          if (inferredLootSource) {
            Log("ACTION_EXEC: TAKE_ITEM inferred source actor=" + actorName +
                " source=" + sourceName + " query='" + itemQueryRaw + "'");
          }

          auto tryMoveItemToActor = [&](Item *item, int maxQuantity,
                                        int &movedQuantityOut) -> bool {
            movedQuantityOut = 0;
            if (!item || (uintptr_t)item <= 0x1000 || !sourceCharacter ||
                (uintptr_t)sourceCharacter <= 0x1000) {
              return false;
            }

            bool wasEquipped = false;
            try {
              wasEquipped = item->isEquipped;
            } catch (...) {
              wasEquipped = false;
            }
            if (wasEquipped) {
              try {
                sourceCharacter->unequipItem(item->inventorySection, item);
              } catch (...) {
              }
            }

            int transferQuantity = 1;
            try {
              if (item->quantity > 0) {
                transferQuantity = item->quantity;
              }
            } catch (...) {
              transferQuantity = 1;
            }
            if (maxQuantity > 0 && transferQuantity > maxQuantity) {
              transferQuantity = maxQuantity;
            }

            Inventory *inv = nullptr;
            try {
              inv = item->getInventory();
            } catch (...) {
              inv = nullptr;
            }
            if (!inv) {
              try {
                inv = sourceCharacter->getInventory();
              } catch (...) {
                inv = nullptr;
              }
            }

            Item *detached = nullptr;
            try {
              detached =
                  inv ? inv->removeItemDontDestroy_returnsItem(item, transferQuantity,
                                                               false)
                      : nullptr;
            } catch (...) {
              detached = nullptr;
            }

            Item *toGive = detached ? detached : item;
            if (!toGive || (uintptr_t)toGive <= 0x1000) {
              return false;
            }
            try {
              npc->giveItem(toGive, true, false);
            } catch (...) {
              return false;
            }
            try {
              movedQuantityOut = toGive->quantity > 0 ? toGive->quantity : transferQuantity;
            } catch (...) {
              movedQuantityOut = transferQuantity;
            }
            if (movedQuantityOut < 1) {
              movedQuantityOut = transferQuantity > 0 ? transferQuantity : 1;
            }
            return true;
          };

          if (!sourceCharacter || (uintptr_t)sourceCharacter <= 0x1000) {
            if (explicitSourceTarget) {
              thisptr->showPlayerAMessage_withLog(
                  actorName + " could not find the requested loot target.", true);
              Log("ACTION_EXEC: TAKE_ITEM blocked actor=" + actorName +
                  " reason=explicit_source_missing target_serial=" +
                  ToString((unsigned int)act.target.serial));
            } else {
              thisptr->showPlayerAMessage_withLog(
                  actorName +
                      " could not find a valid source to take items from.",
                  true);
              Log("ACTION_EXEC: TAKE_ITEM blocked actor=" + actorName +
                  " reason=source_not_found");
            }
          } else if (sourceCharacter == npc) {
            thisptr->showPlayerAMessage_withLog(
                actorName + " cannot take items from themselves.", true);
            Log("ACTION_EXEC: TAKE_ITEM blocked actor=" + actorName +
                " reason=self_transfer");
          } else if (itemQueryToken.empty() && !equipmentSweep && !allItemsSweep) {
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not parse an item to take.", true);
            Log("ACTION_EXEC: TAKE_ITEM blocked actor=" + actorName +
                " source=" + sourceName + " reason=empty_item_query raw='" +
                act.message + "'");
          } else {
            bool sourceIsDead = false;
            bool canLootSource = true;
            if (explicitSourceTarget) {
              float actionDistance = -1.0f;
              std::string rangeReason = "";
              if (!ValidateNpcCloseActionRange(npc, sourceCharacter,
                                               kNpcCloseActionRangeUnits,
                                               actionDistance, rangeReason)) {
                canLootSource = false;
                CloseActionApproachResult approachResult =
                    TryDeferCloseActionUntilInRange(
                        thisptr, npc, sourceCharacter, act, "loot", sourceName,
                        actionDistance, rangeReason, kNpcCloseActionRangeUnits);
                if (approachResult == CLOSE_ACTION_APPROACH_DEFERRED) {
                  queueDeferredAction = true;
                  deferActionQueue = true;
                  deferredAction = act;
                } else if (approachResult == CLOSE_ACTION_APPROACH_TIMED_OUT) {
                  const std::string userReason =
                      DescribeCloseActionRangeReasonForUser(rangeReason);
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " could not reach " + sourceName +
                          " in time to loot them (" + userReason + ").",
                      true);
                  Log("ACTION_EXEC: TAKE_ITEM blocked actor=" + actorName +
                      " source=" + sourceName +
                      " reason=approach_timeout last_reason=" + rangeReason +
                      " dist=" + ToString(actionDistance) + " max_dist=" +
                      ToString(kNpcCloseActionRangeUnits) +
                      " timeout_ms=" + ToString((int)kNpcCloseActionApproachTimeoutMs));
                } else {
                  const std::string userReason =
                      DescribeCloseActionRangeReasonForUser(rangeReason);
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " cannot loot " + sourceName +
                          " because they are " + userReason + ".",
                      true);
                  Log("ACTION_EXEC: TAKE_ITEM blocked actor=" + actorName +
                      " source=" + sourceName + " reason=" + rangeReason +
                      " dist=" + ToString(actionDistance) + " max_dist=" +
                      ToString(kNpcCloseActionRangeUnits));
                }
              } else {
                ResetCloseActionApproachState(npc, act);
                std::string invalidReason = "";
                if (!IsTakeItemLootTargetValid(thisptr, sourceCharacter,
                                               invalidReason, sourceIsDead)) {
                  canLootSource = false;
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " cannot loot " + sourceName + ": " +
                          invalidReason + ".",
                      true);
                  Log("ACTION_EXEC: TAKE_ITEM blocked actor=" + actorName +
                      " source=" + sourceName + " reason=" + invalidReason);
                }
              }
            }

            if (canLootSource) {
              std::vector<Item *> sourceItems;
              GetAllCharacterItems(sourceCharacter, sourceItems);

              int movedCount = 0;
              int movedUnits = 0;
              std::string firstMovedName = "";
              int remainingToTransfer = requestedCount;
              if (equipmentSweep || allItemsSweep) {
                remainingToTransfer = 1000000;
              }
              for (size_t i = 0; i < sourceItems.size(); ++i) {
                Item *item = sourceItems[i];
                if (!item || (uintptr_t)item <= 0x1000) {
                  continue;
                }

                std::string itemName = "";
                try {
                  itemName = item->getName();
                } catch (...) {
                  itemName = "";
                }
                if (itemName.empty()) {
                  continue;
                }

                bool matches = false;
                if (allItemsSweep) {
                  matches = true;
                } else if (equipmentSweep) {
                  bool isEquipped = false;
                  try {
                    isEquipped = item->isEquipped;
                  } catch (...) {
                    isEquipped = false;
                  }
                  matches = isEquipped;
                } else {
                  std::string itemToken = NormalizeInventoryMatchToken(itemName);
                  matches = InventoryTokensMatch(itemToken, itemQueryToken);
                }
                if (!matches) {
                  continue;
                }

                int movedQuantity = 0;
                if (!tryMoveItemToActor(item, remainingToTransfer, movedQuantity)) {
                  continue;
                }

                ++movedCount;
                movedUnits += movedQuantity;
                if (firstMovedName.empty()) {
                  firstMovedName = itemName;
                }
                if (remainingToTransfer > 0) {
                  remainingToTransfer -= movedQuantity;
                  if (remainingToTransfer < 0) {
                    remainingToTransfer = 0;
                  }
                }

                if (!equipmentSweep && !allItemsSweep && remainingToTransfer <= 0) {
                  break;
                }
              }

              // Some dead bodies expose items without the equipped flag set.
              // If "equipment" was requested and none matched, fall back to
              // looting remaining corpse inventory.
              if (movedCount == 0 && equipmentSweep && explicitSourceTarget &&
                  sourceIsDead) {
                for (size_t i = 0; i < sourceItems.size(); ++i) {
                  Item *item = sourceItems[i];
                  if (!item || (uintptr_t)item <= 0x1000) {
                    continue;
                  }
                  std::string itemName = "";
                  try {
                    itemName = item->getName();
                  } catch (...) {
                    itemName = "";
                  }
                  if (itemName.empty()) {
                    continue;
                  }
                  int movedQuantity = 0;
                  if (!tryMoveItemToActor(item, 1000000, movedQuantity)) {
                    continue;
                  }
                  ++movedCount;
                  movedUnits += movedQuantity;
                  if (firstMovedName.empty()) {
                    firstMovedName = itemName;
                  }
                }
              }

              if (movedCount > 0) {
                std::string sourceLabel = explicitSourceTarget ? sourceName : "you";
                std::string itemLabel = firstMovedName;
                if ((equipmentSweep || allItemsSweep) && movedCount > 1) {
                  itemLabel = ToString(movedCount) + " items";
                } else if (!equipmentSweep && !allItemsSweep && movedUnits > 1 &&
                           !firstMovedName.empty()) {
                  itemLabel = ToString(movedUnits) + " x " + firstMovedName;
                } else if ((equipmentSweep || allItemsSweep) && movedCount == 1 &&
                           itemLabel.empty()) {
                  itemLabel = "equipment";
                }
                if (itemLabel.empty()) {
                  itemLabel = "an item";
                }

                if (!explicitSourceTarget) {
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " took " + itemLabel + " from you.", true);
                } else if (sourceIsDead) {
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " looted " + itemLabel + " from " + sourceLabel +
                          "'s corpse.",
                      true);
                } else {
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " took " + itemLabel + " from " + sourceLabel + ".",
                      true);
                }

                Log("ACTION_EXEC: TAKE_ITEM actor=" + actorName +
                    " source=" + sourceLabel + " query='" + itemQueryRaw +
                    "' requested_count=" + ToString(requestedCount) +
                    " moved_count=" + ToString(movedCount) +
                    " moved_units=" + ToString(movedUnits) +
                    " first_item='" + firstMovedName + "' source_dead=" +
                    std::string(sourceIsDead ? "1" : "0"));
                inventoryTimer = 999;
                try {
                  npc->reThinkCurrentAIAction();
                } catch (...) {
                }
                if (explicitSourceTarget) {
                  try {
                    sourceCharacter->reThinkCurrentAIAction();
                  } catch (...) {
                  }
                }
              } else {
                if (!explicitSourceTarget) {
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " could not find that item on you.", true);
                } else {
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " could not find matching loot on " + sourceName +
                          ".",
                      true);
                }
                Log("ACTION_EXEC: TAKE_ITEM blocked actor=" + actorName +
                    " source=" + sourceName + " reason=item_not_found query='" +
                    itemQueryRaw + "' requested_count=" +
                    ToString(requestedCount) + " equipment_sweep=" +
                    std::string(equipmentSweep ? "1" : "0") + " all_sweep=" +
                    std::string(allItemsSweep ? "1" : "0"));
              }
            }
          }
        } else if (act.type == ACT_GIVE_ITEM) {
          std::vector<Item *> items;
          GetAllCharacterItems(npc, items);
          std::string itemQuery = act.message;
          size_t fnot = itemQuery.find_first_not_of(" \t\n\r\"'");
          if (fnot != std::string::npos) {
            itemQuery.erase(0, fnot);
            size_t lnot = itemQuery.find_last_not_of(" \t\n\r\"'");
            if (lnot != std::string::npos)
              itemQuery.erase(lnot + 1);
          } else {
            itemQuery.clear();
          }
          std::transform(itemQuery.begin(), itemQuery.end(), itemQuery.begin(),
                         ::tolower);
          const std::string itemQueryToken =
              NormalizeInventoryMatchToken(itemQuery);
          int requestedCount = act.taskValue > 0 ? act.taskValue : 1;
          if (requestedCount > 100) {
            requestedCount = 100;
          }

          Character *primaryPlayer = GetActivePlayerCharacter(thisptr);
          if (!primaryPlayer && thisptr->player &&
              thisptr->player->playerCharacters.size() > 0) {
            primaryPlayer = thisptr->player->playerCharacters[0];
          }
          Character *recipient = primaryPlayer;
          if (target && target != npc && (uintptr_t)target > 0x1000) {
            recipient = target;
          }
          if (!recipient || (uintptr_t)recipient <= 0x1000) {
            Log("ACTION_EXEC: GIVE_ITEM blocked actor=" + SafeCharacterName(npc) +
                " reason=recipient_missing");
          } else if (recipient == npc) {
            Log("ACTION_EXEC: GIVE_ITEM blocked actor=" + SafeCharacterName(npc) +
                " reason=self_transfer");
          } else if (itemQuery.empty() || itemQueryToken.empty()) {
            thisptr->showPlayerAMessage_withLog(
                SafeCharacterName(npc) + " could not parse an item to give.",
                true);
            Log("ACTION_EXEC: GIVE_ITEM blocked actor=" + SafeCharacterName(npc) +
                " reason=empty_item_query raw='" + act.message + "'");
          } else {
            int transferredCount = 0;
            std::string transferredItemName = "";
            std::string transferSourceLabel = "";

            for (uint32_t i = 0; i < items.size() && transferredCount < requestedCount;
                 ++i) {
              if (!items[i] || (uintptr_t)items[i] <= 0x1000) {
                continue;
              }
              std::string itemName = "";
              try {
                itemName = items[i]->getName();
              } catch (...) {
                itemName = "";
              }
              if (itemName.empty()) {
                continue;
              }
              std::string itemToken = NormalizeInventoryMatchToken(itemName);
              if (itemToken.empty()) {
                continue;
              }
              bool queryMatches = InventoryTokensMatch(itemToken, itemQueryToken);
              if (queryMatches) {
                int stackQuantity = 1;
                try {
                  stackQuantity = items[i]->quantity;
                } catch (...) {
                  stackQuantity = 1;
                }
                if (stackQuantity < 1) {
                  stackQuantity = 1;
                }
                int remaining = requestedCount - transferredCount;
                int transferQuantity =
                    (stackQuantity > remaining) ? remaining : stackQuantity;
                if (transferQuantity <= 0) {
                  continue;
                }

                Log("ACTION_EXEC: Giving item: " + itemName +
                    " qty=" + ToString(transferQuantity));
                if (items[i]->isEquipped) {
                  npc->unequipItem(items[i]->inventorySection, items[i]);
                }
                Inventory *inv = items[i]->getInventory();
                if (!inv) {
                  inv = npc->getInventory();
                }
                Item *detached = inv ? inv->removeItemDontDestroy_returnsItem(
                                           items[i], transferQuantity, false)
                                     : nullptr;
                if (!detached || (uintptr_t)detached <= 0x1000) {
                  continue;
                }
                int actualTransferred = transferQuantity;
                try {
                  if (detached->quantity > 0) {
                    actualTransferred = detached->quantity;
                  }
                } catch (...) {
                  actualTransferred = transferQuantity;
                }
                recipient->giveItem(detached, true, false);
                transferredCount += actualTransferred;
                inventoryTimer = 999;
                if (transferredItemName.empty()) {
                  transferredItemName = itemName;
                }
                transferSourceLabel = "inventory";
              }
            }

            if (transferredCount < requestedCount) {
              bool actorIsTrader = false;
              try {
                actorIsTrader = npc->isATrader();
              } catch (...) {
                actorIsTrader = false;
              }
              std::string cachedTraderInventoryJson = "";
              int cachedTraderInventoryCount = 0;
              int cachedTraderInventoryAgeSeconds = -1;
              bool hasCachedTraderInventory = GetCachedTraderInventorySnapshot(
                  npc, cachedTraderInventoryJson, cachedTraderInventoryCount,
                  &cachedTraderInventoryAgeSeconds);
              if (actorIsTrader || hasCachedTraderInventory) {
                while (transferredCount < requestedCount) {
                  int storageTransferredQuantity = 0;
                  std::string storageItemName = "";
                  std::string storageSourceName = "";
                  int remaining = requestedCount - transferredCount;
                  bool storageTransferred = TryTransferItemFromNearbyTraderStorage(
                      npc, recipient, itemQueryToken, remaining,
                      storageTransferredQuantity, storageItemName,
                      storageSourceName);
                  if (!storageTransferred || storageTransferredQuantity <= 0) {
                    break;
                  }
                  transferredCount += storageTransferredQuantity;
                  inventoryTimer = 999;
                  if (transferredItemName.empty() && !storageItemName.empty()) {
                    transferredItemName = storageItemName;
                  }
                  transferSourceLabel =
                      storageSourceName.empty() ? "shop storage"
                                                : storageSourceName;
                }
                if (transferredCount < requestedCount) {
                  Log("ACTION_EXEC: GIVE_ITEM trader storage miss actor=" +
                      SafeCharacterName(npc) + " query='" + act.message +
                      "' is_trader=" + (actorIsTrader ? "1" : "0") +
                      " cached_items=" + ToString(cachedTraderInventoryCount) +
                      " cache_age_s=" +
                      ToString(cachedTraderInventoryAgeSeconds));
                }
              }
            }

            if (transferredCount > 0) {
              const std::string givenItemName = transferredItemName.empty()
                                                    ? act.message
                                                    : transferredItemName;
              const std::string quantityPrefix =
                  (transferredCount > 1) ? (ToString(transferredCount) + "x ")
                                         : "";
              const std::string partialSuffix =
                  (transferredCount < requestedCount)
                      ? (" (partial " + ToString(transferredCount) + "/" +
                         ToString(requestedCount) + ")")
                      : "";
              if (recipient == primaryPlayer) {
                thisptr->showPlayerAMessage_withLog(
                    SafeCharacterName(npc) + " gave you " + quantityPrefix +
                        givenItemName + partialSuffix + ".",
                    true);
              } else {
                thisptr->showPlayerAMessage_withLog(
                    SafeCharacterName(npc) + " gave " + quantityPrefix +
                        givenItemName + " to " + SafeCharacterName(recipient) +
                        partialSuffix + ".",
                    true);
              }
              Log("ACTION_EXEC: GIVE_ITEM actor=" + SafeCharacterName(npc) +
                  " recipient=" + SafeCharacterName(recipient) +
                  " requested=" + ToString(requestedCount) +
                  " transferred=" + ToString(transferredCount) +
                  " item='" + givenItemName + "' source=" + transferSourceLabel);
              npc->reThinkCurrentAIAction();
            } else {
              thisptr->showPlayerAMessage_withLog(
                  SafeCharacterName(npc) + " could not find item \"" +
                      act.message + "\" to give.",
                  true);
              Log("ACTION_EXEC: GIVE_ITEM actor=" + SafeCharacterName(npc) +
                  " recipient=" + SafeCharacterName(recipient) +
                  " skipped reason=item_not_found query='" + act.message +
                  "' requested=" + ToString(requestedCount));
            }
          }
        } else if (act.type == ACT_DRINK_ITEM) {
          const std::string actorName = SafeCharacterName(npc);
          std::string requestedDrink = TrimCopySimple(act.message);
          if (requestedDrink.empty()) {
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not parse a drink item.", true);
            Log("ACTION_EXEC: DRINK_ITEM blocked actor=" + actorName +
                " reason=empty_item_query");
          } else if (IsCharacterSkeletonRace(npc)) {
            thisptr->showPlayerAMessage_withLog(
                actorName + " cannot drink alcohol (skeleton race).", true);
            Log("ACTION_EXEC: DRINK_ITEM blocked actor=" + actorName +
                " reason=skeleton_race");
          } else {
            Item *drinkItem = nullptr;
            std::string drinkItemName = "";
            std::string drinkCanonicalLabel = "";
            bool hasDrink = TryResolveDrinkItemForActor(
                npc, requestedDrink, drinkItem, drinkItemName,
                drinkCanonicalLabel);
            if (!hasDrink || !drinkItem) {
              thisptr->showPlayerAMessage_withLog(
                  actorName +
                      " could not find that drink (Bloodrum, Cactus Rum, Grog, "
                      "or Sake).",
                  true);
              Log("ACTION_EXEC: DRINK_ITEM blocked actor=" + actorName +
                  " reason=item_not_found query='" + requestedDrink + "'");
            } else if (!ConsumeSingleItemFromActor(npc, drinkItem)) {
              thisptr->showPlayerAMessage_withLog(
                  actorName + " failed to consume " +
                      (drinkCanonicalLabel.empty() ? drinkItemName
                                                   : drinkCanonicalLabel) +
                      ".",
                  true);
              Log("ACTION_EXEC: DRINK_ITEM blocked actor=" + actorName +
                  " reason=consume_failed item='" +
                  (drinkCanonicalLabel.empty() ? drinkItemName
                                               : drinkCanonicalLabel) +
                  "'");
            } else {
              int newLevel = 0;
              int secondsRemaining = 0;
              bool passedOut = false;
              if (!AdvanceNpcDrunkLevel(thisptr, npc, newLevel, secondsRemaining,
                                        passedOut)) {
                thisptr->showPlayerAMessage_withLog(
                    actorName + " is too drunk to drink more right now.", true);
                Log("ACTION_EXEC: DRINK_ITEM blocked actor=" + actorName +
                    " reason=already_passed_out");
              } else {
                std::string drinkDisplay = drinkCanonicalLabel.empty()
                                               ? drinkItemName
                                               : drinkCanonicalLabel;
                if (drinkDisplay.empty()) {
                  drinkDisplay = requestedDrink;
                }
                if (passedOut) {
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " drank " + drinkDisplay +
                          " and passed out drunk.",
                      true);
                  Log("ACTION_EXEC: DRINK_ITEM actor=" + actorName +
                      " item='" + drinkDisplay +
                      "' level=3 passed_out=1 ko_seconds=" +
                      ToString(secondsRemaining));
                } else {
                  std::string levelText = (newLevel >= 2) ? "very drunk" : "drunk";
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " drank " + drinkDisplay + " and is now " +
                          levelText + ".",
                      true);
                  Log("ACTION_EXEC: DRINK_ITEM actor=" + actorName +
                      " item='" + drinkDisplay + "' level=" +
                      ToString(newLevel) + " expires_in_seconds=" +
                      ToString(secondsRemaining));
                }
                inventoryTimer = 999;
                try {
                  npc->reThinkCurrentAIAction();
                } catch (...) {
                }
              }
            }
          }
        } else if (act.type == ACT_FORCE_DRINK) {
          const std::string actorName = SafeCharacterName(npc);
          const std::string targetName =
              target ? SafeCharacterName(target) : std::string("target");
          std::string requestedDrink = TrimCopySimple(act.message);
          if (requestedDrink.empty()) {
            requestedDrink = "Cactus Rum";
          }
          if (IsCharacterSkeletonRace(npc)) {
            thisptr->showPlayerAMessage_withLog(
                actorName + " cannot force drinks (skeleton race).", true);
            Log("ACTION_EXEC: FORCE_DRINK blocked actor=" + actorName +
                " target=" + targetName + " reason=skeleton_race_actor");
          } else if (!target || (uintptr_t)target <= 0x1000) {
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not find a valid force-drink target.", true);
            Log("ACTION_EXEC: FORCE_DRINK blocked actor=" + actorName +
                " reason=target_not_found");
          } else if (IsCharacterSkeletonRace(target)) {
            thisptr->showPlayerAMessage_withLog(
                actorName + " cannot force alcohol into " + targetName +
                    " (skeleton race).",
                true);
            Log("ACTION_EXEC: FORCE_DRINK blocked actor=" + actorName +
                " target=" + targetName + " reason=skeleton_race_target");
          } else {
            float actionDistance = -1.0f;
            std::string rangeReason = "";
            if (!ValidateNpcCloseActionRange(npc, target, kNpcCloseActionRangeUnits,
                                             actionDistance, rangeReason)) {
              CloseActionApproachResult approachResult =
                  TryDeferCloseActionUntilInRange(
                      thisptr, npc, target, act, "force a drink",
                      targetName, actionDistance, rangeReason,
                      kNpcCloseActionRangeUnits);
              if (approachResult == CLOSE_ACTION_APPROACH_DEFERRED) {
                queueDeferredAction = true;
                deferActionQueue = true;
                deferredAction = act;
              } else if (approachResult == CLOSE_ACTION_APPROACH_TIMED_OUT) {
                const std::string userReason =
                    DescribeCloseActionRangeReasonForUser(rangeReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " could not reach " + targetName +
                        " in time to force the drink (" + userReason + ").",
                    true);
                Log("ACTION_EXEC: FORCE_DRINK blocked actor=" + actorName +
                    " target=" + targetName +
                    " reason=approach_timeout last_reason=" + rangeReason +
                    " dist=" + ToString(actionDistance) + " max_dist=" +
                    ToString(kNpcCloseActionRangeUnits) +
                    " timeout_ms=" + ToString((int)kNpcCloseActionApproachTimeoutMs));
              } else {
                const std::string userReason =
                    DescribeCloseActionRangeReasonForUser(rangeReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " cannot force " + targetName +
                        " to drink because they are " + userReason + ".",
                    true);
                Log("ACTION_EXEC: FORCE_DRINK blocked actor=" + actorName +
                    " target=" + targetName + " reason=" + rangeReason +
                    " dist=" + ToString(actionDistance) + " max_dist=" +
                    ToString(kNpcCloseActionRangeUnits));
              }
            } else {
              ResetCloseActionApproachState(npc, act);
              std::string invalidReason = "";
              if (!IsRemoveLimbTargetValid(thisptr, target, invalidReason)) {
                if (invalidReason.empty()) {
                  invalidReason =
                      "target must be knocked out, unconscious, imprisoned, or "
                      "carried";
                }
                thisptr->showPlayerAMessage_withLog(
                    actorName + " cannot force " + targetName + " to drink: " +
                        invalidReason + ".",
                    true);
                Log("ACTION_EXEC: FORCE_DRINK blocked actor=" + actorName +
                    " target=" + targetName + " reason=" + invalidReason);
              } else {
                Item *drinkItem = nullptr;
                std::string drinkItemName = "";
                std::string drinkCanonicalLabel = "";
                bool hasDrink = TryResolveDrinkItemForActor(
                    npc, requestedDrink, drinkItem, drinkItemName,
                    drinkCanonicalLabel);
                if (!hasDrink || !drinkItem) {
                  thisptr->showPlayerAMessage_withLog(
                      actorName +
                          " could not find that drink (Bloodrum, Cactus Rum, "
                          "Grog, or Sake).",
                      true);
                  Log("ACTION_EXEC: FORCE_DRINK blocked actor=" + actorName +
                      " target=" + targetName + " reason=item_not_found query='" +
                      requestedDrink + "'");
                } else if (!ConsumeSingleItemFromActor(npc, drinkItem)) {
                  const std::string drinkDisplay =
                      drinkCanonicalLabel.empty() ? drinkItemName
                                                 : drinkCanonicalLabel;
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " failed to force " + targetName + " to drink " +
                          drinkDisplay + ".",
                      true);
                  Log("ACTION_EXEC: FORCE_DRINK blocked actor=" + actorName +
                      " target=" + targetName + " reason=consume_failed item='" +
                      drinkDisplay + "'");
                } else {
                  int newLevel = 0;
                  int secondsRemaining = 0;
                  bool passedOut = false;
                  if (!AdvanceNpcDrunkLevel(thisptr, target, newLevel,
                                            secondsRemaining, passedOut)) {
                    thisptr->showPlayerAMessage_withLog(
                        actorName + " forced " + targetName + " to drink " +
                            requestedDrink + ", but they were already blackout "
                            "drunk.",
                        true);
                    Log("ACTION_EXEC: FORCE_DRINK blocked actor=" + actorName +
                        " target=" + targetName +
                        " reason=target_already_passed_out");
                  } else {
                    std::string drinkDisplay = drinkCanonicalLabel.empty()
                                                   ? drinkItemName
                                                   : drinkCanonicalLabel;
                    if (drinkDisplay.empty()) {
                      drinkDisplay = requestedDrink;
                    }
                    if (passedOut) {
                      thisptr->showPlayerAMessage_withLog(
                          actorName + " forced " + targetName + " to drink " +
                              drinkDisplay + ", and they passed out drunk.",
                          true);
                      Log("ACTION_EXEC: FORCE_DRINK actor=" + actorName +
                          " target=" + targetName + " item='" + drinkDisplay +
                          "' level=3 passed_out=1 ko_seconds=" +
                          ToString(secondsRemaining));
                    } else {
                      std::string levelText =
                          (newLevel >= 2) ? "very drunk" : "drunk";
                      thisptr->showPlayerAMessage_withLog(
                          actorName + " forced " + targetName + " to drink " +
                              drinkDisplay + ". " + targetName + " is now " +
                              levelText + ".",
                          true);
                      Log("ACTION_EXEC: FORCE_DRINK actor=" + actorName +
                          " target=" + targetName + " item='" + drinkDisplay +
                          "' level=" + ToString(newLevel) +
                          " expires_in_seconds=" + ToString(secondsRemaining));
                    }
                    inventoryTimer = 999;
                    try {
                      npc->reThinkCurrentAIAction();
                    } catch (...) {
                    }
                    try {
                      target->reThinkCurrentAIAction();
                    } catch (...) {
                    }
                  }
                }
              }
            }
          }
        } else if (act.type == ACT_USE_DRUGS) {
          const std::string actorName = SafeCharacterName(npc);
          std::string requestedDrug = TrimCopySimple(act.message);
          if (requestedDrug.empty()) {
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not parse a drug item.", true);
            Log("ACTION_EXEC: USE_DRUGS blocked actor=" + actorName +
                " reason=empty_item_query");
          } else if (IsCharacterSkeletonRace(npc)) {
            thisptr->showPlayerAMessage_withLog(
                actorName + " cannot use drugs (skeleton race).", true);
            Log("ACTION_EXEC: USE_DRUGS blocked actor=" + actorName +
                " reason=skeleton_race");
          } else {
            Item *drugItem = nullptr;
            std::string drugItemName = "";
            std::string drugCanonicalLabel = "";
            bool hasDrug = TryResolveDrugItemForActor(
                npc, requestedDrug, drugItem, drugItemName, drugCanonicalLabel);
            if (!hasDrug || !drugItem) {
              thisptr->showPlayerAMessage_withLog(
                  actorName + " could not find that drug (Hashish).", true);
              Log("ACTION_EXEC: USE_DRUGS blocked actor=" + actorName +
                  " reason=item_not_found query='" + requestedDrug + "'");
            } else if (!ConsumeSingleItemFromActor(npc, drugItem)) {
              thisptr->showPlayerAMessage_withLog(
                  actorName + " failed to consume " +
                      (drugCanonicalLabel.empty() ? drugItemName
                                                  : drugCanonicalLabel) +
                      ".",
                  true);
              Log("ACTION_EXEC: USE_DRUGS blocked actor=" + actorName +
                  " reason=consume_failed item='" +
                  (drugCanonicalLabel.empty() ? drugItemName
                                              : drugCanonicalLabel) +
                  "'");
            } else {
              int secondsRemaining = 0;
              if (!ActivateNpcDrugHighState(thisptr, npc, secondsRemaining)) {
                thisptr->showPlayerAMessage_withLog(
                    actorName + " failed to enter a high state.", true);
                Log("ACTION_EXEC: USE_DRUGS blocked actor=" + actorName +
                    " reason=state_activation_failed");
              } else {
                std::string drugDisplay = drugCanonicalLabel.empty()
                                              ? drugItemName
                                              : drugCanonicalLabel;
                if (drugDisplay.empty()) {
                  drugDisplay = requestedDrug;
                }
                thisptr->showPlayerAMessage_withLog(
                    actorName + " used " + drugDisplay +
                        " and is now high (hunger x1.5).",
                    true);
                Log("ACTION_EXEC: USE_DRUGS actor=" + actorName + " item='" +
                    drugDisplay + "' high_seconds=" +
                    ToString(secondsRemaining) + " hunger_mult=1.5");
                inventoryTimer = 999;
                try {
                  npc->reThinkCurrentAIAction();
                } catch (...) {
                }
              }
            }
          }
        } else if (act.type == ACT_GIVE_CATS && thisptr->player &&
                   thisptr->player->playerCharacters.size() > 0) {
          Character *primaryPlayer = thisptr->player->playerCharacters[0];
          Character *recipient = primaryPlayer;
          if (target && target != npc && (uintptr_t)target > 0x1000) {
            recipient = target;
          }
          if (!recipient || (uintptr_t)recipient <= 0x1000) {
            Log("ACTION_EXEC: GIVE_CATS blocked actor=" + SafeCharacterName(npc) +
                " reason=recipient_missing");
          } else if (recipient == npc) {
            Log("ACTION_EXEC: GIVE_CATS blocked actor=" + SafeCharacterName(npc) +
                " reason=self_transfer");
          } else {
            int actorMoney = npc->getMoney();
            if (actorMoney <= 0 && npc->getOwnerships()) {
              actorMoney = npc->getOwnerships()->getMoney();
            }
            int amt = (act.taskValue > actorMoney) ? actorMoney : act.taskValue;
            if (amt > 0) {
              recipient->takeMoney(-amt);
              npc->takeMoney(amt);
              if (recipient == primaryPlayer) {
                thisptr->showPlayerAMessage_withLog(
                    "Gained " + ToString(amt) + " cats.", true);
              } else {
                thisptr->showPlayerAMessage_withLog(
                    SafeCharacterName(npc) + " gave " + SafeCharacterName(recipient) +
                        " " + ToString(amt) + " cats.",
                    true);
              }
              Log("ACTION_EXEC: GIVE_CATS actor=" + SafeCharacterName(npc) +
                  " recipient=" + SafeCharacterName(recipient) +
                  " amount=" + ToString(amt));
            } else {
              thisptr->showPlayerAMessage_withLog(
                  SafeCharacterName(npc) + " has no cats to give.", true);
              Log("ACTION_EXEC: GIVE_CATS actor=" + SafeCharacterName(npc) +
                  " skipped reason=no_money requested=" + ToString(act.taskValue));
            }
          }
        } else if (act.type == ACT_TAKE_CATS && thisptr->player &&
                   thisptr->player->playerCharacters.size() > 0) {
          Character *primaryPlayer = thisptr->player->playerCharacters[0];
          Character *victim = primaryPlayer;
          if (target && target != npc && (uintptr_t)target > 0x1000) {
            victim = target;
          }
          if (!victim || (uintptr_t)victim <= 0x1000) {
            Log("ACTION_EXEC: TAKE_CATS blocked actor=" + SafeCharacterName(npc) +
                " reason=victim_missing");
          } else if (victim == npc) {
            Log("ACTION_EXEC: TAKE_CATS blocked actor=" + SafeCharacterName(npc) +
                " reason=self_transfer");
          } else {
            std::string invalidReason = "";
            if (!IsRemoveLimbTargetValid(thisptr, victim, invalidReason)) {
              if (invalidReason.empty()) {
                invalidReason =
                    "target must be knocked out, unconscious, imprisoned, or carried";
              }
              thisptr->showPlayerAMessage_withLog(
                  SafeCharacterName(npc) + " cannot take cats from " +
                      SafeCharacterName(victim) + ": " + invalidReason + ".",
                  true);
              Log("ACTION_EXEC: TAKE_CATS blocked actor=" + SafeCharacterName(npc) +
                  " victim=" + SafeCharacterName(victim) +
                  " reason=" + invalidReason);
            } else {
              int victimMoney = victim->getMoney();
              if (victimMoney <= 0 && victim->getOwnerships()) {
                victimMoney = victim->getOwnerships()->getMoney();
              }
              int amt = (act.taskValue > victimMoney) ? victimMoney : act.taskValue;
              if (amt > 0) {
                Log("ACTION_EXEC: TAKE_CATS actor=" + SafeCharacterName(npc) +
                    " victim=" + SafeCharacterName(victim) +
                    " amount=" + ToString(amt));
                victim->takeMoney(amt);
                npc->takeMoney(-amt);
                if (victim == primaryPlayer) {
                  thisptr->showPlayerAMessage_withLog(
                      "Lost " + ToString(amt) + " cats.", true);
                } else {
                  thisptr->showPlayerAMessage_withLog(
                      SafeCharacterName(npc) + " took " + ToString(amt) +
                          " cats from " + SafeCharacterName(victim) + ".",
                      true);
                }
              } else {
                thisptr->showPlayerAMessage_withLog(
                    SafeCharacterName(victim) + " has no cats to take.", true);
                Log("ACTION_EXEC: TAKE_CATS actor=" + SafeCharacterName(npc) +
                    " victim=" + SafeCharacterName(victim) +
                    " skipped reason=no_money requested=" + ToString(act.taskValue));
              }
            }
          }
        } else if (act.type == ACT_REMOVE_LIMB) {
          const std::string actorName = SafeCharacterName(npc);
          Character *limbTarget = target;
          if ((!limbTarget || (uintptr_t)limbTarget <= 0x1000) &&
              thisptr && npc && (uintptr_t)npc > 0x1000) {
            std::string explicitTargetToken = TrimCopySimple(act.targetToken);
            std::string messageTargetToken = TrimCopySimple(act.message);

            if (act.target.isValid() && act.target.serial != 0) {
              std::string serialProbe = "";
              if (!explicitTargetToken.empty()) {
                serialProbe =
                    explicitTargetToken + "|" +
                    ToString((unsigned int)act.target.serial);
              } else if (!messageTargetToken.empty()) {
                serialProbe =
                    messageTargetToken + "|" +
                    ToString((unsigned int)act.target.serial);
              } else {
                serialProbe = ToString((unsigned int)act.target.serial);
              }
              Character *resolvedBySerial = ResolveCharacterByTargetToken(
                  thisptr, serialProbe, npc);
              if (resolvedBySerial && (uintptr_t)resolvedBySerial > 0x1000) {
                limbTarget = resolvedBySerial;
              }
            }

            if ((!limbTarget || (uintptr_t)limbTarget <= 0x1000) &&
                !explicitTargetToken.empty()) {
              Character *resolvedByToken = ResolveCharacterByTargetToken(
                  thisptr, explicitTargetToken, npc);
              if (resolvedByToken && (uintptr_t)resolvedByToken > 0x1000) {
                limbTarget = resolvedByToken;
              }
            }

            if ((!limbTarget || (uintptr_t)limbTarget <= 0x1000) &&
                !messageTargetToken.empty()) {
              Character *resolvedByMessage = ResolveCharacterByTargetToken(
                  thisptr, messageTargetToken, npc);
              if (resolvedByMessage && (uintptr_t)resolvedByMessage > 0x1000) {
                limbTarget = resolvedByMessage;
              }
            }

            if ((!limbTarget || (uintptr_t)limbTarget <= 0x1000) &&
                act.target.isValid()) {
              try {
                Character *resolvedByHandle = act.target.getCharacter();
                if (resolvedByHandle && (uintptr_t)resolvedByHandle > 0x1000) {
                  limbTarget = resolvedByHandle;
                }
              } catch (...) {
              }
            }

            // Dead/KO targets can fall out of update-list resolution but still
            // exist as CHARACTER objects nearby. Resolve by serial from world
            // objects as a final fallback.
            if ((!limbTarget || (uintptr_t)limbTarget <= 0x1000) &&
                act.target.isValid() && act.target.serial != 0) {
              auto tryResolveByObjectsNear = [&](const Ogre::Vector3 &anchorPos,
                                                 RootObject *ignoreObject,
                                                 float range) -> Character * {
                if (!thisptr || range <= 0.0f) {
                  return nullptr;
                }
                lektor<RootObject *> nearbyObjects;
                try {
                  thisptr->getObjectsWithinSphere(nearbyObjects, anchorPos, range,
                                                  CHARACTER, 128, ignoreObject);
                } catch (...) {
                  nearbyObjects.clear();
                }
                for (uint32_t i = 0; i < nearbyObjects.size(); ++i) {
                  Character *candidate = (Character *)nearbyObjects.stuff[i];
                  if (!candidate || (uintptr_t)candidate <= 0x1000) {
                    continue;
                  }
                  unsigned int candidateSerial = 0;
                  try {
                    candidateSerial = candidate->getHandle().serial;
                  } catch (...) {
                    candidateSerial = 0;
                  }
                  if (candidateSerial == act.target.serial) {
                    return candidate;
                  }
                }
                return nullptr;
              };

              Character *resolvedByObjects = nullptr;
              try {
                resolvedByObjects = tryResolveByObjectsNear(
                    npc->getPosition(), (RootObject *)npc, 1200.0f);
              } catch (...) {
                resolvedByObjects = nullptr;
              }

              if ((!resolvedByObjects ||
                   (uintptr_t)resolvedByObjects <= 0x1000) &&
                  thisptr->player && thisptr->player->playerCharacters.size() > 0 &&
                  thisptr->player->playerCharacters[0]) {
                Character *playerAnchor = thisptr->player->playerCharacters[0];
                try {
                  resolvedByObjects = tryResolveByObjectsNear(
                      playerAnchor->getPosition(), (RootObject *)playerAnchor,
                      1800.0f);
                } catch (...) {
                  resolvedByObjects = nullptr;
                }
              }

              if (resolvedByObjects && (uintptr_t)resolvedByObjects > 0x1000) {
                limbTarget = resolvedByObjects;
              }
            }
          }
          target = limbTarget;
          std::string targetName =
              limbTarget ? SafeCharacterName(limbTarget)
                         : TrimCopySimple(act.message);
          if (targetName.empty()) {
            targetName = "target";
          }
          if (!CharacterHasHacksaw(npc)) {
            Log("ACTION_EXEC: REMOVE_LIMB blocked actor=" + actorName +
                " reason=missing_hacksaw");
            thisptr->showPlayerAMessage_withLog(
                actorName + " cannot remove limbs without a hacksaw.", true);
          } else if (!target || (uintptr_t)target <= 0x1000) {
            Log("ACTION_EXEC: REMOVE_LIMB blocked actor=" + actorName +
                " reason=target_not_found target_token='" + act.message +
                "' explicit_target_token='" + TrimCopySimple(act.targetToken) +
                "' requested_serial=" + ToString((unsigned int)act.target.serial));
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not find a valid limb-removal target.", true);
          } else {
            float actionDistance = -1.0f;
            std::string rangeReason = "";
            if (!ValidateNpcCloseActionRange(npc, target, kNpcCloseActionRangeUnits,
                                             actionDistance, rangeReason)) {
              CloseActionApproachResult approachResult =
                  TryDeferCloseActionUntilInRange(
                      thisptr, npc, target, act, "remove a limb from",
                      targetName, actionDistance, rangeReason,
                      kNpcCloseActionRangeUnits);
              if (approachResult == CLOSE_ACTION_APPROACH_DEFERRED) {
                queueDeferredAction = true;
                deferActionQueue = true;
                deferredAction = act;
              } else if (approachResult == CLOSE_ACTION_APPROACH_TIMED_OUT) {
                const std::string userReason =
                    DescribeCloseActionRangeReasonForUser(rangeReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " could not reach " + targetName +
                        " in time to remove a limb (" + userReason + ").",
                    true);
                Log("ACTION_EXEC: REMOVE_LIMB blocked actor=" + actorName +
                    " target=" + targetName +
                    " reason=approach_timeout last_reason=" + rangeReason +
                    " dist=" + ToString(actionDistance) + " max_dist=" +
                    ToString(kNpcCloseActionRangeUnits) +
                    " timeout_ms=" + ToString((int)kNpcCloseActionApproachTimeoutMs));
              } else {
                Log("ACTION_EXEC: REMOVE_LIMB blocked actor=" + actorName +
                    " target=" + targetName + " reason=" + rangeReason +
                    " dist=" + ToString(actionDistance) + " max_dist=" +
                    ToString(kNpcCloseActionRangeUnits));
                const std::string userReason =
                    DescribeCloseActionRangeReasonForUser(rangeReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " cannot remove " + targetName +
                        "'s limb because they are " + userReason + ".",
                    true);
              }
            } else {
              ResetCloseActionApproachState(npc, act);
              std::string invalidReason = "";
              bool targetIsDead = false;
              if (!IsTakeItemLootTargetValid(thisptr, target, invalidReason,
                                             targetIsDead)) {
                if (invalidReason.empty()) {
                  invalidReason =
                      "target must be dead, knocked out, unconscious, imprisoned, "
                      "or carried";
                }
                Log("ACTION_EXEC: REMOVE_LIMB blocked actor=" + actorName +
                    " target=" + targetName + " reason=" + invalidReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " cannot remove " + targetName +
                        "'s limb: " + invalidReason + ".",
                    true);
              } else {
                RobotLimbs::Limb limb = RobotLimbs::NULL_LIMB;
                std::string limbName = "";
                if (!ResolveRobotLimbFromCode(act.taskValue, limb, limbName) ||
                    limb == RobotLimbs::NULL_LIMB) {
                  Log("ACTION_EXEC: REMOVE_LIMB blocked actor=" + actorName +
                      " target=" + targetName +
                      " reason=invalid_limb_code code=" +
                      ToString(act.taskValue));
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " could not determine which limb to remove.",
                      true);
                } else {
                  MedicalSystem *medical = nullptr;
                  try {
                    medical = target->getMedical();
                  } catch (...) {
                    medical = nullptr;
                  }
                  if (!medical || (uintptr_t)medical <= 0x1000) {
                    Log("ACTION_EXEC: REMOVE_LIMB blocked actor=" + actorName +
                        " target=" + targetName + " reason=missing_medical");
                    thisptr->showPlayerAMessage_withLog(
                        actorName + " could not access " + targetName +
                            "'s medical state.",
                        true);
                  } else {
                    LimbState limbState = LIMB_ORIGINAL;
                    bool stateKnown = false;
                    try {
                      limbState = medical->getLimbState(limb);
                      stateKnown = true;
                    } catch (...) {
                      stateKnown = false;
                    }
                    if (stateKnown &&
                        (limbState == LIMB_STUMP || limbState == LIMB_CRUSHED)) {
                      Log("ACTION_EXEC: REMOVE_LIMB skipped actor=" + actorName +
                          " target=" + targetName + " limb=" + limbName +
                          " reason=already_missing");
                      thisptr->showPlayerAMessage_withLog(
                          targetName + "'s " + limbName + " is already missing.",
                          true);
                    } else {
                      bool amputated = false;
                      try {
                        Ogre::Vector3 force(0.0f, 0.0f, 0.0f);
                        medical->amputate(limb, true, force);
                        amputated = true;
                      } catch (...) {
                        amputated = false;
                      }
                      if (amputated) {
                        bool limbHealthForced = false;
                        bool knockoutForced = false;
                        ForcePostAmputationKnockout(
                            medical, limb, limbHealthForced, knockoutForced);
                        unsigned int actorSerial = 0;
                        unsigned int targetSerial = 0;
                        try {
                          actorSerial = npc->getHandle().serial;
                        } catch (...) {
                          actorSerial = 0;
                        }
                        try {
                          targetSerial = target->getHandle().serial;
                        } catch (...) {
                          targetSerial = 0;
                        }
                        auto resolveFactionNameSafe = [](Character *character)
                            -> std::string {
                          if (!character || (uintptr_t)character <= 0x1000) {
                            return "None";
                          }
                          try {
                            Faction *faction =
                                character->getFaction()
                                    ? character->getFaction()
                                    : character->owner;
                            if (faction && (uintptr_t)faction > 0x1000) {
                              std::string factionName = faction->getName();
                              if (!factionName.empty()) {
                                return factionName;
                              }
                              if (faction->data) {
                                std::string fallback = faction->data->name;
                                if (fallback.empty()) {
                                  fallback = faction->data->stringID;
                                }
                                if (!fallback.empty()) {
                                  return fallback;
                                }
                              }
                            }
                          } catch (...) {
                          }
                          return "None";
                        };
                        LogGameEvent(
                            "limb_loss", actorName, resolveFactionNameSafe(npc),
                            targetName, resolveFactionNameSafe(target),
                            "severed " + limbName + " from " + targetName +
                                " with a hacksaw",
                            actorSerial, targetSerial);
                        try {
                          target->reThinkCurrentAIAction();
                        } catch (...) {
                        }
                        try {
                          npc->reThinkCurrentAIAction();
                        } catch (...) {
                        }
                        Log("ACTION_EXEC: REMOVE_LIMB success actor=" + actorName +
                            " target=" + targetName + " limb=" + limbName +
                            " limb_health_forced=" +
                            std::string(limbHealthForced ? "1" : "0") +
                            " knockout_forced=" +
                            std::string(knockoutForced ? "1" : "0"));
                        thisptr->showPlayerAMessage_withLog(
                            actorName + " removed " + targetName + "'s " +
                                limbName + ".",
                            true);
                      } else {
                        Log("ACTION_EXEC: REMOVE_LIMB failed actor=" + actorName +
                            " target=" + targetName + " limb=" + limbName);
                        thisptr->showPlayerAMessage_withLog(
                            actorName + " failed to remove " + targetName +
                                "'s " + limbName + ".",
                            true);
                      }
                    }
                  }
                }
              }
            }
          }
        } else if (act.type == ACT_CUT_HORNS) {
          const std::string actorName = SafeCharacterName(npc);
          std::string targetName =
              target ? SafeCharacterName(target) : TrimCopySimple(act.message);
          if (targetName.empty()) {
            targetName = "target";
          }
          if (!CharacterHasHacksaw(npc)) {
            Log("ACTION_EXEC: CUT_HORNS blocked actor=" + actorName +
                " reason=missing_hacksaw");
            thisptr->showPlayerAMessage_withLog(
                actorName + " cannot cut horns without a hacksaw.", true);
          } else if (!target || (uintptr_t)target <= 0x1000) {
            Log("ACTION_EXEC: CUT_HORNS blocked actor=" + actorName +
                " reason=target_not_found target_token='" + act.message +
                "' explicit_target_token='" + TrimCopySimple(act.targetToken) +
                "' requested_serial=" + ToString((unsigned int)act.target.serial));
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not find a valid horn-cutting target.", true);
          } else {
            float actionDistance = -1.0f;
            std::string rangeReason = "";
            if (!ValidateNpcCloseActionRange(npc, target, kNpcCloseActionRangeUnits,
                                             actionDistance, rangeReason)) {
              CloseActionApproachResult approachResult =
                  TryDeferCloseActionUntilInRange(
                      thisptr, npc, target, act, "cut the horns off",
                      targetName, actionDistance, rangeReason,
                      kNpcCloseActionRangeUnits);
              if (approachResult == CLOSE_ACTION_APPROACH_DEFERRED) {
                queueDeferredAction = true;
                deferActionQueue = true;
                deferredAction = act;
              } else if (approachResult == CLOSE_ACTION_APPROACH_TIMED_OUT) {
                const std::string userReason =
                    DescribeCloseActionRangeReasonForUser(rangeReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " could not reach " + targetName +
                        " in time to cut off their horns (" + userReason + ").",
                    true);
                Log("ACTION_EXEC: CUT_HORNS blocked actor=" + actorName +
                    " target=" + targetName +
                    " reason=approach_timeout last_reason=" + rangeReason +
                    " dist=" + ToString(actionDistance) + " max_dist=" +
                    ToString(kNpcCloseActionRangeUnits) +
                    " timeout_ms=" + ToString((int)kNpcCloseActionApproachTimeoutMs));
              } else {
                Log("ACTION_EXEC: CUT_HORNS blocked actor=" + actorName +
                    " target=" + targetName + " reason=" + rangeReason +
                    " dist=" + ToString(actionDistance) + " max_dist=" +
                    ToString(kNpcCloseActionRangeUnits));
                const std::string userReason =
                    DescribeCloseActionRangeReasonForUser(rangeReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " cannot cut off " + targetName +
                        "'s horns because they are " + userReason + ".",
                    true);
              }
            } else {
              ResetCloseActionApproachState(npc, act);
              std::string invalidReason = "";
              bool targetIsDead = false;
              if (!IsTakeItemLootTargetValid(thisptr, target, invalidReason,
                                             targetIsDead)) {
                if (invalidReason.empty()) {
                  invalidReason =
                      "target must be dead, knocked out, unconscious, imprisoned, "
                      "or carried";
                }
                Log("ACTION_EXEC: CUT_HORNS blocked actor=" + actorName +
                    " target=" + targetName + " reason=" + invalidReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " cannot cut off " + targetName +
                        "'s horns: " + invalidReason + ".",
                    true);
              } else if (!IsCharacterShekRace(target)) {
                Log("ACTION_EXEC: CUT_HORNS blocked actor=" + actorName +
                    " target=" + targetName + " reason=target_not_shek");
                thisptr->showPlayerAMessage_withLog(
                    targetName + " is not Shek and has no Shek horns to cut off.",
                    true);
              } else {
                float previousAverage = 0.0f;
                std::string hornReason = "";
                if (!TryGetCharacterHornAverageInternal(target, previousAverage,
                                                        hornReason)) {
                  Log("ACTION_EXEC: CUT_HORNS blocked actor=" + actorName +
                      " target=" + targetName + " reason=" +
                      (hornReason.empty() ? std::string("horn_data_unavailable")
                                          : hornReason));
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " could not inspect " + targetName +
                          "'s horn length.",
                      true);
                } else if (previousAverage >= kHornCutOffThreshold) {
                  Log("ACTION_EXEC: CUT_HORNS skipped actor=" + actorName +
                      " target=" + targetName + " reason=already_cut average=" +
                      ToString(previousAverage));
                  thisptr->showPlayerAMessage_withLog(
                      targetName + "'s horns have already been cut off.", true);
                } else {
                  bool tempJoinedForCut = false;
                  std::string hornCutOriginToken = "";
                  const bool targetAlreadyInPlayerSquad =
                      IsInPlayerFactionSafe(target) && IsInPlayerRoster(thisptr, target);
                  if (!targetAlreadyInPlayerSquad) {
                    hornCutOriginToken = BuildFactionPlatoonOriginToken(target);
                    if (hornCutOriginToken.empty()) {
                      Log("ACTION_EXEC: CUT_HORNS blocked actor=" + actorName +
                          " target=" + targetName +
                          " reason=origin_capture_failed");
                      thisptr->showPlayerAMessage_withLog(
                          actorName + " could not preserve " + targetName +
                              "'s original squad before cutting their horns.",
                          true);
                    } else {
                      std::string joinReason = "";
                      const bool joinedForCut =
                          TryInternalJoinPlayerSquad(thisptr, target, joinReason);
                      Log("ACTION_EXEC: CUT_HORNS temp_join actor=" + actorName +
                          " target=" + targetName + " ok=" +
                          std::string(joinedForCut ? "1" : "0") + " origin='" +
                          hornCutOriginToken + "' " + joinReason);
                      if (!joinedForCut) {
                        thisptr->showPlayerAMessage_withLog(
                            actorName + " could not temporarily recruit " +
                                targetName + " to cut off their horns.",
                            true);
                      } else {
                        tempJoinedForCut = true;
                      }
                    }
                  }

                  AppearanceBase *appearance = nullptr;
                  GameData *appearanceData = nullptr;
                  std::string appearanceReason = "";
                  if ((!targetAlreadyInPlayerSquad && !tempJoinedForCut) ||
                      !TryGetCharacterAppearanceData(target, appearance,
                                                     appearanceData,
                                                     appearanceReason)) {
                    if (targetAlreadyInPlayerSquad || tempJoinedForCut) {
                      Log("ACTION_EXEC: CUT_HORNS blocked actor=" + actorName +
                          " target=" + targetName + " reason=" +
                          (appearanceReason.empty()
                               ? std::string("appearance_unavailable")
                               : appearanceReason));
                      thisptr->showPlayerAMessage_withLog(
                          actorName + " could not access " + targetName +
                              "'s appearance data.",
                          true);
                    }
                  } else {
                    std::string persistenceReason = "";
                    const bool persistenceReady =
                        PromoteCharacterHornPersistence(target, persistenceReason);
                    bool hornSet = false;
                    std::string cloneReason = "";
                    GameDataCopyStandalone *characterAppearanceData =
                        AssignClonedAppearanceData(target, appearance,
                                                   appearanceData, cloneReason);
                    const bool haveCharacterAppearanceData =
                        characterAppearanceData &&
                        (uintptr_t)characterAppearanceData > 0x1000;
                    if (haveCharacterAppearanceData) {
                      appearanceData = characterAppearanceData;
                    } else {
                      characterAppearanceData =
                          TryGetCharacterAppearanceSourceData(target);
                    }
                    try {
                      ApplyHornCutOffValues(appearanceData);
                      ValidateAppearanceDataForCharacter(appearanceData, target,
                                                         appearance);
                      appearance->updatedAppearanceData = true;
                      appearance->updateBody = true;
                      if (haveCharacterAppearanceData) {
                        target->setAppearanceData(characterAppearanceData);
                        appearance->setAppearanceData(characterAppearanceData);
                        appearance->updatedAppearanceData = true;
                        appearance->updateBody = true;
                      } else {
                        appearance->setAppearanceData(
                            (GameDataCopyStandalone *)appearanceData);
                        appearance->updatedAppearanceData = true;
                        appearance->updateBody = true;
                      }
                      hornSet = true;
                    } catch (...) {
                      hornSet = false;
                    }

                    if (!hornSet) {
                      Log("ACTION_EXEC: CUT_HORNS failed actor=" + actorName +
                          " target=" + targetName +
                          " reason=appearance_write_failed");
                      thisptr->showPlayerAMessage_withLog(
                          actorName + " failed to cut off " + targetName +
                              "'s horns.",
                          true);
                    } else {
                      std::string persistenceCommitReason = "";
                      const bool persistenceCommitted =
                          PromoteCharacterHornPersistence(
                              target, persistenceCommitReason);
                      bool refreshed = RefreshCharacterAppearance(target);
                      AppearanceBase *postAppearance = nullptr;
                      GameData *postAppearanceData = nullptr;
                      std::string postAppearanceReason = "";
                      if (TryGetCharacterAppearanceData(
                              target, postAppearance, postAppearanceData,
                              postAppearanceReason)) {
                        float postAverage = 0.0f;
                        std::string postAverageReason = "";
                        if (!TryGetCharacterHornAverageInternal(
                                target, postAverage, postAverageReason) ||
                            postAverage < kHornCutOffThreshold) {
                          ApplyHornCutOffValues(postAppearanceData);
                          ValidateAppearanceDataForCharacter(
                              postAppearanceData, target, postAppearance);
                          if (haveCharacterAppearanceData &&
                              characterAppearanceData != postAppearanceData) {
                            ApplyHornCutOffValues(characterAppearanceData);
                            ValidateAppearanceDataForCharacter(
                                characterAppearanceData, target, postAppearance);
                            target->setAppearanceData(characterAppearanceData);
                            postAppearance->setAppearanceData(
                                characterAppearanceData);
                          } else {
                            postAppearance->setAppearanceData(
                                (GameDataCopyStandalone *)postAppearanceData);
                          }
                          refreshed = RefreshCharacterAppearance(target) || refreshed;
                          if ((!TryGetCharacterHornAverageInternal(
                                   target, postAverage, postAverageReason) ||
                               postAverage < kHornCutOffThreshold) &&
                              HardReloadCharacterAppearance(target)) {
                            refreshed = true;
                          }
                        }
                      }
                      unsigned int actorSerial = 0;
                      unsigned int targetSerial = 0;
                      try {
                        actorSerial = npc->getHandle().serial;
                      } catch (...) {
                        actorSerial = 0;
                      }
                      try {
                        targetSerial = target->getHandle().serial;
                      } catch (...) {
                        targetSerial = 0;
                      }
                      auto resolveFactionNameSafe = [](Character *character)
                          -> std::string {
                        if (!character || (uintptr_t)character <= 0x1000) {
                          return "None";
                        }
                        try {
                          Faction *faction =
                              character->getFaction()
                                  ? character->getFaction()
                                  : character->owner;
                          if (faction && (uintptr_t)faction > 0x1000) {
                            std::string factionName = faction->getName();
                            if (!factionName.empty()) {
                              return factionName;
                            }
                            if (faction->data) {
                              std::string fallback = faction->data->name;
                              if (fallback.empty()) {
                                fallback = faction->data->stringID;
                              }
                              if (!fallback.empty()) {
                                return fallback;
                              }
                            }
                          }
                        } catch (...) {
                        }
                        return "None";
                      };
                      LogGameEvent(
                          "horn_cut", actorName, resolveFactionNameSafe(npc),
                          targetName, resolveFactionNameSafe(target),
                          "cut off the horns of " + targetName +
                              " with a hacksaw",
                          actorSerial, targetSerial);
                      QueueHornCutReapply(target);
                      if (tempJoinedForCut && !hornCutOriginToken.empty()) {
                        QueueHornCutDismiss(target, hornCutOriginToken);
                      }
                      PushImmediateHornContextSnapshot(target, "horn_cut");
                      try {
                        target->reThinkCurrentAIAction();
                      } catch (...) {
                      }
                      try {
                        npc->reThinkCurrentAIAction();
                      } catch (...) {
                      }
                      Log("ACTION_EXEC: CUT_HORNS success actor=" + actorName +
                          " target=" + targetName +
                          " previous_average=" + ToString(previousAverage) +
                          " persistence_ok=" +
                          std::string((persistenceReady || persistenceCommitted)
                                          ? "1"
                                          : "0") +
                          " refreshed=" + std::string(refreshed ? "1" : "0"));
                      thisptr->showPlayerAMessage_withLog(
                          actorName + " cut off " + targetName +
                              "'s horns with a hacksaw.",
                          true);
                    }
                  }
                }
              }
            }
          }
        } else if (act.type == ACT_KNOCKOUT) {
          const std::string actorName = SafeCharacterName(npc);
          std::string targetName =
              target ? SafeCharacterName(target) : TrimCopySimple(act.message);
          if (targetName.empty()) {
            targetName = "target";
          }
          const bool selfTarget = (target == npc);
          if (!target || (uintptr_t)target <= 0x1000) {
            Log("ACTION_EXEC: KNOCKOUT blocked actor=" + actorName +
                " reason=target_not_found target_token='" + act.message + "'");
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not find a valid knockout target.", true);
          } else {
            float actionDistance = -1.0f;
            std::string rangeReason = "";
            if (!ValidateNpcCloseActionRange(npc, target, kNpcCloseActionRangeUnits,
                                             actionDistance, rangeReason)) {
              CloseActionApproachResult approachResult =
                  TryDeferCloseActionUntilInRange(
                      thisptr, npc, target, act, "knock out", targetName,
                      actionDistance, rangeReason, kNpcCloseActionRangeUnits);
              if (approachResult == CLOSE_ACTION_APPROACH_DEFERRED) {
                queueDeferredAction = true;
                deferActionQueue = true;
                deferredAction = act;
              } else if (approachResult == CLOSE_ACTION_APPROACH_TIMED_OUT) {
                const std::string userReason =
                    DescribeCloseActionRangeReasonForUser(rangeReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " could not reach " + targetName +
                        " in time to knock them out (" + userReason + ").",
                    true);
                Log("ACTION_EXEC: KNOCKOUT blocked actor=" + actorName +
                    " target=" + targetName +
                    " reason=approach_timeout last_reason=" + rangeReason +
                    " dist=" + ToString(actionDistance) + " max_dist=" +
                    ToString(kNpcCloseActionRangeUnits) +
                    " timeout_ms=" + ToString((int)kNpcCloseActionApproachTimeoutMs));
              } else {
                Log("ACTION_EXEC: KNOCKOUT blocked actor=" + actorName +
                    " target=" + targetName + " reason=" + rangeReason +
                    " dist=" + ToString(actionDistance) + " max_dist=" +
                    ToString(kNpcCloseActionRangeUnits));
                const std::string userReason =
                    DescribeCloseActionRangeReasonForUser(rangeReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " cannot knock out " + targetName +
                        " because they are " + userReason + ".",
                    true);
              }
            } else {
              ResetCloseActionApproachState(npc, act);
              std::string invalidReason = "";
              if (!selfTarget &&
                  !IsRemoveLimbTargetValid(thisptr, target, invalidReason)) {
                if (invalidReason.empty()) {
                  invalidReason = "target is not in a valid state";
                }
                Log("ACTION_EXEC: KNOCKOUT blocked actor=" + actorName +
                    " target=" + targetName + " reason=" + invalidReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " cannot knock out " + targetName + ": " +
                        invalidReason + ".",
                    true);
              } else {
                ClearCharacterSpeechBubble(target);

                bool alreadyKnockedOut = false;
                bool knockoutApplied = false;
                bool forceTimerApplied = false;
                bool medicalValidated = false;
                bool sustainedKnockoutScheduled = false;
                int sustainedKnockoutSeconds = 0;
                bool knockoutSucceeded = ForceImmediateCharacterKnockout(
                    target, alreadyKnockedOut, knockoutApplied, forceTimerApplied,
                    medicalValidated);
                if (knockoutSucceeded) {
                  sustainedKnockoutScheduled = BeginNpcSustainedKnockout(
                      thisptr, target, kSustainedKnockoutDurationSeconds,
                      sustainedKnockoutSeconds);
                }

                try {
                  target->reThinkCurrentAIAction();
                } catch (...) {
                }
                try {
                  npc->reThinkCurrentAIAction();
                } catch (...) {
                }

                if (knockoutSucceeded) {
                  Log("ACTION_EXEC: KNOCKOUT success actor=" + actorName +
                      " target=" + targetName +
                      " already_ko=" +
                      std::string(alreadyKnockedOut ? "1" : "0") +
                      " knockout_applied=" +
                      std::string(knockoutApplied ? "1" : "0") +
                      " force_timer_applied=" +
                      std::string(forceTimerApplied ? "1" : "0") +
                      " sustained_knockout=" +
                      std::string(sustainedKnockoutScheduled ? "1" : "0") +
                      " sustained_seconds=" +
                      ToString(sustainedKnockoutSeconds) +
                      " medical_validated=" +
                      std::string(medicalValidated ? "1" : "0"));
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " knocked out " + targetName + ".", true);
                } else {
                  Log("ACTION_EXEC: KNOCKOUT failed actor=" + actorName +
                      " target=" + targetName +
                      " knockout_applied=" +
                      std::string(knockoutApplied ? "1" : "0") +
                      " force_timer_applied=" +
                      std::string(forceTimerApplied ? "1" : "0") +
                      " medical_validated=" +
                      std::string(medicalValidated ? "1" : "0"));
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " failed to knock out " + targetName + ".",
                      true);
                }
              }
            }
          }
        } else if (act.type == ACT_KILL) {
          const std::string actorName = SafeCharacterName(npc);
          std::string targetName =
              target ? SafeCharacterName(target) : TrimCopySimple(act.message);
          if (targetName.empty()) {
            targetName = "target";
          }
          if (!target || (uintptr_t)target <= 0x1000) {
            Log("ACTION_EXEC: KILL blocked actor=" + actorName +
                " reason=target_not_found target_token='" + act.message + "'");
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not find a valid execution target.", true);
          } else if (target == npc) {
            Log("ACTION_EXEC: KILL blocked actor=" + actorName +
                " reason=self_target");
            thisptr->showPlayerAMessage_withLog(
                actorName + " cannot use KILL on themselves.", true);
          } else {
            float actionDistance = -1.0f;
            std::string rangeReason = "";
            if (!ValidateNpcCloseActionRange(npc, target, kNpcCloseActionRangeUnits,
                                             actionDistance, rangeReason)) {
              CloseActionApproachResult approachResult =
                  TryDeferCloseActionUntilInRange(
                      thisptr, npc, target, act, "kill", targetName,
                      actionDistance, rangeReason, kNpcCloseActionRangeUnits);
              if (approachResult == CLOSE_ACTION_APPROACH_DEFERRED) {
                queueDeferredAction = true;
                deferActionQueue = true;
                deferredAction = act;
              } else if (approachResult == CLOSE_ACTION_APPROACH_TIMED_OUT) {
                const std::string userReason =
                    DescribeCloseActionRangeReasonForUser(rangeReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " could not reach " + targetName +
                        " in time to kill them (" + userReason + ").",
                    true);
                Log("ACTION_EXEC: KILL blocked actor=" + actorName +
                    " target=" + targetName +
                    " reason=approach_timeout last_reason=" + rangeReason +
                    " dist=" + ToString(actionDistance) + " max_dist=" +
                    ToString(kNpcCloseActionRangeUnits) +
                    " timeout_ms=" + ToString((int)kNpcCloseActionApproachTimeoutMs));
              } else {
                Log("ACTION_EXEC: KILL blocked actor=" + actorName +
                    " target=" + targetName + " reason=" + rangeReason +
                    " dist=" + ToString(actionDistance) + " max_dist=" +
                    ToString(kNpcCloseActionRangeUnits));
                const std::string userReason =
                    DescribeCloseActionRangeReasonForUser(rangeReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " cannot kill " + targetName +
                        " because they are " + userReason + ".",
                    true);
              }
            } else {
              ResetCloseActionApproachState(npc, act);
              std::string invalidReason = "";
              if (!IsRemoveLimbTargetValid(thisptr, target, invalidReason)) {
                if (invalidReason.empty()) {
                  invalidReason = "target is not in a valid state";
                }
                Log("ACTION_EXEC: KILL blocked actor=" + actorName +
                    " target=" + targetName + " reason=" + invalidReason);
                thisptr->showPlayerAMessage_withLog(
                    actorName + " cannot kill " + targetName + ": " +
                        invalidReason + ".",
                    true);
              } else {
                ClearCharacterSpeechBubble(target);

                MedicalSystem *medical = nullptr;
                try {
                  medical = target->getMedical();
                } catch (...) {
                  medical = nullptr;
                }
                bool bloodForced = false;
                bool validatedHealth = false;
                if (medical && (uintptr_t)medical > 0x1000) {
                  try {
                    medical->blood = 0.0f;
                    bloodForced = true;
                  } catch (...) {
                    bloodForced = false;
                  }
                  try {
                    medical->validateHealthValues();
                    validatedHealth = true;
                  } catch (...) {
                    validatedHealth = false;
                  }
                }

                bool alreadyDead = false;
                try {
                  alreadyDead = target->isDead();
                } catch (...) {
                  alreadyDead = false;
                }

                bool declaredDead = false;
                if (!alreadyDead) {
                  try {
                    target->declareDead();
                    declaredDead = true;
                  } catch (...) {
                    declaredDead = false;
                  }
                }

                bool nowDead = alreadyDead || declaredDead;
                if (!nowDead) {
                  try {
                    nowDead = target->isDead();
                  } catch (...) {
                    nowDead = false;
                  }
                }

                try {
                  target->reThinkCurrentAIAction();
                } catch (...) {
                }
                try {
                  npc->reThinkCurrentAIAction();
                } catch (...) {
                }

                if (nowDead) {
                  Log("ACTION_EXEC: KILL success actor=" + actorName +
                      " target=" + targetName +
                      " blood_forced=" + std::string(bloodForced ? "1" : "0") +
                      " medical_validated=" +
                      std::string(validatedHealth ? "1" : "0") +
                      " already_dead=" + std::string(alreadyDead ? "1" : "0") +
                      " declared_dead=" +
                      std::string(declaredDead ? "1" : "0"));
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " killed " + targetName + ".", true);
                } else {
                  Log("ACTION_EXEC: KILL failed actor=" + actorName +
                      " target=" + targetName +
                      " blood_forced=" + std::string(bloodForced ? "1" : "0") +
                      " medical_validated=" +
                      std::string(validatedHealth ? "1" : "0"));
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " failed to kill " + targetName + ".", true);
                }
              }
            }
          }
        } else if (act.type == ACT_USE_OBJECT) {
          const std::string actorName = SafeCharacterName(npc);
          std::string objectTokenRaw = TrimCopySimple(act.message);
          std::string objectToken = NormalizeUseObjectToken(objectTokenRaw);
          unsigned int requestedSerial = 0;
          ParseUseObjectSerialToken(objectTokenRaw, requestedSerial);

          std::vector<UseObjectCandidate> candidates;
          float searchRange = g_visionRange;
          if (searchRange < 120.0f) {
            searchRange = 120.0f;
          } else if (searchRange > 900.0f) {
            searchRange = 900.0f;
          }
          CollectUseObjectCandidates(thisptr, npc, searchRange, candidates);
          if (candidates.empty()) {
            Log("ACTION_EXEC: USE_OBJECT blocked actor=" + actorName +
                " reason=no_candidates");
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not find any nearby usable objects.", true);
            continue;
          }

          const UseObjectCandidate *bestUsable = nullptr;
          int bestUsableScore = -1;
          const UseObjectCandidate *bestMatched = nullptr;
          int bestMatchedScore = -1;
          for (size_t i = 0; i < candidates.size(); ++i) {
            const UseObjectCandidate &candidate = candidates[i];
            int score =
                ScoreUseObjectCandidate(candidate, objectToken, requestedSerial);
            if (!objectToken.empty() || requestedSerial != 0) {
              if (score <= 0) {
                continue;
              }
            }
            if (!bestMatched || score > bestMatchedScore ||
                (score == bestMatchedScore &&
                 candidate.distance < bestMatched->distance)) {
              bestMatched = &candidate;
              bestMatchedScore = score;
            }
            if (!candidate.usableNow) {
              continue;
            }
            if (!bestUsable || score > bestUsableScore ||
                (score == bestUsableScore &&
                 candidate.distance < bestUsable->distance)) {
              bestUsable = &candidate;
              bestUsableScore = score;
            }
          }

          if (!bestMatched) {
            std::string wanted = objectTokenRaw.empty() ? objectToken : objectTokenRaw;
            if (wanted.empty()) {
              wanted = "that object";
            }
            Log("ACTION_EXEC: USE_OBJECT blocked actor=" + actorName +
                " reason=target_not_found token='" + objectTokenRaw + "'");
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not find a nearby object matching " + wanted +
                    ".",
                true);
            continue;
          }

          if (!bestUsable) {
            std::string reason = ExplainUseObjectUnavailable(*bestMatched);
            Log("ACTION_EXEC: USE_OBJECT blocked actor=" + actorName +
                " object=" + bestMatched->name + " reason=" + reason +
                " serial=" + ToString((unsigned int)bestMatched->serial));
            thisptr->showPlayerAMessage_withLog(
                actorName + " cannot use " + bestMatched->name + " because " +
                    reason + ".",
                true);
            continue;
          }

          if (!bestUsable->building || (uintptr_t)bestUsable->building <= 0x1000) {
            Log("ACTION_EXEC: USE_OBJECT blocked actor=" + actorName +
                " reason=invalid_building_ptr");
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not reach that object right now.", true);
            continue;
          }

          unsigned int actorSerial = 0;
          try {
            actorSerial = npc->getHandle().serial;
          } catch (...) {
            actorSerial = 0;
          }

          try {
            if (npc->dialogue && (uintptr_t)npc->dialogue > 0x1000) {
              npc->dialogue->endDialogue(true);
              npc->dialogue->setInDialog(false);
            }
          } catch (...) {
          }

          if (actorSerial != 0) {
            try {
              ClearFollowTarget(actorSerial);
            } catch (...) {
            }
            try {
              ClearTravelTarget(actorSerial);
            } catch (...) {
            }
          }

          bool passiveCleared = TrySetStandingOrderSafe(
              npc, (MessageForB::StandingOrder)13 /* PASSIVE */, false,
              "USE_OBJECT_CLEAR_PASSIVE");
          bool holdCleared = TrySetStandingOrderSafe(
              npc, (MessageForB::StandingOrder)12 /* HOLD */, false,
              "USE_OBJECT_CLEAR_HOLD");

          try {
            npc->clearAllAIGoals();
          } catch (...) {
          }

          bool jobQueued = false;
          bool goalQueued = false;
          bool destinationQueued = false;
          bool speedBoosted = false;
          try {
            RootObject *subject = (RootObject *)bestUsable->building;
            npc->addJob(bestUsable->taskType, subject, true, false,
                        npc->getPosition());
            jobQueued = true;
          } catch (...) {
            jobQueued = false;
          }
          try {
            npc->addGoal(bestUsable->taskType,
                         (RootObjectBase *)bestUsable->building);
            goalQueued = true;
          } catch (...) {
            goalQueued = false;
          }
          try {
            npc->setDestination(bestUsable->building->getPosition(), false);
            destinationQueued = true;
          } catch (...) {
            destinationQueued = false;
          }
          try {
            CharMovement *movement = npc->getMovement();
            if (movement && (uintptr_t)movement > 0x1000) {
              movement->setDesiredSpeedOrders(RUN);
              movement->setDesiredSpeed(RUN);
              speedBoosted = true;
            }
          } catch (...) {
            speedBoosted = false;
          }

          bool dispatched = jobQueued || goalQueued || destinationQueued;
          if (dispatched) {
            try {
              npc->reThinkCurrentAIAction();
            } catch (...) {
            }
            thisptr->showPlayerAMessage_withLog(
                actorName + " moved to use " + bestUsable->name + ".", true);
            Log("ACTION_EXEC: USE_OBJECT success actor=" + actorName +
                " object=" + bestUsable->name +
                " serial=" + ToString((unsigned int)bestUsable->serial) +
                " class=" + UseObjectClassLabel(bestUsable->classType) +
                " function=" + UseObjectFunctionLabel(bestUsable->functionType) +
                " task=" + ToString((int)bestUsable->taskType) +
                " slot_estimate=" +
                std::string(bestUsable->slotAvailableEstimate ? "1" : "0") +
                " occupied_estimate=" + ToString(bestUsable->occupiedEstimate) +
                " job_queued=" + std::string(jobQueued ? "1" : "0") +
                " goal_queued=" + std::string(goalQueued ? "1" : "0") +
                " dest_queued=" + std::string(destinationQueued ? "1" : "0") +
                " speed_boosted=" + std::string(speedBoosted ? "1" : "0") +
                " passive_cleared=" + std::string(passiveCleared ? "1" : "0") +
                " hold_cleared=" + std::string(holdCleared ? "1" : "0"));
          } else {
            Log("ACTION_EXEC: USE_OBJECT failed actor=" + actorName +
                " object=" + bestUsable->name +
                " serial=" + ToString((unsigned int)bestUsable->serial) +
                " task=" + ToString((int)bestUsable->taskType) +
                " passive_cleared=" + std::string(passiveCleared ? "1" : "0") +
                " hold_cleared=" + std::string(holdCleared ? "1" : "0"));
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not start using " + bestUsable->name + ".",
                true);
          }
        } else if (act.type == ACT_PICKUP_NPC) {
          const std::string actorName = SafeCharacterName(npc);
          Character *pickupTarget = target;
          if ((!pickupTarget || (uintptr_t)pickupTarget <= 0x1000) &&
              thisptr && npc && (uintptr_t)npc > 0x1000) {
            std::string serialProbe = "";
            if (act.target.isValid() && act.target.serial != 0) {
              std::string messageToken = TrimCopySimple(act.message);
              if (!messageToken.empty()) {
                serialProbe = messageToken + "|" + ToString((unsigned int)act.target.serial);
              } else {
                serialProbe = ToString((unsigned int)act.target.serial);
              }
              Character *resolvedBySerial = ResolveCharacterByTargetToken(
                  thisptr, serialProbe, npc);
              if (resolvedBySerial && (uintptr_t)resolvedBySerial > 0x1000) {
                pickupTarget = resolvedBySerial;
              }
            }
            if ((!pickupTarget || (uintptr_t)pickupTarget <= 0x1000) &&
                !TrimCopySimple(act.message).empty()) {
              Character *resolvedByName = ResolveCharacterByTargetToken(
                  thisptr, act.message, npc);
              if (resolvedByName && (uintptr_t)resolvedByName > 0x1000) {
                pickupTarget = resolvedByName;
              }
            }
          }
          std::string targetName =
              pickupTarget ? SafeCharacterName(pickupTarget)
                           : TrimCopySimple(act.message);
          if (targetName.empty()) {
            targetName = "target";
          }
          if (!pickupTarget || (uintptr_t)pickupTarget <= 0x1000) {
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not find a valid pickup target.", true);
            Log("ACTION_EXEC: PICKUP_NPC blocked actor=" + actorName +
                " reason=target_not_found target_token='" + act.message + "'");
          } else if (pickupTarget == npc) {
            thisptr->showPlayerAMessage_withLog(
                actorName + " cannot pick themselves up.", true);
            Log("ACTION_EXEC: PICKUP_NPC blocked actor=" + actorName +
                " reason=self_target");
          } else {
            bool actorAlreadyCarrying = false;
            try {
              actorAlreadyCarrying =
                  npc->isCarryingSomething && npc->carryingObject.isValid();
            } catch (...) {
              actorAlreadyCarrying = false;
            }
            if (actorAlreadyCarrying) {
              thisptr->showPlayerAMessage_withLog(
                  actorName +
                      " is already carrying someone. Use STOP_CARRYING first.",
                  true);
              Log("ACTION_EXEC: PICKUP_NPC blocked actor=" + actorName +
                  " reason=already_carrying");
            } else {
              float actionDistance = -1.0f;
              std::string rangeReason = "";
              if (!ValidateNpcCloseActionRange(npc, pickupTarget,
                                               kNpcCloseActionRangeUnits,
                                               actionDistance, rangeReason)) {
                CloseActionApproachResult approachResult =
                    TryDeferCloseActionUntilInRange(
                        thisptr, npc, pickupTarget, act, "pick up",
                        targetName, actionDistance, rangeReason,
                        kNpcCloseActionRangeUnits);
                if (approachResult == CLOSE_ACTION_APPROACH_DEFERRED) {
                  queueDeferredAction = true;
                  deferActionQueue = true;
                  deferredAction = act;
                } else if (approachResult == CLOSE_ACTION_APPROACH_TIMED_OUT) {
                  const std::string userReason =
                      DescribeCloseActionRangeReasonForUser(rangeReason);
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " could not reach " + targetName +
                          " in time to pick them up (" + userReason + ").",
                      true);
                  Log("ACTION_EXEC: PICKUP_NPC blocked actor=" + actorName +
                      " target=" + targetName +
                      " reason=approach_timeout last_reason=" + rangeReason +
                      " dist=" + ToString(actionDistance) + " max_dist=" +
                      ToString(kNpcCloseActionRangeUnits) +
                      " timeout_ms=" + ToString((int)kNpcCloseActionApproachTimeoutMs));
                } else {
                  const std::string userReason =
                      DescribeCloseActionRangeReasonForUser(rangeReason);
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " cannot pick up " + targetName +
                          " because they are " + userReason + ".",
                      true);
                  Log("ACTION_EXEC: PICKUP_NPC blocked actor=" + actorName +
                      " target=" + targetName + " reason=" + rangeReason +
                      " dist=" + ToString(actionDistance) + " max_dist=" +
                      ToString(kNpcCloseActionRangeUnits));
                }
              } else {
                ResetCloseActionApproachState(npc, act);
                std::string invalidReason = "";
                bool targetDead = false;
                if (!IsPickupNpcTargetValid(thisptr, pickupTarget, invalidReason,
                                            targetDead)) {
                  if (invalidReason.empty()) {
                    invalidReason =
                        "target must be dead, knocked out, unconscious, or "
                        "imprisoned";
                  }
                  thisptr->showPlayerAMessage_withLog(
                      actorName + " cannot pick up " + targetName + ": " +
                          invalidReason + ".",
                      true);
                  Log("ACTION_EXEC: PICKUP_NPC blocked actor=" + actorName +
                      " target=" + targetName + " reason=" + invalidReason);
                } else {
                  bool pickupIssued = false;
                  try {
                    npc->pickupObject(pickupTarget);
                    pickupIssued = true;
                  } catch (...) {
                    pickupIssued = false;
                  }
                  bool pickupSucceeded = false;
                  unsigned int carriedSerial = 0;
                  try {
                    pickupSucceeded =
                        npc->isCarryingSomething && npc->carryingObject.isValid();
                    if (pickupSucceeded) {
                      carriedSerial = npc->carryingObject.serial;
                      unsigned int targetSerial = 0;
                      try {
                        targetSerial = pickupTarget->getHandle().serial;
                      } catch (...) {
                        targetSerial = 0;
                      }
                      if (targetSerial != 0 && carriedSerial != targetSerial) {
                        pickupSucceeded = false;
                      }
                    }
                  } catch (...) {
                    pickupSucceeded = false;
                    carriedSerial = 0;
                  }
                  if (pickupSucceeded) {
                    thisptr->showPlayerAMessage_withLog(
                        actorName + " picked up " + targetName + ".", true);
                    Log("ACTION_EXEC: PICKUP_NPC success actor=" + actorName +
                        " target=" + targetName +
                        " target_dead=" + std::string(targetDead ? "1" : "0") +
                        " target_serial=" + ToString(carriedSerial));
                    try {
                      npc->reThinkCurrentAIAction();
                    } catch (...) {
                    }
                    try {
                      pickupTarget->reThinkCurrentAIAction();
                    } catch (...) {
                    }
                  } else {
                    thisptr->showPlayerAMessage_withLog(
                        actorName + " failed to pick up " + targetName + ".",
                        true);
                    Log("ACTION_EXEC: PICKUP_NPC failed actor=" + actorName +
                        " target=" + targetName +
                        " pickup_issued=" + std::string(pickupIssued ? "1" : "0"));
                  }
                }
              }
            }
          }
        } else if (act.type == ACT_RELEASE) {
          hand carriedHandle;
          bool isCarryingSomething = false;
          try {
            isCarryingSomething =
                npc->isCarryingSomething && npc->carryingObject.isValid();
            if (isCarryingSomething) {
              carriedHandle = npc->carryingObject;
            }
          } catch (...) {
            isCarryingSomething = false;
            carriedHandle = hand();
          }

          if (!isCarryingSomething || !carriedHandle.isValid()) {
            Log("ACTION_EXEC: STOP_CARRYING ignored for " + npc->getName() +
                " (actor is not carrying anything)");
            thisptr->showPlayerAMessage_withLog(
                npc->getName() + " is not carrying anything.", true);
          } else {
            std::string carriedName = SafeRootObjectName(carriedHandle);
            if (carriedName.empty()) {
              carriedName = "their carried target";
            }
            std::string requestedDisplay = TrimCopySimple(act.message);
            if (requestedDisplay.empty() && act.target.isValid()) {
              requestedDisplay = SafeRootObjectName(act.target);
            }
            if (!requestedDisplay.empty()) {
              Log("ACTION_EXEC: STOP_CARRYING ignoring explicit target actor=" +
                  npc->getName() + " requested='" + requestedDisplay +
                  "' carrying='" + carriedName + "'");
            }

            Log("ACTION_EXEC: STOP_CARRYING dropping target actor=" +
                npc->getName() + " carried='" + carriedName + "'");
            npc->dropCarriedObject(false, false);
            npc->clearAllAIGoals();
            npc->addJob(IDLE, NULL, true, false, npc->getPosition());
            npc->addGoal(IDLE, NULL);
            npc->reThinkCurrentAIAction();
            thisptr->showPlayerAMessage_withLog(
                npc->getName() + " put down " + carriedName + ".", true);
          }
        } else if (act.type == ACT_FACTION_RELATIONS) {
          std::string targetToken = act.message;
          auto trim = [](std::string &s) {
            size_t first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) {
              s = "";
              return;
            }
            s.erase(0, first);
            s.erase(s.find_last_not_of(" \t\r\n") + 1);
          };
          trim(targetToken);
          if (!thisptr->factionMgr) {
            Log("ACTION_EXEC: FACTION_RELATIONS skipped (missing faction "
                "manager).");
          } else {
            Faction *actorFaction =
                npc->getFaction() ? npc->getFaction() : npc->owner;
            if (!actorFaction) {
              Log(
                  "ACTION_EXEC: FACTION_RELATIONS skipped (actor faction missing).");
            } else {
              FactionManager *fm = thisptr->factionMgr;
              Faction *targetFaction = nullptr;
              std::string targetFactionName = "";

              if (target) {
                targetFaction =
                    target->getFaction() ? target->getFaction() : target->owner;
                if (targetFaction) {
                  targetFactionName = targetFaction->getName();
                }
              }

              if (!targetFaction) {
                if (targetToken.empty()) {
                  Log("ACTION_EXEC: FACTION_RELATIONS skipped (missing target "
                      "token and no target hand).");
                } else {
                  targetFaction = fm->getFactionByName(targetToken);
                  if (!targetFaction) {
                    targetFaction = fm->getFactionByStringID(targetToken);
                  }
                  if (!targetFaction) {
                    const lektor<Faction *> *all = fm->getAllFactions();
                    if (all) {
                      for (uint32_t i = 0; i < all->count; ++i) {
                        Faction *candidate = all->stuff[i];
                        if (!candidate) {
                          continue;
                        }
                        std::string candidateStringId = "";
                        if (candidate->data && !candidate->data->stringID.empty()) {
                          candidateStringId = candidate->data->stringID;
                        }
                        if (candidate->getName() == targetToken ||
                            candidateStringId == targetToken) {
                          targetFaction = candidate;
                          break;
                        }
                      }
                    }
                  }
                  if (targetFaction) {
                    targetFactionName = targetFaction->getName();
                  }
                }
              }

              if (!targetFaction || targetFaction == actorFaction ||
                  !actorFaction->relations ||
                  !targetFaction->relations) {
                Log("ACTION_EXEC: FACTION_RELATIONS skipped (faction lookup "
                    "failed, same faction, or relations unavailable) target=" +
                    targetToken);
              } else if (act.taskValue == 0) {
                Log("ACTION_EXEC: FACTION_RELATIONS skipped (delta is zero).");
              } else {
                const float kRelationMin = -100.0f;
                const float kRelationMax = 100.0f;
                float delta = (act.taskValue < 0) ? -100.0f : 100.0f;

                float currentRelation =
                    actorFaction->relations->getFactionRelation(targetFaction);
                float newRelation = currentRelation + delta;
                if (newRelation < kRelationMin) {
                  newRelation = kRelationMin;
                } else if (newRelation > kRelationMax) {
                  newRelation = kRelationMax;
                }

                actorFaction->relations->setRelation(targetFaction, newRelation);
                targetFaction->relations->setRelation(actorFaction, newRelation);
                std::string actorFactionName = actorFaction->getName();
                std::string targetName =
                    targetFactionName.empty() ? targetFaction->getName()
                                              : targetFactionName;
                int deltaDisplay = delta < 0.0f ? -100 : 100;
                std::string deltaText =
                    (deltaDisplay > 0 ? "+" : "") + ToString(deltaDisplay);
                Log("ACTION_EXEC: FACTION_RELATIONS " + actorFactionName + " <-> " +
                    targetName + " current=" + ToString(currentRelation) +
                    " delta=" + ToString(delta) +
                    " new=" + ToString(newRelation));
                thisptr->showPlayerAMessage_withLog(
                    "Faction relation changed with " + targetName + " by " +
                        deltaText,
                    true);
              }
            }
          }
        } else if (act.type == ACT_SPAWN_ITEM) {
          Log("ACTION_EXEC: SPAWN_ITEM ignored; action removed.");
          thisptr->showPlayerAMessage_withLog(
              "Spawn item action is currently disabled.", true);
        }

        EnterCriticalSection(&g_uiMutex);
        lockReacquired = true;
      }
      if (!lockReacquired) {
        EnterCriticalSection(&g_uiMutex);
        lockReacquired = true;
      }
      if (queueDeferredAction) {
        g_uiActionQueue.push_front(deferredAction);
      }
      if (blockSpeechQueue) {
        DWORD gateNowTick = GetTickCount();
        speechDelayRemainingScaledMs = static_cast<double>(speechDelayMs);
        speechDelayLastTick = gateNowTick;
        if (speechDelayRemainingScaledMs <= 0.0) {
          g_nextSpeechActionTick = 0;
        } else {
          float gateSpeed = ResolveDialogueGameSpeedMultiplier(thisptr);
          g_nextSpeechActionTick =
              gateNowTick + ResolveSpeechQueueRemainingRealMs(
                                speechDelayRemainingScaledMs, gateSpeed);
        }
        g_lastDialogueTick = gateNowTick;
      }
      if (lockReacquired) {
        LeaveCriticalSection(&g_uiMutex);
      }
      if (!queueDeferredAction && !act.autonomyDecisionId.empty()) {
        ReportAutonomyCatalogActionResult(
            act.autonomyDecisionId, true, "catalog_action_executed");
      }
      if (blockSpeechQueue || deferActionQueue) {
        break;
      }
    }
  }
}
