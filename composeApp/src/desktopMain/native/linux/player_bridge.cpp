// Linux native player bridge (Phase 1 — X11 / XWayland via mpv `wid` embedding).
//
// Ported from src/desktopMain/native/windows/player_bridge.cpp. The mpv control
// surface (properties, tracks, subtitles, seeking) is identical to Windows; only
// the windowing layer differs: instead of a WS_CHILD container HWND we create an
// intermediate X11 child window under the AWT Canvas XID and hand that child to
// mpv via the `wid` option. libmpv is loaded with dlopen (no link-time dependency),
// matching the Windows libmpv-2.dll strategy.
//
// All Xlib calls on our own Display connection are confined to a single worker
// thread (setup + event/resize loop), so Xlib thread-safety is preserved without
// XInitThreads(). mpv's client API is thread-safe and is called from JVM threads
// under mpvMutex, exactly as on Windows.
//
// Phase 1 intentionally STUBS the HTML controls overlay: updateControls is a no-op
// and warmupWebView2/shutdownWebView2Warmup/applyWindowChrome are harmless stubs
// (never invoked on Linux — callers gate on DesktopHostOs.WINDOWS). User input is
// handled on the JVM side (see PlayerEngine.desktop.kt input layer), not here.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <X11/Xlib.h>
#include <jni.h>
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

extern "C" {
typedef struct mpv_handle mpv_handle;

typedef enum mpv_format {
    MPV_FORMAT_NONE = 0,
    MPV_FORMAT_STRING = 1,
    MPV_FORMAT_OSD_STRING = 2,
    MPV_FORMAT_FLAG = 3,
    MPV_FORMAT_INT64 = 4,
    MPV_FORMAT_DOUBLE = 5,
} mpv_format;

typedef enum mpv_event_id {
    MPV_EVENT_NONE = 0,
    MPV_EVENT_SHUTDOWN = 1,
} mpv_event_id;

typedef struct mpv_event {
    mpv_event_id event_id;
    int error;
    uint64_t reply_userdata;
    void *data;
} mpv_event;
}

namespace {

// Marker used only so dladdr() can resolve this shared object's own path.
void nuvioLinuxBridgeMarker() {}

std::string bridgeDirectory() {
    Dl_info info = {};
    if (dladdr(reinterpret_cast<void *>(&nuvioLinuxBridgeMarker), &info) && info.dli_fname) {
        std::string path(info.dli_fname);
        size_t separator = path.find_last_of('/');
        if (separator != std::string::npos) {
            return path.substr(0, separator);
        }
    }
    return std::string();
}

// ---- UTF-16 <-> UTF-8 (full Unicode, matching the Windows bridge fidelity) ----

std::string utf16ToUtf8(const jchar *data, jsize length) {
    std::string out;
    out.reserve((size_t)length + 8);
    for (jsize i = 0; i < length; i++) {
        uint32_t cp = (uint32_t)data[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < length) {
            uint32_t lo = (uint32_t)data[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i++;
            }
        }
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

void utf8ToUtf16(const std::string &value, std::vector<jchar> &out) {
    size_t i = 0;
    size_t n = value.size();
    while (i < n) {
        unsigned char c = (unsigned char)value[i];
        uint32_t cp = 0;
        int extra = 0;
        if (c < 0x80) {
            cp = c;
            extra = 0;
        } else if ((c >> 5) == 0x6) {
            cp = c & 0x1F;
            extra = 1;
        } else if ((c >> 4) == 0xE) {
            cp = c & 0x0F;
            extra = 2;
        } else if ((c >> 3) == 0x1E) {
            cp = c & 0x07;
            extra = 3;
        } else {
            i++;
            continue;
        }
        if (i + (size_t)extra >= n) break;
        bool ok = true;
        for (int k = 1; k <= extra; k++) {
            unsigned char cc = (unsigned char)value[i + (size_t)k];
            if ((cc >> 6) != 0x2) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        i += (size_t)extra + 1;
        if (!ok) continue;
        if (cp < 0x10000) {
            out.push_back((jchar)cp);
        } else {
            cp -= 0x10000;
            out.push_back((jchar)(0xD800 + (cp >> 10)));
            out.push_back((jchar)(0xDC00 + (cp & 0x3FF)));
        }
    }
}

std::string jstringToUtf8(JNIEnv *env, jstring value) {
    if (!value) return std::string();
    jsize length = env->GetStringLength(value);
    const jchar *chars = env->GetStringChars(value, nullptr);
    if (!chars) return std::string();
    std::string result = utf16ToUtf8(chars, length);
    env->ReleaseStringChars(value, chars);
    return result;
}

jstring newJavaStringUtf8(JNIEnv *env, const std::string &value) {
    std::vector<jchar> utf16;
    utf8ToUtf16(value, utf16);
    return env->NewString(utf16.data(), (jsize)utf16.size());
}

std::vector<std::string> jstringArrayToVector(JNIEnv *env, jobjectArray values) {
    std::vector<std::string> result;
    if (!values) return result;
    jsize count = env->GetArrayLength(values);
    result.reserve((size_t)count);
    for (jsize index = 0; index < count; index++) {
        jstring item = (jstring)env->GetObjectArrayElement(values, index);
        std::string value = jstringToUtf8(env, item);
        if (!value.empty()) {
            result.push_back(value);
        }
        env->DeleteLocalRef(item);
    }
    return result;
}

void throwJavaError(JNIEnv *env, const std::string &message) {
    jclass exceptionClass = env->FindClass("java/lang/IllegalStateException");
    if (exceptionClass) {
        env->ThrowNew(exceptionClass, message.c_str());
    }
}

std::string trim(const std::string &value) {
    const char *spaces = " \t\r\n";
    size_t start = value.find_first_not_of(spaces);
    if (start == std::string::npos) return std::string();
    size_t end = value.find_last_not_of(spaces);
    return value.substr(start, end - start + 1);
}

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return (char)std::tolower(ch);
    });
    return value;
}

bool containsCaseInsensitive(const std::string &haystack, const std::string &needle) {
    return lowerCopy(haystack).find(lowerCopy(needle)) != std::string::npos;
}

std::string jsonEscape(const std::string &value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': result += "\\\\"; break;
            case '"': result += "\\\""; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
                    result += buffer;
                } else {
                    result.push_back((char)ch);
                }
        }
    }
    return result;
}

