#include "Platform.h"
#include "Limelight-internal.h"
#include "LinkedBlockingQueue.h"
#include "Video.h"

static PLENTRY nalChainHead;
static int nalChainDataLength;

static int nextFrameNumber;
static int startFrameNumber;
static int waitingForNextSuccessfulFrame;
static int waitingForIdrFrame;
static int lastPacketInStream;
static int decodingFrame;
static int strictIdrFrameWait;
static unsigned long long firstPacketReceiveTime;

#define CONSECUTIVE_DROP_LIMIT 120
static int consecutiveFrameDrops;

static LINKED_BLOCKING_QUEUE decodeUnitQueue;

typedef struct _BUFFER_DESC {
    char* data;
    unsigned int offset;
    unsigned int length;
} BUFFER_DESC, *PBUFFER_DESC;

// Init
void initializeVideoDepacketizer(int pktSize) {
    if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        LbqInitializeLinkedBlockingQueue(&decodeUnitQueue, 15);
    }

    nextFrameNumber = 1;
    startFrameNumber = 0;
    waitingForNextSuccessfulFrame = 0;
    waitingForIdrFrame = 1;
    lastPacketInStream = -1;
    decodingFrame = 0;
    firstPacketReceiveTime = 0;

    LC_ASSERT(NegotiatedVideoFormat != 0);
    strictIdrFrameWait =
            !((NegotiatedVideoFormat == VIDEO_FORMAT_H264 && (VideoCallbacks.capabilities & CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC)) ||
              ((NegotiatedVideoFormat == VIDEO_FORMAT_H265 && (VideoCallbacks.capabilities & CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC))));
}

// Free the NAL chain
static void cleanupFrameState(void) {
    PLENTRY lastEntry;

    while (nalChainHead != NULL) {
        lastEntry = nalChainHead;
        nalChainHead = lastEntry->next;
        free(lastEntry);
    }

    nalChainDataLength = 0;
}

// Cleanup frame state and set that we're waiting for an IDR Frame
static void dropFrameState(void) {
    // We'll need an IDR frame now if we're in strict mode
    if (strictIdrFrameWait) {
        waitingForIdrFrame = 1;
    }

    // Count the number of consecutive frames dropped
    consecutiveFrameDrops++;

    // If we reach our limit, immediately request an IDR frame and reset
    if (consecutiveFrameDrops == CONSECUTIVE_DROP_LIMIT) {
        Limelog("Reached consecutive drop limit\n");

        // Restart the count
        consecutiveFrameDrops = 0;

        // Request an IDR frame
        waitingForIdrFrame = 1;
        requestIdrOnDemand();
    }

    cleanupFrameState();
}

// Cleanup the list of decode units
static void freeDecodeUnitList(PLINKED_BLOCKING_QUEUE_ENTRY entry) {
    PLINKED_BLOCKING_QUEUE_ENTRY nextEntry;

    while (entry != NULL) {
        nextEntry = entry->flink;

        freeQueuedDecodeUnit((PQUEUED_DECODE_UNIT)entry->data);

        entry = nextEntry;
    }
}

void stopVideoDepacketizer(void) {
    if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        LbqSignalQueueShutdown(&decodeUnitQueue);
    }
}

// Cleanup video depacketizer and free malloced memory
void destroyVideoDepacketizer(void) {
    if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        freeDecodeUnitList(LbqDestroyLinkedBlockingQueue(&decodeUnitQueue));
    }

    cleanupFrameState();
}

// Returns 1 if candidate is a frame start and 0 otherwise
static int isSeqFrameStart(PBUFFER_DESC candidate) {
    return (candidate->length == 4 && candidate->data[candidate->offset + candidate->length - 1] == 1);
}

// Returns 1 if candidate is an Annex B start and 0 otherwise
static int isSeqAnnexBStart(PBUFFER_DESC candidate) {
    return (candidate->data[candidate->offset + candidate->length - 1] == 1);
}

// Returns 1 if candidate is padding and 0 otherwise
static int isSeqPadding(PBUFFER_DESC candidate) {
    return (candidate->data[candidate->offset + candidate->length - 1] == 0);
}

