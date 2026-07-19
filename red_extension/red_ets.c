#include "red_ets.h"

#include <string.h>

enum {
    RED_ETS_PUBLISH_RESULT = 0u,
    RED_ETS_PUBLISH_WAVE_BEGIN,
    RED_ETS_PUBLISH_WAVE_CHUNKS,
    RED_ETS_PUBLISH_SPECTRUM,
    RED_ETS_PUBLISH_DONE,
    RED_ETS_PUBLISH_FINISHED
};

static uint32_t red_ets_now(const red_ets_context_t *context)
{
    return context->port.now_ms != NULL ?
           context->port.now_ms(context->port.user) : 0u;
}

static uint32_t red_ets_elapsed(uint32_t now, uint32_t then)
{
    return now - then;
}

static void red_ets_enter(red_ets_context_t *context, red_ets_state_t state)
{
    context->state = state;
    context->state_enter_ms = red_ets_now(context);
    context->capture_armed = 0u;
}

static int red_ets_send(red_ets_context_t *context,
                        uint8_t command,
                        uint8_t flags,
                        const uint8_t *payload,
                        uint16_t payload_length)
{
    if (context->port.send_message == NULL) {
        return RED_ETS_E_DISABLED;
    }
    return context->port.send_message(command, flags, context->sequence,
                                      payload, payload_length,
                                      context->port.user) == 0 ?
           RED_ETS_OK : RED_ETS_E_BUSY;
}

static void red_ets_send_ack(red_ets_context_t *context,
                             uint16_t sequence,
                             uint8_t status,
                             uint16_t detail)
{
    uint8_t payload[4];
    uint16_t saved_sequence = context->sequence;
    context->sequence = sequence;
    red_ets_protocol_build_ack(RED_ETS_CMD_START, status, detail, payload);
    (void)red_ets_send(context, RED_ETS_CMD_ACK, RED_ETS_FLAG_RESPONSE,
                       payload, sizeof(payload));
    context->sequence = saved_sequence;
}

static uint16_t red_ets_map_error(red_ets_status_t status)
{
    switch (status) {
    case RED_ETS_E_DISABLED: return RED_ETS_ERROR_DISABLED;
    case RED_ETS_E_TIMEOUT: return RED_ETS_ERROR_TRIGGER_TIMEOUT;
    case RED_ETS_E_TRIGGER: return RED_ETS_ERROR_TRIGGER_UNSTABLE;
    case RED_ETS_E_RANGE: return RED_ETS_ERROR_FREQUENCY_OUT_OF_RANGE;
    case RED_ETS_E_RECONSTRUCT: return RED_ETS_ERROR_PHASE_COVERAGE_FAILED;
    case RED_ETS_E_ABORTED: return RED_ETS_ERROR_ABORTED_BY_DIRECT_MEASURE;
    default: return RED_ETS_ERROR_PHASE_CAPTURE_FAILED;
    }
}

static void red_ets_fail(red_ets_context_t *context, red_ets_status_t status)
{
    if (context->state == RED_ETS_STATE_IDLE ||
        context->state == RED_ETS_STATE_RELEASE) {
        return;
    }
    context->last_status = status;
    context->error_code = red_ets_map_error(status);
    context->failed_state = (uint8_t)context->state;
    if (context->port.capture_cancel != NULL) {
        context->port.capture_cancel(context->port.user);
    }
    red_ets_enter(context, RED_ETS_STATE_ERROR);
}

static int red_ets_workspace_valid(const red_ets_runtime_config_t *config,
                                   const red_ets_workspace_t *workspace)
{
    return workspace->period_ticks != NULL &&
           workspace->period_capacity >= config->period_samples &&
           workspace->phase_sum != NULL && workspace->phase_count != NULL &&
           workspace->delay_ticks != NULL &&
           workspace->phase_capacity >= config->max_phase_bins &&
           workspace->uniform_q15 != NULL &&
           workspace->uniform_capacity >= RED_ETS_MAX_PHASE_BINS &&
           workspace->spectrum_ppm != NULL &&
           workspace->spectrum_capacity >= RED_ETS_MAX_SPECTRUM_BINS;
}

