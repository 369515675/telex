// $Id: Sessions.cc 6934 2009-11-07 23:32:52Z vern $
//
// See the file "COPYING" in the main distribution directory for copyright.


#include "config.h"

#include <arpa/inet.h>

#include <stdlib.h>
#include <unistd.h>

#include "Net.h"
#include "Event.h"
#include "Timer.h"
#include "NetVar.h"
#include "Sessions.h"
#include "Active.h"
#include "OSFinger.h"

#include "ICMP.h"
#include "UDP.h"

#include "DNS-binpac.h"
#include "HTTP-binpac.h"

#include "SteppingStone.h"
#include "BackDoor.h"
#include "InterConn.h"
#include "Discard.h"
#include "RuleMatcher.h"
#include "ConnCompressor.h"
#include "DPM.h"

#include "PacketSort.h"

// These represent NetBIOS services on ephemeral ports.  They're numbered
// so that we can use a single int to hold either an actual TCP/UDP server
// port or one of these.
enum NetBIOS_Service {
	NETBIOS_SERVICE_START = 0x10000L,	// larger than any port
	NETBIOS_SERVICE_DCE_RPC,
};

NetSessions* sessions;


class NetworkTimer : public Timer {
public:
	NetworkTimer(NetSessions* arg_sess, double arg_t)
		: Timer(arg_t, TIMER_NETWORK)
		{ sess = arg_sess; }

	void Dispatch(double t, int is_expire);

protected:
	NetSessions* sess;
};

void NetworkTimer::Dispatch(double t, int is_expire)
	{
	if ( is_expire )
		return;

	sess->HeartBeat(t);
	}

void TimerMgrExpireTimer::Dispatch(double t, int is_expire)
	{
	if ( mgr->LastAdvance() + timer_mgr_inactivity_timeout < timer_mgr->Time() )
		{
		// Expired.
		DBG_LOG(DBG_TM, "TimeMgr %p has timed out", mgr);
		mgr->Expire();

		// Make sure events are executed.  They depend on the TimerMgr.
		::mgr.Drain();

		sessions->timer_mgrs.erase(mgr->GetTag());
		delete mgr;
		}
	else
		{
		// Reinstall timer.
		if ( ! is_expire )
			{
			double n = mgr->LastAdvance() +
					timer_mgr_inactivity_timeout;
			timer_mgr->Add(new TimerMgrExpireTimer(n, mgr));
			}
		}
	}

NetSessions::NetSessions()
	{
	TypeList* t = new TypeList();
	t->Append(base_type(TYPE_COUNT));	// source IP address
	t->Append(base_type(TYPE_COUNT));	// dest IP address
	t->Append(base_type(TYPE_COUNT));	// source and dest ports

	ch = new CompositeHash(t);

	Unref(t);

	tcp_conns.SetDeleteFunc(bro_obj_delete_func);
	udp_conns.SetDeleteFunc(bro_obj_delete_func);
	fragments.SetDeleteFunc(bro_obj_delete_func);

	if ( (reading_live || pseudo_realtime) && net_stats_update )
		timer_mgr->Add(new NetworkTimer(this, 1.0));

	if ( stp_correlate_pair )
		stp_manager = new SteppingStoneManager();
	else
		stp_manager = 0;

	discarder = new Discarder();
	if ( ! discarder->IsActive() )
		{
		delete discarder;
		discarder = 0;
		}

	packet_filter = 0;

	build_backdoor_analyzer =
		backdoor_stats || rlogin_signature_found ||
		telnet_signature_found || ssh_signature_found ||
		root_backdoor_signature_found || ftp_signature_found ||
		napster_signature_found || kazaa_signature_found ||
		http_signature_found || http_proxy_signature_found;

	dump_this_packet = 0;
	num_packets_processed = 0;

	if ( OS_version_found )
		{
		SYN_OS_Fingerprinter = new OSFingerprint(SYN_FINGERPRINT_MODE);
		if ( SYN_OS_Fingerprinter->Error() )
			exit(1);
		}
	else
		SYN_OS_Fingerprinter = 0;

	if ( pkt_profile_mode && pkt_profile_freq > 0 && pkt_profile_file )
		pkt_profiler = new PacketProfiler(pkt_profile_mode,
				pkt_profile_freq, pkt_profile_file->AsFile());
	else
		pkt_profiler = 0;

	if ( arp_request || arp_reply || bad_arp )
		arp_analyzer = new ARP_Analyzer();
	else
		arp_analyzer = 0;
	}

NetSessions::~NetSessions()
	{
	delete ch;
	delete packet_filter;
	delete SYN_OS_Fingerprinter;
	delete pkt_profiler;
	Unref(arp_analyzer);
	}

void NetSessions::Done()
	{
	delete stp_manager;
	delete discarder;
	}

namespace	// private namespace
	{
	bool looks_like_IPv4_packet(int len, const struct ip* ip_hdr)
		{
		if ( len < int(sizeof(struct ip)) )
			return false;
		return ip_hdr->ip_v == 4 && ntohs(ip_hdr->ip_len) == len;
		}
	}