// Returns 1 on success, 0 otherwise
static int getSpecialSeq(PBUFFER_DESC current, PBUFFER_DESC candidate) {
    if (current->length < 3) {
        return 0;
    }

    if (current->data[current->offset] == 0 &&
        current->data[current->offset + 1] == 0) {
        // Padding or frame start
        if (current->data[current->offset + 2] == 0) {
            if (current->length >= 4 && current->data[current->offset + 3] == 1) {
                // Frame start
                candidate->data = current->data;
                candidate->offset = current->offset;
                candidate->length = 4;
                return 1;
            }
            else {
                // Padding
                candidate->data = current->data;
                candidate->offset = current->offset;
                candidate->length = 3;
                return 1;
            }
        }
        else if (current->data[current->offset + 2] == 1) {
            // NAL start
            candidate->data = current->data;
            candidate->offset = current->offset;
            candidate->length = 3;
            return 1;
        }
    }

    return 0;
}

// Get the first decode unit available
int getNextQueuedDecodeUnit(PQUEUED_DECODE_UNIT* qdu) {
    int err = LbqWaitForQueueElement(&decodeUnitQueue, (void**)qdu);
    if (err == LBQ_SUCCESS) {
        return 1;
    }
    else {
        return 0;
    }
}

// Cleanup a decode unit by freeing the buffer chain and the holder
void freeQueuedDecodeUnit(PQUEUED_DECODE_UNIT qdu) {
    PLENTRY lastEntry;

    while (qdu->decodeUnit.bufferList != NULL) {
        lastEntry = qdu->decodeUnit.bufferList;
        qdu->decodeUnit.bufferList = lastEntry->next;
        free(lastEntry);
    }

    free(qdu);
}


// Returns 1 if the special sequence describes an I-frame
static int isSeqReferenceFrameStart(PBUFFER_DESC specialSeq) {
    switch (specialSeq->data[specialSeq->offset + specialSeq->length]) {
        case 0x20:
        case 0x22:
        case 0x24:
        case 0x26:
        case 0x28:
        case 0x2A:
            // H265
            return 1;
            
        case 0x65:
            // H264
            return 1;
            
        default:
            return 0;
    }
}

// Returns 1 if this buffer describes an IDR frame
static int isIdrFrameStart(PBUFFER_DESC buffer) {
    BUFFER_DESC specialSeq;
    return getSpecialSeq(buffer, &specialSeq) &&
        isSeqFrameStart(&specialSeq) &&
        (specialSeq.data[specialSeq.offset + specialSeq.length] == 0x67 || // H264 SPS
         specialSeq.data[specialSeq.offset + specialSeq.length] == 0x40); // H265 VPS
}

// Reassemble the frame with the given frame number
static void reassembleFrame(int frameNumber) {
    if (nalChainHead != NULL) {
        PQUEUED_DECODE_UNIT qdu = (PQUEUED_DECODE_UNIT)malloc(sizeof(*qdu));
        if (qdu != NULL) {
            qdu->decodeUnit.bufferList = nalChainHead;
            qdu->decodeUnit.fullLength = nalChainDataLength;
            qdu->decodeUnit.frameNumber = frameNumber;
            qdu->decodeUnit.receiveTimeMs = firstPacketReceiveTime;

            nalChainHead = NULL;
            nalChainDataLength = 0;

            if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
                if (LbqOfferQueueItem(&decodeUnitQueue, qdu, &qdu->entry) == LBQ_BOUND_EXCEEDED) {
                    Limelog("Video decode unit queue overflow\n");

                    // Clear frame state and wait for an IDR
                    nalChainHead = qdu->decodeUnit.bufferList;
                    nalChainDataLength = qdu->decodeUnit.fullLength;
                    dropFrameState();

                    // Free the DU
                    free(qdu);

                    // Flush the decode unit queue
                    freeDecodeUnitList(LbqFlushQueueItems(&decodeUnitQueue));

                    // FIXME: Get proper bounds to use reference frame invalidation
                    requestIdrOnDemand();
                    return;
                }
            }
            else {
                int ret = VideoCallbacks.submitDecodeUnit(&qdu->decodeUnit);

                freeQueuedDecodeUnit(qdu);

                if (ret == DR_NEED_IDR) {
                    Limelog("Requesting IDR frame on behalf of DR\n");
                    requestDecoderRefresh();
                }
            }

            // Notify the control connection
            connectionReceivedCompleteFrame(frameNumber);

            // Clear frame drops
            consecutiveFrameDrops = 0;
        }
    }
}