// ---- libmpv loaded via dlopen (mirrors the Windows MpvApi) ----

struct MpvApi {
    using mpv_create_fn = mpv_handle *(*)();
    using mpv_initialize_fn = int (*)(mpv_handle *);
    using mpv_terminate_destroy_fn = void (*)(mpv_handle *);
    using mpv_set_option_fn = int (*)(mpv_handle *, const char *, mpv_format, void *);
    using mpv_set_option_string_fn = int (*)(mpv_handle *, const char *, const char *);
    using mpv_set_property_fn = int (*)(mpv_handle *, const char *, mpv_format, void *);
    using mpv_set_property_string_fn = int (*)(mpv_handle *, const char *, const char *);
    using mpv_get_property_fn = int (*)(mpv_handle *, const char *, mpv_format, void *);
    using mpv_command_fn = int (*)(mpv_handle *, const char **);
    using mpv_error_string_fn = const char *(*)(int);
    using mpv_free_fn = void (*)(void *);
    using mpv_wait_event_fn = mpv_event *(*)(mpv_handle *, double);
    using mpv_wakeup_fn = void (*)(mpv_handle *);

    void *library = nullptr;
    std::once_flag loadOnce;
    std::string loadFailure;

    mpv_create_fn create = nullptr;
    mpv_initialize_fn initialize = nullptr;
    mpv_terminate_destroy_fn terminateDestroy = nullptr;
    mpv_set_option_fn setOption = nullptr;
    mpv_set_option_string_fn setOptionString = nullptr;
    mpv_set_property_fn setProperty = nullptr;
    mpv_set_property_string_fn setPropertyString = nullptr;
    mpv_get_property_fn getProperty = nullptr;
    mpv_command_fn command = nullptr;
    mpv_error_string_fn errorString = nullptr;
    mpv_free_fn freeValue = nullptr;
    mpv_wait_event_fn waitEvent = nullptr;
    mpv_wakeup_fn wakeup = nullptr;

    void ensureLoaded() {
        std::call_once(loadOnce, [this]() { load(); });
        if (!library) {
            throw std::runtime_error(loadFailure.empty() ? "Unable to load libmpv.so.2." : loadFailure);
        }
    }

    std::string errorText(int error) {
        if (!errorString) return "unknown";
        const char *text = errorString(error);
        return text ? text : "unknown";
    }

    void load() {
        std::vector<std::string> candidates;

        const char *envPath = std::getenv("NUVIO_LIBMPV_PATH");
        if (envPath && *envPath) {
            candidates.emplace_back(envPath);
        }

        std::string selfDir = bridgeDirectory();
        if (!selfDir.empty()) {
            candidates.push_back(selfDir + "/libmpv.so.2");
            candidates.push_back(selfDir + "/libmpv.so");
        }
        candidates.push_back("libmpv.so.2");
        candidates.push_back("libmpv.so");

        for (const std::string &candidate : candidates) {
            library = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (library) break;
        }

        if (!library) {
            loadFailure =
                "Unable to load libmpv.so.2. Install libmpv (e.g. 'sudo apt install libmpv2'), "
                "bundle it under native/linux, or set NUVIO_LIBMPV_PATH.";
            return;
        }

        try {
            create = loadSymbol<mpv_create_fn>("mpv_create");
            initialize = loadSymbol<mpv_initialize_fn>("mpv_initialize");
            terminateDestroy = loadSymbol<mpv_terminate_destroy_fn>("mpv_terminate_destroy");
            setOption = loadSymbol<mpv_set_option_fn>("mpv_set_option");
            setOptionString = loadSymbol<mpv_set_option_string_fn>("mpv_set_option_string");
            setProperty = loadSymbol<mpv_set_property_fn>("mpv_set_property");
            setPropertyString = loadSymbol<mpv_set_property_string_fn>("mpv_set_property_string");
            getProperty = loadSymbol<mpv_get_property_fn>("mpv_get_property");
            command = loadSymbol<mpv_command_fn>("mpv_command");
            errorString = loadSymbol<mpv_error_string_fn>("mpv_error_string");
            freeValue = loadSymbol<mpv_free_fn>("mpv_free");
            waitEvent = loadSymbol<mpv_wait_event_fn>("mpv_wait_event");
            wakeup = loadSymbol<mpv_wakeup_fn>("mpv_wakeup");
        } catch (const std::exception &) {
            // loadFailure is populated by loadSymbol; library already closed.
        }
    }

    template <typename T>
    T loadSymbol(const char *name) {
        void *symbol = dlsym(library, name);
        if (!symbol) {
            loadFailure = std::string("libmpv is missing export ") + name + ".";
            dlclose(library);
            library = nullptr;
            throw std::runtime_error(loadFailure);
        }
        return reinterpret_cast<T>(symbol);
    }
};

MpvApi &mpvApi() {
    static MpvApi api;
    api.ensureLoaded();
    return api;
}

// ---- Player ----

