// Nintendo Land Recomp - H.264 ueber Windows Media Foundation (in Windows eingebaut, keine Extra-DLLs)
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Kodiert Bilder in einem eigenen Thread. Grafikkarten-Encoder (NVIDIA/AMD/Intel), sonst Windows-Software-Encoder.
class NLVideoEncoder {
public:
    using PacketFn = std::function<void(const uint8_t* data, size_t size, bool keyframe, uint64_t t_us)>;
    ~NLVideoEncoder() { stop(); }
    bool start(int width, int height, int fps, int kbps, PacketFn on_packet);
    void stop();
    bool running() const { return m_running; }
    // RGBA-Bild abgeben (wird kopiert; aeltere, noch nicht kodierte Bilder werden verworfen)
    void submit_rgba(const uint8_t* rgba, int width, int height, int stride, bool srgb_encode);
    void request_keyframe() { m_want_key = true; }
    void set_bitrate(int kbps) { m_new_kbps = kbps; }   // live, ohne Neustart
    int bitrate() const { return m_kbps; }
    std::string encoder_name() const;
    int width() const { return m_w; }
    int height() const { return m_h; }
    int fps() const { return m_fps; }

private:
    void worker();
    bool m_running = false;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_want_key{true};
    std::atomic<int> m_new_kbps{0};
    std::thread m_thread;
    PacketFn m_on_packet;
    int m_w = 0, m_h = 0, m_fps = 60, m_kbps = 8000;
    // Postfach: immer nur das neueste Bild
    std::mutex m_mx;
    std::condition_variable m_cv;
    std::vector<uint8_t> m_rgba;
    int m_src_w = 0, m_src_h = 0;
    bool m_srgb = false;
    bool m_has_frame = false;
    mutable std::mutex m_name_mx;
    std::string m_name;
    std::atomic<bool> m_started_ok{false};
    std::atomic<bool> m_start_done{false};
};

// Dekodiert H.264 (Annex B) zu BGRA. Nicht thread-sicher: von einem Thread benutzen.
class NLVideoDecoder {
public:
    ~NLVideoDecoder();
    bool init();
    void reset();
    // liefert true, wenn ein neues Bild in bgra steht
    bool decode(const uint8_t* data, size_t size, uint64_t t_us, std::vector<uint8_t>& bgra, int& width, int& height);

private:
    bool configure_output();
    void* m_mft = nullptr;   // IMFTransform*
    int m_w = 0, m_h = 0, m_stride = 0, m_alloc_h = 0;
    bool m_ok = false;
};

// Farbraum-Helfer (BT.709, begrenzter Bereich)
void nl_rgba_to_nv12(const uint8_t* rgba, int sw, int sh, int sstride, bool srgb_encode, uint8_t* nv12, int w, int h);
void nl_nv12_to_bgra(const uint8_t* y, const uint8_t* uv, int pitch, int w, int h, uint8_t* bgra);