int red_ets_init(red_ets_context_t *context,
                 const red_ets_runtime_config_t *config,
                 const red_ets_workspace_t *workspace,
                 const red_ets_port_ops_t *port)
{
    if (context == NULL || config == NULL || workspace == NULL || port == NULL ||
        config->max_phase_bins == 0u ||
        config->max_phase_bins > RED_ETS_MAX_PHASE_BINS ||
        config->period_samples <= config->period_discard + 2u ||
        port->now_ms == NULL || port->period_capture_begin == NULL ||
        port->phase_capture_begin == NULL || port->restore_direct_mode == NULL ||
        port->send_message == NULL || !red_ets_workspace_valid(config, workspace)) {
        return RED_ETS_E_ARGUMENT;
    }
    memset(context, 0, sizeof(*context));
    context->config = *config;
    context->workspace = *workspace;
    context->port = *port;
    context->fft_backend = red_ets_fft_reference_backend;
    context->state = RED_ETS_STATE_IDLE;
    context->last_status = RED_ETS_OK;
    return RED_ETS_OK;
}

void red_ets_set_fft_backend(red_ets_context_t *context,
                             red_ets_fft_backend_t backend,
                             void *backend_user)
{
    if (context != NULL) {
        context->fft_backend = backend;
        context->fft_user = backend_user;
    }
}

void red_ets_set_waveform_limits_uv(red_ets_context_t *context,
                                    int32_t minimum_uv,
                                    int32_t maximum_uv)
{
    if (context != NULL) {
        context->waveform_minimum_uv = minimum_uv;
        context->waveform_maximum_uv = maximum_uv;
    }
}

int red_ets_handle_start(red_ets_context_t *context,
                         uint16_t sequence,
                         const uint8_t *payload,
                         uint16_t payload_length)
{
    if (context == NULL) {
        return RED_ETS_E_ARGUMENT;
    }
#if !RED_ENABLE_ETS
    (void)payload;
    (void)payload_length;
    red_ets_send_ack(context, sequence, RED_ETS_ACK_REJECTED,
                     RED_ETS_ERROR_DISABLED);
    return RED_ETS_E_DISABLED;
#else
    red_ets_request_t request;
    int status;
    status = red_ets_protocol_parse_start(payload, payload_length, &request);
    if (status != RED_ETS_OK) {
        red_ets_send_ack(context, sequence, RED_ETS_ACK_BAD_PAYLOAD, 0u);
        return status;
    }
    status = red_ets_capture_validate_request(&context->config, &request);
    if (status != RED_ETS_OK) {
        red_ets_send_ack(context, sequence, RED_ETS_ACK_BAD_PAYLOAD, 0u);
        return status;
    }
    if (context->last_request_valid && sequence == context->last_sequence &&
        request.request_token == context->last_request_token) {
        red_ets_send_ack(context, sequence, RED_ETS_ACK_OK, 0u);
        return RED_ETS_OK;
    }
    if (red_ets_is_busy(context) ||
        (context->port.direct_measurement_busy != NULL &&
         context->port.direct_measurement_busy(context->port.user))) {
        red_ets_send_ack(context, sequence, RED_ETS_ACK_BUSY, 0u);
        return RED_ETS_E_BUSY;
    }
    context->request = request;
    context->sequence = sequence;
    context->last_sequence = sequence;
    context->last_request_token = request.request_token;
    context->last_request_valid = 1u;
    context->quality_flags = 0u;
    context->phase_index = 0u;
    context->publish_offset = 0u;
    context->publish_chunks = 0u;
    context->publish_stage = RED_ETS_PUBLISH_RESULT;
    context->error_code = 0u;
    context->status_sent = 0u;
    context->last_status = RED_ETS_OK;
    memset(&context->plan, 0, sizeof(context->plan));
    memset(&context->result, 0, sizeof(context->result));
    memset(context->workspace.phase_sum, 0,
           context->workspace.phase_capacity * sizeof(*context->workspace.phase_sum));
    memset(context->workspace.phase_count, 0,
           context->workspace.phase_capacity * sizeof(*context->workspace.phase_count));
    context->start_ms = red_ets_now(context);
    red_ets_enter(context, RED_ETS_STATE_START_RECEIVED);
    red_ets_send_ack(context, sequence, RED_ETS_ACK_OK, 0u);
    if (context->port.frontend_settle_begin != NULL &&
        context->port.frontend_settle_begin(context->port.user) != 0) {
        red_ets_fail(context, RED_ETS_E_CAPTURE);
        return RED_ETS_E_CAPTURE;
    }
    red_ets_enter(context, RED_ETS_STATE_FRONTEND_SETTLE);
    return RED_ETS_OK;
#endif
}