void NetSessions::DispatchPacket(double t, const struct pcap_pkthdr* hdr,
			const u_char* pkt, int hdr_size,
			PktSrc* src_ps, PacketSortElement* pkt_elem)
	{
	const struct ip* ip_hdr = 0;
	const u_char* ip_data = 0;
	int proto = 0;

	if ( hdr->caplen >= hdr_size + sizeof(struct ip) )
		{
		ip_hdr = reinterpret_cast<const struct ip*>(pkt + hdr_size);
		if ( hdr->caplen >= unsigned(hdr_size + (ip_hdr->ip_hl << 2)) )
			ip_data = pkt + hdr_size + (ip_hdr->ip_hl << 2);
		}

	if ( encap_hdr_size > 0 && ip_data )
		{
		// We're doing tunnel encapsulation.  Check whether there's
		// a particular associated port.
		//
		// Should we discourage the use of encap_hdr_size for UDP
		// tunnneling?  It is probably better handled by enabling
		// parse_udp_tunnels instead of specifying a fixed
		// encap_hdr_size.
		if ( udp_tunnel_port > 0 )
			{
			ASSERT(ip_hdr);
			if ( ip_hdr->ip_p == IPPROTO_UDP )
				{
				const struct udphdr* udp_hdr =
					reinterpret_cast<const struct udphdr*>
					(ip_data);

				if ( ntohs(udp_hdr->uh_dport) == udp_tunnel_port )
					{
					// A match.
					hdr_size += encap_hdr_size;
					}
				}
			}

		else
			// Blanket encapsulation (e.g., for VLAN).
			hdr_size += encap_hdr_size;
		}

	// Check IP packets encapsulated through UDP tunnels.
	// Specifying a udp_tunnel_port is optional but recommended (to avoid
	// the cost of checking every UDP packet).
	else if ( parse_udp_tunnels && ip_data && ip_hdr->ip_p == IPPROTO_UDP )
		{
		const struct udphdr* udp_hdr =
			reinterpret_cast<const struct udphdr*>(ip_data);

		if ( udp_tunnel_port == 0 || // 0 matches any port
		     udp_tunnel_port == ntohs(udp_hdr->uh_dport) )
			{
			const u_char* udp_data =
				ip_data + sizeof(struct udphdr);
			const struct ip* ip_encap =
				reinterpret_cast<const struct ip*>(udp_data);
			const int ip_encap_len =
				ntohs(udp_hdr->uh_ulen) - sizeof(struct udphdr);
			const int ip_encap_caplen =
				hdr->caplen - (udp_data - pkt);

			if ( looks_like_IPv4_packet(ip_encap_len, ip_encap) )
				hdr_size = udp_data - pkt;
			}
		}

	if ( src_ps->FilterType() == TYPE_FILTER_NORMAL )
		NextPacket(t, hdr, pkt, hdr_size, pkt_elem);
	else
		NextPacketSecondary(t, hdr, pkt, hdr_size, src_ps);
	}

void NetSessions::NextPacket(double t, const struct pcap_pkthdr* hdr,
			     const u_char* const pkt, int hdr_size,
			     PacketSortElement* pkt_elem)
	{
	SegmentProfiler(segment_logger, "processing-packet");
	if ( pkt_profiler )
		pkt_profiler->ProfilePkt(t, hdr->caplen);

	++num_packets_processed;

	dump_this_packet = 0;

	if ( record_all_packets )
		DumpPacket(hdr, pkt);

	if ( pkt_elem && pkt_elem->IPHdr() )
		// Fast path for "normal" IP packets if an IP_Hdr is
		// already extracted when doing PacketSort. Otherwise
		// the code below tries to extract the IP header, the
		// difference here is that header extraction in
		// PacketSort does not generate Weird events.

		DoNextPacket(t, hdr, pkt_elem->IPHdr(), pkt, hdr_size);

	else
		{
		// ### The following isn't really correct.  What we *should*
		// do is understanding the different link layers in order to
		// find the network-layer protocol ID.  That's a big
		// portability pain, though, unless we just assume everything's
		// Ethernet .... not great, given the potential need to deal
		// with PPP or FDDI (for some older traces).  So instead
		// we look to see if what we have is consistent with an
		// IPv4 packet.  If not, it's either ARP or IPv6 or weird.

		uint32 caplen = hdr->caplen - hdr_size;
		if ( caplen < sizeof(struct ip) )
			{
			Weird("truncated_IP", hdr, pkt);
			return;
			}

		const struct ip* ip = (const struct ip*) (pkt + hdr_size);
		if ( ip->ip_v == 4 )
			{
			IP_Hdr ip_hdr(ip);
			DoNextPacket(t, hdr, &ip_hdr, pkt, hdr_size);
			}

		else if ( arp_analyzer && arp_analyzer->IsARP(pkt, hdr_size) )
			arp_analyzer->NextPacket(t, hdr, pkt, hdr_size);

		else
			{
#ifdef BROv6
			IP_Hdr ip_hdr((const struct ip6_hdr*) (pkt + hdr_size));
			DoNextPacket(t, hdr, &ip_hdr, pkt, hdr_size);
#else
			Weird("non_IPv4_packet", hdr, pkt);
			return;
#endif
			}
		}

	if ( dump_this_packet && ! record_all_packets )
		DumpPacket(hdr, pkt);
	}

void NetSessions::NextPacketSecondary(double /* t */, const struct pcap_pkthdr* hdr,
				const u_char* const pkt, int hdr_size,
				const PktSrc* src_ps)
	{
	SegmentProfiler(segment_logger, "processing-secondary-packet");

	++num_packets_processed;

	uint32 caplen = hdr->caplen - hdr_size;
	if ( caplen < sizeof(struct ip) )
		{
		Weird("truncated_IP", hdr, pkt);
		return;
		}

	const struct ip* ip = (const struct ip*) (pkt + hdr_size);
	if ( ip->ip_v == 4 )
		{
		const secondary_program_list& spt = src_ps->ProgramTable();

		loop_over_list(spt, i)
			{
			SecondaryProgram* sp = spt[i];
			if ( ! net_packet_match(sp->Program(), pkt,
						hdr->len, hdr->caplen) )
				continue;

			val_list* args = new val_list;
			StringVal* cmd_val =
				new StringVal(sp->Event()->Filter());
			args->append(cmd_val);
			args->append(BuildHeader(ip));
			// ### Need to queue event here.
			sp->Event()->Event()->Call(args);
			delete args;
			}
		}
	}