class LinuxMpvPlayer : public std::enable_shared_from_this<LinuxMpvPlayer> {
    struct InitializationState {
        std::mutex mutex;
        std::condition_variable cv;
        bool complete = false;
        std::string failure;
    };

public:
    void initialize(
        Window parent,
        const std::string &sourceUrl,
        const std::vector<std::string> &headerLines,
        bool playWhenReady,
        long long initialPositionMs,
        JavaVM *vm,
        jobject sink,
        jmethodID method
    ) {
        if (parent == 0) {
            throw std::runtime_error("Unable to resolve the AWT host X11 window for native playback.");
        }

        javaVm = vm;
        eventSink = sink;
        eventMethod = method;
        parentWindow = parent;

        auto initState = std::make_shared<InitializationState>();
        auto self = shared_from_this();
        workerThread = std::thread(
            [self, sourceUrl, headerLines, playWhenReady, initialPositionMs, initState]() {
                self->runWorker(sourceUrl, headerLines, playWhenReady, initialPositionMs, initState);
            }
        );

        std::unique_lock<std::mutex> lock(initState->mutex);
        initState->cv.wait(lock, [&]() { return initState->complete; });
        if (!initState->failure.empty()) {
            lock.unlock();
            if (workerThread.joinable()) {
                workerThread.join();
            }
            throw std::runtime_error(initState->failure);
        }
    }

    void shutdown() {
        if (shuttingDown.exchange(true)) {
            return;
        }

        stopping.store(true);
        {
            std::lock_guard<std::mutex> lock(mpvMutex);
            if (mpv && mpvApi().wakeup) {
                mpvApi().wakeup(mpv);
            }
        }
        if (workerThread.joinable()) {
            workerThread.join();
        }
        {
            std::lock_guard<std::mutex> lock(mpvMutex);
            if (mpv) {
                mpvApi().terminateDestroy(mpv);
                mpv = nullptr;
            }
        }
        // Worker thread has exited; this thread is now the sole owner of the X
        // connection, so tearing it down here is race-free.
        cleanupX();

        if (eventSink) {
            bool didAttach = false;
            JNIEnv *env = jniEnvDidAttach(&didAttach);
            if (env) {
                env->DeleteGlobalRef(eventSink);
            }
            if (didAttach) {
                javaVm->DetachCurrentThread();
            }
            eventSink = nullptr;
        }
        eventMethod = nullptr;
        javaVm = nullptr;
    }

    void updateControlsJson(const std::string &) {
        // Phase 1: no HTML controls overlay on Linux. Intentionally a no-op.
    }

    void setPaused(bool paused) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return;
        int flag = paused ? 1 : 0;
        mpvApi().setProperty(mpv, "pause", MPV_FORMAT_FLAG, &flag);
    }

    bool isPaused() {
        return flagProperty("pause", true);
    }

    void seekToMilliseconds(long long positionMs) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return;
        std::string seconds = std::to_string((double)positionMs / 1000.0);
        const char *command[] = {"seek", seconds.c_str(), "absolute+keyframes", nullptr};
        mpvApi().command(mpv, command);
    }

    void seekByMilliseconds(long long offsetMs) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return;
        std::string seconds = std::to_string((double)offsetMs / 1000.0);
        const char *command[] = {"seek", seconds.c_str(), "relative+keyframes", nullptr};
        mpvApi().command(mpv, command);
    }

    void setSpeed(double speed) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return;
        double clamped = std::max(0.25, std::min(4.0, speed));
        mpvApi().setProperty(mpv, "speed", MPV_FORMAT_DOUBLE, &clamped);
    }

    double speed() {
        return doubleProperty("speed", 1.0);
    }

    void adjustVolume(double delta) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return;
        double current = 100.0;
        mpvApi().getProperty(mpv, "volume", MPV_FORMAT_DOUBLE, &current);
        double next = std::max(0.0, std::min(100.0, current + delta));
        mpvApi().setProperty(mpv, "volume", MPV_FORMAT_DOUBLE, &next);
    }

    void setResizeMode(int mode) {
        switch (mode) {
            case 1:
            case 2:
                setStringProperty("panscan", "1.0");
                setStringProperty("video-unscaled", "no");
                break;
            default:
                setStringProperty("panscan", "0.0");
                setStringProperty("video-unscaled", "no");
                break;
        }
    }

    long long durationMs() {
        return (long long)std::llround(doubleProperty("duration", 0.0) * 1000.0);
    }

    long long positionMs() {
        return (long long)std::llround(doubleProperty("time-pos", 0.0) * 1000.0);
    }

    long long bufferedPositionMs() {
        double buffered = rawPositionSeconds() + cacheAheadSeconds();
        return (long long)std::llround(std::max(buffered, 0.0) * 1000.0);
    }

    bool isLoading() {
        bool paused = isPaused();
        bool eofReached = isEnded();
        bool idle = flagProperty("core-idle", true);
        bool seeking = flagProperty("seeking", false);
        bool bufferingCache = flagProperty("paused-for-cache", false);
        bool fileReady = doubleProperty("duration", 0.0) > 0.0 || int64Property("track-list/count", 0) > 0;
        return !fileReady || (idle && !paused && !eofReached) || seeking || bufferingCache;
    }

    bool isEnded() {
        return flagProperty("eof-reached", false);
    }

    std::string audioTracksJson() {
        return tracksJsonForType("audio");
    }

    std::string subtitleTracksJson() {
        return tracksJsonForType("sub");
    }

    void selectAudioTrackId(int trackId) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return;
        int64_t id = trackId;
        mpvApi().setProperty(mpv, "aid", MPV_FORMAT_INT64, &id);
    }

    void selectSubtitleTrackId(int trackId) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return;
        if (trackId < 0) {
            mpvApi().setPropertyString(mpv, "sid", "no");
            return;
        }
        int64_t id = trackId;
        mpvApi().setProperty(mpv, "sid", MPV_FORMAT_INT64, &id);
    }

    void addSubtitleUrl(const std::string &url) {
        if (url.empty()) return;
        command({"sub-add", url, "select"});
    }

    void removeExternalSubtitles() {
        removeExternalSubtitleTracks();
        setStringProperty("sid", "no");
    }

    void removeExternalSubtitlesAndSelect(int trackId) {
        removeExternalSubtitleTracks();
        if (trackId >= 0) {
            selectSubtitleTrackId(trackId);
        } else {
            setStringProperty("sid", "no");
        }
    }

    void setSubtitleDelayMs(int delayMs) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return;
        int clamped = std::max(-60000, std::min(60000, delayMs));
        double delaySeconds = (double)clamped / 1000.0;
        mpvApi().setProperty(mpv, "sub-delay", MPV_FORMAT_DOUBLE, &delaySeconds);
    }

    void applySubtitleStyle(
        const std::string &textColor,
        const std::string &backgroundColor,
        const std::string &outlineColor,
        double outlineSize,
        bool bold,
        double fontSize,
        int subPos
    ) {
        setStringProperty("sub-ass-override", "force");
        setStringProperty("sub-color", textColor.empty() ? "#FFFFFFFF" : textColor);
        setStringProperty("sub-back-color", backgroundColor.empty() ? "#00000000" : backgroundColor);
        setStringProperty("sub-outline-color", outlineColor.empty() ? "#FF000000" : outlineColor);
        setStringProperty(
            "sub-border-style",
            backgroundColor.rfind("#00", 0) == 0 ? "outline-and-shadow" : "opaque-box"
        );
        setStringProperty("sub-bold", bold ? "yes" : "no");

        {
            std::lock_guard<std::mutex> lock(mpvMutex);
            if (!mpv) return;
            double outline = std::max(0.0, std::min(8.0, outlineSize));
            double size = std::max(24.0, std::min(96.0, fontSize));
            int64_t position = std::max(0, std::min(150, subPos));
            mpvApi().setProperty(mpv, "sub-outline-size", MPV_FORMAT_DOUBLE, &outline);
            mpvApi().setProperty(mpv, "sub-font-size", MPV_FORMAT_DOUBLE, &size);
            mpvApi().setProperty(mpv, "sub-pos", MPV_FORMAT_INT64, &position);
        }
    }

    // Available for Phase 2 / native input wiring; unused in Phase 1.
    void sendPlayerEvent(const std::string &type, double value) {
        if (!eventSink || !eventMethod) return;
        bool didAttach = false;
        JNIEnv *env = jniEnvDidAttach(&didAttach);
        if (!env) return;

        jstring eventType = newJavaStringUtf8(env, type);
        env->CallVoidMethod(eventSink, eventMethod, eventType, (jdouble)value);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
        }
        if (eventType) {
            env->DeleteLocalRef(eventType);
        }
        if (didAttach) {
            javaVm->DetachCurrentThread();
        }
    }