static void red_ets_begin_period_capture(red_ets_context_t *context)
{
    int status;
    status = context->port.period_capture_begin(
        context->workspace.period_ticks, context->config.period_samples,
        context->config.trigger_timeout_ms, context->port.user);
    if (status != 0) {
        red_ets_fail(context, RED_ETS_E_CAPTURE);
        return;
    }
    context->capture_armed = 1u;
    red_ets_enter(context, RED_ETS_STATE_TRIGGER_ACQUIRE);
    context->capture_armed = 1u;
}

static void red_ets_begin_phase_capture(red_ets_context_t *context)
{
    int status;
    uint16_t index = context->phase_index;
    status = context->port.phase_capture_begin(
        context->workspace.delay_ticks[index], context->plan.cycle_divider,
        context->plan.averages_per_phase, context->config.adc_clip_low_code,
        context->config.adc_clip_high_code, context->config.phase_timeout_ms,
        context->port.user);
    if (status != 0) {
        red_ets_fail(context, RED_ETS_E_CAPTURE);
        return;
    }
    red_ets_enter(context, RED_ETS_STATE_PHASE_CAPTURE);
    context->capture_armed = 1u;
}

static uint8_t red_ets_progress(const red_ets_context_t *context)
{
    if (context->plan.phase_bins == 0u) {
        return 0u;
    }
    return (uint8_t)(10u + ((uint32_t)context->phase_index * 70u /
                            context->plan.phase_bins));
}

static void red_ets_publish_status(red_ets_context_t *context, uint8_t stage)
{
    uint8_t payload[12];
    uint32_t now = red_ets_now(context);
    uint32_t elapsed = red_ets_elapsed(now, context->start_ms);
    if (context->status_sent &&
        red_ets_elapsed(now, context->last_status_ms) < 250u) {
        return;
    }
    red_ets_protocol_build_status(stage, red_ets_progress(context),
                                  elapsed > 65535u ? 65535u : (uint16_t)elapsed,
                                  context->phase_index, context->quality_flags,
                                  payload);
    (void)red_ets_send(context, RED_ETS_CMD_STATUS, RED_ETS_FLAG_EVENT,
                       payload, sizeof(payload));
    context->last_status_ms = now;
    context->status_sent = 1u;
}

