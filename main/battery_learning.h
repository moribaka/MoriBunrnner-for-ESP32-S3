#ifndef BATTERY_LEARNING_H
#define BATTERY_LEARNING_H

#include "battery_estimator.h"

#define BATTERY_LEARNING_MAGIC 0x42415431u
#define BATTERY_LEARNING_VERSION 1u
#define BATTERY_CAPACITY_MIN_UAH 20000u
#define BATTERY_CAPACITY_MAX_UAH 30000000u
#define BATTERY_CURVE_BINS 11u
#define BATTERY_CYCLE_POINTS 32u

/* 仅保存电池测量和学习结果，不含充电器的控制参数。 */
typedef struct {
    uint32_t magic, version;
    uint32_t capacity_uah, remaining_uah;
    uint32_t charge_raw, discharge_raw;
    uint32_t anchor_charge_raw, anchor_discharge_raw;
    uint16_t sample_hz, cycles, discharge_cycles;
    uint16_t anchor_voltage_mv;
    uint8_t anchor_kind; /* 0=等待参考，1=满电，2=低电量 */
    bool counters_valid, soc_trusted;
    uint16_t curve_mv[BATTERY_CURVE_BINS];
    uint16_t curve_count[BATTERY_CURVE_BINS];
    uint32_t point_uah[BATTERY_CYCLE_POINTS];
    uint16_t point_mv[BATTERY_CYCLE_POINTS], point_ma[BATTERY_CYCLE_POINTS];
    uint8_t point_count;
} battery_learning_profile_t;

typedef struct {
    battery_learning_profile_t profile;
    bool initialized, full_pending, low_pending, full_latched, low_latched;
    bool dirty, checkpoint;
    uint64_t updated_ms, full_since_ms, low_since_ms, point_ms, curve_ms;
} battery_learning_t;

typedef struct {
    uint64_t now_ms;
    bool present, external_power, charging, full;
    bool gauge_valid, counters_valid, fresh_epoch;
    uint8_t gauge_percent;
    uint32_t voltage_mv, charge_ma_x10, discharge_ma_x10;
    uint16_t target_mv, shutdown_mv, sample_hz;
    uint32_t charge_raw, discharge_raw;
} battery_learning_sample_t;

typedef struct {
    bool percent_valid, curve_estimate;
    uint8_t percent, curve_points;
    uint32_t observed_uah;
} battery_learning_result_t;

static inline void battery_learning_reset(battery_learning_t *state)
{
    memset(state, 0, sizeof(*state));
    state->profile.magic = BATTERY_LEARNING_MAGIC;
    state->profile.version = BATTERY_LEARNING_VERSION;
    state->dirty = state->checkpoint = true;
}

static inline bool battery_learning_profile_valid(const battery_learning_profile_t *p)
{
    return p->magic == BATTERY_LEARNING_MAGIC && p->version == BATTERY_LEARNING_VERSION &&
        (p->capacity_uah == 0u || (p->capacity_uah >= BATTERY_CAPACITY_MIN_UAH && p->capacity_uah <= BATTERY_CAPACITY_MAX_UAH)) &&
        p->remaining_uah <= p->capacity_uah && p->anchor_kind <= 2u && p->point_count <= BATTERY_CYCLE_POINTS &&
        (p->sample_hz == 0u || p->sample_hz == 25u || p->sample_hz == 50u || p->sample_hz == 100u || p->sample_hz == 200u);
}

static inline uint64_t battery_counts_to_uah(uint32_t counts, uint16_t sample_hz)
{
    return sample_hz ? (uint64_t)counts * 65536u * 500u / (3600u * sample_hz) : 0u;
}

static inline unsigned battery_learning_curve_points(const battery_learning_profile_t *p)
{
    unsigned count = 0;
    for (unsigned i = 0; i < BATTERY_CURVE_BINS; ++i) if (p->curve_count[i]) ++count;
    return count;
}

static inline bool battery_learning_curve_percent(const battery_learning_profile_t *p, uint32_t mv, uint8_t *percent)
{
    if (battery_learning_curve_points(p) < 4u || !p->curve_count[0] || !p->curve_count[10]) return false;
    unsigned previous = 0;
    if (mv <= p->curve_mv[0]) { *percent = 0; return true; }
    for (unsigned i = 1; i < BATTERY_CURVE_BINS; ++i) {
        if (!p->curve_count[i]) continue;
        if (p->curve_mv[i] <= p->curve_mv[previous]) return false;
        if (mv <= p->curve_mv[i]) {
            *percent = (uint8_t)(previous * 10u + (mv - p->curve_mv[previous]) * (i - previous) * 10u /
                (p->curve_mv[i] - p->curve_mv[previous]));
            return true;
        }
        previous = i;
    }
    *percent = 100; return true;
}

