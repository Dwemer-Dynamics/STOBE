#include <windows.h>
#include <string>

#include <kenshi/Item.h>
#include <kenshi/gui/InventoryGUI.h>
#include <kenshi/gui/PortraitManager.h>

namespace {

template <typename Fn> static Fn ResolveKenshiExport(const char *name) {
  static Fn fn = nullptr;
  static bool resolved = false;
  if (resolved) {
    return fn;
  }
  resolved = true;

  if (!name || name[0] == '\0') {
    return nullptr;
  }

  HMODULE kenshiLib = GetModuleHandleA("KenshiLib.dll");
  if (!kenshiLib) {
    return nullptr;
  }

  FARPROC thunk = GetProcAddress(kenshiLib, name);
  if (!thunk) {
    return nullptr;
  }
  fn = reinterpret_cast<Fn>(thunk);
  return fn;
}

} // namespace

PortraitManager *PortraitManager::getInstance() {
  typedef PortraitManager *(*Fn)();
  Fn fn = ResolveKenshiExport<Fn>("?getInstance@PortraitManager@@SAPEAV1@XZ");
  if (!fn) {
    return nullptr;
  }
  return fn();
}

PortraitData *PortraitManager::getPortrait(const hand &characterHandle) {
  typedef PortraitData *(*Fn)(PortraitManager *, const hand &);
  Fn fn = ResolveKenshiExport<Fn>(
      "?getPortrait@PortraitManager@@QEAAPEAVPortraitData@@AEBVhand@@@Z");
  if (!fn) {
    return nullptr;
  }
  return fn(this, characterHandle);
}

bool PortraitManager::updatePortraitImage(const hand &characterHandle) {
  typedef bool (*Fn)(PortraitManager *, const hand &);
  Fn fn = ResolveKenshiExport<Fn>(
      "?updatePortraitImage@PortraitManager@@QEAA_NAEBVhand@@@Z");
  if (!fn) {
    return false;
  }
  return fn(this, characterHandle);
}

void InventoryIcon::createIconImage(Item *item, std::string &name, iVector2 &size) {
  typedef void (*Fn)(Item *, std::string &, iVector2 &);
  Fn fn = ResolveKenshiExport<Fn>(
      "?createIconImage@InventoryIcon@@SAXPEAVItem@@AEAV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@AEAViVector2@@@Z");
  if (!fn) {
    name.clear();
    size.x = 0;
    size.y = 0;
    return;
  }
  fn(item, name, size);
}
