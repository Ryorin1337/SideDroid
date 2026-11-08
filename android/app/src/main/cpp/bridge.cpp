// JNI bridge: receives and decodes on separate native threads, away from the JVM's garbage collector.
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sidedroid/client.h"
#include "decoder_queue.h"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "SideDroid", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "SideDroid", __VA_ARGS__)

using namespace sd;

namespace {

JavaVM* g_vm = nullptr;

// Attaches for the length of one callback. These fire rarely, never per frame.
class JniScope {
public:
    JniScope() {
        if (g_vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6) != JNI_OK) {
            g_vm->AttachCurrentThread(&env_, nullptr);
            attached_ = true;
        }
    }
    ~JniScope() {
        if (attached_) g_vm->DetachCurrentThread();
    }
    JNIEnv* operator->() { return env_; }
    JNIEnv* get() { return env_; }

private:
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

class Decoder {
public:
    ~Decoder() { shutdown(); }

    void start(std::function<void()> requestKeyframe, std::function<void()> reportDrop) {
        std::lock_guard<std::mutex> lock(mutex_);
        requestKeyframe_ = std::move(requestKeyframe);
        reportDrop_ = std::move(reportDrop);
        running_ = true;
        inputThread_ = std::thread(&Decoder::inputLoop, this);
    }

    void setSurface(ANativeWindow* w) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingWindow_) ANativeWindow_release(pendingWindow_);
        pendingWindow_ = w;
        resetLocked();
    }

    void configure(const Config& c) {
        std::lock_guard<std::mutex> lock(mutex_);
        pendingConfig_ = c;
        queue_.configure(c.fps);
        generation_ = queue_.generation();
        ready_.notify_one();
    }

    void enqueue(const FrameHeader& h, const uint8_t* data, size_t size) {
        bool dropped;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_ || !pendingWindow_ || pendingConfig_.encodeWidth == 0) return;
            const uint64_t before = queue_.drops();
            queue_.push(h, data, size, monotonicUs());
            dropped = queue_.drops() != before;
            generation_ = queue_.generation();
            ready_.notify_one();
        }
        if (dropped && reportDrop_) reportDrop_();
    }

    uint32_t lastDecodeUs() const { return decodeUs_.load(); }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            resetLocked();
        }
        if (inputThread_.joinable()) inputThread_.join();
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingWindow_) ANativeWindow_release(pendingWindow_);
        pendingWindow_ = nullptr;
    }

