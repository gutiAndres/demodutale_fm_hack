#include "webrtc_opus_tx.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <opus/opus.h>

// libdatachannel (C++ API)
#include <rtc/rtc.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

struct webrtc_opus_tx {
    webrtc_opus_tx_cfg_t cfg;

    // WebRTC
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track>          track;

    std::atomic<bool> ready{false};
    std::atomic<bool> failed{false};

    // Opus
    OpusEncoder *enc = nullptr;
    std::vector<uint8_t> opus_buf;

    // RTP
    uint16_t seq = 0;
    uint32_t ts  = 0;
    uint32_t ssrc = 0;
    int samples_per_frame = 0;

    std::mutex mtx;
};

/* ------------------ util: random u32 ------------------ */
static uint32_t rand_u32() {
    static std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<uint32_t> dist;
    return dist(rng);
}

/* ------------------ util: minimal HTTP POST ------------------
   Envía offer SDP al server y obtiene answer SDP (body completo).
   Server: POST {path} con body = SDP offer, responde 200 con body = SDP answer.
*/
static bool http_post_sdp(const char* host, int port, const char* path,
                          const std::string& offer_sdp,
                          std::string& answer_sdp_out)
{
    answer_sdp_out.clear();

    // resolve
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = nullptr;
    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return false;
    }

    int fd = -1;
    for (auto p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) return false;

    std::string req;
    req += "POST " + std::string(path) + " HTTP/1.1\r\n";
    req += "Host: " + std::string(host) + ":" + std::to_string(port) + "\r\n";
    req += "Content-Type: application/sdp\r\n";
    req += "Content-Length: " + std::to_string(offer_sdp.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += offer_sdp;

    // send all
    size_t off = 0;
    while (off < req.size()) {
        ssize_t n = ::send(fd, req.data() + off, req.size() - off, 0);
        if (n <= 0) { ::close(fd); return false; }
        off += (size_t)n;
    }

    // read all
    std::string resp;
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, buf + n);
    }
    ::close(fd);

    // very small parse: split header/body by \r\n\r\n
    auto pos = resp.find("\r\n\r\n");
    if (pos == std::string::npos) return false;
    std::string header = resp.substr(0, pos);
    std::string body   = resp.substr(pos + 4);

    // status 200?
    if (header.find("200") == std::string::npos) return false;

    answer_sdp_out = body;
    return !answer_sdp_out.empty();
}

/* ------------------ RTP pack: 12-byte header + Opus payload ------------------ */
static void build_rtp_packet(std::vector<uint8_t>& out,
                             uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc,
                             const uint8_t* payload, size_t payload_len)
{
    out.resize(12 + payload_len);
    uint8_t *p = out.data();

    // V=2, P=0, X=0, CC=0
    p[0] = 0x80;
    // M=0 (puedes poner 1 si frame boundary), PT
    p[1] = (uint8_t)(pt & 0x7F);

    uint16_t nseq = htons(seq);
    uint32_t nts  = htonl(ts);
    uint32_t nssrc= htonl(ssrc);

    memcpy(p + 2, &nseq, 2);
    memcpy(p + 4, &nts,  4);
    memcpy(p + 8, &nssrc,4);

    memcpy(p + 12, payload, payload_len);
}

