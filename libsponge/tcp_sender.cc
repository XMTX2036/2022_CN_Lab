#include "tcp_sender.hh"

#include "tcp_config.hh"

#include <random>

// Dummy implementation of a TCP sender

// For Lab 3, please replace with a real implementation that passes the
// automated checks run by `make check_lab3`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _initial_retransmission_timeout{retx_timeout}
    , _stream(capacity) {}

uint64_t TCPSender::bytes_in_flight() const { return _bytes_in_flight; }

void TCPSender::fill_window() {
    // 1. 如果远程窗口大小为 0,则把其视为1进行操作
    size_t current_window_size;
    if(!_last_window_size) current_window_size = 1;
    else current_window_size = _last_window_size;
    // 2. 循环填充窗口
    while (current_window_size > _bytes_in_flight) {
        // 2.1 如果尚未发送SYN数据包，则设置header的syn位
        TCPSegment segment;
        if (!_syn_set) {
            segment.header().syn = true;
            _syn_set = true;
        }
        // 2.2&2.3 设置 seqno 和 payload
        segment.header().seqno = next_seqno();

        const size_t payload_size =
            min(TCPConfig::MAX_PAYLOAD_SIZE, current_window_size - _bytes_in_flight - segment.header().syn);
        string payload = _stream.read(payload_size);

        // 2.4 若满足条件则增加FIN
        if (!_fin_set // 2.4.1. 从来没发送过 FIN
            && _stream.eof() // 2.4.2 输入字节流处于 EOF
            && payload.size()+_bytes_in_flight < current_window_size // 2.4.3 window减去payload大小后，仍可存放下FIN
            )
            _fin_set = segment.header().fin = true;

        segment.payload() = Buffer(std::move(payload));

        // 2.5 如果没有任何数据，则停止数据包的发送
        if(!segment.length_in_sequence_space()) return;

        // 2.6 如果没有正在等待的数据包，则重设更新时间
        if (_segments_outstanding.empty()) {
            _timeout = _initial_retransmission_timeout;
            _ms_current = 0;
        }

        // 2.7 发送数据包并追踪；更新待发送的absseqno
        // 2.7.1 发送
        _segments_out.push(segment); 
        // 2.7.2 追踪数据包
        _bytes_in_flight += segment.length_in_sequence_space();
        _segments_outstanding.insert(make_pair(_next_seqno, segment));
        // 2.7.3 更新待发送absseqno
        _next_seqno += segment.length_in_sequence_space();

        // 2.8 如果设置了fin,则直接退出填充window的操作
        if (segment.header().fin) break;
    }
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
void TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    uint64_t abs_seqno = unwrap(ackno, _isn, _next_seqno);
    // 1. 如果传入的 ack 是不可靠的，则直接丢弃
    if(abs_seqno > _next_seqno) return;
    // 2. 遍历数据结构，将已经接收到的数据包丢弃
    auto iter = _segments_outstanding.begin();
    while(iter != _segments_outstanding.end()) {
        // 2.1 如果一个发送的数据包已经被成功接收
        if (iter->first + iter->second.length_in_sequence_space() <= abs_seqno) {
            _bytes_in_flight -= iter->second.length_in_sequence_space();
            iter = _segments_outstanding.erase(iter);
            _timeout = _initial_retransmission_timeout;
            _ms_current = 0; // 2.2 从追踪列表中丢弃并清空超时时间
        } else break;
    }
    // 3. 调用fill_window继续填充数据
    _consecutive_retransmissions = 0;
    _last_window_size = window_size;
    fill_window();
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {
    _ms_current += ms_since_last_tick;

    auto iter = _segments_outstanding.begin();
    // 如果存在发送中的数据包，并且定时器超时
    if (iter != _segments_outstanding.end() && _ms_current >= _timeout) {
        // 如果窗口大小不为0还超时，则说明网络拥堵
        if(_last_window_size > 0) _timeout *= 2;
        _ms_current = 0;
        _segments_out.push(iter->second);
        // 连续重传计时器增加
        ++_consecutive_retransmissions;
    }
}

unsigned int TCPSender::consecutive_retransmissions() const { return _consecutive_retransmissions; }

void TCPSender::send_empty_segment() {
    TCPSegment segment;
    segment.header().seqno = next_seqno();
    _segments_out.push(segment);
}
