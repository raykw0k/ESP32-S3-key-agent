#include "config_json.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static bool integer(cJSON *obj, const char *key, uint32_t low, uint32_t high, uint32_t *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) ||
        v->valuedouble < low || v->valuedouble > high || floor(v->valuedouble) != v->valuedouble) return false;
    *out = (uint32_t)v->valuedouble;
    return true;
}
bool config_parse(const char *text, sequence_t *out, char *error, unsigned size)
{
    sequence_t c = {0};
    cJSON *root = cJSON_ParseWithOpts(text, NULL, true);
    const char *why = "Invalid configuration format";
    if (!cJSON_IsObject(root)) goto fail;
    cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    if (enabled && !cJSON_IsBool(enabled)) { why = "Enabled must be a boolean"; goto fail; }
    c.enabled = !enabled || cJSON_IsTrue(enabled); // Older saved configs remain enabled.
    if (!integer(root, "loops", 0, 100000, &c.loops)) {
        why = "Loops must be an integer from 0 to 100000"; goto fail;
    }
    cJSON *actions = cJSON_GetObjectItemCaseSensitive(root, "actions");
    if (!cJSON_IsArray(actions) || cJSON_GetArraySize(actions) < 1 ||
        cJSON_GetArraySize(actions) > MAX_ACTIONS) {
        why = "Configure between 1 and 32 actions"; goto fail;
    }
    c.count = cJSON_GetArraySize(actions);
    for (unsigned i = 0; i < c.count; i++) {
        cJSON *item = cJSON_GetArrayItem(actions, i);
        cJSON *type = cJSON_GetObjectItemCaseSensitive(item, "type");
        action_t *a = &c.actions[i];
        if (!cJSON_IsString(type)) goto fail;
        if (!strcmp(type->valuestring, "wait")) a->is_wait = true;
        else if (strcmp(type->valuestring, "key")) goto fail;
        if (!integer(item, "wait", a->is_wait ? 1 : 0, 86400000, &a->wait_ms)) {
            why = "Wait must be 0-86400000 ms (standalone waits: at least 1 ms)"; goto fail;
        }
        if (!a->is_wait) {
            uint32_t key, mods;
            if (!integer(item, "key", 4, 115, &key) ||
                !integer(item, "mods", 0, 15, &mods) ||
                !integer(item, "hold", 20, 10000, &a->hold_ms)) {
                why = "Invalid key parameters: hold must be 20-10000 ms"; goto fail;
            }
            a->key = key; a->mods = mods;
        }
    }
    cJSON_Delete(root);
    *out = c;
    return true;
fail:
    snprintf(error, size, "%s", why);
    cJSON_Delete(root);
    return false;
}
char *config_encode(const sequence_t *c)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    if (!cJSON_AddBoolToObject(root, "enabled", c->enabled)) goto fail;
    if (!cJSON_AddNumberToObject(root, "loops", c->loops)) goto fail;
    cJSON *actions = cJSON_AddArrayToObject(root, "actions");
    if (!actions) goto fail;
    for (unsigned i = 0; i < c->count; i++) {
        const action_t *a = &c->actions[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) goto fail;
        if (!cJSON_AddItemToArray(actions, item)) { cJSON_Delete(item); goto fail; }
        if (!cJSON_AddStringToObject(item, "type", a->is_wait ? "wait" : "key") ||
            !cJSON_AddNumberToObject(item, "wait", a->wait_ms)) goto fail;
        if (!a->is_wait && (!cJSON_AddNumberToObject(item, "key", a->key) ||
            !cJSON_AddNumberToObject(item, "mods", a->mods) ||
            !cJSON_AddNumberToObject(item, "hold", a->hold_ms))) goto fail;
    }
    char *result = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return result;
fail:
    cJSON_Delete(root);
    return NULL;
}