int NetSessions::CheckConnectionTag(Connection* conn)
	{
	if ( current_iosrc->GetCurrentTag() )
		{
		// Packet is tagged.
		if ( conn->GetTimerMgr() == timer_mgr )
			{
			// Connection uses global timer queue.  But the
			// packet has a tag that means we got it externally,
			// probably from the Time Machine.
			DBG_LOG(DBG_TM, "got packet with tag %s for already"
					"known connection, reinstantiating",
					current_iosrc->GetCurrentTag()->c_str());
			return 0;
			}
		else
			{
			// Connection uses local timer queue.
			TimerMgrMap::iterator i =
				timer_mgrs.find(*current_iosrc->GetCurrentTag());
			if ( i != timer_mgrs.end() &&
			     conn->GetTimerMgr() != i->second )
				{
				// Connection uses different local queue
				// than the tag for the current packet
				// indicates.
				//
				// This can happen due to:
				//     (1) getting same packets with
				//		different tags
				//     (2) timer mgr having already expired
				DBG_LOG(DBG_TM, "packet ignored due old/inconsistent tag");
				return -1;
				}

			return 1;
			}
		}

	// Packet is not tagged.
	if ( conn->GetTimerMgr() != timer_mgr )
		{
		// Connection does not use the global timer queue.  That
		// means that this is a live packet belonging to a
		// connection for which we have already switched to
		// processing external input.
		DBG_LOG(DBG_TM, "packet ignored due to processing it in external data");
		return -1;
		}

	return 1;
	}


static bool looks_like_IPv4_packet(int len, const struct ip* ip_hdr)
	{
	if ( (unsigned int) len < sizeof(struct ip) )
		return false;

	if ( ip_hdr->ip_v == 4 && ntohs(ip_hdr->ip_len) == len )
		return true;
	else
		return false;
	}

void NetSessions::DoNextPacket(double t, const struct pcap_pkthdr* hdr,
				const IP_Hdr* ip_hdr, const u_char* const pkt,
				int hdr_size)
	{
	uint32 caplen = hdr->caplen - hdr_size;
	const struct ip* ip4 = ip_hdr->IP4_Hdr();

	uint32 len = ip_hdr->TotalLen();
	if ( hdr->len < len + hdr_size )
		{
		Weird("truncated_IP", hdr, pkt);
		return;
		}

	// Ignore if packet matches packet filter.
	if ( packet_filter && packet_filter->Match(ip_hdr, len, caplen) )
		 return;

	int ip_hdr_len = ip_hdr->HdrLen();
	if ( ! ignore_checksums && ip4 &&
	     ones_complement_checksum((void*) ip4, ip_hdr_len, 0) != 0xffff )
		{
		Weird("bad_IP_checksum", hdr, pkt);
		return;
		}

	if ( discarder && discarder->NextPacket(ip_hdr, len, caplen) )
		return;

	int proto = ip_hdr->NextProto();
	if ( proto != IPPROTO_TCP && proto != IPPROTO_UDP &&
	     proto != IPPROTO_ICMP )
		{
		dump_this_packet = 1;
		return;
		}

	// Check for TTL/MTU problems from Active Mapping
#ifdef ACTIVE_MAPPING
	if ( ip4 )
		{
		const NumericData* numeric;
		get_map_result(ip4->ip_dst.s_addr, numeric);

		if ( numeric->hops && ip4->ip_ttl < numeric->hops )
			{
			debug_msg("Packet destined for %s had ttl %d but there are %d hops to host.\n",
			inet_ntoa(ip4->ip_dst), ip4->ip_ttl, numeric->hops);
			return;
			}
		}
#endif

	FragReassembler* f = 0;
	uint32 frag_field = ip_hdr->FragField();

#ifdef ACTIVE_MAPPING
	if ( ip4 && numeric->path_MTU && (frag_field & IP_DF) )
		{
		if ( htons(ip4->ip_len) > numeric->path_MTU )
			{
			debug_msg("Packet destined for %s has DF flag but its size %d is greater than pmtu of %d\n",
			inet_ntoa(ip4->ip_dst), htons(ip4->ip_len), numeric->path_MTU);
			return;
			}
		}
#endif

	if ( (frag_field & 0x3fff) != 0 )
		{
		dump_this_packet = 1;	// always record fragments

		if ( caplen < len )
			{
			Weird("incompletely_captured_fragment", ip_hdr);

			// Don't try to reassemble, that's doomed.
			// Discard all except the first fragment (which
			// is useful in analyzing header-only traces)
			if ( (frag_field & 0x1fff) != 0 )
				return;
			}
		else
			{
			f = NextFragment(t, ip_hdr, pkt + hdr_size, frag_field);
			const IP_Hdr* ih = f->ReassembledPkt();
			if ( ! ih )
				// It didn't reassemble into anything yet.
				return;

			ip4 = ih->IP4_Hdr();
			ip_hdr = ih;

			caplen = len = ip_hdr->TotalLen();
			ip_hdr_len = ip_hdr->HdrLen();
			}
		}

	len -= ip_hdr_len;	// remove IP header
	caplen -= ip_hdr_len;

	uint32 min_hdr_len = (proto == IPPROTO_TCP) ?  sizeof(struct tcphdr) :
		(proto == IPPROTO_UDP ? sizeof(struct udphdr) : ICMP_MINLEN);

	if ( len < min_hdr_len )
		{
		Weird("truncated_header", hdr, pkt);
		if ( f )
			Remove(f);	// ###
		return;
		}
	if ( caplen < min_hdr_len )
		{
		Weird("internally_truncated_header", hdr, pkt);
		if ( f )
			Remove(f);	// ###
		return;
		}

	const u_char* data = ip_hdr->Payload();

	ConnID id;
	id.src_addr = ip_hdr->SrcAddr();
	id.dst_addr = ip_hdr->DstAddr();
	Dictionary* d = 0;
	bool pass_to_conn_compressor = false;

	switch ( proto ) {
	case IPPROTO_TCP:
		{
		const struct tcphdr* tp = (const struct tcphdr *) data;
		id.src_port = tp->th_sport;
		id.dst_port = tp->th_dport;
		id.is_one_way = 0;
		d = &tcp_conns;
		pass_to_conn_compressor = ip4 && use_connection_compressor;
		break;
		}

	case IPPROTO_UDP:
		{
		const struct udphdr* up = (const struct udphdr *) data;
		id.src_port = up->uh_sport;
		id.dst_port = up->uh_dport;
		id.is_one_way = 0;
		d = &udp_conns;
		break;
		}

	case IPPROTO_ICMP:
		{
		const struct icmp* icmpp = (const struct icmp *) data;

		id.src_port = icmpp->icmp_type;
		id.dst_port = ICMP_counterpart(icmpp->icmp_type,
						icmpp->icmp_code,
						id.is_one_way);

		id.src_port = htons(id.src_port);
		id.dst_port = htons(id.dst_port);

		d = &icmp_conns;
		break;
		}

	default:
		Weird(fmt("unknown_protocol %d", proto), hdr, pkt);
		return;
	}

	HashKey* h = id.BuildConnKey();
	if ( ! h )
		internal_error("hash computation failed");

	Connection* conn = 0;

	// FIXME: The following is getting pretty complex. Need to split up
	// into separate functions.
	if ( pass_to_conn_compressor )
		conn = conn_compressor->NextPacket(t, h, ip_hdr, hdr, pkt);
	else
		{
		conn = (Connection*) d->Lookup(h);
		if ( ! conn )
			{
			conn = NewConn(h, t, &id, data, proto);
			if ( conn )
				d->Insert(h, conn);
			}
		else
			{
			// We already know that connection.
			int consistent = CheckConnectionTag(conn);
			if ( consistent < 0 )
				{
				delete h;
				return;
				}

			if ( ! consistent || conn->IsReuse(t, data) )
				{
				if ( consistent )
					conn->Event(connection_reused, 0);

				Remove(conn);
				conn = NewConn(h, t, &id, data, proto);
				if ( conn )
					d->Insert(h, conn);
				}
			else
				delete h;
			}

		if ( ! conn )
			delete h;
		}

	if ( ! conn )
		return;

	int record_packet = 1;	// whether to record the packet at all
	int record_content = 1;	// whether to record its data

	int is_orig = addr_eq(id.src_addr, conn->OrigAddr()) &&
			id.src_port == conn->OrigPort();

	if ( new_packet && ip4 )
		conn->Event(new_packet, 0, BuildHeader(ip4));

	conn->NextPacket(t, is_orig, ip_hdr, len, caplen, data,
				record_packet, record_content,
			        hdr, pkt, hdr_size);

    if ( new_packet_final && ip4 )
      conn->Event(new_packet_final, 0, BuildHeader(ip4));
	// Override content record setting according to
	// flags set by the policy script.
	if ( dump_original_packets_if_not_rewriting )
		record_packet = record_content = 1;
	if ( dump_selected_source_packets )
		record_packet = record_content = 0;

	if ( f )
		{
		// Above we already recorded the fragment in its entirety.
		f->DeleteTimer();
		Remove(f);	// ###
		}

	else if ( record_packet && ! conn->RewritingTrace() )
		{
		if ( record_content )
			dump_this_packet = 1;	// save the whole thing

		else
			{
			int hdr_len = data - pkt;
			DumpPacket(hdr, pkt, hdr_len);	// just save the header
			}
		}
	}

