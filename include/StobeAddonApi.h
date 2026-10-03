/*
 * StobeAddonApi.h - public native addon interface for Stobe (Kenshi, x64).
 *
 * Plain C so that addons built with any MSVC toolset (Stobe itself requires
 * v100) share one ABI. No C++ or STL object crosses this boundary.
 *
 * Lookup:  HMODULE stobe = GetModuleHandleA("Stobe.dll");
 *          StobeGetApiFn get = (StobeGetApiFn)GetProcAddress(stobe, STOBE_GET_API_EXPORT);
 *          const StobeAddonApiV1 *api = get ? get(STOBE_ADDON_API_VERSION) : 0;
 * A NULL table means Stobe is absent or does not support the requested
 * version; the addon must then stay inert.
 *
 * Ownership: every string or struct passed to Stobe is copied before the call
 * returns. Strings passed to addon callbacks are owned by Stobe and are valid
 * only until the callback returns. Nothing is freed across modules.
 *
 * Threading: every function may be called from any thread unless noted.
 * Operations that touch the game are queued and run on Kenshi's game thread
 * during Stobe's PlayerInterface update. A STOBE_QUEUED return means accepted
 * into that queue, not that dialogue was spoken or the action completed.
 * Addon callbacks are always invoked on the game thread.
 *
 * Identity: actors are addressed by Kenshi handle serial plus the Stobe load
 * generation in which the serial was observed. References from an earlier
 * save/load are rejected as STOBE_E_STALE when the call is made; work queued
 * before a load is discarded at dispatch. There is no name-based routing.
 */
#ifndef STOBE_ADDON_API_H
#define STOBE_ADDON_API_H

