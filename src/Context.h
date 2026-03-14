#pragma once
#include <string>
#include <vector>

class Character;
class Item;

// Context payload builders consumed by StobeServer.
void GetAllCharacterItems(Character *npc, std::vector<Item *> &outItems);
bool BuildInventorySnapshot(Character *npc, std::string &inventoryJsonOut,
                            std::string &inventoryHashOut, int &itemCountOut);
std::string BuildNpcContextEnvelope(Character *npc, const std::string &type = "npc");
std::string BuildWorldEventDigest();
std::string BuildIdentityBootstrapContext(Character *npc);
std::string GetIdentityFaction(Character *npc);
std::string GetStorageIDFor(Character *npc, const std::string &name,
                            const std::string &factionName);