static int red_ets_publish_next(red_ets_context_t *context)
{
    uint8_t payload[RED_ETS_PROTOCOL_MAX_PAYLOAD];
    size_t length = 0u;
    int status;
    switch (context->publish_stage) {
    case RED_ETS_PUBLISH_RESULT:
        length = red_ets_protocol_build_result(&context->result, payload);
        status = red_ets_send(context, RED_ETS_CMD_RESULT,
                              RED_ETS_FLAG_EVENT | RED_ETS_FLAG_ACK_REQ,
                              payload, (uint16_t)length);
        if (status == RED_ETS_OK) {
            context->publish_stage = RED_ETS_PUBLISH_WAVE_BEGIN;
            ++context->publish_chunks;
        }
        return status;

    case RED_ETS_PUBLISH_WAVE_BEGIN:
        if ((context->request.features & RED_ETS_FEATURE_WAVEFORM) == 0u) {
            context->publish_stage = RED_ETS_PUBLISH_SPECTRUM;
            return RED_ETS_OK;
        }
        length = red_ets_protocol_build_wave_begin(
            context->plan.fft_bins, context->waveform_minimum_uv,
            context->waveform_maximum_uv, payload);
        status = red_ets_send(context, RED_ETS_CMD_WAVE_BEGIN,
                              RED_ETS_FLAG_EVENT | RED_ETS_FLAG_ACK_REQ,
                              payload, (uint16_t)length);
        if (status == RED_ETS_OK) {
            context->publish_stage = RED_ETS_PUBLISH_WAVE_CHUNKS;
            context->publish_offset = 0u;
            ++context->publish_chunks;
        }
        return status;

    case RED_ETS_PUBLISH_WAVE_CHUNKS:
        if (context->publish_offset >= context->plan.fft_bins) {
            context->publish_stage = RED_ETS_PUBLISH_SPECTRUM;
            context->publish_offset = 0u;
            return RED_ETS_OK;
        } else {
            uint16_t remaining = (uint16_t)(context->plan.fft_bins -
                                            context->publish_offset);
            uint8_t count = remaining > 60u ? 60u : (uint8_t)remaining;
            status = red_ets_protocol_build_wave_chunk(
                context->publish_offset,
                &context->workspace.uniform_q15[context->publish_offset],
                count, payload, sizeof(payload), &length);
            if (status != RED_ETS_OK) {
                return status;
            }
            status = red_ets_send(context, RED_ETS_CMD_WAVE_CHUNK,
                                  RED_ETS_FLAG_EVENT | RED_ETS_FLAG_ACK_REQ,
                                  payload, (uint16_t)length);
            if (status == RED_ETS_OK) {
                context->publish_offset = (uint16_t)(context->publish_offset + count);
                ++context->publish_chunks;
            }
            return status;
        }

    case RED_ETS_PUBLISH_SPECTRUM:
        if ((context->request.features & RED_ETS_FEATURE_SPECTRUM) == 0u) {
            context->publish_stage = RED_ETS_PUBLISH_DONE;
            return RED_ETS_OK;
        } else {
            uint8_t count = (uint8_t)(context->request.harmonic_limit + 1u);
            status = red_ets_protocol_build_spectrum_chunk(
                0u, context->workspace.spectrum_ppm, count,
                payload, sizeof(payload), &length);
            if (status != RED_ETS_OK) {
                return status;
            }
            status = red_ets_send(context, RED_ETS_CMD_SPECTRUM_CHUNK,
                                  RED_ETS_FLAG_EVENT | RED_ETS_FLAG_ACK_REQ,
                                  payload, (uint16_t)length);
            if (status == RED_ETS_OK) {
                context->publish_stage = RED_ETS_PUBLISH_DONE;
                ++context->publish_chunks;
            }
            return status;
        }

    case RED_ETS_PUBLISH_DONE: {
        uint16_t flags = 1u;
        if ((context->request.features & RED_ETS_FEATURE_WAVEFORM) != 0u) {
            flags |= 2u;
        }
        if ((context->request.features & RED_ETS_FEATURE_SPECTRUM) != 0u) {
            flags |= 4u;
        }
        length = red_ets_protocol_build_done(context->request.request_token,
                                             flags, context->publish_chunks,
                                             payload);
        status = red_ets_send(context, RED_ETS_CMD_DONE,
                              RED_ETS_FLAG_EVENT | RED_ETS_FLAG_ACK_REQ,
                              payload, (uint16_t)length);
        if (status == RED_ETS_OK) {
            context->publish_stage = RED_ETS_PUBLISH_FINISHED;
        }
        return status;
    }

    default:
        return RED_ETS_OK;
    }
}

