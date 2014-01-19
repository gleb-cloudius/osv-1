/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#include <osv/net_channel.hh>
#include <bsd/sys/sys/mbuf.h>
#include <bsd/sys/net/ethernet.h>
#include <bsd/sys/netinet/ip.h>
#include <bsd/sys/netinet/ip.h>
#include <bsd/sys/netinet/tcp.h>
#include <bsd/sys/net/ethernet.h>

void net_channel::process_queue()
{
    mbuf* m;
    while (_queue.pop(m)) {
        _process_packet(m);
    }
}

classifier::classifier()
    : _ipv4_tcp_channels(new ipv4_tcp_channels)
{
}

void classifier::add(ipv4_tcp_conn_id id, net_channel* channel)
{
    WITH_LOCK(_mtx) {
        auto old = _ipv4_tcp_channels.read_by_owner();
        std::unique_ptr<ipv4_tcp_channels> neww{new ipv4_tcp_channels(*old)};
        neww->emplace(id, channel);
        _ipv4_tcp_channels.assign(neww.release());
        osv::rcu_dispose(old);
    }
}

void classifier::remove(ipv4_tcp_conn_id id)
{
    WITH_LOCK(_mtx) {
        auto old = _ipv4_tcp_channels.read_by_owner();
        std::unique_ptr<ipv4_tcp_channels> neww{new ipv4_tcp_channels(*old)};
        neww->erase(id);
        _ipv4_tcp_channels.assign(neww.release());
        osv::rcu_dispose(old);
    }
}

bool classifier::post_packet(mbuf* m)
{
    WITH_LOCK(osv::rcu_read_lock) {
        if (auto nc = classify_ipv4_tcp(m)) {
            nc->push(m);
            // FIXME: find a way to batch wakes
            nc->wake();
            return true;
        }
    }
    return false;
}

// must be called with rcu lock held
net_channel* classifier::classify_ipv4_tcp(mbuf* m)
{
    caddr_t h = m->m_hdr.mh_data;
    if (unsigned(m->m_hdr.mh_len) < ETHER_HDR_LEN + sizeof(ip)) {
        return nullptr;
    }
    auto ether_hdr = reinterpret_cast<ether_header*>(h);
    if (ntohs(ether_hdr->ether_type) != ETHERTYPE_IP) {
        return nullptr;
    }
    h += ETHER_HDR_LEN;
    auto ip_hdr = reinterpret_cast<ip*>(h);
    unsigned ip_size = ip_hdr->ip_hl << 2;
    if (ip_size < sizeof(ip)) {
        return nullptr;
    }
    if (ip_hdr->ip_p != IPPROTO_TCP) {
        return nullptr;
    }
    if (ntohs(ip_hdr->ip_off) & ~IP_DF) {
        return nullptr;
    }
    auto src_addr = ip_hdr->ip_src;
    auto dst_addr = ip_hdr->ip_dst;
    h += ip_size;
    auto tcp_hdr = reinterpret_cast<tcphdr*>(h);
    auto src_port = ntohs(tcp_hdr->th_sport);
    auto dst_port = ntohs(tcp_hdr->th_dport);
    auto id = ipv4_tcp_conn_id{src_addr, dst_addr, src_port, dst_port};
    auto ht = _ipv4_tcp_channels.read();
    auto i = ht->find(id);
    if (i == ht->end()) {
        return nullptr;
    }
    return i->second;
}