static void queueFragment(char*data, int offset, int length) {
    PLENTRY entry = (PLENTRY)malloc(sizeof(*entry) + length);
    if (entry != NULL) {
        entry->next = NULL;
        entry->length = length;
        entry->data = (char*)(entry + 1);

        memcpy(entry->data, &data[offset], entry->length);

        nalChainDataLength += entry->length;

        if (nalChainHead == NULL) {
            nalChainHead = entry;
        }
        else {
            PLENTRY currentEntry = nalChainHead;

            while (currentEntry->next != NULL) {
                currentEntry = currentEntry->next;
            }

            currentEntry->next = entry;
        }
    }
}

// Process an RTP Payload
static void processRtpPayloadSlow(PNV_VIDEO_PACKET videoPacket, PBUFFER_DESC currentPos) {
    BUFFER_DESC specialSeq;
    int decodingVideo = 0;

    while (currentPos->length != 0) {
        int start = currentPos->offset;

        if (getSpecialSeq(currentPos, &specialSeq)) {
            if (isSeqAnnexBStart(&specialSeq)) {
                // Now we're decoding video
                decodingVideo = 1;

                if (isSeqFrameStart(&specialSeq)) {
                    // Now we're working on a frame
                    decodingFrame = 1;

                    // Reassemble any pending frame
                    reassembleFrame(videoPacket->frameIndex);

                    if (isSeqReferenceFrameStart(&specialSeq)) {
                        // No longer waiting for an IDR frame
                        waitingForIdrFrame = 0;
                        
                        // Cancel any pending IDR frame request
                        waitingForNextSuccessfulFrame = 0;
                    }
                }

                // Skip the start sequence
                currentPos->length -= specialSeq.length;
                currentPos->offset += specialSeq.length;
            }
            else {
                // Check if this is padding after a full frame
                if (decodingVideo && isSeqPadding(currentPos)) {
                    reassembleFrame(videoPacket->frameIndex);
                }

                // Not decoding video
                decodingVideo = 0;

                // Just skip this byte
                currentPos->length--;
                currentPos->offset++;
            }
        }

        // Move to the next special sequence
        while (currentPos->length != 0) {
            // Check if this should end the current NAL
            if (getSpecialSeq(currentPos, &specialSeq)) {
                if (decodingVideo || !isSeqPadding(&specialSeq)) {
                    break;
                }
            }

            // This byte is part of the NAL data
            currentPos->offset++;
            currentPos->length--;
        }

        if (decodingVideo) {
            queueFragment(currentPos->data, start, currentPos->offset - start);
        }
    }
}

// Dumps the decode unit queue and ensures the next frame submitted to the decoder will be
// an IDR frame
void requestDecoderRefresh(void) {
    // Wait for the next IDR frame
    waitingForIdrFrame = 1;
    
    // Flush the decode unit queue and pending state
    dropFrameState();
    if ((VideoCallbacks.capabilities & CAPABILITY_DIRECT_SUBMIT) == 0) {
        freeDecodeUnitList(LbqFlushQueueItems(&decodeUnitQueue));
    }
    
    // Request the IDR frame
    requestIdrOnDemand();
}

// Return 1 if packet is the first one in the frame
static int isFirstPacket(char flags) {
    // Clear the picture data flag
    flags &= ~FLAG_CONTAINS_PIC_DATA;

    // Check if it's just the start or both start and end of a frame
    return (flags == (FLAG_SOF | FLAG_EOF) ||
        flags == FLAG_SOF);
}

// Adds a fragment directly to the queue
static void processRtpPayloadFast(BUFFER_DESC location) {
    queueFragment(location.data, location.offset, location.length);
}