void red_ets_poll(red_ets_context_t *context)
{
    uint32_t now;
    int status;
    if (context == NULL || context->state == RED_ETS_STATE_IDLE) {
        return;
    }
    now = red_ets_now(context);
    if (context->state != RED_ETS_STATE_ERROR &&
        context->state != RED_ETS_STATE_RELEASE &&
        red_ets_elapsed(now, context->start_ms) > context->request.deadline_ms) {
        red_ets_fail(context, RED_ETS_E_TIMEOUT);
    }

    switch (context->state) {
    case RED_ETS_STATE_FRONTEND_SETTLE:
        if (red_ets_elapsed(now, context->state_enter_ms) >=
            context->config.frontend_settle_ms) {
            red_ets_publish_status(context, 1u);
            red_ets_begin_period_capture(context);
        }
        break;

    case RED_ETS_STATE_TRIGGER_ACQUIRE:
        if (red_ets_elapsed(now, context->state_enter_ms) >
            context->config.trigger_timeout_ms) {
            context->quality_flags |= RED_ETS_QUALITY_TRIGGER_MISSING;
            red_ets_fail(context, RED_ETS_E_TIMEOUT);
        }
        break;

    case RED_ETS_STATE_PERIOD_VALIDATE:
        status = red_ets_capture_make_plan(
            &context->config, &context->request,
            context->workspace.period_ticks, context->config.period_samples,
            &context->plan, &context->quality_flags);
        if (status != RED_ETS_OK) {
            red_ets_fail(context, (red_ets_status_t)status);
            break;
        }
        status = red_ets_capture_build_delays(
            &context->plan, context->workspace.delay_ticks,
            context->workspace.phase_capacity, &context->quality_flags);
        if (status != RED_ETS_OK) {
            red_ets_fail(context, (red_ets_status_t)status);
            break;
        }
        red_ets_publish_status(context, 2u);
        red_ets_enter(context, RED_ETS_STATE_PLAN_PHASES);
        break;

    case RED_ETS_STATE_PLAN_PHASES:
        context->phase_index = 0u;
        red_ets_begin_phase_capture(context);
        break;

    case RED_ETS_STATE_PHASE_CAPTURE:
        if (red_ets_elapsed(now, context->state_enter_ms) >
            context->config.phase_timeout_ms) {
            red_ets_fail(context, RED_ETS_E_TIMEOUT);
        }
        break;

    case RED_ETS_STATE_PHASE_VALIDATE:
        red_ets_publish_status(context, 4u);
        red_ets_enter(context, RED_ETS_STATE_RESAMPLE);
        break;

    case RED_ETS_STATE_RESAMPLE:
        status = red_ets_reconstruct_uniform_q15(
            &context->plan, context->workspace.delay_ticks,
            context->workspace.phase_sum, context->workspace.phase_count,
            context->workspace.uniform_q15, context->workspace.uniform_capacity,
            &context->quality_flags);
        if (status != RED_ETS_OK) {
            red_ets_fail(context, (red_ets_status_t)status);
            break;
        }
        red_ets_publish_status(context, 5u);
        red_ets_enter(context, RED_ETS_STATE_FFT);
        break;

    case RED_ETS_STATE_FFT:
        if (context->fft_backend == NULL) {
            red_ets_fail(context, RED_ETS_E_ANALYSIS);
            break;
        }
        status = context->fft_backend(
            context->workspace.uniform_q15, context->plan.fft_bins,
            context->request.harmonic_limit,
            context->workspace.spectrum_ppm,
            context->workspace.spectrum_capacity,
            &context->result.thd_1_to_5_ppm, context->fft_user);
        if (status != RED_ETS_OK) {
            red_ets_fail(context, (red_ets_status_t)status);
            break;
        }
        red_ets_enter(context, RED_ETS_STATE_RESULT_VALIDATE);
        break;

    case RED_ETS_STATE_RESULT_VALIDATE: {
        uint32_t disqualifying = RED_ETS_QUALITY_TRIGGER_MISSING |
            RED_ETS_QUALITY_TRIGGER_JITTER_HIGH |
            RED_ETS_QUALITY_PHASE_COVERAGE_INCOMPLETE |
            RED_ETS_QUALITY_SIGNAL_CHANGED |
            RED_ETS_QUALITY_INPUT_CLIPPED |
            RED_ETS_QUALITY_LOW_SNR |
            RED_ETS_QUALITY_FUNDAMENTAL_OUT_OF_RANGE |
            RED_ETS_QUALITY_MISSING_PHASE_BINS |
            RED_ETS_QUALITY_FRONTEND_NOT_SETTLED;
        context->result.fundamental_millihz = context->plan.fundamental_millihz;
        context->result.adc_max_rate_hz = context->config.adc_max_rate_hz;
        context->result.equivalent_sample_rate_hz =
            context->plan.equivalent_sample_rate_hz;
        context->result.phase_bins = context->plan.phase_bins;
        context->result.averages_per_phase = context->plan.averages_per_phase;
        context->result.timer_clock_hz = context->config.timer_clock_hz;
        context->result.calibrated_analog_bandwidth_hz =
            context->config.calibrated_analog_bandwidth_hz;
        context->result.highest_valid_harmonic =
            red_ets_fft_highest_valid_harmonic(
                context->plan.fundamental_millihz,
                context->plan.equivalent_sample_rate_hz,
                context->config.calibrated_analog_bandwidth_hz,
                context->request.harmonic_limit);
        if (context->config.calibrated_analog_bandwidth_hz == 0u) {
            context->quality_flags |= RED_ETS_QUALITY_ANALOG_BW_UNCALIBRATED;
        }
        if (context->result.highest_valid_harmonic < 5u) {
            context->quality_flags |= RED_ETS_QUALITY_ALIAS_OR_HARMONIC_AMBIGUOUS;
        }
        if ((context->quality_flags & disqualifying) == 0u &&
            context->result.highest_valid_harmonic >= 5u) {
            context->quality_flags |= RED_ETS_QUALITY_VALID;
        }
        context->result.quality_flags = context->quality_flags;
        context->publish_stage = RED_ETS_PUBLISH_RESULT;
        red_ets_publish_status(context, 6u);
        red_ets_enter(context, RED_ETS_STATE_PUBLISH);
        break;
    }

    case RED_ETS_STATE_PUBLISH:
        status = red_ets_publish_next(context);
        if (status != RED_ETS_OK && status != RED_ETS_E_BUSY) {
            red_ets_fail(context, (red_ets_status_t)status);
        } else if (context->publish_stage == RED_ETS_PUBLISH_FINISHED) {
            red_ets_enter(context, RED_ETS_STATE_RELEASE);
        }
        break;

    case RED_ETS_STATE_ERROR: {
        uint8_t payload[8];
        red_ets_protocol_build_error(context->error_code,
                                     context->failed_state,
                                     RED_ETS_CMD_START,
                                     (uint32_t)(-context->last_status), payload);
        status = red_ets_send(context, RED_ETS_CMD_ERROR,
                              RED_ETS_FLAG_EVENT | RED_ETS_FLAG_ERROR,
                              payload, sizeof(payload));
        if (status == RED_ETS_OK) {
            red_ets_enter(context, RED_ETS_STATE_RELEASE);
        }
        break;
    }

    case RED_ETS_STATE_RELEASE:
        red_ets_release(context);
        break;

    default:
        break;
    }
}

