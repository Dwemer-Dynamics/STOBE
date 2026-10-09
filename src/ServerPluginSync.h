#pragma once

class GameWorld;

// Uploads schema-4 server packages carried by active Kenshi mods under
// <mod>/Stobe/server-plugins/<package>/<version>.dwpkg|.zip to the resolved
// StobeServer through ui/api/plugin_packages.php. Runs once per process.
namespace Stobe {
namespace ServerPluginSync {

// Game thread: snapshots GameWorld::activeMods once and starts the background
// sync. Later calls return immediately.
void OnGameThreadTick(GameWorld *world);

} // namespace ServerPluginSync
} // namespace Stobe
