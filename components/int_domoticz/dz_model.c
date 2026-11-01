#include "dz_model.h"

#include <stdio.h>
#include <string.h>

bool dz_model_init(dz_model_t *m, dz_entry_t *entries, size_t cap, dz_device_t *stage, size_t stage_cap,
                   const dz_model_cfg_t *cfg) {
  if (!m || !entries || !stage || !cap || !stage_cap) return false;
  memset(m, 0, sizeof *m);
  m->e = entries;
  m->cap = cap;
  m->stage = stage;
  m->stage_cap = stage_cap;
  if (cfg) m->cfg = *cfg;
  if (!m->cfg.ttl_ms) m->cfg.ttl_ms = 300000;
  if (!m->cfg.old_update_s) m->cfg.old_update_s = 21600;
  m->server_epoch_s = -1;
  m->mu = ru_mutex_create();
  return m->mu != NULL;
}

void dz_model_deinit(dz_model_t *m) {
  if (m && m->mu) ru_mutex_destroy(m->mu);
  if (m) m->mu = NULL;
}

static bool is_scene_dev(const dz_device_t *d) { return dz_cap_is_scene_ns(d->cap); }

bool dz_status_is_on(const dz_device_t *d) {
  const char *s = d->status;
  return !strcmp(s, "On") || !strncmp(s, "Set Level", 9) || !strcmp(s, "Mixed");
}

bool dz_time_parse(const char *s, int64_t *out) {
  if (!s || strlen(s) != 19 || s[4] != '-' || s[7] != '-' || s[10] != ' ' || s[13] != ':' || s[16] != ':') return false;
  int v[6], pos[6] = {0, 5, 8, 11, 14, 17}, len[6] = {4, 2, 2, 2, 2, 2};
  for (int i = 0; i < 6; i++) {
    v[i] = 0;
    for (int k = 0; k < len[i]; k++) {
      char c = s[pos[i] + k];
      if (c < '0' || c > '9') return false;
      v[i] = v[i] * 10 + (c - '0');
    }
  }
  if (v[0] < 1970 || v[1] < 1 || v[1] > 12 || v[2] < 1 || v[2] > 31 || v[3] > 23 || v[4] > 59 || v[5] > 60) return false;
  /* days from civil (Howard Hinnant) */
  int64_t y = v[0] - (v[1] <= 2), era = y / 400, yoe = y - era * 400;
  int64_t doy = (153 * (v[1] + (v[1] > 2 ? -3 : 9)) + 2) / 5 + v[2] - 1;
  int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  int64_t days = era * 146097 + doe - 719468;
  *out = days * 86400 + v[3] * 3600 + v[4] * 60 + v[5];
  return true;
}