int red_ets_period_capture_complete(red_ets_context_t *context,
                                    size_t captured_periods)
{
    if (context == NULL || context->state != RED_ETS_STATE_TRIGGER_ACQUIRE ||
        !context->capture_armed ||
        captured_periods < context->config.period_samples) {
        return RED_ETS_E_STATE;
    }
    context->capture_armed = 0u;
    red_ets_enter(context, RED_ETS_STATE_PERIOD_VALIDATE);
    return RED_ETS_OK;
}

int red_ets_phase_capture_complete(red_ets_context_t *context,
                                   uint32_t sample_sum,
                                   uint16_t valid_samples,
                                   int clipped)
{
    int status;
    if (context == NULL || context->state != RED_ETS_STATE_PHASE_CAPTURE ||
        !context->capture_armed) {
        return RED_ETS_E_STATE;
    }
    context->capture_armed = 0u;
    status = red_ets_capture_store_phase(
        context->phase_index, sample_sum, valid_samples, clipped,
        context->workspace.phase_sum, context->workspace.phase_count,
        context->workspace.phase_capacity, &context->quality_flags);
    if (status != RED_ETS_OK) {
        red_ets_fail(context, (red_ets_status_t)status);
        return status;
    }
    ++context->phase_index;
    if (context->phase_index >= context->plan.phase_bins) {
        red_ets_enter(context, RED_ETS_STATE_PHASE_VALIDATE);
    } else {
        red_ets_publish_status(context, 3u);
        red_ets_begin_phase_capture(context);
    }
    return RED_ETS_OK;
}

void red_ets_capture_failed(red_ets_context_t *context,
                            red_ets_status_t reason)
{
    if (context != NULL) {
        red_ets_fail(context, reason);
    }
}

void red_ets_abort_for_direct_measurement(red_ets_context_t *context)
{
    if (context != NULL && red_ets_is_busy(context)) {
        red_ets_fail(context, RED_ETS_E_ABORTED);
    }
}

void red_ets_release(red_ets_context_t *context)
{
    if (context == NULL) {
        return;
    }
    if (context->port.capture_cancel != NULL) {
        context->port.capture_cancel(context->port.user);
    }
    if (context->port.restore_direct_mode != NULL) {
        context->port.restore_direct_mode(context->port.user);
    }
    context->capture_armed = 0u;
    context->state = RED_ETS_STATE_IDLE;
}

int red_ets_is_busy(const red_ets_context_t *context)
{
    return context != NULL && context->state != RED_ETS_STATE_IDLE &&
           context->state != RED_ETS_STATE_DONE;
}

red_ets_state_t red_ets_get_state(const red_ets_context_t *context)
{
    return context != NULL ? context->state : RED_ETS_STATE_ERROR;
}

const red_ets_result_t *red_ets_get_result(const red_ets_context_t *context)
{
    return context != NULL ? &context->result : NULL;
}