// Process an RTP Payload
void processRtpPayload(PNV_VIDEO_PACKET videoPacket, int length, unsigned long long receiveTimeMs) {
    BUFFER_DESC currentPos;
    int frameIndex;
    char flags;
    int firstPacket;
    int streamPacketIndex;

    // Mask the top 8 bits from the SPI
    videoPacket->streamPacketIndex >>= 8;
    videoPacket->streamPacketIndex &= 0xFFFFFF;

    currentPos.data = (char*)(videoPacket + 1);
    currentPos.offset = 0;
    currentPos.length = length - sizeof(*videoPacket);

    frameIndex = videoPacket->frameIndex;
    flags = videoPacket->flags;
    firstPacket = isFirstPacket(flags);

    streamPacketIndex = videoPacket->streamPacketIndex;
    
    // The packets and frames must be in sequence from the FEC queue
    LC_ASSERT(!isBeforeSignedInt((short)streamPacketIndex, (short)(lastPacketInStream + 1), 0));
    LC_ASSERT(!isBeforeSignedInt(frameIndex, nextFrameNumber, 0));

    // Notify the listener of the latest frame we've seen from the PC
    connectionSawFrame(frameIndex);
    
    // Verify that we didn't receive an incomplete frame
    LC_ASSERT(firstPacket ^ decodingFrame);
    
    // Check sequencing of this frame to ensure we didn't
    // miss one in between
    if (firstPacket) {
        // Make sure this is the next consecutive frame
        if (isBeforeSignedInt(nextFrameNumber, frameIndex, 1)) {
            Limelog("Network dropped an entire frame\n");
            nextFrameNumber = frameIndex;

            // Wait until next complete frame
            waitingForNextSuccessfulFrame = 1;
            dropFrameState();
        }
        else {
            LC_ASSERT(nextFrameNumber == frameIndex);
        }

        // We're now decoding a frame
        decodingFrame = 1;
        firstPacketReceiveTime = receiveTimeMs;
    }

    // This must be the first packet in a frame or be contiguous with the last
    // packet received.
    LC_ASSERT(firstPacket || streamPacketIndex == (int)(lastPacketInStream + 1));

    // Notify the server of any packet losses
    if (streamPacketIndex != (int)(lastPacketInStream + 1)) {
        // Packets were lost so report this to the server
        connectionLostPackets(lastPacketInStream, streamPacketIndex);
    }
    lastPacketInStream = streamPacketIndex;

    // If this is the first packet, skip the frame header (if one exists)
    if (firstPacket){
        if ((AppVersionQuad[0] > 7) ||
            (AppVersionQuad[0] == 7 && AppVersionQuad[1] > 1) ||
            (AppVersionQuad[0] == 7 && AppVersionQuad[1] == 1 && AppVersionQuad[2] >= 350)) {
            // >= 7.1.350 should use the 8 byte header again
            currentPos.offset += 8;
            currentPos.length -= 8;
        }
        else if ((AppVersionQuad[0] > 7) ||
            (AppVersionQuad[0] == 7 && AppVersionQuad[1] > 1) ||
            (AppVersionQuad[0] == 7 && AppVersionQuad[1] == 1 && AppVersionQuad[2] >= 320)) {
            // [7.1.320, 7.1.350) should use the 12 byte frame header
            currentPos.offset += 12;
            currentPos.length -= 12;
        }
        else if (AppVersionQuad[0] >= 5) {
            // [5.x, 7.1.320) should use the 8 byte header
            currentPos.offset += 8;
            currentPos.length -= 8;
        }
        else {
            // Other versions don't have a frame header at all
        }
    }

    if (firstPacket && isIdrFrameStart(&currentPos))
    {
        // SPS and PPS prefix is padded between NALs, so we must decode it with the slow path
        processRtpPayloadSlow(videoPacket, &currentPos);
    }
    else
    {
        processRtpPayloadFast(currentPos);
    }

    if (flags & FLAG_EOF) {
        // Move on to the next frame
        decodingFrame = 0;
        nextFrameNumber = frameIndex + 1;

        // If waiting for next successful frame and we got here
        // with an end flag, we can send a message to the server
        if (waitingForNextSuccessfulFrame) {
            // This is the next successful frame after a loss event
            connectionDetectedFrameLoss(startFrameNumber, frameIndex - 1);
            waitingForNextSuccessfulFrame = 0;
        }

        // If we need an IDR frame first, then drop this frame
        if (waitingForIdrFrame) {
            Limelog("Waiting for IDR frame\n");

            dropFrameState();
            return;
        }

        reassembleFrame(frameIndex);

        startFrameNumber = nextFrameNumber;
    }
}

// Add an RTP Packet to the queue
void queueRtpPacket(PRTPFEC_QUEUE_ENTRY queueEntry) {
    int dataOffset;

    dataOffset = sizeof(*queueEntry->packet);
    if (queueEntry->packet->header & FLAG_EXTENSION) {
        dataOffset += 4; // 2 additional fields
    }

    processRtpPayload((PNV_VIDEO_PACKET)(((char*)queueEntry->packet) + dataOffset),
                      queueEntry->length - dataOffset,
                      queueEntry->receiveTimeMs);
}
