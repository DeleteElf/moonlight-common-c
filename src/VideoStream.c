#include "Limelight-internal.h"

#define FIRST_FRAME_MAX 1500
#define FIRST_FRAME_TIMEOUT_SEC 10

#define FIRST_FRAME_PORT 47996

static PRTP_VIDEO_QUEUE rtpQueues;
static int rtpQueueCount;

static PPLT_CRYPTO_CONTEXT decryptionCtx;

static PLT_THREAD udpPingThread;
static PLT_THREAD receiveThread;
static PLT_THREAD decoderThread;

static bool receivedDataFromPeer;
static uint64_t firstDataTimeMs;
static bool receivedFullFrame;

// We can't request an IDR frame until the depacketizer knows
// that a packet was lost. This timeout bounds the time that
// the RTP queue will wait for missing/reordered packets.
#define RTP_QUEUE_DELAY 10

// This is the desired number of video packets that can be
// stored in the socket's receive buffer. 2048 is chosen
// because it should be large enough for all reasonable
// frame sizes (probably 2 or 3 frames) without using too
// much kernel memory with larger packet sizes. It also
// can smooth over transient pauses in network traffic
// and subsequent packet/frame bursts that follow.
#define RTP_RECV_PACKETS_BUFFERED 2048

// Initialize the video stream
void initializeVideoStream(int displayCount) {
    initializeVideoDepacketizer(StreamConfig.packetSize,displayCount);
    rtpQueueCount=displayCount;
    rtpQueues=malloc(sizeof(RTP_VIDEO_QUEUE)*rtpQueueCount);
    for (int i = 0; i < rtpQueueCount; ++i) {
        RTP_VIDEO_QUEUE rtpQueue;
        RtpvInitializeQueue(&rtpQueue);
        rtpQueues[i]=rtpQueue;
    }
//    RtpvInitializeQueue(&rtpQueue);
    decryptionCtx = PltCreateCryptoContext();
    receivedDataFromPeer = false;
    firstDataTimeMs = 0;
    receivedFullFrame = false;
}

// Clean up the video stream
void destroyVideoStream(void) {
    PltDestroyCryptoContext(decryptionCtx);
    destroyVideoDepacketizer();
//    RtpvCleanupQueue(&rtpQueue);
    for (int i = 0; i < rtpQueueCount; ++i) {
        RtpvCleanupQueue(&rtpQueues[i]);
    }
    free(rtpQueues);
}

// UDP Ping proc
static void VideoPingThreadProc(void* context) {
    char legacyPingData[] = { 0x50, 0x49, 0x4E, 0x47 };

    // We do not check for errors here. Socket errors will be handled
    // on the read-side in ReceiveThreadProc(). This avoids potential
    // issues related to receiving ICMP port unreachable messages due
    // to sending a packet prior to the host PC binding to that port.
    int pingCount = 0;
    while (!PltIsThreadInterrupted(&udpPingThread)) {
        if (VideoPingPayload.payload[0] != 0) {
            pingCount++;
            VideoPingPayload.sequenceNumber = BE32(pingCount);
            if(networkSendCallback!=NULL){
                networkSendCallback((char *) &VideoPingPayload,sizeof(VideoPingPayload),SocketChannelVideo,-1);
            }
        }
        else {
            if(networkSendCallback!=NULL){
                networkSendCallback(legacyPingData, sizeof(legacyPingData),SocketChannelVideo,-1);
            }
        }

        PltSleepMsInterruptible(&udpPingThread, 500);
    }
}

// Receive thread proc
static void VideoReceiveThreadProc(void* context) {
    int bufferSize, receiveSize, decryptedSize, minSize;
    char* buffer;
    int queueStatus;
    int waitingForVideoMs;

    decryptedSize = StreamConfig.packetSize + MAX_RTP_HEADER_SIZE;
    minSize = sizeof(RTP_PACKET) ;
    receiveSize = decryptedSize;
    bufferSize = decryptedSize + sizeof(RTPV_QUEUE_ENTRY);
    buffer = NULL;

    waitingForVideoMs = 0;
    while (!PltIsThreadInterrupted(&receiveThread)) {
        if (buffer == NULL) {
            buffer = (char*)malloc(bufferSize);
            if (buffer == NULL) {
                Limelog("Video Receive: malloc() failed\n");
                ListenerCallbacks.connectionTerminated(-1);
                break;
            }
        }
        PRTP_PACKET packet;
        int length=0;
        if(networkReceiveCallback!=NULL){
            BufferPacket bufferPacket;
            bufferPacket.len=receiveSize;
            bufferPacket.buf=buffer;
            networkReceiveCallback(&bufferPacket,SocketChannelVideo);
            if (bufferPacket.len<=0) {
                Limelog("接收视频数据失败\n", (int)LastSocketError());
                ListenerCallbacks.connectionTerminated(-1);
                break;
            }
            length=  bufferPacket.len;
        }else {
            Limelog("未设置有效的networkReceiveCallback");
            break;
        }

        if (length < 0) {
            Limelog("Video Receive: recvUdpSocket() failed: %d\n", (int)LastSocketError());
            ListenerCallbacks.connectionTerminated(LastSocketFail());
            break;
        }
        else if  (length == 0) {
            if (!receivedDataFromPeer) {
                // If we wait many seconds without ever receiving a video packet,
                // assume something is broken and terminate the connection.
                waitingForVideoMs += UDP_RECV_POLL_TIMEOUT_MS;
                if (waitingForVideoMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                    Limelog("Terminating connection due to lack of video traffic\n");
                    ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_TRAFFIC);
                    break;
                }
            }
            
            // Receive timed out; try again
            continue;
        }

        if (!receivedDataFromPeer) {
            receivedDataFromPeer = true;
            Limelog("Received first video packet after %d ms\n", waitingForVideoMs);

            firstDataTimeMs = PltGetMillis();
        }

#ifndef LC_FUZZING
        if (!receivedFullFrame) {
            uint64_t now = PltGetMillis();

            if (now - firstDataTimeMs >= FIRST_FRAME_TIMEOUT_SEC * 1000) {
                Limelog("Terminating connection due to lack of a successful video frame\n");
                ListenerCallbacks.connectionTerminated(ML_ERROR_NO_VIDEO_FRAME);
                break;
            }
        }
#endif

        if (length < minSize) {
            // Runt packet
            continue;
        }

        // Convert fields to host byte-order
        packet = (PRTP_PACKET)&buffer[0];
        packet->sequenceNumber = BE16(packet->sequenceNumber);
        packet->timestamp = BE32(packet->timestamp);
        packet->ssrc = BE32(packet->ssrc);

        // Limelog("receive video packet===========================>%d\n",packet->ssrc);
        queueStatus = RtpvAddPacket(&rtpQueues[packet->ssrc], packet, length, (PRTPV_QUEUE_ENTRY)&buffer[decryptedSize]);
        if (queueStatus == RTPF_RET_QUEUED) {
            // The queue owns the buffer
            buffer = NULL;
        }
    }

    if (buffer != NULL) {
        free(buffer);
    }

}

