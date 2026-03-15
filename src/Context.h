#pragma once
#include <string>
#include <vector>

class Character;
class Item;
class Inventory;

// Context payload builders consumed by StobeServer.
void GetAllInventoryItemsFromInventory(Inventory *inv,
                                       std::vector<Item *> &outItems);
void GetAllCharacterItems(Character *npc, std::vector<Item *> &outItems);
bool BuildInventorySnapshot(Character *npc, std::string &inventoryJsonOut,
                            std::string &inventoryHashOut, int &itemCountOut);
bool CaptureTraderInventorySnapshot(Character *npc,
                                    const std::string &reason = "");
bool GetCachedTraderInventorySnapshot(Character *npc,
                                      std::string &inventoryJsonOut,
                                      int &itemCountOut,
                                      int *ageSecondsOut = nullptr);
std::string BuildNpcContextEnvelope(Character *npc, const std::string &type = "npc");
std::string BuildWorldEventDigest();
std::string BuildIdentityBootstrapContext(Character *npc);
std::string GetIdentityFaction(Character *npc);
std::string GetStorageIDFor(Character *npc, const std::string &name,
                            const std::string &factionName);