static void make_val(const dz_device_t *d, uint32_t now, uint32_t ttl, ru_val_t *v) {
  *v = ru_val_unknown();
  switch (d->cap) {
    case DZ_CAP_THERMOSTAT:
      /* RU_UNIT_NONE: the unit (TempSign or vunit) belongs to the screen, which reads it from the model */
      if (d->has_setpoint) *v = ru_val_make(d->setpoint, RU_UNIT_NONE, now, ttl);
      break;
    case DZ_CAP_TEMP:
    case DZ_CAP_WEATHER:
    case DZ_CAP_SENSOR: {
      /* primary reading: the room temperature if there is one, else the first number (never an auxiliary temp
       * ahead of a real one). Unit-less on purpose; the screen adds TempSign / WindSign / the reading's own unit. */
      const dz_reading_t *p = dz_dev_room_temp(d);
      for (int i = 0; !p && i < d->n_rd; i++) if (d->rd[i].has && d->rd[i].kind != DZ_RK_TEXT) p = &d->rd[i];
      if (p) *v = ru_val_make(p->v, RU_UNIT_NONE, now, ttl);
      break;
    }
    case DZ_CAP_ENERGY:
      ru_val_parse(d->data, now, ttl, v);
      break;
    case DZ_CAP_DIMMER:
      /* An Off dimmer keeps a stale stored Level (research 7.1): show state, not Level. */
      if (d->status[0] && d->has_level && d->max_level > 0) {
        float pct = dz_status_is_on(d) ? 100.0f * (float)d->level / (float)d->max_level : 0.0f;
        *v = ru_val_make(pct, RU_UNIT_PCT, now, ttl);
      }
      break;
    case DZ_CAP_SELECTOR:
      if (d->has_level) *v = ru_val_make((float)d->level_int, RU_UNIT_NONE, now, ttl);
      break;
    case DZ_CAP_ONOFF:
    case DZ_CAP_SCENE:
      if (d->status[0]) *v = ru_val_make(dz_status_is_on(d) ? 1.0f : 0.0f, RU_UNIT_NONE, now, ttl);
      break;
    case DZ_CAP_GROUP: /* 1 = all on, 0 = all off, 0.5 = MIXED (members differ). Read dz_device_state() for the label. */
      if (d->status[0]) {
        dz_state_t st = dz_device_state(d);
        if (st != DZ_STATE_UNKNOWN) *v = ru_val_make(st == DZ_STATE_ON ? 1.0f : st == DZ_STATE_MIXED ? 0.5f : 0.0f, RU_UNIT_NONE, now, ttl);
      }
      break;
    case DZ_CAP_MODE:
      if (d->has_mode) *v = ru_val_make((float)d->mode, RU_UNIT_NONE, now, ttl);
      break;
    case DZ_CAP_BLINDS:
    case DZ_CAP_PUSH:
    case DZ_CAP_LOCK:
    case DZ_CAP_STATUS:
    case DZ_CAP_READONLY:
      break;
  }
}

static bool sensor_cap(dz_cap_t c) {
  return c == DZ_CAP_TEMP || c == DZ_CAP_WEATHER || c == DZ_CAP_ENERGY || c == DZ_CAP_THERMOSTAT || c == DZ_CAP_SENSOR;
}

static ru_val_state_t entry_state(const dz_model_t *m, const dz_entry_t *e, uint32_t now) {
  const dz_device_t *d = &e->dev;
  if ((d->cap == DZ_CAP_TEMP || d->cap == DZ_CAP_THERMOSTAT) && !e->val.valid) return RU_VAL_UNKNOWN;
  if (d->timeout) return RU_VAL_STALE;
  if (now - e->ts_ms > m->cfg.ttl_ms) return RU_VAL_STALE;
  if (sensor_cap(d->cap) && m->server_epoch_s >= 0 && m->cfg.old_update_s != UINT32_MAX) {
    int64_t lu;
    if (dz_time_parse(d->last_update, &lu)) {
      int64_t srv_now = m->server_epoch_s + (int64_t)((uint32_t)(now - m->server_sync_ms) / 1000u);
      if (srv_now - lu > (int64_t)m->cfg.old_update_s) return RU_VAL_STALE;
    }
  }
  return RU_VAL_OK;
}

void dz_model_begin(dz_model_t *m) {
  ru_mutex_lock(m->mu);
  m->stage_n = 0;
  m->stage_dropped = 0;
  ru_mutex_unlock(m->mu);
}

bool dz_model_stage_cb(const dz_device_t *d, void *user) {
  dz_model_t *m = user;
  ru_mutex_lock(m->mu);
  if (m->stage_n < m->stage_cap) m->stage[m->stage_n++] = *d;
  else m->stage_dropped++;
  ru_mutex_unlock(m->mu);
  return true;
}

void dz_model_abort(dz_model_t *m) {
  ru_mutex_lock(m->mu);
  m->stage_n = 0;
  m->stage_dropped = 0;
  m->aborts++;
  ru_mutex_unlock(m->mu);
}

