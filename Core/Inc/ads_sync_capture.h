#ifndef ADS_SYNC_CAPTURE_H
#define ADS_SYNC_CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

#define ADS_SYNC_CAPTURE_LENGTH 2500U
#define ADS_SYNC_MARKER_PLATEAU_SAMPLES 8U
#define ADS_SYNC_QUIET_SAMPLES 32U
#define ADS_SYNC_MARKER_THRESHOLD_COUNTS 2000

typedef enum
{
    ADS_SYNC_WAIT_MARKER = 0U,
    ADS_SYNC_WAIT_QUIET,
    ADS_SYNC_CAPTURE_PAYLOAD,
    ADS_SYNC_CAPTURE_FROZEN
} ADS_SyncCaptureState;

typedef struct
{
    volatile int32_t *capture_buffer;
    int32_t baseline;
    uint32_t marker_phase;
    uint32_t marker_plateau_count;
    uint32_t quiet_count;
    uint32_t capture_count;
    bool baseline_valid;
    bool sync_detected;
    bool capture_active;
    bool capture_frozen;
    ADS_SyncCaptureState state;
} ADS_SyncCapture;

static inline void ADS_SyncCapture_Init(ADS_SyncCapture *capture,
                                        volatile int32_t *capture_buffer)
{
    capture->capture_buffer = capture_buffer;
    capture->baseline = 0;
    capture->marker_phase = 0U;
    capture->marker_plateau_count = 0U;
    capture->quiet_count = 0U;
    capture->capture_count = 0U;
    capture->baseline_valid = false;
    capture->sync_detected = false;
    capture->capture_active = false;
    capture->capture_frozen = false;
    capture->state = ADS_SYNC_WAIT_MARKER;
}

static inline bool ADS_SyncCapture_MatchesMarker(int32_t delta,
                                                  uint32_t phase)
{
    if ((phase & 1U) == 0U)
        return delta >= ADS_SYNC_MARKER_THRESHOLD_COUNTS;
    return delta <= -ADS_SYNC_MARKER_THRESHOLD_COUNTS;
}

static inline void ADS_SyncCapture_UpdateBaseline(ADS_SyncCapture *capture,
                                                   int32_t sample)
{
    capture->baseline += (sample - capture->baseline) / 32;
}

static inline bool ADS_SyncCapture_Push(ADS_SyncCapture *capture,
                                        int32_t sample)
{
    int32_t delta;

    if (!capture->baseline_valid)
    {
        capture->baseline = sample;
        capture->baseline_valid = true;
        return false;
    }
    if (capture->state == ADS_SYNC_CAPTURE_FROZEN)
        return false;

    if (capture->state == ADS_SYNC_WAIT_MARKER)
    {
        delta = sample - capture->baseline;
        if (ADS_SyncCapture_MatchesMarker(delta, capture->marker_phase))
        {
            capture->marker_plateau_count++;
            if (capture->marker_plateau_count >= ADS_SYNC_MARKER_PLATEAU_SAMPLES)
            {
                capture->marker_plateau_count = 0U;
                capture->marker_phase++;
                if (capture->marker_phase >= 4U)
                {
                    capture->sync_detected = true;
                    capture->state = ADS_SYNC_WAIT_QUIET;
                }
            }
        }
        else
        {
            capture->marker_phase = 0U;
            capture->marker_plateau_count = 0U;
            ADS_SyncCapture_UpdateBaseline(capture, sample);
        }
        return false;
    }

    if (capture->state == ADS_SYNC_WAIT_QUIET)
    {
        capture->quiet_count++;
        if (capture->quiet_count >= ADS_SYNC_QUIET_SAMPLES)
        {
            capture->capture_active = true;
            capture->state = ADS_SYNC_CAPTURE_PAYLOAD;
        }
        return false;
    }

    capture->capture_buffer[capture->capture_count] = sample;
    capture->capture_count++;
    if (capture->capture_count >= ADS_SYNC_CAPTURE_LENGTH)
    {
        capture->capture_active = false;
        capture->capture_frozen = true;
        capture->state = ADS_SYNC_CAPTURE_FROZEN;
        return true;
    }
    return false;
}

#endif
