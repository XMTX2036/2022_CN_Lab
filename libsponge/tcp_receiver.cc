#include "tcp_receiver.hh"

#include <algorithm>

// Dummy implementation of a TCP receiver

// For Lab 2, please replace with a real implementation that passes the
// automated checks run by `make check_lab2`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

void TCPReceiver::segment_received(const TCPSegment &seg) {
    const TCPHeader head = seg.header();

    if (!head.syn && !_syn_received) return;

    // extract data from the payload
    string data = seg.payload().copy();

    bool eof = false;

    if (head.syn && !_syn_received) {
        _syn_received = true;
        _isn = head.seqno; // Set the Initial Sequence Number if necessary.
        if (head.fin) _fin_received = eof = true;
        _reassembler.push_substring(data, 0, eof); // push any data, or the end-of-stream marker to the StreamReassembler
        return;
    }

    if (_syn_received && head.fin) _fin_received = eof = true;
    

    // convert the seqno into absolute seqno
    uint64_t checkpoint = _reassembler.stream_out().bytes_written() + 1;
    uint64_t abs_seqno = unwrap(head.seqno, _isn, checkpoint);
    uint64_t stream_idx = abs_seqno - _syn_received;

    // push the data into stream reassembler
    _reassembler.push_substring(data, stream_idx, eof);
}

optional<WrappingInt32> TCPReceiver::ackno() const {
    // If the ISN hasn’t been set yet, return an empty optional.
    if(!_syn_received) return std::nullopt;
    else {
        uint64_t ack_no = _reassembler.stream_out().bytes_written() + 1;
        if(_reassembler.empty() && _fin_received) ++ack_no;
        return _isn + ack_no;
    }
}

// window_size = first unacceptable index - first unassembled index
size_t TCPReceiver::window_size() const { return _capacity - _reassembler.stream_out().buffer_size(); }