/* ------------------ create ------------------ */
webrtc_opus_tx_t* webrtc_opus_tx_create(const webrtc_opus_tx_cfg_t *cfg)
{
    if (!cfg || !cfg->signaling_host || !cfg->signaling_path) return nullptr;

    auto *tx = new webrtc_opus_tx();
    tx->cfg = *cfg;

    // defaults
    if (tx->cfg.payload_type == 0) tx->cfg.payload_type = 111;
    if (tx->cfg.frame_ms <= 0) tx->cfg.frame_ms = 20;

    tx->samples_per_frame = (tx->cfg.sample_rate * tx->cfg.frame_ms) / 1000;
    tx->ssrc = (tx->cfg.ssrc != 0) ? tx->cfg.ssrc : rand_u32();
    tx->seq  = (uint16_t)(rand_u32() & 0xFFFF);
    tx->ts   = rand_u32();

    // Opus encoder
    int err = 0;
    tx->enc = opus_encoder_create(tx->cfg.sample_rate, tx->cfg.channels, OPUS_APPLICATION_AUDIO, &err);
    if (!tx->enc || err != OPUS_OK) {
        delete tx;
        return nullptr;
    }
    opus_encoder_ctl(tx->enc, OPUS_SET_BITRATE(tx->cfg.bitrate));
    opus_encoder_ctl(tx->enc, OPUS_SET_COMPLEXITY(tx->cfg.complexity));
    opus_encoder_ctl(tx->enc, OPUS_SET_VBR(tx->cfg.vbr ? 1 : 0));

    tx->opus_buf.resize(4000); // suficiente para 20ms mono a bitrates típicos

    // WebRTC init logger (opcional)
    rtc::InitLogger(rtc::LogLevel::Info);

    rtc::Configuration rcfg;
    // STUN para pruebas; en LAN/local puede funcionar sin, pero es buena práctica.
    rcfg.iceServers.emplace_back("stun:stun.l.google.com:19302");

    tx->pc = std::make_shared<rtc::PeerConnection>(rcfg);

    tx->pc->onStateChange([tx](rtc::PeerConnection::State st) {
        if (st == rtc::PeerConnection::State::Failed ||
            st == rtc::PeerConnection::State::Disconnected ||
            st == rtc::PeerConnection::State::Closed) {
            tx->failed = true;
        }
    });

    // Track de audio Opus (sendonly)
    rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
    audio.addOpusCodec(tx->cfg.payload_type);
    tx->track = tx->pc->addTrack(audio);

    // Para saber cuándo está listo para enviar
    tx->track->onOpen([tx]() {
        tx->ready = true;
    });

    // Create offer -> gather ICE -> HTTP POST -> setRemote(answer)
    auto offer = tx->pc->createOffer();

    // Esperar a que ICE gathering termine para enviar SDP completo
    std::promise<void> gatherDone;
    auto gatherDoneF = gatherDone.get_future();

    tx->pc->onGatheringStateChange([&gatherDone](rtc::PeerConnection::GatheringState gs) {
        if (gs == rtc::PeerConnection::GatheringState::Complete) {
            try { gatherDone.set_value(); } catch (...) {}
        }
    });

    tx->pc->setLocalDescription(offer);

    // Wait gather complete (timeout defensivo)
    if (gatherDoneF.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        // Si no completa, igualmente intentamos con el localDescription actual
        // (pero en redes NAT duras conviene esperar completo).
    }

    std::string offer_sdp = tx->pc->localDescription().value().generateSdp();
    std::string answer_sdp;

    if (!http_post_sdp(tx->cfg.signaling_host, tx->cfg.signaling_port,
                       tx->cfg.signaling_path, offer_sdp, answer_sdp)) {
        tx->failed = true;
        webrtc_opus_tx_destroy(tx);
        return nullptr;
    }

    rtc::Description answer(answer_sdp, rtc::Description::Type::Answer);
    tx->pc->setRemoteDescription(answer);

    // Espera corta a que track abra (no bloqueante largo)
    for (int i = 0; i < 200; i++) {
        if (tx->ready.load() || tx->failed.load()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!tx->ready.load()) {
        tx->failed = true;
        webrtc_opus_tx_destroy(tx);
        return nullptr;
    }

    return tx;
}

/* ------------------ send PCM ------------------ */
int webrtc_opus_tx_send_pcm(webrtc_opus_tx_t *tx_, const int16_t *pcm, int frame_samples)
{
    auto *tx = (webrtc_opus_tx*)tx_;
    if (!tx || !pcm) return -1;
    if (!tx->ready.load() || tx->failed.load()) return -2;
    if (frame_samples != tx->samples_per_frame) return -3;

    std::lock_guard<std::mutex> lk(tx->mtx);

    // Opus encode
    int nbytes = opus_encode(tx->enc,
                            pcm,
                            frame_samples,
                            tx->opus_buf.data(),
                            (opus_int32)tx->opus_buf.size());
    if (nbytes < 0) return -4;

    // RTP packetize
    std::vector<uint8_t> rtp;
    build_rtp_packet(rtp,
                     tx->cfg.payload_type,
                     tx->seq++,
                     tx->ts,
                     tx->ssrc,
                     tx->opus_buf.data(),
                     (size_t)nbytes);

    // Timestamp increment (clock 48k)
    tx->ts += (uint32_t)frame_samples; // 20ms@48k = 960

    // Send as RTP; libdatachannel aplica SRTP y envía (UDP/ICE)
    tx->track->send(rtc::binary(rtp.begin(), rtp.end()));
    return 0;
}

/* ------------------ destroy ------------------ */
void webrtc_opus_tx_destroy(webrtc_opus_tx_t *tx_)
{
    auto *tx = (webrtc_opus_tx*)tx_;
    if (!tx) return;

    try {
        if (tx->pc) tx->pc->close();
    } catch (...) {}

    if (tx->enc) opus_encoder_destroy(tx->enc);
    tx->enc = nullptr;

    tx->track.reset();
    tx->pc.reset();

    delete tx;
}