static ptrdiff_t find_locked(const dz_model_t *m, const char *idx, bool scene) {
  for (size_t i = 0; i < m->n; i++)
    if (m->e[i].is_scene == scene && !strcmp(m->e[i].dev.idx, idx)) return (ptrdiff_t)i;
  return -1;
}

int dz_model_commit(dz_model_t *m, dz_commit_t mode, const char *server_time, uint32_t now_ms) {
  int changed = 0;
  ru_mutex_lock(m->mu);
  bool scenes_src = mode == DZ_COMMIT_FULL_SCENES;
  bool complete = m->stage_dropped == 0; /* a truncated stage must never trigger removals */
  m->overflow += (uint32_t)m->stage_dropped;
  uint32_t gen = m->generation + 1;
  /* upsert */
  for (size_t s = 0; s < m->stage_n; s++) {
    const dz_device_t *d = &m->stage[s];
    bool sc = is_scene_dev(d);
    ptrdiff_t at = find_locked(m, d->idx, sc);
    dz_entry_t *e;
    if (at < 0) {
      if (m->n >= m->cap) { m->overflow++; complete = false; continue; }
      e = &m->e[m->n++];
      memset(e, 0, sizeof *e);
      changed++;
      e->dev = *d;
      e->changed_gen = gen;
    } else {
      e = &m->e[at];
      if (memcmp(&e->dev, d, sizeof *d) != 0) { e->dev = *d; e->changed_gen = gen; changed++; }
    }
    e->is_scene = sc;
    e->from_scenes = scenes_src;
    e->ts_ms = now_ms;
    make_val(&e->dev, now_ms, m->cfg.ttl_ms, &e->val);
    if (e->dev.timeout) e->val.ts_mono_ms = now_ms - m->cfg.ttl_ms - 1; /* HaveTimeout: born stale */
  }
  if (mode == DZ_COMMIT_DELTA || mode == DZ_COMMIT_FULL_DEVICES || mode == DZ_COMMIT_FULL_SCENES) {
    bool full = mode != DZ_COMMIT_DELTA;
    for (size_t i = 0; i < m->n;) {
      dz_entry_t *e = &m->e[i];
      bool in_scope = scenes_src ? e->from_scenes : !e->from_scenes;
      bool seen = false;
      for (size_t s = 0; s < m->stage_n && !seen; s++)
        seen = dz_cap_is_scene_ns(m->stage[s].cap) ? (e->is_scene && !strcmp(e->dev.idx, m->stage[s].idx))
                                                : (!e->is_scene && !strcmp(e->dev.idx, m->stage[s].idx));
      if (in_scope && !seen) {
        if (full && complete) {
          memmove(e, e + 1, (m->n - i - 1) * sizeof *e);
          m->n--;
          changed++;
          continue;
        }
        if (!full) {
          /* delta: server confirmed it unchanged. Refresh the clock and keep the value's timestamp honest. */
          e->ts_ms = now_ms;
          if (e->val.valid && !e->dev.timeout) e->val.ts_mono_ms = now_ms;
        }
      }
      i++;
    }
  }
  int64_t st;
  if (server_time && dz_time_parse(server_time, &st) && mode != DZ_COMMIT_PARTIAL) {
    m->server_epoch_s = st;
    m->server_sync_ms = now_ms;
  }
  m->stage_n = 0;
  m->stage_dropped = 0;
  m->commits++;
  if (changed) { m->generation = gen; m->changes += (uint32_t)changed; }
  ru_mutex_unlock(m->mu);
  return changed;
}

dz_err_t dz_model_apply(dz_model_t *m, const char *json, size_t len, dz_commit_t mode, uint32_t now_ms, dz_result_t *res,
                        int *changed) {
  dz_result_t local;
  if (!res) res = &local;
  dz_model_begin(m);
  dz_err_t e = dz_parse_response(json, len, dz_model_stage_cb, m, res);
  if (e == DZ_OK) {
    int c = dz_model_commit(m, mode, res->server_time, now_ms);
    if (changed) *changed = c;
  } else {
    dz_model_abort(m);
    if (changed) *changed = 0;
  }
  return e;
}

