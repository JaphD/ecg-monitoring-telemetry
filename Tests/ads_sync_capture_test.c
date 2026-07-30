#include "ads_sync_capture.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

int main(void)
{
    ADS_SyncCapture capture;
    int32_t buffer[ADS_SYNC_CAPTURE_LENGTH] = {0};
    uint32_t i;

    ADS_SyncCapture_Init(&capture, buffer);
    for (i = 0U; i < 64U; i++)
        ADS_SyncCapture_Push(&capture, 120000);

    for (i = 0U; i < 4U; i++) ADS_SyncCapture_Push(&capture, 125000);
    for (i = 0U; i < 4U; i++) ADS_SyncCapture_Push(&capture, 115000);
    for (i = 0U; i < 4U; i++) ADS_SyncCapture_Push(&capture, 125000);
    for (i = 0U; i < 4U; i++) ADS_SyncCapture_Push(&capture, 115000);
    assert(capture.sync_detected);

    for (i = 0U; i < ADS_SYNC_QUIET_SAMPLES; i++)
        ADS_SyncCapture_Push(&capture, 120000);
    assert(capture.capture_active);

    for (i = 0U; i < ADS_SYNC_CAPTURE_LENGTH; i++)
        ADS_SyncCapture_Push(&capture, 200000 + (int32_t)i);

    assert(capture.capture_frozen);
    assert(capture.capture_count == ADS_SYNC_CAPTURE_LENGTH);
    for (i = 0U; i < ADS_SYNC_CAPTURE_LENGTH; i++)
        assert(buffer[i] == 200000 + (int32_t)i);

    ADS_SyncCapture_Push(&capture, -999999);
    assert(buffer[ADS_SYNC_CAPTURE_LENGTH - 1U] ==
           200000 + (int32_t)(ADS_SYNC_CAPTURE_LENGTH - 1U));

    puts("ads_sync_capture_test: PASS");
    return 0;
}
