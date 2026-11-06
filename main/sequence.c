#include "sequence.h"
#include <string.h>

void sequence_defaults(sequence_t *c)
{
    memset(c, 0, sizeof(*c));
    c->enabled = true;
    c->count = 3;
    for (unsigned i = 0; i < 3; i++)
        c->actions[i] = (action_t){.key = 30 + i, .hold_ms = 40, .wait_ms = 500};
}
void engine_init(engine_t *e, const sequence_t *c)
{
    memset(e, 0, sizeof(*e));
    e->config = *c;
    e->release_pending = true;
}
void engine_stop(engine_t *e)
{
    e->running = false;
    e->release_pending = true;
}
bool engine_start(engine_t *e)
{
    if (!e->connected || !e->config.enabled || !e->config.count) return false;
    e->running = true;
    e->release_pending = true;
    e->index = 0;
    e->completed = 0;
    e->phase = PHASE_READY;
    return true;
}
static void advance(engine_t *e)
{
    if (++e->index == e->config.count) {
        e->index = 0;
        e->completed++;
        if (e->config.loops && e->completed >= e->config.loops) {
            engine_stop(e);
        }
    }
    e->phase = PHASE_READY;
}
void engine_tick(engine_t *e, uint32_t now, bool connected, bool ready,
                 send_report_fn send, void *context)
{
    if (connected != e->connected) {
        e->connected = connected;
        engine_stop(e);
        // Host state will be cleared with a zero report after reconnect.
        e->held = false;
    }
    if (!connected) return;
    if (e->release_pending) {
        if (ready && send(context, 0, 0)) {
            e->release_pending = false;
            e->held = false;
            e->phase = PHASE_READY;
            e->since = now;
        }
        return;
    }
    if (!e->running) return;
    const action_t *a = &e->config.actions[e->index];
    switch (e->phase) {
    case PHASE_READY:
        if (a->is_wait) {
            e->phase = PHASE_WAIT;
            e->since = now;
        } else if (ready && send(context, a->mods, a->key)) {
            e->held = true;
            e->phase = PHASE_HOLD;
            e->since = now;
        }
        break;
    case PHASE_HOLD:
        if ((uint32_t)(now - e->since) >= a->hold_ms && ready && send(context, 0, 0)) {
            e->held = false;
            e->phase = PHASE_WAIT;
            e->since = now;
        }
        break;
    case PHASE_WAIT:
        if ((uint32_t)(now - e->since) >= a->wait_ms) advance(e);
        break;
    }
}

// A report slot belongs to one logic until its release has been accepted.
typedef struct {
    scheduler_t *scheduler;
    unsigned logic;
    bool ready, sent;
    send_report_fn send;
    void *context;
} dispatch_t;
static bool dispatch(void *context, uint8_t mods, uint8_t key)
{
    dispatch_t *d = context;
    scheduler_t *s = d->scheduler;
    if (!key && s->owner != (int)d->logic) return true;
    if (key && s->owner >= 0 && s->owner != (int)d->logic) return false;
    if (!d->ready || d->sent || !d->send(d->context, mods, key)) return false;
    d->sent = true;
    s->owner = key ? (int)d->logic : -1;
    return true;
}
void scheduler_init(scheduler_t *s, const sequence_t configs[LOGIC_COUNT])
{
    s->owner = -1;
    for (unsigned i = 0; i < LOGIC_COUNT; i++) engine_init(&s->engines[i], &configs[i]);
}
void scheduler_stop_all(scheduler_t *s)
{
    for (unsigned i = 0; i < LOGIC_COUNT; i++) engine_stop(&s->engines[i]);
}
bool scheduler_running(const scheduler_t *s)
{
    for (unsigned i = 0; i < LOGIC_COUNT; i++) if (s->engines[i].running) return true;
    return false;
}
void scheduler_tick(scheduler_t *s, uint32_t now, bool connected, bool ready,
                    send_report_fn send, void *context)
{
    if (!connected) s->owner = -1;
    dispatch_t d = {.scheduler=s, .ready=ready, .send=send, .context=context};
    // Make all expired waits eligible before applying priority order.
    for (unsigned i = 0; i < LOGIC_COUNT; i++) {
        engine_t *e = &s->engines[i];
        if (connected && e->connected && e->running && !e->release_pending &&
            e->phase == PHASE_WAIT &&
            (uint32_t)(now-e->since) >= e->config.actions[e->index].wait_ms) advance(e);
    }
    // Finish an existing hold before choosing the highest-priority ready logic.
    int owner = s->owner;
    if (owner >= 0) {
        d.logic = owner;
        engine_tick(&s->engines[owner], now, connected, true, dispatch, &d);
    }
    for (unsigned i = 0; i < LOGIC_COUNT; i++) {
        if ((int)i == owner) continue;
        d.logic = i;
        engine_tick(&s->engines[i], now, connected, true, dispatch, &d);
    }
}

bool scheduler_start_all(scheduler_t *s)
{
    bool any = false;
    for (unsigned i = 0; i < LOGIC_COUNT; i++) {
        engine_t *e = &s->engines[i];
        if (!e->config.enabled) continue;
        any = true;
        if (!e->connected || !e->config.count) return false;
    }
    if (!any) return false;
    for (unsigned i = 0; i < LOGIC_COUNT; i++)
        if (s->engines[i].config.enabled && !s->engines[i].running)
            engine_start(&s->engines[i]);
    return true;
}
