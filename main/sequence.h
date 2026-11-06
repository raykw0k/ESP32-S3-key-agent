#pragma once
#include <stdbool.h>
#include <stdint.h>
#define MAX_ACTIONS 32
#define MAX_CONFIG_JSON 8192

typedef struct {
    bool is_wait;
    uint8_t key, mods;
    uint32_t hold_ms, wait_ms;
} action_t;
typedef struct {
    bool enabled;
    uint32_t loops; // zero means unlimited
    unsigned count;
    action_t actions[MAX_ACTIONS];
} sequence_t;
typedef bool (*send_report_fn)(void *context, uint8_t mods, uint8_t key);
typedef enum { PHASE_READY, PHASE_HOLD, PHASE_WAIT } phase_t;
typedef struct {
    sequence_t config;
    bool connected, running, held, release_pending;
    unsigned index;
    uint32_t completed, since;
    phase_t phase;
} engine_t;
void sequence_defaults(sequence_t *config);
void engine_init(engine_t *engine, const sequence_t *config);
void engine_stop(engine_t *engine);
bool engine_start(engine_t *engine);
void engine_tick(engine_t *engine, uint32_t now, bool connected, bool ready,
                 send_report_fn send, void *context);

#define LOGIC_COUNT 4
typedef struct {
    engine_t engines[LOGIC_COUNT];
    int owner;
} scheduler_t;
void scheduler_init(scheduler_t *s, const sequence_t configs[LOGIC_COUNT]);
void scheduler_tick(scheduler_t *s, uint32_t now, bool connected, bool ready,
                    send_report_fn send, void *context);
void scheduler_stop_all(scheduler_t *s);
bool scheduler_running(const scheduler_t *s);

bool scheduler_start_all(scheduler_t *s);
