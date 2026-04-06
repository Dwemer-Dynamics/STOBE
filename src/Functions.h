#pragma once
#include <string>
#include <windows.h>

class Character;
class GameWorld;

bool CharacterHasHacksaw(Character *npc);
bool IsCharacterSkeletonRace(Character *npc);
bool IsRemoveLimbTargetValid(GameWorld *world, Character *target,
                             std::string &reasonOut);
bool IsTakeItemLootTargetValid(GameWorld *world, Character *target,
                               std::string &reasonOut, bool &isDeadOut);
bool ResolveCharacterDrinkItemMatch(Character *npc, const std::string &rawQuery,
                                    std::string &matchedNameOut);
bool ResolveCharacterDrugItemMatch(Character *npc, const std::string &rawQuery,
                                   std::string &matchedNameOut);
bool GetCharacterDrunkPromptState(Character *npc, int &levelOut,
                                  bool &isDrunkOut, std::string &statusOut,
                                  int &secondsRemainingOut);
bool GetCharacterDrugPromptState(Character *npc, bool &isHighOut,
                                 std::string &statusOut,
                                 int &secondsRemainingOut,
                                 float &hungerRateMultiplierOut);
void UpdateNpcDrunkStates(GameWorld *world);
void UpdateNpcDrugStates(GameWorld *world);

// Action execution entry points for work queued from chat/rechat responses.
void PerformLeaveSquad(Character *npc, GameWorld *world,
                       const std::string &originFaction);
void ExecuteQueuedActions(GameWorld *thisptr, int &inventoryTimer);
