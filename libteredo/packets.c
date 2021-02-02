/*
 * packets.c - helpers to send Teredo packet from relay/client
 *
 * See "Teredo: Tunneling IPv6 over UDP through NATs"
 * for more information
 */

/***********************************************************************
 *  Copyright © 2004-2007 Rémi Denis-Courmont.                         *
 *  This program is free software; you can redistribute and/or modify  *
 *  it under the terms of the GNU General Public License as published  *
 *  by the Free Software Foundation; version 2 of the license, or (at  *
 *  your option) any later version.                                    *
 *                                                                     *
 *  This program is distributed in the hope that it will be useful,    *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of     *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.               *
 *  See the GNU General Public License for more details.               *
 *                                                                     *
 *  You should have received a copy of the GNU General Public License  *
 *  along with this program; if not, you can get it from:              *
 *  http://www.gnu.org/copyleft/gpl.html                               *
 ***********************************************************************/

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <gettext.h>

#include <string.h>
#include <stdbool.h>
#include <inttypes.h>

#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/ip6.h> /* struct ip6_hdr */
#include <netinet/icmp6.h> /* router solicication */
#include <syslog.h>
#include <sys/uio.h>

#include "teredo.h"
#include "v4global.h" // is_ipv4_global_unicast()
#include "teredo-udp.h"

#include <time.h>
#include "security.h"

#include "packets.h"
#include "checksum.h"


int
teredo_send_bubble (int fd, uint32_t ip, uint16_t port,
                    const struct in6_addr *src, const struct in6_addr *dst)
{
	if (is_ipv4_global_unicast (ip))
	{
		static const uint8_t head[] =
			"\x60\x00\x00\x00" /* flow */
			"\x00\x00" /* plen = 0 */
			"\x3b" /* nxt = IPPROTO_NONE */
			"\x00" /* hlim = 0 */;
		struct iovec iov[3] =
		{
			{ (void *)head, 8 },
			{ (void *)src, 16 },
			{ (void *)dst, 16 }
		};

		return teredo_sendv (fd, iov, 3, ip, port) == 40 ? 0 : -1;
	}

	return 0;
}


int
SendBubbleFromDst (int fd, const struct in6_addr *dst, bool indirect)
{
	uint32_t ip = IN6_TEREDO_IPV4 (dst);
	uint16_t port = IN6_TEREDO_PORT (dst);

	struct in6_addr src;
	memcpy (src.s6_addr, "\xfe\x80\x00\x00\x00\x00\x00\x00", 8);
	/* TODO: use some time information */
	teredo_get_nonce (0, ip, port, src.s6_addr + 8);
	src.s6_addr[8] &= 0xfc; /* Modified EUI-64 */

	if (indirect)
	{
		ip = IN6_TEREDO_SERVER (dst);
		port = htons (IPPORT_TEREDO);
	}

	return teredo_send_bubble (fd, ip, port, &src, dst);
}


int CheckBubble (const teredo_packet *packet)
{
	const struct ip6_hdr *ip6 = packet->ip6;
	const struct in6_addr *me = &ip6->ip6_dst, *it = &ip6->ip6_src;

	uint8_t hash[8];
	/* TODO: use some time information */
	teredo_get_nonce (0, IN6_TEREDO_IPV4 (it), IN6_TEREDO_PORT (it), hash);
	hash[0] &= 0xfc; /* Modified EUI-64: non-global, non-group address */

	return memcmp (hash, me->s6_addr + 8, 8) ? -1 : 0;
}


#ifdef MIREDO_TEREDO_CLIENT
static const struct in6_addr in6addr_allrouters =
{ { { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x2 } } };