private:
    Window parentWindow = 0;
    Window childWindow = 0;
    Display *display = nullptr;
    unsigned int lastWidth = 0;
    unsigned int lastHeight = 0;

    std::thread workerThread;

    std::mutex mpvMutex;
    mpv_handle *mpv = nullptr;
    std::atomic_bool stopping{false};
    std::atomic_bool shuttingDown{false};

    JavaVM *javaVm = nullptr;
    jobject eventSink = nullptr;
    jmethodID eventMethod = nullptr;

    double initialStartSeconds = 0.0;

    void runWorker(
        std::string sourceUrl,
        std::vector<std::string> headerLines,
        bool playWhenReady,
        long long initialPositionMs,
        std::shared_ptr<InitializationState> initState
    ) {
        std::string failure;
        try {
            setupOnWorker(sourceUrl, headerLines, playWhenReady, initialPositionMs);
        } catch (const std::exception &error) {
            failure = error.what();
            // Setup failed before any other thread touches the X connection, so
            // it is safe to release it here.
            {
                std::lock_guard<std::mutex> lock(mpvMutex);
                if (mpv) {
                    mpvApi().terminateDestroy(mpv);
                    mpv = nullptr;
                }
            }
            cleanupX();
        }

        {
            std::lock_guard<std::mutex> lock(initState->mutex);
            initState->failure = failure;
            initState->complete = true;
        }
        initState->cv.notify_one();

        if (!failure.empty()) {
            return;
        }

        // Event + resize loop. mpv_wait_event blocks up to 0.25s, giving the
        // child-window resize check a ~250ms cadence (mirrors the Windows 500ms
        // WM_TIMER SetWindowPos). State is polled by Kotlin every 500ms, so no
        // property observation is required here.
        while (!stopping.load()) {
            mpv_handle *current = nullptr;
            {
                std::lock_guard<std::mutex> lock(mpvMutex);
                current = mpv;
            }
            if (!current) break;

            mpv_event *event = mpvApi().waitEvent(current, 0.25);
            if (event && event->event_id == MPV_EVENT_SHUTDOWN) {
                break;
            }
            maybeResizeChild();
        }
    }

    void setupOnWorker(
        const std::string &sourceUrl,
        const std::vector<std::string> &headerLines,
        bool playWhenReady,
        long long initialPositionMs
    ) {
        display = XOpenDisplay(nullptr);
        if (!display) {
            throw std::runtime_error(
                "Unable to open the X11 display. Native playback requires an X11 or XWayland "
                "session (DISPLAY must be set)."
            );
        }

        Window root = 0;
        int x = 0;
        int y = 0;
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int borderWidth = 0;
        unsigned int depth = 0;
        if (!XGetGeometry(display, parentWindow, &root, &x, &y, &width, &height, &borderWidth, &depth)) {
            throw std::runtime_error("Unable to query the AWT host X11 window geometry.");
        }
        if (width == 0) width = 1;
        if (height == 0) height = 1;

        int screen = DefaultScreen(display);
        unsigned long black = BlackPixel(display, screen);
        childWindow = XCreateSimpleWindow(
            display,
            parentWindow,
            0,
            0,
            width,
            height,
            0,
            black,
            black
        );
        if (childWindow == 0) {
            throw std::runtime_error("Unable to create the X11 child window for the player.");
        }
        XMapWindow(display, childWindow);
        XSync(display, False);
        lastWidth = width;
        lastHeight = height;

        MpvApi &api = mpvApi();
        {
            std::lock_guard<std::mutex> lock(mpvMutex);
            mpv = api.create();
            if (!mpv) {
                throw std::runtime_error("mpv_create failed.");
            }
            initialStartSeconds = initialPositionMs > 0 ? (double)initialPositionMs / 1000.0 : 0.0;

            setMpvOptionStringLocked("config", "no");
            setMpvOptionStringLocked("osc", "no");
            setMpvOptionStringLocked("input-default-bindings", "yes");
            setMpvOptionStringLocked("input-vo-keyboard", "no");
            setMpvOptionStringLocked("keep-open", "yes");
            setMpvOptionStringLocked("vo", "gpu-next");
            setMpvOptionStringLocked("gpu-context", "x11egl");
            setMpvOptionStringLocked("hwdec", "auto");
            setMpvOptionStringLocked("hwdec-codecs", "all");
            setMpvOptionStringLocked("vd-lavc-software-fallback", "yes");
            setMpvOptionStringLocked("vd-lavc-threads", "4");
            setMpvOptionStringLocked("target-colorspace-hint", "yes");
            setMpvOptionStringLocked("tone-mapping", "auto");
            setMpvOptionStringLocked("dither-depth", "auto");
            setMpvOptionStringLocked("deband", "yes");
            setMpvOptionStringLocked("scale", "spline36");
            setMpvOptionStringLocked("cscale", "spline36");
            setMpvOptionStringLocked("demuxer-max-bytes", "64MiB");
            setMpvOptionStringLocked("demuxer-max-back-bytes", "16MiB");
            setMpvOptionStringLocked("demuxer-seekable-cache", "no");
            setMpvOptionStringLocked("cache-secs", "30");
            setMpvOptionStringLocked("hr-seek", "no");

            int64_t wid = (int64_t)childWindow;
            int widResult = api.setOption(mpv, "wid", MPV_FORMAT_INT64, &wid);
            if (widResult < 0) {
                throw std::runtime_error(std::string("mpv wid option failed: ") + api.errorText(widResult));
            }

            if (!headerLines.empty()) {
                std::string headers;
                for (size_t index = 0; index < headerLines.size(); index++) {
                    if (index > 0) headers.push_back(',');
                    headers += headerLines[index];
                }
                setMpvOptionStringLocked("http-header-fields", headers.c_str());
            }

            int initResult = api.initialize(mpv);
            if (initResult < 0) {
                throw std::runtime_error(std::string("mpv_initialize failed: ") + api.errorText(initResult));
            }

            std::vector<const char *> loadCommand = {"loadfile", sourceUrl.c_str()};
            std::string loadOptions;
            if (initialPositionMs > 0) {
                char startBuffer[64];
                std::snprintf(startBuffer, sizeof(startBuffer), "start=%.3f", (double)initialPositionMs / 1000.0);
                loadOptions = startBuffer;
                loadCommand.push_back("replace");
                loadCommand.push_back("-1");
                loadCommand.push_back(loadOptions.c_str());
            }
            loadCommand.push_back(nullptr);

            int commandResult = api.command(mpv, loadCommand.data());
            if (commandResult < 0) {
                throw std::runtime_error(std::string("mpv loadfile failed: ") + api.errorText(commandResult));
            }
        }

        setPaused(!playWhenReady);
    }

    void maybeResizeChild() {
        if (!display || childWindow == 0 || parentWindow == 0) return;
        Window root = 0;
        int x = 0;
        int y = 0;
        unsigned int width = 0;
        unsigned int height = 0;
        unsigned int borderWidth = 0;
        unsigned int depth = 0;
        if (!XGetGeometry(display, parentWindow, &root, &x, &y, &width, &height, &borderWidth, &depth)) {
            return;
        }
        if (width == 0) width = 1;
        if (height == 0) height = 1;
        if (width != lastWidth || height != lastHeight) {
            XMoveResizeWindow(display, childWindow, 0, 0, width, height);
            XFlush(display);
            lastWidth = width;
            lastHeight = height;
        }
    }

    void cleanupX() {
        if (display) {
            if (childWindow != 0) {
                XDestroyWindow(display, childWindow);
                childWindow = 0;
            }
            XCloseDisplay(display);
            display = nullptr;
        }
    }

    JNIEnv *jniEnvDidAttach(bool *didAttach) {
        if (didAttach) *didAttach = false;
        if (!javaVm) return nullptr;
        JNIEnv *env = nullptr;
        jint status = javaVm->GetEnv((void **)&env, JNI_VERSION_1_6);
        if (status == JNI_OK) return env;
        if (status != JNI_EDETACHED) return nullptr;
        if (javaVm->AttachCurrentThread((void **)&env, nullptr) != JNI_OK) {
            return nullptr;
        }
        if (didAttach) *didAttach = true;
        return env;
    }

    void setMpvOptionStringLocked(const char *name, const char *value) {
        (void)mpvApi().setOptionString(mpv, name, value);
    }

    double doubleProperty(const char *name, double fallback) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return fallback;
        double value = fallback;
        int result = mpvApi().getProperty(mpv, name, MPV_FORMAT_DOUBLE, &value);
        if (result < 0) {
            return fallback;
        }
        return value;
    }

    long long int64Property(const char *name, long long fallback) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return fallback;
        int64_t value = fallback;
        int result = mpvApi().getProperty(mpv, name, MPV_FORMAT_INT64, &value);
        if (result < 0) {
            return fallback;
        }
        return value;
    }

    bool flagProperty(const char *name, bool fallback) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return fallback;
        int flag = fallback ? 1 : 0;
        int result = mpvApi().getProperty(mpv, name, MPV_FORMAT_FLAG, &flag);
        if (result < 0) {
            return fallback;
        }
        return flag != 0;
    }

    std::string stringProperty(const char *name, const std::string &fallback) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return fallback;
        char *value = nullptr;
        int propertyResult = mpvApi().getProperty(mpv, name, MPV_FORMAT_STRING, &value);
        if (propertyResult < 0 || !value) {
            return fallback;
        }
        std::string result(value);
        mpvApi().freeValue(value);
        return result;
    }

    void setStringProperty(const char *name, const std::string &value) {
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return;
        mpvApi().setPropertyString(mpv, name, value.c_str());
    }

    void command(const std::vector<std::string> &args) {
        if (args.empty()) return;
        std::lock_guard<std::mutex> lock(mpvMutex);
        if (!mpv) return;
        std::vector<const char *> cargs;
        cargs.reserve(args.size() + 1);
        for (const std::string &arg : args) {
            cargs.push_back(arg.c_str());
        }
        cargs.push_back(nullptr);
        mpvApi().command(mpv, cargs.data());
    }

    double rawPositionSeconds() {
        double position = doubleProperty("time-pos", 0.0);
        return std::isfinite(position) ? std::max(position, 0.0) : 0.0;
    }

    double effectiveCachePositionSeconds() {
        double position = rawPositionSeconds();
        if (initialStartSeconds > 0.0 && position + 5.0 < initialStartSeconds) {
            return initialStartSeconds;
        }
        return position;
    }

    double cacheAheadSeconds() {
        double effectivePosition = effectiveCachePositionSeconds();
        double cacheTime = doubleProperty("demuxer-cache-time", 0.0);
        if (std::isfinite(cacheTime) && cacheTime > 0.0) {
            if (cacheTime >= effectivePosition - 5.0) {
                return std::max(cacheTime - effectivePosition, 0.0);
            }
            return cacheTime;
        }

        double cacheDuration = doubleProperty("demuxer-cache-duration", 0.0);
        if (std::isfinite(cacheDuration) && cacheDuration > 0.0) {
            return cacheDuration;
        }
        return 0.0;
    }

    void removeExternalSubtitleTracks() {
        long long count = int64Property("track-list/count", 0);
        if (count <= 0) return;
        for (long long index = count - 1; index >= 0; index--) {
            std::string prefix = "track-list/" + std::to_string(index);
            std::string type = stringProperty((prefix + "/type").c_str(), "");
            bool external = flagProperty((prefix + "/external").c_str(), false);
            if (type == "sub" && external) {
                long long trackId = int64Property((prefix + "/id").c_str(), -1);
                if (trackId >= 0) {
                    command({"sub-remove", std::to_string(trackId)});
                }
            }
        }
    }

    std::string tracksJsonForType(const std::string &wantedType) {
        long long count = int64Property("track-list/count", 0);
        std::ostringstream json;
        json << "[";
        int logicalIndex = 0;
        bool first = true;
        for (long long index = 0; index < count; index++) {
            std::string prefix = "track-list/" + std::to_string(index);
            std::string type = stringProperty((prefix + "/type").c_str(), "");
            if (type != wantedType) continue;

            long long trackId = int64Property((prefix + "/id").c_str(), logicalIndex + 1);
            std::string title = trackStringAtIndex(index, "title");
            std::string language = trackStringAtIndex(index, "lang");
            std::string codec = trackStringAtIndex(index, "codec");
            std::string decoderDescription = trackStringAtIndex(index, "decoder-desc");
            std::string channels = trackStringAtIndex(index, "demux-channels");
            long long channelCount = int64Property((prefix + "/demux-channel-count").c_str(), 0);
            bool selected = flagProperty((prefix + "/selected").c_str(), false);
            bool forced = flagProperty((prefix + "/forced").c_str(), false);
            std::string label = formatTrackTitle(type, logicalIndex, title, language, codec, decoderDescription, channels, (int)channelCount);

            if (!first) json << ",";
            first = false;
            json << "{"
                 << "\"index\":" << logicalIndex << ","
                 << "\"id\":\"" << jsonEscape(std::to_string(trackId)) << "\","
                 << "\"label\":\"" << jsonEscape(label) << "\","
                 << "\"language\":\"" << jsonEscape(language) << "\","
                 << "\"selected\":" << (selected ? "true" : "false") << ","
                 << "\"forced\":" << (forced ? "true" : "false")
                 << "}";
            logicalIndex++;
        }
        json << "]";
        return json.str();
    }

    std::string trackStringAtIndex(long long index, const std::string &field) {
        return trim(stringProperty(("track-list/" + std::to_string(index) + "/" + field).c_str(), ""));
    }

    std::string formatTrackTitle(
        const std::string &type,
        int index,
        const std::string &title,
        const std::string &language,
        const std::string &codec,
        const std::string &decoderDescription,
        const std::string &channels,
        int channelCount
    ) {
        std::string base = !trim(title).empty()
            ? trim(title)
            : (!trim(language).empty()
                ? trim(language)
                : (type == "sub" ? "Subtitle " + std::to_string(index + 1) : "Track " + std::to_string(index + 1)));
        std::string codecName = codecDisplayName(codec);
        if (codecName.empty()) codecName = codecDisplayName(decoderDescription);
        std::string channelName = type == "audio" ? channelLayoutName(channels, channelCount) : "";

        std::vector<std::string> details;
        for (const std::string &detail : {channelName, codecName}) {
            if (!detail.empty() && !containsCaseInsensitive(base, detail)) {
                details.push_back(detail);
            }
        }
        if (details.empty()) return base;
        std::string suffix;
        for (size_t detailIndex = 0; detailIndex < details.size(); detailIndex++) {
            if (detailIndex > 0) suffix += ", ";
            suffix += details[detailIndex];
        }
        return base + " (" + suffix + ")";
    }

    std::string channelLayoutName(const std::string &channels, int channelCount) {
        std::string normalized = trim(channels);
        if (!normalized.empty() && lowerCopy(normalized) != "unknown") {
            std::string lower = lowerCopy(normalized);
            if (lower == "mono") return "Mono";
            if (lower == "stereo") return "Stereo";
            return normalized;
        }
        switch (channelCount) {
            case 1: return "Mono";
            case 2: return "Stereo";
            case 6: return "5.1";
            case 8: return "7.1";
            default: return channelCount > 0 ? std::to_string(channelCount) + "ch" : "";
        }
    }

    std::string codecDisplayName(const std::string &value) {
        std::string raw = trim(value);
        if (raw.empty()) return "";
        std::string codec = lowerCopy(raw);
        if (codec.find("eac3") != std::string::npos || codec.find("e-ac-3") != std::string::npos || codec.find("e ac-3") != std::string::npos) {
            return codec.find("joc") != std::string::npos || codec.find("atmos") != std::string::npos ? "E-AC-3-JOC" : "E-AC-3";
        }
        if (codec.find("truehd") != std::string::npos || codec.find("true hd") != std::string::npos) return "TrueHD";
        if (codec.find("ac3") != std::string::npos || codec.find("ac-3") != std::string::npos) return "AC-3";
        if (codec.find("dts-hd") != std::string::npos || codec.find("dtshd") != std::string::npos || codec.find("dts hd") != std::string::npos) return "DTS-HD";
        if (codec.find("dts") != std::string::npos || codec == "dca") return "DTS";
        if (codec.find("aac") != std::string::npos) return "AAC";
        if (codec.find("mp3") != std::string::npos || codec.find("mpeg audio") != std::string::npos) return "MP3";
        if (codec.find("mp2") != std::string::npos) return "MP2";
        if (codec.find("opus") != std::string::npos) return "Opus";
        if (codec.find("vorbis") != std::string::npos) return "Vorbis";
        if (codec.find("flac") != std::string::npos) return "FLAC";
        if (codec.find("alac") != std::string::npos) return "ALAC";
        if (codec.find("pcm") != std::string::npos || codec.find("wav") != std::string::npos) return "WAV";
        if (codec.find("pgs") != std::string::npos || codec.find("hdmv") != std::string::npos) return "PGS";
        if (codec.find("subrip") != std::string::npos || codec == "srt") return "SRT";
        if (codec.find("ass") != std::string::npos || codec.find("ssa") != std::string::npos) return "SSA";
        if (codec.find("webvtt") != std::string::npos || codec == "vtt") return "VTT";
        if (codec.find("ttml") != std::string::npos) return "TTML";
        if (codec.find("mov_text") != std::string::npos || codec.find("tx3g") != std::string::npos) return "TX3G";
        if (codec.find("dvb") != std::string::npos) return "DVB";
        return raw;
    }
};