static inline void battery_learning_curve_add(battery_learning_profile_t *p, unsigned bin, uint16_t mv)
{
    if (bin >= BATTERY_CURVE_BINS || mv < 3000u || mv > 4500u) return;
    if (!p->curve_count[bin]) p->curve_mv[bin] = mv;
    else p->curve_mv[bin] = (uint16_t)((7u * p->curve_mv[bin] + mv) / 8u);
    if (p->curve_count[bin] < UINT16_MAX) ++p->curve_count[bin];
}

static inline bool battery_learning_accept_capacity(battery_learning_profile_t *p, uint64_t capacity, bool discharge)
{
    if (capacity < BATTERY_CAPACITY_MIN_UAH || capacity > BATTERY_CAPACITY_MAX_UAH) return false;
    bool changed_cell = p->capacity_uah && (capacity < p->capacity_uah / 2u || capacity > p->capacity_uah * 3u / 2u);
    if (changed_cell) {
        p->cycles = p->discharge_cycles = 0;
        memset(p->curve_mv, 0, sizeof(p->curve_mv));
        memset(p->curve_count, 0, sizeof(p->curve_count));
    }
    if (!p->capacity_uah || changed_cell || (discharge && !p->discharge_cycles)) p->capacity_uah = (uint32_t)capacity;
    else if (discharge || !p->discharge_cycles) p->capacity_uah = (uint32_t)((3u * (uint64_t)p->capacity_uah + capacity) / 4u);
    if (p->cycles < UINT16_MAX) ++p->cycles;
    if (discharge && p->discharge_cycles < UINT16_MAX) ++p->discharge_cycles;
    return true;
}