Val* NetSessions::BuildHeader(const struct ip* ip)
	{
	static RecordType* pkt_hdr_type = 0;
	static RecordType* ip_hdr_type = 0;
	static RecordType* tcp_hdr_type = 0;
	static RecordType* udp_hdr_type = 0;
	static RecordType* icmp_hdr_type;

	if ( ! pkt_hdr_type )
		{
		pkt_hdr_type = internal_type("pkt_hdr")->AsRecordType();
		ip_hdr_type = internal_type("ip_hdr")->AsRecordType();
		tcp_hdr_type = internal_type("tcp_hdr")->AsRecordType();
		udp_hdr_type = internal_type("udp_hdr")->AsRecordType();
		icmp_hdr_type = internal_type("icmp_hdr")->AsRecordType();
		}

	RecordVal* pkt_hdr = new RecordVal(pkt_hdr_type);

	RecordVal* ip_hdr = new RecordVal(ip_hdr_type);

	int ip_hdr_len = ip->ip_hl * 4;
	int ip_pkt_len = ntohs(ip->ip_len);

	ip_hdr->Assign(0, new Val(ip->ip_hl * 4, TYPE_COUNT));
	ip_hdr->Assign(1, new Val(ip->ip_tos, TYPE_COUNT));
	ip_hdr->Assign(2, new Val(ip_pkt_len, TYPE_COUNT));
	ip_hdr->Assign(3, new Val(ntohs(ip->ip_id), TYPE_COUNT));
	ip_hdr->Assign(4, new Val(ip->ip_ttl, TYPE_COUNT));
	ip_hdr->Assign(5, new Val(ip->ip_p, TYPE_COUNT));
	ip_hdr->Assign(6, new AddrVal(ip->ip_src.s_addr));
	ip_hdr->Assign(7, new AddrVal(ip->ip_dst.s_addr));

	pkt_hdr->Assign(0, ip_hdr);

	// L4 header.
	const u_char* data = ((const u_char*) ip) + ip_hdr_len;

	int proto = ip->ip_p;
	switch ( proto ) {
	case IPPROTO_TCP:
		{
		const struct tcphdr* tp = (const struct tcphdr*) data;
		RecordVal* tcp_hdr = new RecordVal(tcp_hdr_type);

		int tcp_hdr_len = tp->th_off * 4;
		int data_len = ip_pkt_len - ip_hdr_len - tcp_hdr_len;

		tcp_hdr->Assign(0, new PortVal(ntohs(tp->th_sport), TRANSPORT_TCP));
		tcp_hdr->Assign(1, new PortVal(ntohs(tp->th_dport), TRANSPORT_TCP));
		tcp_hdr->Assign(2, new Val(uint32(ntohl(tp->th_seq)), TYPE_COUNT));
		tcp_hdr->Assign(3, new Val(uint32(ntohl(tp->th_ack)), TYPE_COUNT));
		tcp_hdr->Assign(4, new Val(tcp_hdr_len, TYPE_COUNT));
		tcp_hdr->Assign(5, new Val(data_len, TYPE_COUNT));
		tcp_hdr->Assign(6, new Val(tp->th_flags, TYPE_COUNT));
		tcp_hdr->Assign(7, new Val(ntohs(tp->th_win), TYPE_COUNT));

		pkt_hdr->Assign(1, tcp_hdr);
		break;
		}

	case IPPROTO_UDP:
		{
		const struct udphdr* up = (const struct udphdr*) data;
		RecordVal* udp_hdr = new RecordVal(udp_hdr_type);

		udp_hdr->Assign(0, new PortVal(ntohs(up->uh_sport), TRANSPORT_UDP));
		udp_hdr->Assign(1, new PortVal(ntohs(up->uh_dport), TRANSPORT_UDP));
		udp_hdr->Assign(2, new Val(ntohs(up->uh_ulen), TYPE_COUNT));

		pkt_hdr->Assign(2, udp_hdr);
		break;
		}

	case IPPROTO_ICMP:
		{
		const struct icmp* icmpp = (const struct icmp *) data;
		RecordVal* icmp_hdr = new RecordVal(icmp_hdr_type);

		icmp_hdr->Assign(0, new Val(icmpp->icmp_type, TYPE_COUNT));

		pkt_hdr->Assign(3, icmp_hdr);
		break;
		}

	default:
		{
		// This is not a protocol we understand.
		}
	}

    pkt_hdr->Assign(4, new StringVal(ip_pkt_len, (const char*)ip));

	return pkt_hdr;
	}