int
teredo_send_rs (int fd, uint32_t server_ip,
                const unsigned char *nonce, bool cone)
{
	uint8_t auth[13] = { 0, 1 };
	struct
	{
		struct ip6_hdr ip6;
		struct nd_router_solicit rs;
	} rs;
	struct iovec iov[] =
	{
		{ auth, 13 },
		{ &rs, sizeof (rs) }
	};

	// Authentication header
	// TODO: secure qualification

	memcpy (auth + 4, nonce, 8);

	rs.ip6.ip6_flow = htonl (0x60000000);
	rs.ip6.ip6_plen = htons (sizeof (rs) - sizeof (rs.ip6));
	rs.ip6.ip6_nxt = IPPROTO_ICMPV6;
	rs.ip6.ip6_hlim = 255;
	rs.ip6.ip6_src = cone ? teredo_cone : teredo_restrict;
	rs.ip6.ip6_dst = in6addr_allrouters;

	rs.rs.nd_rs_type = ND_ROUTER_SOLICIT;
	rs.rs.nd_rs_code = 0;
	// Checksums are pre-computed
	rs.rs.nd_rs_cksum = cone ? htons (0x125d) : htons (0x7d37);
	rs.rs.nd_rs_reserved = 0;

	return (teredo_sendv (fd, iov, sizeof (iov) / sizeof (iov[0]),
	                      server_ip, htons (IPPORT_TEREDO)) > 0) ? 0 : -1;
}


int
teredo_parse_ra (const teredo_packet *restrict packet,
                 union teredo_addr *restrict newaddr, bool cone,
                 uint16_t *restrict mtu)
{
	if (packet->orig_ipv4 == 0)
		return -1;

	const struct ip6_hdr *ip6 = packet->ip6;
	size_t length = ntohs (ip6->ip6_plen);

	if (!IN6_ARE_ADDR_EQUAL (&ip6->ip6_dst,
	                         cone ? &teredo_cone : &teredo_restrict)
	 || (ip6->ip6_nxt != IPPROTO_ICMPV6)
	 || (length < sizeof (struct nd_router_advert)))
		return -1;

	// Only read bytes, so no need to align
	const struct nd_router_advert *ra =
		(const struct nd_router_advert *)(ip6 + 1);
	length -= sizeof (*ra);

	if ((ra->nd_ra_type != ND_ROUTER_ADVERT)
	 || (ra->nd_ra_code != 0)
	 || (length < sizeof (struct nd_opt_prefix_info)))
	/*
	 * We don't check checksum, because it is rather useless.
	 * There were already (at least) two lower-level checksums.
	 */
		return -1;

	uint32_t net_mtu = 0;
	newaddr->teredo.server_ip = 0;

	// Looks for a prefix information option
	const struct nd_opt_hdr *hdr;
	for (hdr = (const struct nd_opt_hdr *)(ra + 1); length >= 8;
	     hdr = (const struct nd_opt_hdr *)
	               (((const uint8_t *)hdr) + (hdr->nd_opt_len << 3)))
	{
		size_t optlen = (size_t)(hdr->nd_opt_len << 3);

		if ((length < optlen) /* too short */
		 || (optlen == 0) /* invalid */)
			return -1;

		switch (hdr->nd_opt_type)
		{
		/* Prefix information option */
		 case ND_OPT_PREFIX_INFORMATION:
		 {
			const struct nd_opt_prefix_info *pi = 
				(const struct nd_opt_prefix_info *)hdr;

			if ((optlen < sizeof (*pi)) /* option too short */
			 || (pi->nd_opt_pi_prefix_len != 64) /* unsupp. prefix size */)
				return -1;

			if (newaddr->teredo.server_ip != 0)
			{
				/* The Teredo specification excludes multiple prefixes */
				syslog (LOG_ERR, _("Multiple Teredo prefixes received"));
				return -1;
			}

			memcpy (newaddr, &pi->nd_opt_pi_prefix, 8);
			break;
		 }

		/* MTU option */
		 case ND_OPT_MTU:
		 {
			const struct nd_opt_mtu *mo = (const struct nd_opt_mtu *)hdr;

			/*if (optlen < sizeof (*mo)) -- not possible (optlen >= 8)
				return -1;*/

			net_mtu = ntohl (mo->nd_opt_mtu_mtu);
			if ((net_mtu < 1280) || (net_mtu > 65535))
				return -1; // invalid IPv6 MTU

			break;
		 }
		}

		length -= optlen;
	}

	/*
	 * NOTE: bug-to-bug-to-bug (sic!) compatibility with Microsoft servers.
	 * These severs still advertise the obsolete broken experimental Teredo
	 * prefix. That's obviously for backward (but kinda useless) compatibility
	 * with Windows XP SP1/SP2 clients, that only accept that broken prefix,
	 * unless a dedicated registry key is merged.
	 *
	 * This work-around works because Microsoft servers handles both the
	 * standard and the experimental prefixes.
	 */
	if (newaddr->teredo.prefix == htonl (TEREDO_PREFIX_OBSOLETE))
		newaddr->teredo.prefix = htonl (TEREDO_PREFIX);

	if (!is_valid_teredo_prefix (newaddr->teredo.prefix))
		return -1;

	// only accept the cone flag:
	newaddr->teredo.flags = cone ? htons (TEREDO_FLAG_CONE) : 0;

	newaddr->teredo.client_port = ~packet->orig_port;
	newaddr->teredo.client_ip = ~packet->orig_ipv4;

	if (net_mtu != 0)
		*mtu = (uint16_t)net_mtu;

	return 0;
}