static inline battery_learning_result_t battery_learning_update(battery_learning_t *state, const battery_learning_sample_t *s)
{
    battery_learning_result_t out = {0};
    battery_learning_profile_t *p = &state->profile;
    if (!battery_learning_profile_valid(p)) battery_learning_reset(state);
    if (!s->present) {
        if (state->initialized || p->capacity_uah || p->anchor_kind) battery_learning_reset(state);
        return out;
    }
    if (s->voltage_mv < 2000u || s->voltage_mv > 4600u) {
        state->full_pending = state->low_pending = false;
        return out;
    }
    out.curve_points = (uint8_t)battery_learning_curve_points(p);
    if (!s->counters_valid || (s->sample_hz != 25u && s->sample_hz != 50u && s->sample_hz != 100u && s->sample_hz != 200u)) {
        if (battery_learning_curve_percent(p, s->voltage_mv, &out.percent)) out.curve_estimate = true;
        state->full_pending = state->low_pending = false;
        return out;
    }
    bool first = !state->initialized || s->now_ms < state->updated_ms;
    bool restart = (first && s->fresh_epoch) || (p->counters_valid && p->sample_hz != s->sample_hz);
    uint64_t charge = 0, discharge = 0;
    if (p->counters_valid && !restart) {
        charge = battery_counts_to_uah((uint32_t)(s->charge_raw - p->charge_raw), s->sample_hz);
        discharge = battery_counts_to_uah((uint32_t)(s->discharge_raw - p->discharge_raw), s->sample_hz);
        uint64_t maximum = first ? BATTERY_CAPACITY_MAX_UAH : 1000u + 5000u * (s->now_ms - state->updated_ms) / 3600u;
        if (charge > maximum || discharge > maximum) restart = true;
    }
    if (restart) {
        p->counters_valid = p->soc_trusted = false;
        p->anchor_kind = p->point_count = 0;
        state->full_pending = state->low_pending = state->full_latched = state->low_latched = false;
        state->dirty = state->checkpoint = true;
    }
    if (!p->counters_valid) {
        uint8_t initial = s->gauge_valid ? s->gauge_percent : battery_voltage_percent(s->voltage_mv);
        if (!s->gauge_valid) battery_learning_curve_percent(p, s->voltage_mv, &initial);
        p->remaining_uah = p->capacity_uah / 100u * initial;
    } else if (p->capacity_uah) {
        int64_t remaining = (int64_t)p->remaining_uah + (int64_t)charge - (int64_t)discharge;
        p->remaining_uah = remaining < 0 ? 0u : remaining > p->capacity_uah ? p->capacity_uah : (uint32_t)remaining;
    }
    if (!p->counters_valid || p->charge_raw != s->charge_raw || p->discharge_raw != s->discharge_raw) state->dirty = true;
    p->charge_raw = s->charge_raw; p->discharge_raw = s->discharge_raw;
    p->sample_hz = s->sample_hz; p->counters_valid = true;
    bool full = s->full && s->target_mv >= 4000u && s->voltage_mv + 70u >= s->target_mv;
    uint16_t low_mv = s->shutdown_mv + 100u;
    if (low_mv < 3300u) low_mv = 3300u;
    bool low = !full && s->gauge_valid && s->gauge_percent <= 5u &&
        s->voltage_mv <= (uint32_t)low_mv + (s->charging ? 150u : 0u);
    if (full && !state->full_pending) { state->full_pending = true; state->full_since_ms = s->now_ms; }
    if (!full) state->full_pending = state->full_latched = false;
    if (low && !state->low_pending) { state->low_pending = true; state->low_since_ms = s->now_ms; }
    if (!low) state->low_pending = state->low_latched = false;
    bool anchor_full = full && !state->full_latched && s->now_ms - state->full_since_ms >= 60000u;
    bool anchor_low = low && !state->low_latched && s->now_ms - state->low_since_ms >= 20000u;
    uint64_t used_charge = battery_counts_to_uah((uint32_t)(s->charge_raw - p->anchor_charge_raw), s->sample_hz);
    uint64_t used_discharge = battery_counts_to_uah((uint32_t)(s->discharge_raw - p->anchor_discharge_raw), s->sample_hz);
    if (p->anchor_kind && used_charge <= BATTERY_CAPACITY_MAX_UAH && used_discharge <= BATTERY_CAPACITY_MAX_UAH) {
        out.observed_uah = (uint32_t)(p->anchor_kind == 1u ?
            (used_discharge > used_charge ? used_discharge - used_charge : 0u) :
            (used_charge > used_discharge ? used_charge - used_discharge : 0u));
    }
    if (p->anchor_kind == 1u && used_discharge >= used_charge && !s->external_power && !s->charging &&
        s->now_ms - state->point_ms >= 30000u && used_discharge - used_charge <= BATTERY_CAPACITY_MAX_UAH &&
        (!p->point_count || s->voltage_mv + 20u <= p->point_mv[p->point_count - 1u])) {
        unsigned point = p->point_count < BATTERY_CYCLE_POINTS ? p->point_count++ : BATTERY_CYCLE_POINTS - 1u;
        p->point_uah[point] = (uint32_t)(used_discharge - used_charge);
        p->point_mv[point] = (uint16_t)s->voltage_mv;
        p->point_ma[point] = (uint16_t)(s->discharge_ma_x10 / 10u);
        state->point_ms = s->now_ms;
        state->dirty = true;
    }
    if (anchor_low && p->anchor_kind == 1u && used_discharge > used_charge &&
        battery_learning_accept_capacity(p, used_discharge - used_charge, true)) {
        battery_learning_curve_add(p, 0, (uint16_t)s->voltage_mv);
        battery_learning_curve_add(p, 10, p->anchor_voltage_mv);
        for (unsigned i = 0; i < p->point_count; ++i) {
            if (p->point_uah[i] > p->capacity_uah || p->point_ma[i] > p->capacity_uah / 5000u) continue;
            unsigned percent = 100u - (uint32_t)((uint64_t)p->point_uah[i] * 100u / p->capacity_uah);
            battery_learning_curve_add(p, (percent + 5u) / 10u, p->point_mv[i]);
        }
    }
    if (anchor_full && p->anchor_kind == 2u && used_charge > used_discharge &&
        battery_learning_accept_capacity(p, used_charge - used_discharge, false)) {
        battery_learning_curve_add(p, 0, p->anchor_voltage_mv);
        battery_learning_curve_add(p, 10, (uint16_t)s->voltage_mv);
    }
    if (anchor_full || anchor_low) {
        p->anchor_kind = anchor_full ? 1u : 2u;
        p->anchor_charge_raw = s->charge_raw; p->anchor_discharge_raw = s->discharge_raw;
        p->anchor_voltage_mv = (uint16_t)s->voltage_mv;
        p->point_count = 0; p->soc_trusted = true;
        p->remaining_uah = anchor_full ? p->capacity_uah : 0u;
        state->full_latched = anchor_full; state->low_latched = anchor_low;
        state->dirty = state->checkpoint = true;
    }
    if (p->capacity_uah && p->soc_trusted && !s->external_power &&
        s->discharge_ma_x10 / 10u <= p->capacity_uah / 5000u && s->now_ms - state->curve_ms >= 60000u) {
        unsigned percent = (uint32_t)((uint64_t)p->remaining_uah * 100u / p->capacity_uah);
        unsigned bin = (percent + 5u) / 10u;
        if ((int)percent - (int)bin * 10 <= 2 && (int)percent - (int)bin * 10 >= -2) {
            battery_learning_curve_add(p, bin, (uint16_t)s->voltage_mv);
            state->curve_ms = s->now_ms; state->dirty = true;
        }
    }
    state->initialized = true; state->updated_ms = s->now_ms;
    out.curve_points = (uint8_t)battery_learning_curve_points(p);
    if (p->capacity_uah) {
        out.percent_valid = true;
        out.percent = (uint8_t)(((uint64_t)p->remaining_uah * 100u + p->capacity_uah / 2u) / p->capacity_uah);
    }
    return out;
}

#endif