FragReassembler* NetSessions::NextFragment(double t, const IP_Hdr* ip,
					const u_char* pkt, uint32 frag_field)
	{
	uint32 src_addr = uint32(ip->SrcAddr4());
	uint32 dst_addr = uint32(ip->DstAddr4());
	uint32 frag_id = ntohs(ip->ID4());	// we actually could skip conv.

	ListVal* key = new ListVal(TYPE_ANY);
	key->Append(new Val(src_addr, TYPE_COUNT));
	key->Append(new Val(dst_addr, TYPE_COUNT));
	key->Append(new Val(frag_id, TYPE_COUNT));

	HashKey* h = ch->ComputeHash(key, 1);
	if ( ! h )
		internal_error("hash computation failed");

	FragReassembler* f = fragments.Lookup(h);
	if ( ! f )
		{
		f = new FragReassembler(this, ip, pkt, frag_field, h, t);
		fragments.Insert(h, f);
		Unref(key);
		return f;
		}

	delete h;
	Unref(key);

	f->AddFragment(t, ip, pkt, frag_field);
	return f;
	}

int NetSessions::Get_OS_From_SYN(struct os_type* retval,
		  uint16 tot, uint8 DF_flag, uint8 TTL, uint16 WSS,
		  uint8 ocnt, uint8* op, uint16 MSS, uint8 win_scale,
		  uint32 tstamp, /* uint8 TOS, */ uint32 quirks,
		  uint8 ECN) const
	{
	return SYN_OS_Fingerprinter ?
		SYN_OS_Fingerprinter->FindMatch(retval, tot, DF_flag, TTL,
				WSS, ocnt, op, MSS, win_scale, tstamp,
				quirks, ECN) : 0;
	}

bool NetSessions::CompareWithPreviousOSMatch(uint32 addr, int id) const
	{
	return SYN_OS_Fingerprinter ?
		SYN_OS_Fingerprinter->CacheMatch(addr, id) : 0;
	}

Connection* NetSessions::FindConnection(Val* v)
	{
	BroType* vt = v->Type();
	if ( ! IsRecord(vt->Tag()) )
		return 0;

	RecordType* vr = vt->AsRecordType();
	const val_list* vl = v->AsRecord();

	int orig_h, orig_p;	// indices into record's value list
	int resp_h, resp_p;

	if ( vr == conn_id )
		{
		orig_h = 0;
		orig_p = 1;
		resp_h = 2;
		resp_p = 3;
		}

	else
		{
		// While it's not a conn_id, it may have equivalent fields.
		orig_h = vr->FieldOffset("orig_h");
		resp_h = vr->FieldOffset("resp_h");
		orig_p = vr->FieldOffset("orig_p");
		resp_p = vr->FieldOffset("resp_p");

		if ( orig_h < 0 || resp_h < 0 || orig_p < 0 || resp_p < 0 )
			return 0;

		// ### we ought to check that the fields have the right
		// types, too.
		}

	addr_type orig_addr = (*vl)[orig_h]->AsAddr();
	addr_type resp_addr = (*vl)[resp_h]->AsAddr();

	PortVal* orig_portv = (*vl)[orig_p]->AsPortVal();
	PortVal* resp_portv = (*vl)[resp_p]->AsPortVal();

	ConnID id;

#ifdef BROv6
	id.src_addr = orig_addr;
	id.dst_addr = resp_addr;
#else
	id.src_addr = &orig_addr;
	id.dst_addr = &resp_addr;
#endif

	id.src_port = htons((unsigned short) orig_portv->Port());
	id.dst_port = htons((unsigned short) resp_portv->Port());

	id.is_one_way = 0;	// ### incorrect for ICMP connections

	HashKey* h = id.BuildConnKey();
	if ( ! h )
		internal_error("hash computation failed");

	Dictionary* d;

	if ( orig_portv->IsTCP() )
		{
		if ( use_connection_compressor )
			{
			Connection* conn = conn_compressor->Lookup(h);
			delete h;
			return conn;
			}
		else
			d = &tcp_conns;
		}
	else if ( orig_portv->IsUDP() )
		d = &udp_conns;
	else if ( orig_portv->IsICMP() )
		d = &icmp_conns;
	else
		{
		// This can happen due to pseudo-connections we
		// construct, for example for packet headers embedded
		// in ICMPs.
		delete h;
		return 0;
		}

	Connection* conn = (Connection*) d->Lookup(h);

	delete h;

	return conn;
	}

