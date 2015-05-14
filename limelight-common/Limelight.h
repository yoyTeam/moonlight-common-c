//
// This header exposes the public streaming API for client usage
//

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define IP_ADDRESS unsigned int

typedef struct _STREAM_CONFIGURATION {
	// Dimensions in pixels of the desired video stream
	int width;
	int height;

	// FPS of the desired video stream
	int fps;

	// Bitrate of the desired video stream (audio adds another ~1 Mbps)
	int bitrate;

	// Max video packet size in bytes (use 1024 if unsure)
	int packetSize;

	// AES encryption data for the remote input stream. This must be
	// the same as what was passed as rikey and rikeyid 
	// in /launch and /resume requests.
	char remoteInputAesKey[16];
	char remoteInputAesIv[16];
} STREAM_CONFIGURATION, *PSTREAM_CONFIGURATION;

typedef struct _LENTRY {
	// Pointer to the next entry or NULL if this is the last entry
	struct _LENTRY *next;

	// Pointer to data (never NULL)
	char* data;

	// Size of data in bytes (never <= 0)
	int length;
} LENTRY, *PLENTRY;

// A decode unit describes a buffer chain of H264 data from multiple packets
typedef struct _DECODE_UNIT {
	// Length of the entire buffer chain in bytes
	int fullLength;

	// Head of the buffer chain (never NULL)
	PLENTRY bufferList;
} DECODE_UNIT, *PDECODE_UNIT;

// This callback is invoked to provide details about the video stream and allow configuration of the decoder
typedef void(*DecoderRendererSetup)(int width, int height, int redrawRate, void* context, int drFlags);

// This callback is invoked right before video data starts being submitted to the decoder
typedef void(*DecoderRendererStart)(void);

// After this callback is invoked, no more video data will be submitted to the decoder
typedef void(*DecoderRendererStop)(void);

// This callback performs the final teardown of the video decoder
typedef void(*DecoderRendererRelease)(void);

// This callback provides Annex B formatted H264 elementary stream data to the
// decoder. If the decoder is unable to process the submitted data for some reason,
// it must return DR_NEED_IDR to generate a keyframe.
#define DR_OK 0
#define DR_NEED_IDR -1
typedef int(*DecoderRendererSubmitDecodeUnit)(PDECODE_UNIT decodeUnit);

typedef struct _DECODER_RENDERER_CALLBACKS {
	DecoderRendererSetup setup;
	DecoderRendererStart start;
	DecoderRendererStop stop;
	DecoderRendererRelease release;
	DecoderRendererSubmitDecodeUnit submitDecodeUnit;
} DECODER_RENDERER_CALLBACKS, *PDECODER_RENDERER_CALLBACKS;

// This callback initializes the audio renderer
typedef void(*AudioRendererInit)(void);

// This callback occurs before audio data is submitted
typedef void(*AudioRendererStart)(void);

// After this callback is invoked, no more audio data will be submitted
typedef void(*AudioRendererStop)(void);

// This callback performs the final teardown of the audio decoder
typedef void(*AudioRendererRelease)(void);

// This callback provides Opus audio data to be decoded and played. sampleLength is in bytes.
typedef void(*AudioRendererDecodeAndPlaySample)(char* sampleData, int sampleLength);

typedef struct _AUDIO_RENDERER_CALLBACKS {
	AudioRendererInit init;
	AudioRendererStart start;
	AudioRendererStop stop;
	AudioRendererRelease release;
	AudioRendererDecodeAndPlaySample decodeAndPlaySample;
} AUDIO_RENDERER_CALLBACKS, *PAUDIO_RENDERER_CALLBACKS;

// Subject to change in future releases
// Use LiGetStageName() for stable stage names
#define STAGE_NONE 0
#define STAGE_PLATFORM_INIT 1
#define STAGE_RTSP_HANDSHAKE 2
#define STAGE_CONTROL_STREAM_INIT 3
#define STAGE_VIDEO_STREAM_INIT 4
#define STAGE_AUDIO_STREAM_INIT 5
#define STAGE_INPUT_STREAM_INIT 6
#define STAGE_CONTROL_STREAM_START 7
#define STAGE_VIDEO_STREAM_START 8
#define STAGE_AUDIO_STREAM_START 9
#define STAGE_INPUT_STREAM_START 10
#define STAGE_MAX 11

// This callback is invoked to indicate that a stage of initialization is about to begin
typedef void(*ConnListenerStageStarting)(int stage);

// This callback is invoked to indicate that a stage of initialization has completed
typedef void(*ConnListenerStageComplete)(int stage);

// This callback is invoked to indicate that a stage of initialization has failed
typedef void(*ConnListenerStageFailed)(int stage, long errorCode);

// This callback is invoked after initialization has finished
typedef void(*ConnListenerConnectionStarted)(void);

