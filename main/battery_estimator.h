#ifndef BATTERY_ESTIMATOR_H
#define BATTERY_ESTIMATOR_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef enum {
    BATTERY_PERCENT_UNKNOWN = 0,
    BATTERY_PERCENT_FUEL_GAUGE,
    BATTERY_PERCENT_VOLTAGE,
    BATTERY_PERCENT_COULOMB,
    BATTERY_PERCENT_LEARNED_CURVE,
} battery_percent_source_t;

typedef struct {
    bool initialized;
    bool gauge_seen;
    uint8_t percent;
    uint8_t last_gauge_percent;
    uint8_t voltage_count;
    uint8_t voltage_index;
    uint16_t voltages[3];
    uint32_t filtered_mv_x256;
    uint64_t updated_ms;
    uint64_t stepped_ms;
    uint64_t gauge_ms;
    uint64_t low_voltage_ms;
    bool low_voltage_pending;
    battery_percent_source_t last_reference_source;
} battery_estimator_t;

typedef struct {
    bool valid;
    uint8_t percent;
    uint8_t raw_percent;
    uint32_t filtered_voltage_mv;
    battery_percent_source_t source;
} battery_estimate_t;

/* 通用单节锂电池的电压估算只作兜底，不代替芯片电量计或实测标定。 */
static inline uint8_t battery_voltage_percent(uint32_t mv)
{
    static const struct { uint16_t mv; uint8_t percent; } curve[] = {
        {3300, 0}, {3500, 5}, {3600, 10}, {3650, 15}, {3700, 25},
        {3750, 40}, {3800, 55}, {3850, 65}, {3900, 75}, {3950, 82},
        {4000, 88}, {4050, 92}, {4100, 96}, {4200, 100},
    };
    if (mv <= curve[0].mv) return 0;
    for (unsigned i = 1; i < sizeof(curve) / sizeof(curve[0]); ++i) {
        if (mv <= curve[i].mv) {
            return (uint8_t)(curve[i - 1].percent +
                (mv - curve[i - 1].mv) * (curve[i].percent - curve[i - 1].percent) /
                (curve[i].mv - curve[i - 1].mv));
        }
    }
    return 100;
}

static inline battery_estimate_t battery_estimator_update(
    battery_estimator_t *state, uint64_t now_ms, bool present, uint32_t voltage_mv,
    bool gauge_valid, uint8_t gauge_percent, bool charging, bool full,
    battery_percent_source_t reference_source)
{
    battery_estimate_t result = {0};
    if (!present) {
        memset(state, 0, sizeof(*state));
        return result;
    }
    if (voltage_mv < 2000u || voltage_mv > 4600u) return result;
    if (state->initialized && now_ms < state->updated_ms) memset(state, 0, sizeof(*state));
    bool first = !state->initialized;
    state->voltages[state->voltage_index] = (uint16_t)voltage_mv;
    state->voltage_index = (uint8_t)((state->voltage_index + 1u) % 3u);
    if (state->voltage_count < 3u) ++state->voltage_count;
    uint32_t median_mv = voltage_mv;
    if (state->voltage_count == 3u) {
        uint16_t a = state->voltages[0], b = state->voltages[1], c = state->voltages[2];
        median_mv = a > b ? (b > c ? b : (a > c ? c : a)) : (a > c ? a : (b > c ? c : b));
    }
    if (first) state->filtered_mv_x256 = median_mv * 256u;
    else {
        uint64_t elapsed_ms = now_ms - state->updated_ms;
        if (elapsed_ms > 30000u) elapsed_ms = 30000u;
        int64_t delta = (int64_t)median_mv * 256 - state->filtered_mv_x256;
        state->filtered_mv_x256 = (uint32_t)((int64_t)state->filtered_mv_x256 +
            delta * (int64_t)elapsed_ms / (12000 + (int64_t)elapsed_ms));
    }
    result.filtered_voltage_mv = (state->filtered_mv_x256 + 128u) / 256u;
    if (gauge_valid && gauge_percent <= 100u) {
        state->last_gauge_percent = gauge_percent;
        state->gauge_ms = now_ms;
        state->gauge_seen = true;
        state->last_reference_source = reference_source;
    }
    if (state->gauge_seen && now_ms - state->gauge_ms <= 60000u) {
        result.source = state->last_reference_source;
        result.raw_percent = state->last_gauge_percent;
    } else {
        result.source = BATTERY_PERCENT_VOLTAGE;
        result.raw_percent = battery_voltage_percent(result.filtered_voltage_mv);
        if (result.raw_percent == 100u && !full) result.raw_percent = 99u;
    }
    uint8_t target = result.raw_percent;
    if (charging && target == 100u && !full) target = 99u;
    if (first || full || (gauge_valid && reference_source == BATTERY_PERCENT_COULOMB)) {
        state->percent = full ? 100u : target;
        state->stepped_ms = now_ms;
    } else if (now_ms - state->stepped_ms >= 10000u && state->percent != target) {
        state->percent = (uint8_t)(state->percent + (target > state->percent ? 1 : -1));
        state->stepped_ms = now_ms;
    }
    /* 持续低电压要及时体现，不能让平滑显示掩盖真实低电量。 */
    if (median_mv <= 3300u && result.filtered_voltage_mv <= 3400u) {
        if (!state->low_voltage_pending) {
            state->low_voltage_pending = true;
            state->low_voltage_ms = now_ms;
        } else if (now_ms - state->low_voltage_ms >= 15000u && state->percent > 5u) {
            state->percent = target < 5u ? target : 5u;
            state->stepped_ms = now_ms;
        }
    } else state->low_voltage_pending = false;
    state->initialized = true;
    state->updated_ms = now_ms;
    result.percent = state->percent;
    result.valid = true;
    return result;
}

#endif