void NetSessions::Remove(Connection* c)
	{
	HashKey* k = c->Key();
	if ( k )
		{
		c->CancelTimers();

		TCP_Analyzer* ta = (TCP_Analyzer*) c->GetRootAnalyzer();
		if ( ta && c->ConnTransport() == TRANSPORT_TCP )
			{
			assert(ta->GetTag() == AnalyzerTag::TCP);
			TCP_Endpoint* to = ta->Orig();
			TCP_Endpoint* tr = ta->Resp();

			tcp_stats.StateLeft(to->state, tr->state);
			}

		if ( c->IsPersistent() )
			persistence_serializer->Unregister(c);

		c->Done();

		if ( connection_state_remove )
			c->Event(connection_state_remove, 0);

		// Zero out c's copy of the key, so that if c has been Ref()'d
		// up, we know on a future call to Remove() that it's no
		// longer in the dictionary.
		c->ClearKey();

		switch ( c->ConnTransport() ) {
		case TRANSPORT_TCP:
			if ( use_connection_compressor &&
			     conn_compressor->Remove(k) )
				// Note, if the Remove() returned false
				// then the compressor doesn't know about
				// this connection, which *should* mean that
				// we never gave it the connection in the
				// first place, and thus we should check
				// the regular TCP table instead.
				;

			else if ( ! tcp_conns.RemoveEntry(k) )
				internal_error(fmt("connection missing"));
			break;

		case TRANSPORT_UDP:
			if ( ! udp_conns.RemoveEntry(k) )
				internal_error("connection missing");
			break;

		case TRANSPORT_ICMP:
			if ( ! icmp_conns.RemoveEntry(k) )
				internal_error("connection missing");
			break;

		case TRANSPORT_UNKNOWN:
			internal_error("unknown transport when removing connection");
			break;
		}

		Unref(c);
		delete k;
		}
	}

void NetSessions::Remove(FragReassembler* f)
	{
	HashKey* k = f->Key();
	if ( ! k )
		internal_error("fragment block not in dictionary");

	if ( ! fragments.RemoveEntry(k) )
		internal_error("fragment block missing");

	Unref(f);
	}

void NetSessions::Insert(Connection* c)
	{
	assert(c->Key());

	Connection* old = 0;

	switch ( c->ConnTransport() ) {
	// Remove first. Otherwise the dictioanry would still
	// reference the old key for already existing connections.

	case TRANSPORT_TCP:
		if ( use_connection_compressor )
			old = conn_compressor->Insert(c);
		else
			{
			old = (Connection*) tcp_conns.Remove(c->Key());
			tcp_conns.Insert(c->Key(), c);
			}
		break;

	case TRANSPORT_UDP:
		old = (Connection*) udp_conns.Remove(c->Key());
		udp_conns.Insert(c->Key(), c);
		break;

	case TRANSPORT_ICMP:
		old = (Connection*) icmp_conns.Remove(c->Key());
		icmp_conns.Insert(c->Key(), c);
		break;

	default:
		internal_error("unknown connection type");
	}

	if ( old && old != c )
		{
		// Some clean-ups similar to those in Remove() (but invisible
		// to the script layer).
		old->CancelTimers();
		if ( old->IsPersistent() )
			persistence_serializer->Unregister(old);
		delete old->Key();
		old->ClearKey();
		Unref(old);
		}
	}

void NetSessions::Drain()
	{
	if ( use_connection_compressor )
		conn_compressor->Drain();

	IterCookie* cookie = tcp_conns.InitForIteration();
	Connection* tc;

	while ( (tc = tcp_conns.NextEntry(cookie)) )
		{
		tc->Done();
		tc->Event(connection_state_remove, 0);
		}

	cookie = udp_conns.InitForIteration();
	Connection* uc;

	while ( (uc = udp_conns.NextEntry(cookie)) )
		{
		uc->Done();
		uc->Event(connection_state_remove, 0);
		}

	cookie = icmp_conns.InitForIteration();
	Connection* ic;

	while ( (ic = icmp_conns.NextEntry(cookie)) )
		{
		ic->Done();
		ic->Event(connection_state_remove, 0);
		}

	ExpireTimerMgrs();
	}

void NetSessions::HeartBeat(double t)
	{
	unsigned int recv = 0;
	unsigned int drop = 0;
	unsigned int link = 0;

	loop_over_list(pkt_srcs, i)
		{
		PktSrc* ps = pkt_srcs[i];

		struct PktSrc::Stats stat;
		ps->Statistics(&stat);
		recv += stat.received;
		drop += stat.dropped;
		link += stat.link;
		}

	val_list* vl = new val_list;

	vl->append(new Val(t, TYPE_TIME));

	RecordVal* ns = new RecordVal(net_stats);
	ns->Assign(0, new Val(recv, TYPE_COUNT));
	ns->Assign(1, new Val(drop, TYPE_COUNT));
	ns->Assign(2, new Val(link, TYPE_COUNT));

	vl->append(ns);

	mgr.QueueEvent(net_stats_update, vl);

	timer_mgr->Add(new NetworkTimer(this, t + heartbeat_interval));
	}