std::shared_ptr<LinuxMpvPlayer> playerFromHandle(jlong handle) {
    if (handle == 0) return nullptr;
    auto *holder = reinterpret_cast<std::shared_ptr<LinuxMpvPlayer> *>(handle);
    return holder ? *holder : nullptr;
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_create(
    JNIEnv *env,
    jobject,
    jlong hostViewPtr,
    jstring sourceUrl,
    jobjectArray headerLines,
    jboolean playWhenReady,
    jlong initialPositionMs,
    jstring controlsPageUrl,
    jobject eventSink
) {
    (void)controlsPageUrl;
    Window parentWindow = (Window)(unsigned long)hostViewPtr;
    std::string sourceUrlText = jstringToUtf8(env, sourceUrl);
    std::vector<std::string> headerLineValues = jstringArrayToVector(env, headerLines);
    JavaVM *javaVm = nullptr;
    env->GetJavaVM(&javaVm);

    jobject eventSinkRef = nullptr;
    jmethodID eventMethod = nullptr;
    if (eventSink) {
        eventSinkRef = env->NewGlobalRef(eventSink);
        jclass eventSinkClass = env->GetObjectClass(eventSink);
        eventMethod = env->GetMethodID(eventSinkClass, "onPlayerEvent", "(Ljava/lang/String;D)V");
        env->DeleteLocalRef(eventSinkClass);
        if (!eventMethod) {
            if (eventSinkRef) env->DeleteGlobalRef(eventSinkRef);
            throwJavaError(env, "Native player event sink is missing onPlayerEvent(String, Double).");
            return 0;
        }
    }

    auto player = std::make_shared<LinuxMpvPlayer>();
    try {
        player->initialize(
            parentWindow,
            sourceUrlText,
            headerLineValues,
            playWhenReady == JNI_TRUE,
            initialPositionMs,
            javaVm,
            eventSinkRef,
            eventMethod
        );
    } catch (const std::exception &error) {
        player->shutdown();
        throwJavaError(env, error.what());
        return 0;
    }

    auto *holder = new std::shared_ptr<LinuxMpvPlayer>(player);
    return (jlong)(intptr_t)holder;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_dispose(JNIEnv *, jobject, jlong handle) {
    if (handle == 0) return;
    auto *holder = reinterpret_cast<std::shared_ptr<LinuxMpvPlayer> *>(handle);
    std::shared_ptr<LinuxMpvPlayer> player = *holder;
    delete holder;
    if (player) player->shutdown();
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_updateControls(JNIEnv *env, jobject, jlong handle, jstring controlsJson) {
    auto player = playerFromHandle(handle);
    std::string controlsJsonText = jstringToUtf8(env, controlsJson);
    if (player) player->updateControlsJson(controlsJsonText);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setPaused(JNIEnv *, jobject, jlong handle, jboolean paused) {
    auto player = playerFromHandle(handle);
    if (player) player->setPaused(paused == JNI_TRUE);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_seekTo(JNIEnv *, jobject, jlong handle, jlong positionMs) {
    auto player = playerFromHandle(handle);
    if (player) player->seekToMilliseconds(positionMs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_seekBy(JNIEnv *, jobject, jlong handle, jlong offsetMs) {
    auto player = playerFromHandle(handle);
    if (player) player->seekByMilliseconds(offsetMs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setSpeed(JNIEnv *, jobject, jlong handle, jfloat speed) {
    auto player = playerFromHandle(handle);
    if (player) player->setSpeed(speed);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_adjustVolume(JNIEnv *, jobject, jlong handle, jfloat delta) {
    auto player = playerFromHandle(handle);
    if (player) player->adjustVolume(delta);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_durationMs(JNIEnv *, jobject, jlong handle) {
    auto player = playerFromHandle(handle);
    return player ? player->durationMs() : 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_positionMs(JNIEnv *, jobject, jlong handle) {
    auto player = playerFromHandle(handle);
    return player ? player->positionMs() : 0;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_bufferedPositionMs(JNIEnv *, jobject, jlong handle) {
    auto player = playerFromHandle(handle);
    return player ? player->bufferedPositionMs() : 0;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isLoading(JNIEnv *, jobject, jlong handle) {
    auto player = playerFromHandle(handle);
    return player && player->isLoading() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isEnded(JNIEnv *, jobject, jlong handle) {
    auto player = playerFromHandle(handle);
    return player && player->isEnded() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_isPaused(JNIEnv *, jobject, jlong handle) {
    auto player = playerFromHandle(handle);
    return !player || player->isPaused() ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jfloat JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_speed(JNIEnv *, jobject, jlong handle) {
    auto player = playerFromHandle(handle);
    return player ? (jfloat)player->speed() : 1.0f;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setResizeMode(JNIEnv *, jobject, jlong handle, jint mode) {
    auto player = playerFromHandle(handle);
    if (player) player->setResizeMode(mode);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_audioTracksJson(JNIEnv *env, jobject, jlong handle) {
    auto player = playerFromHandle(handle);
    return newJavaStringUtf8(env, player ? player->audioTracksJson() : "[]");
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_subtitleTracksJson(JNIEnv *env, jobject, jlong handle) {
    auto player = playerFromHandle(handle);
    return newJavaStringUtf8(env, player ? player->subtitleTracksJson() : "[]");
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_selectAudioTrack(JNIEnv *, jobject, jlong handle, jint trackId) {
    auto player = playerFromHandle(handle);
    if (player) player->selectAudioTrackId(trackId);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_selectSubtitleTrack(JNIEnv *, jobject, jlong handle, jint trackId) {
    auto player = playerFromHandle(handle);
    if (player) player->selectSubtitleTrackId(trackId);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_addSubtitleUrl(JNIEnv *env, jobject, jlong handle, jstring url) {
    auto player = playerFromHandle(handle);
    if (player) player->addSubtitleUrl(jstringToUtf8(env, url));
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_clearExternalSubtitles(JNIEnv *, jobject, jlong handle) {
    auto player = playerFromHandle(handle);
    if (player) player->removeExternalSubtitles();
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_clearExternalSubtitlesAndSelect(JNIEnv *, jobject, jlong handle, jint trackId) {
    auto player = playerFromHandle(handle);
    if (player) player->removeExternalSubtitlesAndSelect(trackId);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_setSubtitleDelayMs(JNIEnv *, jobject, jlong handle, jint delayMs) {
    auto player = playerFromHandle(handle);
    if (player) player->setSubtitleDelayMs(delayMs);
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_applySubtitleStyle(
    JNIEnv *env,
    jobject,
    jlong handle,
    jstring textColor,
    jstring backgroundColor,
    jstring outlineColor,
    jfloat outlineSize,
    jboolean bold,
    jfloat fontSize,
    jint subPos
) {
    auto player = playerFromHandle(handle);
    if (!player) return;
    player->applySubtitleStyle(
        jstringToUtf8(env, textColor),
        jstringToUtf8(env, backgroundColor),
        jstringToUtf8(env, outlineColor),
        outlineSize,
        bold == JNI_TRUE,
        fontSize,
        subPos
    );
}

// ---- Windows-only surface: harmless stubs so JNI symbol resolution never fails ----

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_applyWindowChrome(
    JNIEnv *,
    jobject,
    jlong,
    jboolean,
    jint,
    jint,
    jint
) {
    // No Linux window-manager analogue; no-op.
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_warmupWebView2(JNIEnv *, jobject, jstring) {
    // No webview overlay on Linux in Phase 1.
    return JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_nuvio_app_features_player_desktop_NativePlayerBridge_shutdownWebView2Warmup(JNIEnv *, jobject) {
    // No-op.
}
