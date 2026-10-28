#pragma once

#include <string>

class GameWorld;

// Starts the server polling worker. It never reads or writes Kenshi objects.
void StartAutonomyController();

// Runs from PlayerInterface::update after the world has stabilized.
void UpdateAutonomyController(GameWorld *world);

// Invalidates the runtime binding on save/load and requires an explicit resume.
void ResetAutonomyController(const char *reason);

// Called by the main-thread queued-action executor after it verifies the
// postcondition for an action produced by an autonomy decision.
void ReportAutonomyActionExecutionResult(const std::string &decisionId,
                                         bool success,
                                         const std::string &reason);
