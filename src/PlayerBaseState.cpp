#include "PlayerBaseState.h"

#include "Comm.h"
#include "KenshiTownIdentity.h"
#include "KenshiTownCompat.h"
#include "Utils.h"
#include <cmath>
#include <kenshi/Character.h>
#include <kenshi/Faction.h>
#include <kenshi/GameData.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/PlayerInterface.h>
#include <kenshi/SharedKing.h>
#include <kenshi/util/TimeOfDay.h>
#include <sstream>
#include <windows.h>

namespace Stobe {
namespace PlayerBase {

namespace {
Snapshot g_selectedSnapshot;
bool g_hasSelectedSnapshot = false;
DWORD g_lastCaptureTick = 0;
DWORD g_lastPostTick = 0;
std::string g_lastDigest;
std::string g_sessionId;
bool g_captureFaulted = false;
bool g_captureFaultLogged = false;

bool IsUsablePointer(const void *value) {
  return value && reinterpret_cast<uintptr_t>(value) > 0x1000;
}

float SafeMetric(float value) {
  return _finite(value) && value > 0.0f ? value : 0.0f;
}

int ResolveGameTs(GameWorld *world) {
  if (!IsUsablePointer(world)) {
    return 0;
  }
  const int value =
      static_cast<int>(world->getTimeStamp_inGameHours().getTotalSeconds());
  return value > 0 ? value : 0;
}

bool IsPlayerOwnedTownAt(TownBase *candidate, const Ogre::Vector3 &position) {
  if (!IsUsablePointer(candidate) || candidate->getDataType() != TOWN ||
      !candidate->withinBordersRange(position, 1.0f)) {
    return false;
  }
  Faction *owner = candidate->getFaction();
  return IsUsablePointer(owner) && owner->isThePlayer();
}

TownBase *ResolvePlayerOwnedTown(Character *actor,
                                 const Ogre::Vector3 &position) {
  TownBase *candidate = nullptr;
  if (IsUsablePointer(shou) && IsUsablePointer(shou->townList)) {
    candidate =
        shou->townList->getNearestWithinItsRadius(position, false);
  }
  if (IsPlayerOwnedTownAt(candidate, position)) {
    return candidate;
  }

  candidate = actor->getCurrentTownLocation();
  return IsPlayerOwnedTownAt(candidate, position) ? candidate : nullptr;
}

std::string BuildStableBaseId(Town *town, const Ogre::Vector3 &townPosition) {
  char instanceUid[256] = {};
  int baseIndex = 0;
  int modIndex = 0;
  if (ReadTownInstanceIdentity(town, instanceUid, sizeof(instanceUid),
                               &baseIndex, &modIndex)) {
    return std::string(instanceUid) + ":" + ToString(baseIndex) + ":" +
           ToString(modIndex);
  }

  std::string sourceId = "player_town";
  GameData *townData = town->getOriginalGameData();
  if (IsUsablePointer(townData) && !townData->stringID.empty()) {
    sourceId = townData->stringID;
  }

  const int x = static_cast<int>(
      townPosition.x >= 0.0f ? townPosition.x + 0.5f : townPosition.x - 0.5f);
  const int z = static_cast<int>(
      townPosition.z >= 0.0f ? townPosition.z + 0.5f : townPosition.z - 0.5f);
  return sourceId + ":" + ToString(x) + ":" + ToString(z);
}

int CountPlayerMembersInside(GameWorld *world, TownBase *base) {
  if (!IsUsablePointer(world) || !IsUsablePointer(world->player) ||
      !IsUsablePointer(base)) {
    return 0;
  }

  int count = 0;
  const uint32_t total = world->player->playerCharacters.size();
  for (uint32_t i = 0; i < total; ++i) {
    Character *member = world->player->playerCharacters[i];
    if (!IsUsablePointer(member)) {
      continue;
    }
    const Ogre::Vector3 memberPosition = member->getPosition();
    if (base->withinBordersRange(memberPosition, 1.0f)) {
      ++count;
    }
  }
  return count;
}

bool CaptureUnsafe(GameWorld *world, Character *actor, Snapshot &out) {
  out.Clear();
  if (!IsUsablePointer(world) || !IsUsablePointer(actor)) {
    return true;
  }

  out.gameTs = ResolveGameTs(world);
  out.observerSerial = actor->getHandle().serial;
  out.observerName = actor->getName();

  const Ogre::Vector3 actorPosition = actor->getPosition();
  TownBase *base = ResolvePlayerOwnedTown(actor, actorPosition);
  if (!base) {
    return true;
  }

  Town *town = static_cast<Town *>(base);
  const Ogre::Vector3 townPosition = town->getPosition();
  out.inside = true;
  out.baseId = BuildStableBaseId(town, townPosition);
  out.name = town->getKnownName();
  if (out.name.empty()) {
    out.name = town->getName();
  }
  if (out.name.empty()) {
    out.name = "Player Base";
  }
  out.powerGenerated = SafeMetric(town->getTotalPower());
  out.powerRequired = SafeMetric(town->getRequiredPower());
  out.batteryCharge = SafeMetric(town->getBatteryCharge());
  out.batteryCapacity = SafeMetric(town->getBatteryChargeMax());
  out.batteryDrain = SafeMetric(town->getBatteryDrain());
  out.batteryCharging = SafeMetric(town->getBatteryChargingUpAmount());
  out.batteryMode = town->isBatteryMode();
  out.hasSparePower = town->hasSparePower();
  out.membersInside = CountPlayerMembersInside(world, base);
  out.hasGates = town->_NV_hasGates();
  out.gatesClosed = out.hasGates && town->_NV_gatesAllClosed();
  return true;
}

bool CaptureCandidateSeh(GameWorld *world, Character *actor,
                         Snapshot *candidate) {
  bool sampled = false;
  __try {
    sampled = CaptureUnsafe(world, actor, *candidate);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    sampled = false;
    g_captureFaulted = true;
  }
  return sampled;
}

bool CaptureSeh(GameWorld *world, Character *actor, Snapshot *out) {
  if (g_captureFaulted) {
    return false;
  }

  Snapshot *candidate = new Snapshot();
  const bool sampled = CaptureCandidateSeh(world, actor, candidate);
  if (sampled) {
    *out = *candidate;
    delete candidate;
  }
  // A candidate interrupted by an access violation is intentionally abandoned.
  return sampled;
}

Character *ResolveObservedPlayer(GameWorld *world, Character *selected) {
  if (IsUsablePointer(selected)) {
    Faction *faction = selected->getFaction();
    if (IsUsablePointer(faction) && faction->isThePlayer()) {
      return selected;
    }
  }
  if (!IsUsablePointer(world) || !IsUsablePointer(world->player) ||
      world->player->playerCharacters.size() == 0) {
    return nullptr;
  }
  Character *primary = world->player->playerCharacters[0];
  return IsUsablePointer(primary) ? primary : nullptr;
}

Character *ResolveObservedPlayerSeh(GameWorld *world, Character *selected) {
  Character *observer = nullptr;
  __try {
    observer = ResolveObservedPlayer(world, selected);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    observer = nullptr;
  }
  return observer;
}

std::string EnsureSessionId() {
  if (g_sessionId.empty()) {
    g_sessionId = ToString(static_cast<int>(GetCurrentProcessId())) + "-" +
                  ToString(static_cast<int>(GetTickCount()));
  }
  return g_sessionId;
}

std::string BuildDigest(const Snapshot &snapshot) {
  std::ostringstream stream;
  stream << (snapshot.inside ? "1" : "0") << "|" << snapshot.baseId << "|"
         << snapshot.observerSerial << "|" << snapshot.powerGenerated << "|"
         << snapshot.powerRequired << "|" << snapshot.batteryCharge << "|"
         << snapshot.batteryCapacity << "|" << snapshot.batteryDrain << "|"
         << snapshot.batteryCharging << "|" << (snapshot.batteryMode ? 1 : 0)
         << "|" << (snapshot.hasSparePower ? 1 : 0) << "|"
         << snapshot.membersInside << "|" << (snapshot.hasGates ? 1 : 0)
         << "|" << (snapshot.gatesClosed ? 1 : 0);
  return stream.str();
}

std::string BuildPresenceJson(const Snapshot &snapshot) {
  return "{\"session_id\":\"" + EscapeJSON(EnsureSessionId()) +
         "\",\"observer_serial\":" +
         ToString(static_cast<int>(snapshot.observerSerial)) +
         ",\"observer_name\":\"" + EscapeJSON(snapshot.observerName) +
         "\",\"game_ts\":" + ToString(snapshot.gameTs) +
         ",\"player_base\":" + BuildJson(snapshot) + "}";
}
} // namespace

Snapshot::Snapshot() { Clear(); }

void Snapshot::Clear() {
  inside = false;
  baseId.clear();
  name.clear();
  observerSerial = 0;
  observerName.clear();
  powerGenerated = 0.0f;
  powerRequired = 0.0f;
  batteryCharge = 0.0f;
  batteryCapacity = 0.0f;
  batteryDrain = 0.0f;
  batteryCharging = 0.0f;
  batteryMode = false;
  hasSparePower = false;
  membersInside = 0;
  hasGates = false;
  gatesClosed = false;
  gameTs = 0;
}

bool Capture(GameWorld *world, Character *actor, Snapshot &out) {
  out.Clear();
  const bool sampled = CaptureSeh(world, actor, &out);
  if (!sampled && g_captureFaulted && !g_captureFaultLogged) {
    g_captureFaultLogged = true;
    Log("PLAYER_BASE: capture faulted; disabled until the next world load.");
  }
  return sampled && out.inside;
}

std::string BuildJson(const Snapshot &snapshot) {
  if (!snapshot.inside) {
    return "{\"inside\":false,\"observed_game_ts\":" +
           ToString(snapshot.gameTs) + "}";
  }

  return "{\"inside\":true,\"base_id\":\"" +
         EscapeJSON(snapshot.baseId) + "\",\"name\":\"" +
         EscapeJSON(snapshot.name) + "\",\"power_generated\":" +
         ToString(snapshot.powerGenerated) + ",\"power_required\":" +
         ToString(snapshot.powerRequired) + ",\"battery_charge\":" +
         ToString(snapshot.batteryCharge) + ",\"battery_capacity\":" +
         ToString(snapshot.batteryCapacity) + ",\"battery_drain\":" +
         ToString(snapshot.batteryDrain) + ",\"battery_charging\":" +
         ToString(snapshot.batteryCharging) + ",\"battery_mode\":" +
         std::string(snapshot.batteryMode ? "true" : "false") +
         ",\"has_spare_power\":" +
         std::string(snapshot.hasSparePower ? "true" : "false") +
         ",\"members_inside\":" + ToString(snapshot.membersInside) +
         ",\"has_gates\":" +
         std::string(snapshot.hasGates ? "true" : "false") +
         ",\"gates_closed\":" +
         std::string(snapshot.gatesClosed ? "true" : "false") +
         ",\"observed_game_ts\":" + ToString(snapshot.gameTs) + "}";
}

void Update(GameWorld *world, Character *selected) {
  const DWORD now = GetTickCount();
  if (g_lastCaptureTick != 0 && now - g_lastCaptureTick < 1000) {
    return;
  }
  g_lastCaptureTick = now;

  Snapshot current;
  Character *observer = ResolveObservedPlayerSeh(world, selected);
  Capture(world, observer, current);
  g_selectedSnapshot = current;
  g_hasSelectedSnapshot = true;

  const std::string digest = BuildDigest(current);
  const bool changed = digest != g_lastDigest;
  if (!changed && g_lastPostTick != 0 && now - g_lastPostTick < 10000) {
    return;
  }

  AsyncPostToStobeSerial(L"/player_base_state", BuildPresenceJson(current));
  g_lastDigest = digest;
  g_lastPostTick = now;
}

bool GetSelectedSnapshot(Snapshot &out) {
  if (!g_hasSelectedSnapshot) {
    return false;
  }
  out = g_selectedSnapshot;
  return true;
}

void Reset() {
  g_selectedSnapshot.Clear();
  g_hasSelectedSnapshot = false;
  g_lastCaptureTick = 0;
  g_lastPostTick = 0;
  g_lastDigest.clear();
  g_sessionId.clear();
  g_captureFaulted = false;
  g_captureFaultLogged = false;
}

} // namespace PlayerBase
} // namespace Stobe