private:
    void resetLocked() {
        queue_.reset();
        generation_ = queue_.generation();
        ready_.notify_one();
    }

    void inputLoop() {
        uint64_t appliedGeneration = UINT64_MAX;
        uint64_t lastKeyRequestUs = 0;
        uint64_t lastKeyRequestGeneration = UINT64_MAX;
        while (true) {
            std::optional<DecoderFrame> frame;
            Config config;
            ANativeWindow* window = nullptr;
            uint64_t generation, maxAgeUs;
            bool changed, dropped, request = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                ready_.wait_for(lock, std::chrono::milliseconds(100), [&] {
                    return !running_ || queue_.generation() != appliedGeneration || !queue_.empty();
                });
                if (!running_) break;
                const uint64_t before = queue_.drops();
                frame = queue_.take(monotonicUs());
                dropped = queue_.drops() != before;
                generation = queue_.generation();
                generation_ = generation;
                maxAgeUs = queue_.maxAgeUs();
                changed = generation != appliedGeneration;
                if (changed) {
                    config = pendingConfig_;
                    window = pendingWindow_;
                    if (window) ANativeWindow_acquire(window);
                }
                const uint64_t now = monotonicUs();
                if (queue_.awaitingKey() && pendingWindow_ && pendingConfig_.encodeWidth &&
                    (generation != lastKeyRequestGeneration || now - lastKeyRequestUs >= 250000)) {
                    lastKeyRequestUs = now;
                    lastKeyRequestGeneration = generation;
                    request = true;
                }
            }
            if (dropped && reportDrop_) reportDrop_();
            if (changed) {
                const bool sameStream = window_ == window && width_ == config.encodeWidth &&
                                        height_ == config.encodeHeight && codec_ == config.codec && fps_ == config.fps;
                if (sameStream) flushCodec(generation);
                else closeCodec();
                if (window_) ANativeWindow_release(window_);
                window_ = window;
                width_ = config.encodeWidth;
                height_ = config.encodeHeight;
                codec_ = config.codec;
                fps_ = config.fps;
                appliedGeneration = generation;
            }
            if (request && requestKeyframe_) requestKeyframe_();
            if (!frame || generation != generation_.load()) continue;
            if (!submit(*frame, generation, maxAgeUs)) {
                dropped = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (generation == queue_.generation()) {
                        queue_.drop();
                        generation_ = queue_.generation();
                        ready_.notify_one();
                        dropped = true;
                    }
                }
                if (dropped && reportDrop_) reportDrop_();
            }
        }
        closeCodec();
        if (window_) ANativeWindow_release(window_);
        window_ = nullptr;
    }

    bool submit(const DecoderFrame& frame, uint64_t generation, uint64_t maxAgeUs) {
        if (!window_ || !width_) return false;
        if (!codecObj_ && !openCodec(generation)) return false;
        const uint64_t age = monotonicUs() - frame.receivedUs;
        if (age >= maxAgeUs) return false;
        const int64_t waitUs = static_cast<int64_t>(std::min<uint64_t>(20000, maxAgeUs - age));
        ssize_t idx = AMediaCodec_dequeueInputBuffer(codecObj_, waitUs);
        if (idx < 0) return false;
        codecDirty_ = true;  // A flush must also reclaim an input buffer abandoned before queueing.
        if (generation != generation_.load() || monotonicUs() - frame.receivedUs >= maxAgeUs) return false;
        const auto& h = frame.header;
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(codecObj_, static_cast<size_t>(idx), &cap);
        if (!buf || frame.data.size() > cap) return false;
        memcpy(buf, frame.data.data(), frame.data.size());
        {
            std::lock_guard<std::mutex> t(timesMutex_);
            queuedAt_[h.captureUs] = frame.receivedUs;
            while (queuedAt_.size() > 64) queuedAt_.erase(queuedAt_.begin());
        }
        media_status_t st = AMediaCodec_queueInputBuffer(codecObj_, static_cast<size_t>(idx), 0, frame.data.size(), h.captureUs, 0);
        if (st != AMEDIA_OK) {
            LOGW("queueInputBuffer: %d", st);
            return false;
        }
        return true;
    }

    bool openCodec(uint64_t generation) {
        const char* mime = codec_ == kCodecHevc ? "video/hevc" : "video/avc";
        AMediaCodec* c = AMediaCodec_createDecoderByType(mime);
        if (!c) {
            LOGW("no decoder for %s", mime);
            return false;
        }
        AMediaFormat* f = AMediaFormat_new();
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, width_);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, height_);
        AMediaFormat_setInt32(f, "frame-rate", fps_);
        AMediaFormat_setInt32(f, "priority", 0);  // realtime
        // Official on Android 11+. Older devices have only vendor switches, and
        // decoders ignore keys they do not know, so setting them all is safe.
        AMediaFormat_setInt32(f, "low-latency", 1);
        AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
        AMediaFormat_setInt32(f, "vendor.low-latency.enable", 1);
        AMediaFormat_setInt32(f, "vendor.hisi-ext-low-latency-video-dec.video-scene-for-low-latency-req", 1);
        AMediaFormat_setInt32(f, "vdec-lowlatency", 1);

        media_status_t st = AMediaCodec_configure(c, f, window_, nullptr, 0);
        if (st != AMEDIA_OK) {
            // Keep the standard low-latency request when only a vendor extension is rejected.
            LOGW("configure with vendor keys failed: %d", st);
            AMediaCodec_delete(c);
            c = AMediaCodec_createDecoderByType(mime);
            if (!c) { AMediaFormat_delete(f); return false; }
            AMediaFormat_delete(f);
            f = AMediaFormat_new();
            AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
            AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, width_);
            AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, height_);
            AMediaFormat_setInt32(f, "frame-rate", fps_);
            AMediaFormat_setInt32(f, "priority", 0);
            AMediaFormat_setInt32(f, "low-latency", 1);
            st = AMediaCodec_configure(c, f, window_, nullptr, 0);
        }
        if (st != AMEDIA_OK) {
            LOGW("configure with standard low latency keys failed: %d", st);
            AMediaCodec_delete(c);
            c = AMediaCodec_createDecoderByType(mime);
            if (!c) { AMediaFormat_delete(f); return false; }
            AMediaFormat_delete(f);
            f = AMediaFormat_new();
            AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
            AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, width_);
            AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, height_);
            st = AMediaCodec_configure(c, f, window_, nullptr, 0);
        }
        AMediaFormat_delete(f);
        if (st != AMEDIA_OK || AMediaCodec_start(c) != AMEDIA_OK) {
            LOGW("cannot start decoder: %d", st);
            AMediaCodec_delete(c);
            return false;
        }
        codecObj_ = c;
        codecDirty_ = false;
        outputGeneration_ = generation;
        outputRunning_ = true;
        outputThread_ = std::thread(&Decoder::outputLoop, this, c);
        LOGI("decoder started %s %dx%d", mime, width_, height_);
        return true;
    }

    void closeCodec() {
        if (!codecObj_) return;
        outputRunning_ = false;
        if (outputThread_.joinable()) outputThread_.join();
        AMediaCodec_stop(codecObj_);
        AMediaCodec_delete(codecObj_);
        codecObj_ = nullptr;
        codecDirty_ = false;
        std::lock_guard<std::mutex> t(timesMutex_);
        queuedAt_.clear();
        decodeUs_ = 0;
    }

    void flushCodec(uint64_t generation) {
        if (!codecObj_) return;
        // A slow initial configure or flush can expire its keyframe. Keep the empty codec warm
        // for the next keyframe instead of spending its entire age budget on another reset.
        if (!codecDirty_) {
            outputGeneration_ = generation;
            return;
        }
        outputRunning_ = false;
        if (outputThread_.joinable()) outputThread_.join();
        if (AMediaCodec_flush(codecObj_) != AMEDIA_OK) {
            closeCodec();
            return;
        }
        {
            std::lock_guard<std::mutex> t(timesMutex_);
            queuedAt_.clear();
            decodeUs_ = 0;
        }
        codecDirty_ = false;
        outputGeneration_ = generation;
        outputRunning_ = true;
        outputThread_ = std::thread(&Decoder::outputLoop, this, codecObj_);
    }

    void recordOutput(const AMediaCodecBufferInfo& info) {
        std::lock_guard<std::mutex> t(timesMutex_);
        auto it = queuedAt_.find(static_cast<uint64_t>(info.presentationTimeUs));
        if (it != queuedAt_.end()) {
            decodeUs_ = static_cast<uint32_t>(monotonicUs() - it->second);
            queuedAt_.erase(it);
        }
    }

    void outputLoop(AMediaCodec* c) {
        AMediaCodecBufferInfo info;
        while (outputRunning_) {
            ssize_t idx = AMediaCodec_dequeueOutputBuffer(c, &info, 10000);
            if (idx < 0) continue;
            recordOutput(info);
            // References have already been decoded. Only obsolete presentation buffers are dropped.
            while (outputRunning_) {
                AMediaCodecBufferInfo newer;
                ssize_t next = AMediaCodec_dequeueOutputBuffer(c, &newer, 0);
                if (next == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED || next == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
                    continue;
                if (next < 0) break;
                AMediaCodec_releaseOutputBuffer(c, static_cast<size_t>(idx), false);
                idx = next;
                info = newer;
                recordOutput(info);
            }
            AMediaCodec_releaseOutputBuffer(c, static_cast<size_t>(idx),
                                           outputRunning_ && outputGeneration_.load() == generation_.load() && info.size > 0);
        }
    }

    std::mutex mutex_, timesMutex_;
    std::condition_variable ready_;
    DecoderQueue queue_;
    Config pendingConfig_;
    ANativeWindow* pendingWindow_ = nullptr;
    std::function<void()> requestKeyframe_;
    std::function<void()> reportDrop_;
    std::thread inputThread_;
    bool running_ = false;
    std::atomic<uint64_t> generation_{0};
    ANativeWindow* window_ = nullptr;
    AMediaCodec* codecObj_ = nullptr;
    bool codecDirty_ = false;
    std::thread outputThread_;
    std::atomic<uint64_t> outputGeneration_{0};
    std::atomic<bool> outputRunning_{false};
    std::atomic<uint32_t> decodeUs_{0};
    std::map<uint64_t, uint64_t> queuedAt_;
    int width_ = 0, height_ = 0, fps_ = 0;
    uint8_t codec_ = 0;
};