void NetSessions::GetStats(SessionStats& s) const
	{
	s.num_TCP_conns = tcp_conns.Length();
	s.num_UDP_conns = udp_conns.Length();
	s.num_ICMP_conns = icmp_conns.Length();
	s.num_fragments = fragments.Length();
	s.num_packets = num_packets_processed;
	s.num_timers = timer_mgr->Size();
	s.num_events_queued = num_events_queued;
	s.num_events_dispatched = num_events_dispatched;

	s.max_TCP_conns = tcp_conns.MaxLength();
	s.max_UDP_conns = udp_conns.MaxLength();
	s.max_ICMP_conns = icmp_conns.MaxLength();
	s.max_fragments = fragments.MaxLength();
	s.max_timers = timer_mgr->PeakSize();
	}

Connection* NetSessions::NewConn(HashKey* k, double t, const ConnID* id,
					const u_char* data, int proto)
	{
	// FIXME: This should be cleaned up a bit, it's too protocol-specific.
	// But I'm not yet sure what the right abstraction for these things is.
	int src_h = ntohs(id->src_port);
	int dst_h = ntohs(id->dst_port);
	int flags = 0;

	// Hmm... This is not great.
	TransportProto tproto;
	switch ( proto ) {
		case IPPROTO_ICMP:
			tproto = TRANSPORT_ICMP;
			break;
		case IPPROTO_TCP:
			tproto = TRANSPORT_TCP;
			break;
		case IPPROTO_UDP:
			tproto = TRANSPORT_UDP;
			break;
		default:
			internal_error("unknown transport protocol");
			break;
	};

	if ( tproto == TRANSPORT_TCP )
		{
		const struct tcphdr* tp = (const struct tcphdr*) data;
		flags = tp->th_flags;
		}

	bool flip = false;

	if ( ! WantConnection(src_h, dst_h, tproto, flags, flip) )
		return 0;

	if ( flip )
		{
		// Make a guess that we're seeing the tail half of
		// an analyzable connection.
		ConnID flip_id = *id;

		const uint32* ta = flip_id.src_addr;
		flip_id.src_addr = flip_id.dst_addr;
		flip_id.dst_addr = ta;

		uint32 t = flip_id.src_port;
		flip_id.src_port = flip_id.dst_port;
		flip_id.dst_port = t;

		id = &flip_id;
		}

	Connection* conn = new Connection(this, k, t, id);
	conn->SetTransport(tproto);
	dpm->BuildInitialAnalyzerTree(tproto, conn, data);

	bool external = conn->IsExternal();

	if ( external )
		conn->AppendAddl(fmt("tag=%s",
					conn->GetTimerMgr()->GetTag().c_str()));

	// If the connection compressor is active, it takes care of the
	// new_connection/connection_external events for TCP connections.
	if ( new_connection &&
	     (tproto != TRANSPORT_TCP || ! use_connection_compressor) )
		{
		conn->Event(new_connection, 0);

		if ( external )
			{
			val_list* vl = new val_list(2);
			vl->append(conn->BuildConnVal());
			vl->append(new StringVal(conn->GetTimerMgr()->GetTag().c_str()));
			conn->ConnectionEvent(connection_external, 0, vl);
			}
		}

	return conn;
	}

bool NetSessions::IsLikelyServerPort(uint32 port, TransportProto proto) const
	{
	// We keep a cached in-core version of the table to speed up the lookup.
	static set<bro_uint_t> port_cache;
	static bool have_cache = false;

	if ( ! have_cache )
		{
		ListVal* lv = likely_server_ports->ConvertToPureList();
		for ( int i = 0; i < lv->Length(); i++ )
			port_cache.insert(lv->Index(i)->InternalUnsigned());
		have_cache = true;
		Unref(lv);
		}

	// We exploit our knowledge of PortVal's internal storage mechanism
	// here.
	if ( proto == TRANSPORT_TCP )
		port |= TCP_PORT_MASK;
	else if ( proto == TRANSPORT_UDP )
		port |= UDP_PORT_MASK;
	else if ( proto == TRANSPORT_ICMP )
		port |= ICMP_PORT_MASK;

	return port_cache.find(port) != port_cache.end();
	}

bool NetSessions::WantConnection(uint16 src_port, uint16 dst_port,
					TransportProto transport_proto,
					uint8 tcp_flags, bool& flip_roles)
	{
	flip_roles = false;

	if ( transport_proto == TRANSPORT_TCP )
		{
		if ( ! (tcp_flags & TH_SYN) || (tcp_flags & TH_ACK) )
			{
			// The new connection is starting either without a SYN,
			// or with a SYN ack. This means it's a partial connection.
			if ( ! partial_connection_ok )
				return false;

			if ( tcp_flags & TH_SYN && ! tcp_SYN_ack_ok )
				return false;

			// Try to guess true responder by the port numbers.
			// (We might also think that for SYN acks we could
			// safely flip the roles, but that doesn't work
			// for stealth scans.)
			if ( IsLikelyServerPort(src_port, TRANSPORT_TCP) )
				{ // connection is a candidate for flipping
				if ( IsLikelyServerPort(dst_port, TRANSPORT_TCP) )
					// Hmmm, both source and destination
					// are plausible.  Heuristic: flip only
					// if (1) this isn't a SYN ACK (to avoid
					// confusing stealth scans) and
					// (2) dest port > src port (to favor
					// more plausible servers).
					flip_roles = ! (tcp_flags & TH_SYN) && src_port < dst_port;
				else
					// Source is plausible, destination isn't.
					flip_roles = true;
				}
			}
		}

	else if ( transport_proto == TRANSPORT_UDP )
		flip_roles =
			IsLikelyServerPort(src_port, TRANSPORT_UDP) &&
			! IsLikelyServerPort(dst_port, TRANSPORT_UDP);

	return true;
	}

