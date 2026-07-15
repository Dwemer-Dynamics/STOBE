#pragma once

class GameWorld;

// Starts the server polling worker. It never reads or writes Kenshi objects.
void StartAutonomyController();

// Runs from PlayerInterface::update after the world has stabilized.
void UpdateAutonomyController(GameWorld *world);

// Invalidates the runtime binding on save/load and requires an explicit resume.
void ResetAutonomyController(const char *reason);