#ifdef __cplusplus
extern "C" {
#endif

#define STOBE_ADDON_API_VERSION 1u
#define STOBE_GET_API_EXPORT "Stobe_GetApi"
#define STOBE_CALL __cdecl

typedef unsigned int StobeU32; /* 32 bits on every MSVC target */
typedef StobeU32 StobeAddonId; /* 0 is never a valid id */

/* Return codes. Non-negative values are success. */
#define STOBE_OK 0
#define STOBE_QUEUED 1
#define STOBE_E_INVALID_ARGUMENT (-1)
#define STOBE_E_NOT_REGISTERED (-2)
#define STOBE_E_STALE (-3)        /* actor/request belongs to an earlier load */
#define STOBE_E_NOT_FOUND (-4)    /* actor not loaded, request unknown */
#define STOBE_E_BUSY (-5)         /* Stobe or the player is mid-interaction */
#define STOBE_E_LIMIT (-6)        /* queue, registry or rate limit reached */
#define STOBE_E_CONFLICT (-7)     /* bridge already owned by another addon */
#define STOBE_E_WRONG_THREAD (-8) /* call is only valid on the game thread */
#define STOBE_E_DISABLED (-9)     /* Stobe interaction is switched off */

/* Bounds. Longer strings are rejected, not truncated. */
#define STOBE_MAX_NAME_BYTES 48u  /* addon and bridge names */
#define STOBE_MAX_TEXT_BYTES 1000u

typedef struct StobeActorRef {
  StobeU32 serial;     /* Kenshi hand serial; 0 means "none" */
  StobeU32 generation; /* Stobe load generation stamped by MakeActorRef */
} StobeActorRef;       /* 8 bytes, passed by value */

typedef struct StobeAddonInfo {
  StobeU32 struct_size; /* sizeof(StobeAddonInfo) */
  StobeU32 reserved;    /* must be 0 */
  const char *name;     /* [A-Za-z0-9_.-], 1..48 bytes, unique per process */
  const char *version;  /* free-form, <= 48 bytes, for logs */
} StobeAddonInfo;

/* Runtime flags returned by GetRuntimeFlags. */
#define STOBE_RUNTIME_INTERACTION_ENABLED 0x1u /* Stobe interaction toggle is on */
#define STOBE_RUNTIME_AI_REQUEST_ACTIVE 0x2u   /* a dialogue response is streaming */
#define STOBE_RUNTIME_DIRECTOR_ACTIVE 0x4u     /* a director/continue scene owns dialogue */
#define STOBE_RUNTIME_WORLD_READY 0x8u         /* a loaded world is being updated */

/* Actor flags reported by GetActorState. */
#define STOBE_ACTOR_LOADED 0x1u       /* a live character has this serial */
#define STOBE_ACTOR_ALIVE 0x2u
#define STOBE_ACTOR_CONSCIOUS 0x4u
#define STOBE_ACTOR_PLAYER_SQUAD 0x8u
#define STOBE_ACTOR_IN_STOBE_SPEECH 0x10u /* current talk target or queued Stobe line */

typedef struct StobeActorState {
  StobeU32 struct_size; /* caller sets sizeof(StobeActorState) */
  StobeU32 flags;       /* STOBE_ACTOR_* */
} StobeActorState;

/* Event kinds for SendEvent; both use StobeServer's stream event route. */
#define STOBE_EVENT_INFO 1u         /* "infoaction": context visible to the NPC model */
#define STOBE_EVENT_PLUGIN_STATE 2u /* "addon_state": addon key=value state record */

/* An ExtCmd<Bridge>_<Action>@<parameter> action selected by the server. */
typedef struct StobeActionRequest {
  StobeU32 struct_size;   /* sizeof(StobeActionRequest) as built by Stobe */
  StobeU32 request_id;    /* pass to ReportActionResult */
  StobeActorRef actor;    /* NPC performing the action */
  const char *actor_name; /* display only; never use for routing */
  const char *command;    /* full command, e.g. "ExtCmdParityProbe_Ping" */
  const char *bridge;     /* "ParityProbe" */
  const char *action;     /* "Ping" */
  const char *parameter;  /* text after '@'; may be "" */
} StobeActionRequest;

/* Handler return values. */
#define STOBE_ACTION_REJECTED 0 /* Stobe reports a failure for this request */
#define STOBE_ACTION_ACCEPTED 1 /* addon will call ReportActionResult exactly once */

typedef int(STOBE_CALL *StobeActionHandler)(void *user_data,
                                           const StobeActionRequest *request);
typedef void(STOBE_CALL *StobeGameThreadCallback)(void *user_data);

typedef struct StobeAddonApiV1 {
  StobeU32 struct_size; /* sizeof(StobeAddonApiV1); later versions only append */
  StobeU32 api_version; /* STOBE_ADDON_API_VERSION */
  const char *stobe_version;

  int(STOBE_CALL *RegisterAddon)(const StobeAddonInfo *info, StobeAddonId *out_id);
  /* Removes bridges and drops queued work. Accepted requests not yet reported
   * are reported to the server as failed ("addon unregistered"). */
  int(STOBE_CALL *UnregisterAddon)(StobeAddonId id);

  /* Stamps the current load generation. Validated when the work runs. */
  int(STOBE_CALL *MakeActorRef)(StobeU32 serial, StobeActorRef *out_ref);
  StobeU32(STOBE_CALL *GetRuntimeFlags)(void);
  /* Game thread only (inside a Stobe callback). */
  int(STOBE_CALL *GetActorState)(StobeActorRef actor, StobeActorState *out_state);

  /* Player says text to target through Stobe's normal chat request. */
  int(STOBE_CALL *SendPlayerInput)(StobeAddonId id, StobeActorRef target,
                                   const char *text);
  /* Speaker-locked request without player input. listener.serial 0 lets
   * Stobe choose the listener; direction may be NULL or "". */
  int(STOBE_CALL *RequestContextualResponse)(StobeAddonId id,
                                             StobeActorRef speaker,
                                             StobeActorRef listener,
                                             const char *direction);
  /* kind STOBE_EVENT_INFO: key must be NULL. STOBE_EVENT_PLUGIN_STATE: key
   * required ([A-Za-z0-9_.-], <= 48 bytes). actor.serial may be 0. */
  int(STOBE_CALL *SendEvent)(StobeAddonId id, StobeU32 kind, StobeActorRef actor,
                             const char *key, const char *text);
  /* Interrupts queued and playing Stobe dialogue (new interrupt generation). */
  int(STOBE_CALL *CancelDialogue)(StobeAddonId id);

  int(STOBE_CALL *RegisterActionBridge)(StobeAddonId id, const char *bridge,
                                        StobeActionHandler handler,
                                        void *user_data);
  int(STOBE_CALL *UnregisterActionBridge)(StobeAddonId id, const char *bridge);
  /* message may be NULL. The first report for an accepted request wins;
   * a repeat, or a report after the 10-minute timeout, is STOBE_E_NOT_FOUND. */
  int(STOBE_CALL *ReportActionResult)(StobeAddonId id, StobeU32 request_id,
                                      int succeeded, const char *message);
  /* Runs fn once on the game thread unless the load generation changes. */
  int(STOBE_CALL *QueueGameThreadCallback)(StobeAddonId id,
                                           StobeGameThreadCallback fn,
                                           void *user_data);
} StobeAddonApiV1;

typedef const StobeAddonApiV1 *(STOBE_CALL *StobeGetApiFn)(StobeU32 version);

#ifdef __cplusplus
}
#endif

#endif /* STOBE_ADDON_API_H */
