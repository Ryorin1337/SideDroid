// JNI bridge: runs the shared core client and feeds frames straight into the
// hardware decoder on native threads, away from the JVM's garbage collector.
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sidedroid/client.h"

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
    ~Decoder() { shutdown(); setSurface(nullptr); }

    void setSurface(ANativeWindow* w) {
        std::lock_guard<std::mutex> lock(mutex_);
        closeCodecLocked();
        if (window_) ANativeWindow_release(window_);
        window_ = w;
        awaitingKey_ = true;
    }

    void configure(const Config& c) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (c.encodeWidth != width_ || c.encodeHeight != height_ || c.codec != codec_) closeCodecLocked();
        width_ = c.encodeWidth;
        height_ = c.encodeHeight;
        codec_ = c.codec;
        fps_ = c.fps;
        awaitingKey_ = true;
    }

    // Returns false when a keyframe is needed to continue.
    bool feed(const FrameHeader& h, const uint8_t* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!window_ || width_ == 0) return false;
        if (awaitingKey_) {
            if (!h.keyframe) return false;
            awaitingKey_ = false;
        }
        if (!codecObj_ && !openCodecLocked()) {
            awaitingKey_ = true;
            return false;
        }
        ssize_t idx = AMediaCodec_dequeueInputBuffer(codecObj_, 20000);
        if (idx < 0) {
            // Decoder is behind. Queueing more only adds latency, so restart from a keyframe.
            awaitingKey_ = true;
            return false;
        }
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(codecObj_, static_cast<size_t>(idx), &cap);
        if (!buf || size > cap) {
            AMediaCodec_queueInputBuffer(codecObj_, static_cast<size_t>(idx), 0, 0, h.captureUs, 0);
            awaitingKey_ = true;
            return false;
        }
        memcpy(buf, data, size);
        {
            std::lock_guard<std::mutex> t(timesMutex_);
            queuedAt_[h.captureUs] = monotonicUs();
            while (queuedAt_.size() > 64) queuedAt_.erase(queuedAt_.begin());
        }
        media_status_t st = AMediaCodec_queueInputBuffer(codecObj_, static_cast<size_t>(idx), 0, size, h.captureUs, 0);
        if (st != AMEDIA_OK) {
            LOGW("queueInputBuffer: %d", st);
            closeCodecLocked();
            awaitingKey_ = true;
            return false;
        }
        return true;
    }

    uint32_t lastDecodeUs() const { return decodeUs_.load(); }
    void shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        closeCodecLocked();
    }

private:
    bool openCodecLocked() {
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
            // A strict vendor decoder may refuse the extras: fall back to the plain set.
            LOGW("configure with low latency keys failed: %d", st);
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
        outputRunning_ = true;
        outputThread_ = std::thread(&Decoder::outputLoop, this, c);
        LOGI("decoder started %s %dx%d", mime, width_, height_);
        return true;
    }

    void closeCodecLocked() {
        if (!codecObj_) return;
        outputRunning_ = false;
        if (outputThread_.joinable()) outputThread_.join();
        AMediaCodec_stop(codecObj_);
        AMediaCodec_delete(codecObj_);
        codecObj_ = nullptr;
    }

    // Separate thread, so each picture is shown the moment it is decoded.
    void outputLoop(AMediaCodec* c) {
        AMediaCodecBufferInfo info;
        while (outputRunning_) {
            ssize_t idx = AMediaCodec_dequeueOutputBuffer(c, &info, 10000);
            if (idx < 0) continue;
            {
                std::lock_guard<std::mutex> t(timesMutex_);
                auto it = queuedAt_.find(static_cast<uint64_t>(info.presentationTimeUs));
                if (it != queuedAt_.end()) {
                    decodeUs_ = static_cast<uint32_t>(monotonicUs() - it->second);
                    queuedAt_.erase(it);
                }
            }
            AMediaCodec_releaseOutputBuffer(c, static_cast<size_t>(idx), info.size > 0);
        }
    }

    std::mutex mutex_, timesMutex_;
    ANativeWindow* window_ = nullptr;
    AMediaCodec* codecObj_ = nullptr;
    std::thread outputThread_;
    std::atomic<bool> outputRunning_{false};
    std::atomic<uint32_t> decodeUs_{0};
    std::map<uint64_t, uint64_t> queuedAt_;
    int width_ = 0, height_ = 0, fps_ = 0;
    uint8_t codec_ = 0;
    bool awaitingKey_ = true;
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
        call(s->listener, "onConfig", "(IIIIIII)V", c.displayWidth, c.displayHeight, c.encodeWidth, c.encodeHeight,
             c.fps, c.codec, static_cast<jint>(c.bitrateKbps));
    };
    cb.onFrame = [weak](const FrameHeader& h, const uint8_t* d, size_t n) {
        auto s = weak.lock();
        if (!s) return;
        if (!s->decoder.feed(h, d, n)) s->client->requestKeyframe();
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
    if (w) s->client->requestKeyframe();
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

JNIEXPORT void JNICALL Java_com_sidedroid_app_NativeBridge_sendSetDisplay(JNIEnv*, jobject, jint w, jint h, jint hz, jint kbps) {
    if (auto s = current())
        s->client->sendSetDisplay(SetDisplay{static_cast<uint16_t>(w), static_cast<uint16_t>(h), static_cast<uint16_t>(hz), static_cast<uint32_t>(kbps)});
}

JNIEXPORT jint JNICALL Java_com_sidedroid_app_NativeBridge_rttUs(JNIEnv*, jobject) {
    auto s = current();
    return s ? static_cast<jint>(s->client->lastRttUs()) : 0;
}

}  // extern "C"