static void out_entry(const dz_model_t *m, const dz_entry_t *e, dz_device_t *dev, ru_val_t *val, ru_val_state_t *st,
                      bool *scene, uint32_t now) {
  if (dev) *dev = e->dev;
  if (val) *val = e->val;
  if (st) *st = entry_state(m, e, now);
  if (scene) *scene = e->is_scene;
}

bool dz_model_get(dz_model_t *m, const char *idx, bool scene, dz_device_t *dev, ru_val_t *val, ru_val_state_t *st,
                  uint32_t now_ms) {
  ru_mutex_lock(m->mu);
  ptrdiff_t at = idx ? find_locked(m, idx, scene) : -1;
  if (at >= 0) out_entry(m, &m->e[at], dev, val, st, NULL, now_ms);
  else if (st) *st = RU_VAL_UNKNOWN;
  ru_mutex_unlock(m->mu);
  return at >= 0;
}

bool dz_model_at(dz_model_t *m, size_t i, dz_device_t *dev, ru_val_t *val, ru_val_state_t *st, bool *scene,
                 uint32_t now_ms) {
  ru_mutex_lock(m->mu);
  bool ok = i < m->n;
  if (ok) out_entry(m, &m->e[i], dev, val, st, scene, now_ms);
  ru_mutex_unlock(m->mu);
  return ok;
}

#define GETTER(name, field, type) \
  type name(dz_model_t *m) { ru_mutex_lock(m->mu); type v = m->field; ru_mutex_unlock(m->mu); return v; }
size_t dz_model_count(dz_model_t *m) { ru_mutex_lock(m->mu); size_t v = m->n; ru_mutex_unlock(m->mu); return v; }
GETTER(dz_model_overflow, overflow, uint32_t)
GETTER(dz_model_changes, changes, uint32_t)
GETTER(dz_model_generation, generation, uint32_t)

bool dz_model_units(dz_model_t *m, dz_units_t *out) {
  ru_mutex_lock(m->mu);
  dz_units_t u = m->units;
  ru_mutex_unlock(m->mu);
  if (out) *out = u;
  return u.have_temp || u.have_wind || u.have_currency || u.have_temp_scale || u.have_wind_scale;
}

void dz_model_set_units(dz_model_t *m, const dz_units_t *u) {
  ru_mutex_lock(m->mu);
  if (u) m->units = *u; else memset(&m->units, 0, sizeof m->units);
  ru_mutex_unlock(m->mu);
}

void dz_units_temp_text(const dz_units_t *u, char *out, size_t n) {
  if (!n) return;
  out[0] = 0;
  if (u && u->have_temp && (u->temp[0] == 'C' || u->temp[0] == 'F') && !u->temp[1]) snprintf(out, n, "\xC2\xB0%s", u->temp);
}

bool dz_model_plan_temp(dz_model_t *m, const dz_device_t *d, dz_device_t *sensor, ru_val_t *val, ru_val_state_t *st,
                        uint32_t now_ms) {
  int found = 0;
  size_t at = 0;
  if (!d || d->n_plan_ids <= 0) return false;
  ru_mutex_lock(m->mu);
  for (size_t i = 0; i < m->n; i++) {
    const dz_entry_t *e = &m->e[i];
    if (e->is_scene || !strcmp(e->dev.idx, d->idx) || e->dev.cap == DZ_CAP_THERMOSTAT || !dz_dev_room_temp(&e->dev)) continue;
    bool shared = false;
    for (int k = 0; k < d->n_plan_ids && !shared; k++) shared = dz_dev_in_plan(&e->dev, d->plan_ids[k]);
    if (!shared) continue;
    if (++found == 1) at = i;
  }
  if (found == 1) out_entry(m, &m->e[at], sensor, val, st, NULL, now_ms);
  ru_mutex_unlock(m->mu);
  return found == 1;
}