#define PING_PAYLOAD (LIBTEREDO_HMAC_LEN - 4)
int
SendPing (int fd, const union teredo_addr *src, const struct in6_addr *dst)
{
	struct
	{
		struct ip6_hdr ip6;
		struct icmp6_hdr icmp6;
		uint8_t payload[PING_PAYLOAD];
	} ping;

	ping.ip6.ip6_flow = htonl (0x60000000);
	ping.ip6.ip6_plen = htons (sizeof (ping.icmp6) + PING_PAYLOAD);
	ping.ip6.ip6_nxt = IPPROTO_ICMPV6;
	ping.ip6.ip6_hlim = 128;
	ping.ip6.ip6_src = src->ip6;
	ping.ip6.ip6_dst = *dst;
	
	ping.icmp6.icmp6_type = ICMP6_ECHO_REQUEST;
	ping.icmp6.icmp6_code = 0;
	ping.icmp6.icmp6_cksum = 0;
	/*
	ping.icmp6.icmp6_id = 0;
	ping.icmp6.icmp6_seq = 0;
	 */
	teredo_get_pinghash ((uint32_t)time (NULL), &ping.ip6.ip6_src,
	                     &ping.ip6.ip6_dst, (uint8_t *)&ping.icmp6.icmp6_id);

	ping.icmp6.icmp6_cksum = icmp6_checksum (&ping.ip6, &ping.icmp6);

	return teredo_send (fd, &ping, sizeof (ping.ip6) + sizeof (ping.icmp6)
	                    + PING_PAYLOAD, IN6_TEREDO_SERVER (src),
	                    htons (IPPORT_TEREDO)) > 0 ? 0 : -1;
}