struct Session {
    std::unique_ptr<Client> client;
    Decoder decoder;
    jobject listener = nullptr;
};

std::mutex g_sessionMutex;
std::shared_ptr<Session> g_session;

std::shared_ptr<Session> current() {
    std::lock_guard<std::mutex> lock(g_sessionMutex);
    return g_session;
}

std::string toStd(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c);
    env->ReleaseStringUTFChars(s, c);
    return r;
}

void call(jobject listener, const char* name, const char* sig, ...) {
    JniScope env;
    jclass cls = env->GetObjectClass(listener);
    jmethodID m = env->GetMethodID(cls, name, sig);
    va_list ap;
    va_start(ap, sig);
    if (m) env->CallVoidMethodV(listener, m, ap);
    va_end(ap);
    env->DeleteLocalRef(cls);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    return JNI_VERSION_1_6;
}

// Returns null on success, or a short error code: connect, auth, send, udp.
JNIEXPORT jstring JNICALL Java_com_sidedroid_app_NativeBridge_start(
    JNIEnv* env, jobject, jstring host, jint port, jint mode, jbyteArray clientId, jbyteArray secret, jstring pairingCode,
    jstring deviceName, jint width, jint height, jint dpi, jint codecs, jint maxW, jint maxH, jintArray rates,
    jboolean udp, jobject listener) {
    auto session = std::make_shared<Session>();
    session->listener = env->NewGlobalRef(listener);

    ClientOptions opts;
    opts.host = toStd(env, host);
    opts.port = static_cast<uint16_t>(port);
    opts.mode = static_cast<AuthMode>(mode);
    if (clientId && env->GetArrayLength(clientId) == 16)
        env->GetByteArrayRegion(clientId, 0, 16, reinterpret_cast<jbyte*>(opts.clientId.data()));
    if (secret && env->GetArrayLength(secret) == 32)
        env->GetByteArrayRegion(secret, 0, 32, reinterpret_cast<jbyte*>(opts.pairedSecret.data()));
    opts.pairingCode = toStd(env, pairingCode);
    opts.hello.deviceName = toStd(env, deviceName);
    opts.hello.nativeWidth = static_cast<uint16_t>(width);
    opts.hello.nativeHeight = static_cast<uint16_t>(height);
    opts.hello.densityDpi = static_cast<uint16_t>(dpi);
    opts.hello.codecs = static_cast<uint8_t>(codecs);
    opts.hello.maxDecodeWidth = static_cast<uint16_t>(maxW);
    opts.hello.maxDecodeHeight = static_cast<uint16_t>(maxH);
    opts.hello.wantUdp = udp;
    if (rates) {
        jsize n = env->GetArrayLength(rates);
        std::vector<jint> r(static_cast<size_t>(n));
        env->GetIntArrayRegion(rates, 0, n, r.data());
        for (jint v : r) opts.hello.refreshRates.push_back(static_cast<uint16_t>(v));
    }

    // Callbacks hold a weak reference, so that a stopped session can be destroyed.
    std::weak_ptr<Session> weak = session;
    Client::Callbacks cb;
    cb.onConfig = [weak](const Config& c) {
        auto s = weak.lock();
        if (!s) return;
        s->decoder.configure(c);
        call(s->listener, "onConfig", "(IIIIIIIZIIIII)V", c.displayWidth, c.displayHeight, c.encodeWidth, c.encodeHeight,
             c.fps, c.codec, static_cast<jint>(c.bitrateKbps), c.mirror ? JNI_TRUE : JNI_FALSE, c.manualWidth, c.manualHeight, c.manualHz, c.manualScale, static_cast<jint>(c.fill));
    };
    cb.onDisplayOptions = [weak](const DisplayOptions& o) {
        auto s = weak.lock();
        if (!s) return;
        JniScope e;
        std::vector<jint> flat;
        for (const auto& r : o.resolutions) {
            flat.push_back(r.width);
            flat.push_back(r.height);
        }
        jintArray arr = e->NewIntArray(static_cast<jsize>(flat.size()));
        e->SetIntArrayRegion(arr, 0, static_cast<jsize>(flat.size()), flat.data());
        call(s->listener, "onDisplayOptions", "(IIIIII[I)V", o.phone.width, o.phone.height, o.pc.width, o.pc.height,
             o.recommendedScale, o.currentScale, arr);
        e->DeleteLocalRef(arr);
    };
    cb.onFrame = [weak](const FrameHeader& h, const uint8_t* d, size_t n) {
        auto s = weak.lock();
        if (!s) return;
        s->decoder.enqueue(h, d, n);
        s->client->reportDecodeUs(s->decoder.lastDecodeUs());
    };
    cb.onDisconnected = [weak](const std::string& why) {
        auto s = weak.lock();
        if (!s) return;
        JniScope e;
        jstring js = e->NewStringUTF(why.c_str());
        call(s->listener, "onDisconnected", "(Ljava/lang/String;)V", js);
        e->DeleteLocalRef(js);
    };
    cb.onPaired = [weak](const Key32& k) {
        auto s = weak.lock();
        if (!s) return;
        JniScope e;
        jbyteArray arr = e->NewByteArray(32);
        e->SetByteArrayRegion(arr, 0, 32, reinterpret_cast<const jbyte*>(k.data()));
        call(s->listener, "onPaired", "([B)V", arr);
        e->DeleteLocalRef(arr);
    };

    session->client = std::make_unique<Client>(std::move(opts), std::move(cb));
    // Session destroys the decoder before the client, so this callback stays valid through shutdown.
    session->decoder.start([client = session->client.get()] { client->requestKeyframe(); },
                           [client = session->client.get()] { client->reportDecoderDrop(); });
    {
        std::lock_guard<std::mutex> lock(g_sessionMutex);
        g_session = session;
    }
    std::string error;
    if (!session->client->start(&error)) {
        std::lock_guard<std::mutex> lock(g_sessionMutex);
        g_session.reset();
        env->DeleteGlobalRef(session->listener);
        session->listener = nullptr;
        return env->NewStringUTF(error.c_str());
    }
    return nullptr;
}