// This callback is invoked when a connection failure occurs. It will not
// occur as a result of a call to LiStopConnection()
typedef void(*ConnListenerConnectionTerminated)(long errorCode);

// This callback is invoked to display a dialog-type message to the user
typedef void(*ConnListenerDisplayMessage)(char* message);

// This callback is invoked to display a transient message for the user
// while streaming
typedef void(*ConnListenerDisplayTransientMessage)(char* message);

typedef struct _CONNECTION_LISTENER_CALLBACKS {
	ConnListenerStageStarting stageStarting;
	ConnListenerStageComplete stageComplete;
	ConnListenerStageFailed stageFailed;
	ConnListenerConnectionStarted connectionStarted;
	ConnListenerConnectionTerminated connectionTerminated;
	ConnListenerDisplayMessage displayMessage;
	ConnListenerDisplayTransientMessage displayTransientMessage;
} CONNECTION_LISTENER_CALLBACKS, *PCONNECTION_LISTENER_CALLBACKS;

// This is a Windows-only callback used to indicate that the client
// should call LiCompleteThreadStart() from a new thread as soon as possible.
typedef void(*PlatformThreadStart)(void);

// This is a Windows-only callback used to display a debug message for
// developer use.
typedef void(*PlatformDebugPrint)(char* string);

typedef struct _PLATFORM_CALLBACKS {
	PlatformThreadStart threadStart;
	PlatformDebugPrint debugPrint;
} PLATFORM_CALLBACKS, *PPLATFORM_CALLBACKS;

// This function begins streaming.
//
// Callbacks are all optional. Pass NULL for individual callbacks within each struct or pass NULL for the entire struct
// to use the defaults for all callbacks.
//
// _serverMajorVersion is the major version number of the 'appversion' tag in the /serverinfo request
//
int LiStartConnection(IP_ADDRESS host, PSTREAM_CONFIGURATION streamConfig, PCONNECTION_LISTENER_CALLBACKS clCallbacks,
	PDECODER_RENDERER_CALLBACKS drCallbacks, PAUDIO_RENDERER_CALLBACKS arCallbacks, PPLATFORM_CALLBACKS plCallbacks,
	void* renderContext, int drFlags, int _serverMajorVersion);

// This function stops streaming.
void LiStopConnection(void);

// Use to get a user-visible string to display initialization progress
// from the integer passed to the ConnListenerStageXXX callbacks
const char* LiGetStageName(int stage);

#ifdef _WIN32
/* Call in the context of a new thread */
void LiCompleteThreadStart(void);
#endif

// This function queues a mouse move event to be sent to the remote server.
int LiSendMouseMoveEvent(short deltaX, short deltaY);

// This function queues a mouse button event to be sent to the remote server.
#define BUTTON_ACTION_PRESS 0x07
#define BUTTON_ACTION_RELEASE 0x08
#define BUTTON_LEFT 0x01
#define BUTTON_MIDDLE 0x02
#define BUTTON_RIGHT 0x03
int LiSendMouseButtonEvent(char action, int button);

// This function queues a keyboard event to be sent to the remote server.
#define KEY_ACTION_DOWN 0x03
#define KEY_ACTION_UP 0x04
#define MODIFIER_SHIFT 0x01
#define MODIFIER_CTRL 0x02
#define MODIFIER_ALT 0x04
int LiSendKeyboardEvent(short keyCode, char keyAction, char modifiers);

// Button flags
#define A_FLAG     0x1000
#define B_FLAG     0x2000
#define X_FLAG     0x4000
#define Y_FLAG     0x8000
#define UP_FLAG    0x0001
#define DOWN_FLAG  0x0002
#define LEFT_FLAG  0x0004
#define RIGHT_FLAG 0x0008
#define LB_FLAG    0x0100
#define RB_FLAG    0x0200
#define PLAY_FLAG  0x0010
#define BACK_FLAG  0x0020
#define LS_CLK_FLAG  0x0040
#define RS_CLK_FLAG  0x0080
#define SPECIAL_FLAG 0x0400

// This function queues a controller event to be sent to the remote server. It will
// be seen by the computer as the first controller.
int LiSendControllerEvent(short buttonFlags, char leftTrigger, char rightTrigger,
	short leftStickX, short leftStickY, short rightStickX, short rightStickY);

// This function queues a controller event to be sent to the remote server. The controllerNumber
// parameter is a zero-based index of which controller this event corresponds to. The largest legal
// controller number is 3 (for a total of 4 controllers, the Xinput maximum). On generation 3 servers (GFE 2.1.x),
// these will be sent as controller 0 regardless of the controllerNumber parameter.
int LiSendMultiControllerEvent(short controllerNumber, short buttonFlags, char leftTrigger, char rightTrigger,
    short leftStickX, short leftStickY, short rightStickX, short rightStickY);

// This function queues a vertical scroll event to the remote server.
int LiSendScrollEvent(char scrollClicks);

#ifdef __cplusplus
}
#endif