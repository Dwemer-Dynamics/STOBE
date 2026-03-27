#include "Functions.h"
#include "AudioPlayback.h"
#include "Context.h"
#include "Globals.h"
#include "KenshiBuildingCompat.h"
#include "Utils.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <core/Functions.h>
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
#include <ogre/OgreColourValue.h>
#include <map>
#include <vector>

std::string TrimCopySimple(const std::string &value);
void ApplyDrunkKnockoutPulse(Character *npc);
int ResolveCurrentGameTimestampSeconds(GameWorld *world);
std::string SafeBuildingName(Building *building);

const int kDrunkLevelDurationSeconds = 5 * 60 * 60;
const int kDrunkPassoutDurationSeconds = 2 * 60 * 60;
const DWORD kDrunkKnockoutPulseMs = 1000;
const int kDrugHighDurationSeconds = 5 * 60 * 60;
const float kDrugHungerMultiplier = 1.5f;
const float kDrugExtraHungerMultiplier = kDrugHungerMultiplier - 1.0f;

struct NpcDrunkState {
  int level;
  int levelExpiresGameTs;
  int passedOutUntilGameTs;
  DWORD nextKnockoutPulseTick;

  NpcDrunkState()
      : level(0), levelExpiresGameTs(0), passedOutUntilGameTs(0),
        nextKnockoutPulseTick(0) {}
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
static std::map<unsigned int, NpcDrugState> g_npcDrugStates;

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
  const float kNearDistanceUnits = 40.0f;
  float farDistanceUnits = g_shoutRadius;
  if (farDistanceUnits < 120.0f) {
    farDistanceUnits = 120.0f;
  } else if (farDistanceUnits > 1200.0f) {
    farDistanceUnits = 1200.0f;
  }

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
    ApplyDrunkKnockoutPulse(target);
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
    bool queryMatches = itemToken.find(queryToken) != std::string::npos ||
                        queryToken.find(itemToken) != std::string::npos;
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

void ApplyDrunkKnockoutPulse(Character *npc) {
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
    // Force an immediate KO state for drunken pass-out.
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
    ApplyDrunkKnockoutPulse(npc);
  }
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

  UpdateNpcDrunkStates(thisptr);
  UpdateNpcDrugStates(thisptr);

