#pragma once

class Character;
class GameWorld;

// Runs on the hooked PlayerInterface update thread after the world is stable.
void UpdateAutonomySafetyProbe(GameWorld *world, Character *selectedCharacter);

// Invalidates the runtime target binding without repeating a consumed command.
void ResetAutonomySafetyProbe(const char *reason);