void notifyKeyFrameReceived(int displayIndex) {
    // Remember that we got a full frame successfully
    receivedFullFrame = true;
    Limelog("收到关键帧,trackIndex:%d\n",displayIndex);
}

// Decoder thread proc
static void VideoDecoderThreadProc(void* context) {
    int trackIndex=0;
    while (!PltIsThreadInterrupted(&decoderThread)) {
        VIDEO_FRAME_HANDLE frameHandle;
        PDECODE_UNIT decodeUnit;
        if (!LiWaitForNextVideoFrame(&frameHandle, &decodeUnit,trackIndex)) {
            return;
        }
        LiCompleteVideoFrame(frameHandle, VideoCallbacks.submitDecodeUnit(decodeUnit),trackIndex);
    }
}

// Read the first frame of the video stream
int readFirstFrame(void) {
    // All that matters is that we close this socket.
    // This starts the flow of video on Gen 3 servers.

    return 0;
}

// Terminate the video stream
void stopVideoStream(void) {
    if (!receivedDataFromPeer) {
        Limelog("No video traffic was ever received from the host!\n");
    }

    VideoCallbacks.stop();

    // Wake up client code that may be waiting on the decode unit queue
    stopVideoDepacketizer();
    
    PltInterruptThread(&udpPingThread);
    PltInterruptThread(&receiveThread);
    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        PltInterruptThread(&decoderThread);
    }
    if (networkChannelStopCallback != NULL) {
        int ret=networkChannelStopCallback(SocketChannelVideo);
        // return 0; //不再直接返回，仍要执行注销 线程逻辑
        if(ret>0){
            //考虑打印错误
        }
    }
    PltJoinThread(&udpPingThread);
    PltJoinThread(&receiveThread);
    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        PltJoinThread(&decoderThread);
    }
    VideoCallbacks.cleanup();
}

// Start the video stream
int startVideoStream(void* rendererContext, int drFlags) {
    int err;
    // This must be called before the decoder thread starts submitting
    // decode units
    LC_ASSERT(NegotiatedVideoFormat != 0);
    err = VideoCallbacks.setup(NegotiatedVideoFormat, StreamConfig.width,
        StreamConfig.height, StreamConfig.fps, rendererContext, drFlags);
    if (err != 0) {
        return err;
    }

    VideoCallbacks.start();

    err = PltCreateThread("VideoRecv", VideoReceiveThreadProc, NULL, &receiveThread);
    if (err != 0) {
        VideoCallbacks.stop();
        VideoCallbacks.cleanup();
        return err;
    }

    if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
        err = PltCreateThread("VideoDec", VideoDecoderThreadProc, NULL, &decoderThread);//todo:这里需要传递正确的上下文进去，以获得displayIndex
        if (err != 0) {
            VideoCallbacks.stop();
            PltInterruptThread(&receiveThread);
            PltJoinThread(&receiveThread);
            VideoCallbacks.cleanup();
            return err;
        }
    }

    // Start pinging before reading the first frame so GFE knows where
    // to send UDP data
    // void* displayIndex_ptr=&displayIndex;
    err = PltCreateThread("VideoPing", VideoPingThreadProc, NULL, &udpPingThread);
    if (err != 0) {
        VideoCallbacks.stop();
        stopVideoDepacketizer();
        PltInterruptThread(&receiveThread);
        if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
            PltInterruptThread(&decoderThread);
        }
        PltJoinThread(&receiveThread);
        if ((VideoCallbacks.capabilities & (CAPABILITY_DIRECT_SUBMIT | CAPABILITY_PULL_RENDERER)) == 0) {
            PltJoinThread(&decoderThread);
        }
        VideoCallbacks.cleanup();
        return err;
    }

    return 0;
}

int getLastSeenFrame(int trackIndex){
    return rtpQueues[trackIndex].lastSeenFrame;
}

int getLastGoodFrame(int trackIndex){
    return rtpQueues[trackIndex].lastGoodFrame;
}
void setLastGoodFrame(int trackIndex,int frameIndex){
    rtpQueues[trackIndex].lastGoodFrame = frameIndex;
    rtpQueues[trackIndex].intervalGoodFrameCount++;
}
const RTP_VIDEO_STATS* LiGetRTPVideoStats(int trackIndex) {
    return &rtpQueues[trackIndex].stats;
}