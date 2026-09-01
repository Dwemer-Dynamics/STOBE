#pragma once
#include <kenshi/util/hand.h>
#include <string>
#include <windows.h>

class Character;
class GameWorld;
class RootObjectBase;

Character *ResolveLiveCharacter(GameWorld *world, const hand &characterHandle);
bool CharacterHasHacksaw(Character *npc);
bool IsCharacterSkeletonRace(Character *npc);
bool IsCharacterShekRace(Character *npc);
bool TryGetCharacterHornAverage(Character *npc, float &averageOut);
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

// Overlays negotiated faction truces without changing persistent relations.
bool ShouldTreatFactionCeasefireTargetAsNeutral(Character *observer,
                                                RootObjectBase *target);
bool ShouldSuppressFactionCeasefireAttack(Character *first,
                                          Character *second);
bool BreakFactionCeasefireForExplicitAttack(Character *attacker,
                                            Character *target,
                                            const std::string &source);
bool BreakFactionCeasefireForPlayerOrder(Character *attacker,
                                         Character *target,
                                         const std::string &source);
void RejectFactionCeasefireAttack(Character *attacker, Character *target,
                                  const std::string &gate);

// Action execution entry points for work queued from chat/rechat responses.
void PerformLeaveSquad(Character *npc, GameWorld *world,
                       const std::string &originFaction);
void ExecuteQueuedActions(GameWorld *thisptr, int &inventoryTimer);