TimerMgr* NetSessions::LookupTimerMgr(const TimerMgr::Tag* tag, bool create)
	{
	if ( ! tag )
		{
		DBG_LOG(DBG_TM, "no tag, using global timer mgr %p", timer_mgr);
		return timer_mgr;
		}

	TimerMgrMap::iterator i = timer_mgrs.find(*tag);
	if ( i != timer_mgrs.end() )
		{
		DBG_LOG(DBG_TM, "tag %s, using non-global timer mgr %p", tag->c_str(), i->second);
		return i->second;
		}
	else
		{
		if ( ! create )
			return 0;

		// Create new queue for tag.
		TimerMgr* mgr = new CQ_TimerMgr(*tag);
		DBG_LOG(DBG_TM, "tag %s, creating new non-global timer mgr %p", tag->c_str(), mgr);
		timer_mgrs.insert(TimerMgrMap::value_type(*tag, mgr));
		double t = timer_mgr->Time() + timer_mgr_inactivity_timeout;
		timer_mgr->Add(new TimerMgrExpireTimer(t, mgr));
		return mgr;
		}
	}

void NetSessions::ExpireTimerMgrs()
	{
	for ( TimerMgrMap::iterator i = timer_mgrs.begin();
	      i != timer_mgrs.end(); ++i )
		{
		i->second->Expire();
		delete i->second;
		}
	}

void NetSessions::DumpPacket(const struct pcap_pkthdr* hdr,
				const u_char* pkt, int len)
	{
	if ( ! pkt_dumper )
		return;

	if ( len == 0 )
		pkt_dumper->Dump(hdr, pkt);
	else
		{
		struct pcap_pkthdr h = *hdr;
		h.caplen = len;
		if ( h.caplen > hdr->caplen )
			internal_error("bad modified caplen");
		pkt_dumper->Dump(&h, pkt);
		}
	}

void NetSessions::Internal(const char* msg, const struct pcap_pkthdr* hdr,
				const u_char* pkt)
	{
	DumpPacket(hdr, pkt);
	internal_error(msg);
	}

void NetSessions::Weird(const char* name,
			const struct pcap_pkthdr* hdr, const u_char* pkt)
	{
	if ( hdr )
		dump_this_packet = 1;

	if ( net_weird )
		{
		val_list* vl = new val_list;
		vl->append(new StringVal(name));
		mgr.QueueEvent(net_weird, vl);
		}
	else
		fprintf(stderr, "weird: %.06f %s\n", network_time, name);
	}

void NetSessions::Weird(const char* name, const IP_Hdr* ip)
	{
	if ( flow_weird )
		{
		val_list* vl = new val_list;
		vl->append(new StringVal(name));
		vl->append(new AddrVal(ip->SrcAddr4()));
		vl->append(new AddrVal(ip->DstAddr4()));
		mgr.QueueEvent(flow_weird, vl);
		}
	else
		fprintf(stderr, "weird: %.06f %s\n", network_time, name);
	}

unsigned int NetSessions::ConnectionMemoryUsage()
	{
	unsigned int mem = 0;

	if ( terminating )
		// Connections have been flushed already.
		return 0;

	IterCookie* cookie = tcp_conns.InitForIteration();
	Connection* tc;

	while ( (tc = tcp_conns.NextEntry(cookie)) )
		mem += tc->MemoryAllocation();

	cookie = udp_conns.InitForIteration();
	Connection* uc;

	while ( (uc = udp_conns.NextEntry(cookie)) )
		mem += uc->MemoryAllocation();

	cookie = icmp_conns.InitForIteration();
	Connection* ic;

	while ( (ic = icmp_conns.NextEntry(cookie)) )
		mem += ic->MemoryAllocation();

	return mem;
	}

unsigned int NetSessions::ConnectionMemoryUsageConnVals()
	{
	unsigned int mem = 0;

	if ( terminating )
		// Connections have been flushed already.
		return 0;

	IterCookie* cookie = tcp_conns.InitForIteration();
	Connection* tc;

	while ( (tc = tcp_conns.NextEntry(cookie)) )
		mem += tc->MemoryAllocationConnVal();

	cookie = udp_conns.InitForIteration();
	Connection* uc;

	while ( (uc = udp_conns.NextEntry(cookie)) )
		mem += uc->MemoryAllocationConnVal();

	cookie = icmp_conns.InitForIteration();
	Connection* ic;

	while ( (ic = icmp_conns.NextEntry(cookie)) )
		mem += ic->MemoryAllocationConnVal();

	return mem;
	}

unsigned int NetSessions::MemoryAllocation()
	{
	return ConnectionMemoryUsage()
		+ padded_sizeof(*this)
		+ ch->MemoryAllocation()
		// must take care we don't count the HaskKeys twice.
		+ tcp_conns.MemoryAllocation() - padded_sizeof(tcp_conns) -
		// 12 is sizeof(Key) from ConnID::BuildConnKey();
		// it can't be (easily) accessed here. :-(
			(tcp_conns.Length() * pad_size(12))
		+ udp_conns.MemoryAllocation() - padded_sizeof(udp_conns) -
			(udp_conns.Length() * pad_size(12))
		+ icmp_conns.MemoryAllocation() - padded_sizeof(icmp_conns) -
			(icmp_conns.Length() * pad_size(12))
		+ fragments.MemoryAllocation() - padded_sizeof(fragments)
		// FIXME: MemoryAllocation() not implemented for rest.
		;
	}
