/**
 * HA Intercom Protocol Definitions
 *
 * Shared constants for the intercom protocol.
 * Keep in sync with tools/protocol.py
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

// Firmware version - bump with every firmware change
#define FIRMWARE_VERSION        "2.9.2"

// Network Configuration
#define CONTROL_PORT        5004
#define AUDIO_PORT          5005
#define MULTICAST_GROUP     "239.255.0.100"
#define MULTICAST_TTL       1

// Audio Configuration
#define SAMPLE_RATE         16000
#define CHANNELS            1
#define FRAME_DURATION_MS   20
#define FRAME_SIZE          (SAMPLE_RATE * FRAME_DURATION_MS / 1000)  // 320 samples
#define OPUS_BITRATE        32000  // 32kbps VBR — matches codec.c and intercom_hub.py

// Protocol Configuration
#define HEARTBEAT_INTERVAL_MS   30000
#define DEVICE_ID_LENGTH        8
#define SEQUENCE_LENGTH         4
#define PRIORITY_LENGTH         1
#define HEADER_LENGTH           (DEVICE_ID_LENGTH + SEQUENCE_LENGTH + PRIORITY_LENGTH)  // 13 bytes

// Priority levels for preemption / DND override
#define PRIORITY_NORMAL         0   // Default PTT (first-to-talk collision avoidance)
#define PRIORITY_HIGH           1   // Override normal transmissions (parent override)
#define PRIORITY_EMERGENCY      2   // Override everything, force max volume, bypass mute

// Max packet size (header + max opus frame)
#define MAX_PACKET_SIZE         256

// Message types (control plane)
typedef enum {
    MSG_TYPE_ANNOUNCE = 1,
    MSG_TYPE_CONFIG = 2,
    MSG_TYPE_PING = 3,
    MSG_TYPE_PONG = 4,
} message_type_t;

// Cast types (audio plane)
typedef enum {
    CAST_UNICAST = 0,
    CAST_MULTICAST = 1,
    CAST_BROADCAST = 2,  // Same as multicast
} cast_type_t;

// Audio packet structure
typedef struct __attribute__((packed)) {
    uint8_t device_id[DEVICE_ID_LENGTH];
    uint32_t sequence;
    uint8_t priority;     // PRIORITY_NORMAL / PRIORITY_HIGH / PRIORITY_EMERGENCY
    uint8_t opus_data[];  // Variable length
} audio_packet_t;

// Device configuration (received from HA)
typedef struct {
    char room[32];
    char default_target[32];
    char target_ip[16];
    uint8_t volume;
    bool muted;
} device_config_t;

// LED states
typedef enum {
    LED_STATE_OFF = 0,
    LED_STATE_IDLE,         // Solid white - connected
    LED_STATE_TRANSMITTING, // Solid cyan - sending
    LED_STATE_RECEIVING,    // Solid green - receiving
    LED_STATE_MUTED,        // Solid red - muted
    LED_STATE_ERROR,        // Blinking red - error
    LED_STATE_BUSY,         // Solid orange - channel busy (someone else talking)
    LED_STATE_DND,          // Solid purple/violet - Do Not Disturb active
} led_state_t;

// Parameters for the test_tone task spawned by /api/test
typedef struct {
    uint8_t device_id[DEVICE_ID_LENGTH];
    int     duration_frames;  // Number of 20ms frames to generate (default 150 = 3s)
    int     result;           // 0 = success, 1 = aborted by PTT
    void   *semaphore;        // SemaphoreHandle_t — signalled on completion
} test_tone_params_t;

// Default test tone duration: 150 frames = 3 seconds at 20ms/frame
#define TEST_TONE_DEFAULT_FRAMES    150
#define TEST_TONE_MIN_FRAMES        1       // 0.02s minimum
#define TEST_TONE_MAX_FRAMES        600     // 12s maximum

// Threshold above which test_tone returns async (caller polls /api/status)
#define TEST_TONE_SYNC_MAX_FRAMES   150     // <= 3s: synchronous response

#endif // PROTOCOL_H
