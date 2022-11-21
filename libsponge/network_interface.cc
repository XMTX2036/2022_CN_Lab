#include "network_interface.hh"

#include "arp_message.hh"
#include "ethernet_frame.hh"

#include <iostream>
#include <algorithm>

// Dummy implementation of a network interface
// Translates from {IP datagram, next hop address} to link-layer frame, and from link-layer frame to IP datagram

// For Lab 5, please replace with a real implementation that passes the
// automated checks run by `make check_lab5`.

// You will need to add private members to the class declaration in `network_interface.hh`

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! \param[in] ethernet_address Ethernet (what ARP calls "hardware") address of the interface
//! \param[in] ip_address IP (what ARP calls "protocol") address of the interface
NetworkInterface::NetworkInterface(const EthernetAddress &ethernet_address, const Address &ip_address)
    : _ethernet_address(ethernet_address), _ip_address(ip_address) {
    cerr << "DEBUG: Network interface has Ethernet address " << to_string(_ethernet_address) << " and IP address "
         << ip_address.ip() << "\n";
}

//! Helper Function
void NetworkInterface::send_arp_request(const uint32_t next_hop_ip) {
    ARPMessage arp_request;
    arp_request.opcode = ARPMessage::OPCODE_REQUEST;
    arp_request.sender_ethernet_address = _ethernet_address;
    arp_request.sender_ip_address = _ip_address.ipv4_numeric();
    arp_request.target_ip_address = next_hop_ip;
    
    EthernetFrame eth_frame;
    eth_frame.header() = {ETHERNET_BROADCAST, _ethernet_address, EthernetHeader::TYPE_ARP};
    eth_frame.payload() = arp_request.serialize();
    _frames_out.push(std::move(eth_frame));
}

//! \param[in] dgram the IPv4 datagram to be sent
//! \param[in] next_hop the IP address of the interface to send it to (typically a router or default gateway, but may also be another host if directly connected to the same network as the destination)
//! (Note: the Address type can be converted to a uint32_t (raw 32-bit IP address) with the Address::ipv4_numeric() method.)
void NetworkInterface::send_datagram(const InternetDatagram &dgram, const Address &next_hop) {
    // convert IP address of next hop to raw 32-bit representation (used in ARP header)
    const uint32_t next_hop_ip = next_hop.ipv4_numeric();
    // 1. search the ARP table, for the corresponding MAC address. 
    const auto &arp_iter = _arp_table.find(next_hop_ip);
    if (arp_iter != _arp_table.end()) { // 2.1 if there exists the corresponding MAC address
        /* wrap the EthernetFrame and send it */
        EthernetFrame ef;
        ef.header() = {arp_iter->second.eth_addr, _ethernet_address, EthernetHeader::TYPE_IPv4};
        ef.payload() = dgram.serialize();
        _frames_out.push(std::move(ef));
    } else { // 2.2 if there isn't the corresponding MAC address
        /* wrap ARP package and send it */
        auto dgram_iter = _waiting_map.find(next_hop_ip);
        if (dgram_iter == _waiting_map.end()) {
            /* push the datagram into the waiting queue */
            send_arp_request(next_hop_ip);
            _waiting_map[next_hop_ip] = RESPONSE_WAITING_TIME;
        }

        _arp_datagram.push_back({next_hop, dgram});
    }
}

//! \param[in] frame the incoming Ethernet frame
optional<InternetDatagram> NetworkInterface::recv_frame(const EthernetFrame &frame) {
    // 是不是发送到该网络接口的？地址是否匹配或者是否为广播
    // 1. 否，直接过滤掉不响应
    if (frame.header().dst!=_ethernet_address && frame.header().dst!=ETHERNET_BROADCAST) return nullopt;
    // 2. 是，看是不是IPv4或者ARP包
    // 2.1 都不是，不响应
    if(frame.header().type != EthernetHeader::TYPE_IPv4 && frame.header().type != EthernetHeader::TYPE_ARP) return nullopt;
    // 2.2 
    // 2.2.1 是IPv4，看能否正常解析,能则将报文返回给调用者
    else if(frame.header().type == EthernetHeader::TYPE_IPv4) {
        InternetDatagram datagram;
        if(datagram.parse(frame.payload()) == ParseResult::NoError) return datagram;
        return nullopt;
    }
    // 2.2.2 是ARP包，看能否正常解析
    else if (frame.header().type == EthernetHeader::TYPE_ARP) {
        ARPMessage message;
        // 2.2.2.1 不能正常解析，则直接返回
        if(message.parse(frame.payload()) != ParseResult::NoError) return nullopt;

        bool flag = false;
        auto iter0 = _arp_table.find(message.sender_ip_address);
        if(iter0 != _arp_table.end()){
            iter0->second.eth_addr = message.sender_ethernet_address;
            flag = true;
        }
        // 2.2.2.2 
        if(message.target_ip_address == _ip_address.ipv4_numeric()) {
            if(!flag) {
                // _arp_table[message.sender_ip_address].eth_addr = message.sender_ethernet_address;
                _arp_table[message.sender_ip_address] = {message.sender_ethernet_address, MAPPING_REMEMBER_TIME};
                // 将对应数据从原先等待队列里删除
                auto iter = _arp_datagram.begin();
                while(iter != _arp_datagram.end()) {
                    if(iter->first.ipv4_numeric() == message.sender_ip_address) {
                        send_datagram(iter->second, iter->first);
                        iter = _arp_datagram.erase(iter);
                    } else ++iter;
                }
                _waiting_map.erase(message.sender_ip_address);
            }
            if(message.opcode == ARPMessage::OPCODE_REQUEST) {
                message.opcode = ARPMessage::OPCODE_REPLY;
                std::swap(message.sender_ip_address, message.target_ip_address);
                message.target_ethernet_address = message.sender_ethernet_address;
                message.sender_ethernet_address = _ethernet_address;

                EthernetFrame ef;
                ef.header() = {message.target_ethernet_address, _ethernet_address, EthernetHeader::TYPE_ARP};
                ef.payload() = message.serialize();
                _frames_out.push(std::move(ef));
            }
        }
    }
    return nullopt;
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void NetworkInterface::tick(const size_t ms_since_last_tick) {
    // 1. 将ARP table中过期的条目删除，检查每一条
    // 1.1 if the record is outdated, delete it.
    // 1.2 if the record isn't outdated, update it.
    auto iter1 = _arp_table.begin();
    while(iter1 != _arp_table.end()) {
        if(iter1->second.ttl <= ms_since_last_tick) iter1 = _arp_table.erase(iter1);
        else {
            iter1->second.ttl -= ms_since_last_tick;
            ++iter1;
        }
    }

    // 2. 如果ARP等待队列中的ARP请求过期，则重新发送ARP请求；检查每一条
    auto iter2 = _waiting_map.begin();
    while(iter2 != _waiting_map.end()) {
        if(iter2->second <= ms_since_last_tick) {
            send_arp_request(iter2->first);
            iter2->second = RESPONSE_WAITING_TIME;            
        } else {
            iter2->second -= ms_since_last_tick;
            ++iter2;
        }
    }
}