int CheckPing (const teredo_packet *packet)
{
	const struct ip6_hdr *ip6 = packet->ip6;
	size_t length = ntohs (ip6->ip6_plen);

	if ((ip6->ip6_nxt != IPPROTO_ICMPV6)
	 || (length < (sizeof (struct icmp6_hdr) + PING_PAYLOAD)))
		return -1;

	const struct icmp6_hdr *icmp6 = (const struct icmp6_hdr *)(ip6 + 1);
	const struct in6_addr *me = &ip6->ip6_dst, *it = &ip6->ip6_src;

	if (icmp6->icmp6_type == ICMP6_DST_UNREACH)
	{
		/*
		 * NOTE:
		 * Some brain-dead IPv6 nodes/firewalls don't reply to pings (which is
		 * as explicit breakage of the IPv6/ICMPv6 specifications, btw). Some
		 * of these brain-dead hosts reply with ICMPv6 unreachable messages.
		 * We can authenticate them by looking at the payload of the message
		 * and see if it is an ICMPv6 Echo request with the matching nonce in
		 * it (Yes, this is a nasty kludge).
		 *
		 * NOTE 2:
		 * We don't check source and destination addresses there...
		 */
		length -= sizeof (*icmp6);
		ip6 = (const struct ip6_hdr *)(icmp6 + 1);

		if ((length < (sizeof (*ip6) + sizeof (*icmp6) + PING_PAYLOAD))
		 || (ip6->ip6_nxt != IPPROTO_ICMPV6))
			return -1;

		length = ntohs (ip6->ip6_plen);
		if (length != (sizeof (*icmp6) + PING_PAYLOAD))
			return -1; // not a ping from us

		icmp6 = (const struct icmp6_hdr *)(ip6 + 1);

		if (!IN6_ARE_ADDR_EQUAL (&ip6->ip6_src, me)
		 || (icmp6->icmp6_type != ICMP6_ECHO_REQUEST))
			return -1;

		/*
		 * The callers wants to check the ping as regards the Unreachable
		 * error packet's source, not as regards the inner Echo Request
		 * packet destination. Sometimes, firewalls send error with their
		 * own address (as a router) as source. In that case, we must not
		 * accept the packet. If we did, the core relaying logic would
		 * believe that we have authenticated the firewall address, while
		 * we really authenticated the inner packet destination address.
		 * In practice, this would potentially be a way to bypass the HMAC
		 * authentication of Teredo relays: once a relays catches one valid
		 * HMAC authentified Echo reply from an IPv6 node, it could fake
		 * error message and spoof any other non-Teredo IPv6 node, by setting
		 * the ICMPv6 Destination Unreachable packet IPv6 source address to
		 * that of a host to be spoofed.
		 *
		 * An alternive to simply dropping these packets would be to implement
		 * a complex interaction between the caller and CheckPing(), so that
		 * CheckPing() could notify the caller that the authenticated IPv6
		 * node is not the packet's source. However, this is probably
		 * overkill, and might not even work properly depending on the cuplrit
		 * firewall rules.
		 */
		if (!IN6_ARE_ADDR_EQUAL (&ip6->ip6_dst, it))
			return -1;

		me = &ip6->ip6_src;
		it = &ip6->ip6_dst;
	}
	else
	if (icmp6->icmp6_type != ICMP6_ECHO_REPLY)
		return -1;

	if (icmp6->icmp6_code != 0)
		return -1;

	return teredo_verify_pinghash ((uint32_t)time (NULL), me, it,
	                               (const uint8_t *)&icmp6->icmp6_id);
	/* TODO: check the sum(?) */
}
#endif


int
BuildICMPv6Error (struct icmp6_hdr *restrict out, uint8_t type, uint8_t code,
                  const struct ip6_hdr *restrict in, size_t inlen)
{
	const struct in6_addr *p;

	/* don't reply if the packet is too small */
	if (inlen < sizeof (*in))
		return 0;

	/* don't reply to ICMPv6 error */
	if ((in->ip6_nxt == IPPROTO_ICMPV6)
	  && ((((const struct icmp6_hdr *)(in + 1))->icmp6_type & 0x80) == 0))
		return 0;

	/* don't reply to multicast */
	if (IN6_IS_ADDR_MULTICAST (&in->ip6_dst))
		return 0;

	p = &in->ip6_src;
	/* don't reply to incorrect source address (multicast, undefined) */
	if (IN6_IS_ADDR_MULTICAST (p) /* multicast */
	 || IN6_IS_ADDR_UNSPECIFIED (p))
		return 0;

	out->icmp6_type = type;
	out->icmp6_code = code;
	out->icmp6_cksum = 0;
	out->icmp6_data32[0] = 0;

	if (inlen > 1280 - (sizeof (struct ip6_hdr) + sizeof (struct icmp6_hdr)))
		inlen = 1280 - (sizeof (struct ip6_hdr) + sizeof (struct icmp6_hdr));

	/* TODO: use iovecs */
	memcpy (out + 1, in, inlen);

	return sizeof (*out) + inlen;
}


#if 0
int
BuildIPv6Error (struct ip6_hdr *out, const struct in6_addr *src,
                uint8_t type, uint8_t code,
                const struct ip6_hdr *in, size_t len)
{
	struct icmp6_hdr *h;

	h = (struct icmp6_hdr *)(out + 1);
	len = BuildICMPv6Error (h, type, code, in, len);
	if (len == 0)
		return 0;

	out->ip6_flow = htonl (0x60000000);
	out->ip6_plen = htons (len);
	out->ip6_nxt = IPPROTO_ICMPV6;
	out->ip6_hlim = 255;
	out->ip6_src = *src;
	out->ip6_dst = in->ip6_src;
	
	len += sizeof (struct ip6_hdr);

	h->icmp6_cksum = icmp6_checksum (out, h);
	return len;
}
#endif
