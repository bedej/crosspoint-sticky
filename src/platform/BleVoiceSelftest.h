#pragma once

// VOICE_BLE_SELFTEST builds only: advertise the voice-relay service and stream
// mic audio to a subscribed central, skipping WiFi/UI/activity entirely. Never
// returns. See BleVoiceSelftest.cpp for why this is standalone.
void runBleVoiceSelftest();