  if (TryEnterCriticalSection(&g_uiMutex)) {
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
        float speed = thisptr->getFrameSpeedMultiplier();
        if (speed < 1.0f) {
          speed = 1.0f;
        }
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

      Character *npc = ResolveLiveCharacter(thisptr, act.actor);
      Character *target = ResolveLiveCharacter(thisptr, act.target);
      if (!npc && act.type != ACT_NOTIFY && act.type != ACT_SAY &&
          act.type != ACT_PLAY_TTS) {
        Log("ACTION_EXEC: Failed to resolve actor serial=" +
            ToString((int)act.actor.serial) +
            " action_type=" + ToString((int)act.type));
      }

      if (act.type == ACT_NOTIFY) {
        thisptr->showPlayerAMessage_withLog(act.message, true);
        bool hasTtsClip = g_ttsEnabled && !act.ttsHash.empty();
        bool playbackQueued = false;
        if (hasTtsClip) {
          playbackQueued = QueueTtsPlayback(act.ttsHash);
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
        Log("ACTION_EXEC: SAY [" + target->getName() + "]: " + act.message +
            (isPC ? " (PC)" : " (NPC)"));
        try {
          // If npc is in vanilla dialogue state, bubbles are often suppressed.
          // Force a reset if they seem stuck.
          if (!isPC && target->dialogue && (uintptr_t)target->dialogue > 0x1000) {
            target->dialogue->endDialogue(true);
            target->dialogue->setInDialog(false);
          }

          // Primary method: sayALine (supports multiple lines/delays)
          target->sayALine(act.message, !isPC);
          // Force native floating text path as well. This is the most reliable
          // way to surface overhead speech bubbles across player and NPC actors.
          target->say(act.message);
          forcedSayFallback = true;

          // ðŸš¨ FIX: Speech bubbles disappear too fast at high game speeds.
          // Scale the timer by game speed to keep real-time duration stable.
          // When TTS metadata is present, sync bubble lifetime to clip length.
          if (target->dialogue && (uintptr_t)target->dialogue > 0x1000) {
            target->dialogue->npcReplyText = act.message;
            float speed = thisptr->getFrameSpeedMultiplier();
            if (speed < 1.0f)
              speed = 1.0f;
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
            float duration = baseDuration * speed;
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
                QueueTtsPlayback(act.ttsHash, ttsVolumePercent, act.target.serial);
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
      } else if (act.type == ACT_PLAY_TTS && target &&
                 !IsCharacterUnavailableForDialogue(target)) {
        float appliedBubbleDuration = 0.0f;
        if (target->dialogue && (uintptr_t)target->dialogue > 0x1000) {
          float speed = thisptr->getFrameSpeedMultiplier();
          if (speed < 1.0f)
            speed = 1.0f;
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
          float duration = baseDuration * speed;
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
                QueueTtsPlayback(act.ttsHash, ttsVolumePercent, act.target.serial);
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
            PerformLeaveSquad(npc, thisptr, "");
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
          Character *player =
              (thisptr->player && thisptr->player->playerCharacters.size() > 0)
                  ? thisptr->player->playerCharacters[0]
                  : nullptr;
          if (player) {
            std::vector<Item *> pItems;
            GetAllCharacterItems(player, pItems);
            std::string targetName = act.message;
            size_t fnot = targetName.find_first_not_of(" \t\n\r\"'");
            if (fnot != std::string::npos) {
              targetName.erase(0, fnot);
              size_t lnot = targetName.find_last_not_of(" \t\n\r\"'");
              if (lnot != std::string::npos)
                targetName.erase(lnot + 1);
            }
            std::transform(targetName.begin(), targetName.end(),
                           targetName.begin(), ::tolower);

            for (uint32_t i = 0; i < pItems.size(); ++i) {
              std::string itemName = pItems[i]->getName();
              std::transform(itemName.begin(), itemName.end(), itemName.begin(),
                             ::tolower);
              if (itemName.find(targetName) != std::string::npos) {
                Log("ACTION_EXEC: Taking item: " + pItems[i]->getName());
                if (pItems[i]->isEquipped)
                  player->unequipItem(pItems[i]->inventorySection, pItems[i]);
                Inventory *inv = pItems[i]->getInventory();
                if (!inv)
                  inv = player->getInventory();
                Item *detached =
                    inv ? inv->removeItemDontDestroy_returnsItem(
                              pItems[i], pItems[i]->quantity, false)
                        : nullptr;
                npc->giveItem(detached ? detached : pItems[i], true, false);
                thisptr->showPlayerAMessage_withLog(npc->getName() + " took " +
                                                        pItems[i]->getName() +
                                                        " from you.",
                                                    true);
                npc->reThinkCurrentAIAction();
                inventoryTimer = 999;
                break;
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
              bool queryMatches =
                  itemToken.find(itemQueryToken) != std::string::npos ||
                  itemQueryToken.find(itemToken) != std::string::npos;
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
          std::string targetName =
              target ? SafeCharacterName(target) : TrimCopySimple(act.message);
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
                " reason=target_not_found target_token='" + act.message + "'");
            thisptr->showPlayerAMessage_withLog(
                actorName + " could not find a valid limb-removal target.", true);
          } else {
            std::string invalidReason = "";
            if (!IsRemoveLimbTargetValid(thisptr, target, invalidReason)) {
              if (invalidReason.empty()) {
                invalidReason = "target is not in a valid state";
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
                    " target=" + targetName + " reason=invalid_limb_code code=" +
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
                          actorName + " failed to remove " + targetName + "'s " +
                              limbName + ".",
                          true);
                    }
                  }
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
            const std::string normalizedCarriedName =
                NormalizeCarryTargetToken(carriedName);

            bool explicitTargetProvided =
                act.target.isValid() || !TrimCopySimple(act.message).empty();
            bool targetMatches = true;
            if (explicitTargetProvided) {
              targetMatches = false;
              if (act.target.isValid() &&
                  act.target.serial == carriedHandle.serial) {
                targetMatches = true;
              }

              if (!targetMatches && act.target.isValid()) {
                const std::string requestedHandleName =
                    NormalizeCarryTargetToken(SafeRootObjectName(act.target));
                if (!requestedHandleName.empty() &&
                    !normalizedCarriedName.empty() &&
                    (requestedHandleName == normalizedCarriedName ||
                     requestedHandleName.find(normalizedCarriedName) !=
                         std::string::npos ||
                     normalizedCarriedName.find(requestedHandleName) !=
                         std::string::npos)) {
                  targetMatches = true;
                }
              }

              if (!targetMatches) {
                const std::string requestedByName =
                    NormalizeCarryTargetToken(act.message);
                if (!requestedByName.empty() && !normalizedCarriedName.empty() &&
                    (requestedByName == normalizedCarriedName ||
                     requestedByName.find(normalizedCarriedName) !=
                         std::string::npos ||
                     normalizedCarriedName.find(requestedByName) !=
                         std::string::npos)) {
                  targetMatches = true;
                }
              }
            }

            if (!targetMatches) {
              std::string requestedDisplay = TrimCopySimple(act.message);
              if (requestedDisplay.empty()) {
                requestedDisplay = SafeRootObjectName(act.target);
              }
              Log("ACTION_EXEC: STOP_CARRYING target mismatch actor=" +
                  npc->getName() + " carrying='" + carriedName +
                  "' requested='" + requestedDisplay + "'");
              if (!requestedDisplay.empty()) {
                thisptr->showPlayerAMessage_withLog(
                    npc->getName() + " is carrying " + carriedName + ", not " +
                        requestedDisplay + ".",
                    true);
              } else {
                thisptr->showPlayerAMessage_withLog(
                    npc->getName() + " is carrying " + carriedName + ".", true);
              }
            } else {
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
      if (blockSpeechQueue) {
        g_nextSpeechActionTick = GetTickCount() + speechDelayMs;
        g_lastDialogueTick = GetTickCount();
      }
      if (lockReacquired) {
        LeaveCriticalSection(&g_uiMutex);
      }
      if (blockSpeechQueue) {
        break;
      }
    }
  }
}