JNIEXPORT void JNICALL Java_com_sidedroid_app_NativeBridge_stop(JNIEnv* env, jobject) {
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(g_sessionMutex);
        s = std::move(g_session);
        g_session.reset();
    }
    if (!s) return;
    s->client->stop();
    s->decoder.shutdown();
    if (s->listener) env->DeleteGlobalRef(s->listener);
    s->listener = nullptr;
}

JNIEXPORT void JNICALL Java_com_sidedroid_app_NativeBridge_setSurface(JNIEnv* env, jobject, jobject surface) {
    auto s = current();
    if (!s) return;
    ANativeWindow* w = surface ? ANativeWindow_fromSurface(env, surface) : nullptr;
    s->decoder.setSurface(w);
}

// Flat array, five ints per contact: id, action, x, y, pressure.
JNIEXPORT void JNICALL Java_com_sidedroid_app_NativeBridge_sendTouch(JNIEnv* env, jobject, jintArray data, jint count) {
    auto s = current();
    if (!s || count <= 0) return;
    std::vector<jint> v(static_cast<size_t>(count) * 5);
    env->GetIntArrayRegion(data, 0, count * 5, v.data());
    std::vector<TouchPoint> pts(static_cast<size_t>(count));
    for (jint i = 0; i < count; ++i) {
        const jint* p = &v[static_cast<size_t>(i) * 5];
        pts[static_cast<size_t>(i)] = {static_cast<uint8_t>(p[0]), static_cast<TouchAction>(p[1]), static_cast<uint16_t>(p[2]),
                                       static_cast<uint16_t>(p[3]), static_cast<uint16_t>(p[4])};
    }
    s->client->sendTouch(pts);
}

JNIEXPORT void JNICALL Java_com_sidedroid_app_NativeBridge_sendViewport(JNIEnv*, jobject, jint w, jint h) {
    if (auto s = current()) s->client->sendViewport(static_cast<uint16_t>(w), static_cast<uint16_t>(h));
}

JNIEXPORT void JNICALL Java_com_sidedroid_app_NativeBridge_sendSetDisplay(JNIEnv*, jobject, jint w, jint h, jint hz, jboolean mirror, jint scale, jint fill) {
    // The phone has no quality control of its own, so that setting stays as it is.
    if (auto s = current())
        s->client->sendSetDisplay(SetDisplay{static_cast<uint16_t>(w), static_cast<uint16_t>(h), static_cast<uint16_t>(hz),
                                             kKeepBitrate, mirror ? kMirrorOn : kMirrorOff, static_cast<uint16_t>(scale),
                                             static_cast<uint8_t>(fill)});
}

JNIEXPORT jint JNICALL Java_com_sidedroid_app_NativeBridge_rttUs(JNIEnv*, jobject) {
    auto s = current();
    return s ? static_cast<jint>(s->client->lastRttUs()) : 0;
}

}  // extern "C"
