/*
 *	Linux NET3:	EOIP over IP protocol decoder.
 *
 *	Based on the code from ip_gre.c
 *
 *	Authors: Alexey Kuznetsov (kuznet@ms2.inr.ac.ru)
 *	Authors: Boian Bonev (bbonev@ipacct.com)
 *
 *	This program is free software; you can redistribute it and/or
 *	modify it under the terms of the GNU General Public License
 *	as published by the Free Software Foundation; either version
 *	2 of the License, or (at your option) any later version.
 *
 */

#include <linux/capability.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/skbuff.h>
#include <linux/netdevice.h>
#include <linux/in.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/if_arp.h>
#include <linux/mroute.h>
#include <linux/init.h>
#include <linux/in6.h>
#include <linux/inetdevice.h>
#include <linux/igmp.h>
#include <linux/netfilter_ipv4.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>

#include <net/sock.h>
#include <net/ip.h>
#include <net/icmp.h>
#include <net/protocol.h>
#include <net/arp.h>
#include <net/checksum.h>
#include <net/dsfield.h>
#include <net/xfrm.h>
#include <net/net_namespace.h>
#include <net/netns/generic.h>
#include <net/rtnetlink.h>
#include <net/gre.h>

#if defined(CONFIG_IPV6) || defined(CONFIG_IPV6_MODULE)
#include <net/ipv6.h>
#include <net/ip6_fib.h>
#include <net/ip6_route.h>
#endif

static struct rtnl_link_ops eoip_ops __read_mostly;
static int eoip_tunnel_bind_dev(struct net_device *dev);
static void eoip_setup(struct net_device *dev);

/* Fallback tunnel: no source, no destination, no key, no options */

#define EOIP_HASH_SIZE 16

static int eoip_net_id __read_mostly;
struct eoip_net {
	struct ip_tunnel __rcu *tunnels[EOIP_HASH_SIZE];
};

/* Tunnel hash table */

#define HASH(addr) (((__force u32)addr^((__force u32)addr>>4))&0xF)

/*
 * Locking : hash tables are protected by RCU and RTNL
 */

/* Given src, dst and key, find appropriate for input tunnel. */

static struct ip_tunnel *eoip_tunnel_lookup(struct net_device *dev,
						__be32 remote, __be32 local,
						__be32 key)
{
	struct net *net = dev_net(dev);
	int link = dev->ifindex;
	unsigned int h0 = HASH(key);
	struct ip_tunnel *t, *cand = NULL;
	struct eoip_net *ign = net_generic(net, eoip_net_id);
	int dev_type = ARPHRD_ETHER;
	int score, cand_score = 4;

	for_each_ip_tunnel_rcu(t, ign->tunnels[h0]) {
		if (local != t->parms.iph.saddr ||
				remote != t->parms.iph.daddr ||
				key != t->parms.i_key ||
				!(t->dev->flags & IFF_UP))
			continue;

		score = 0;
		if (t->parms.link != link)
			score |= 1;
		if (t->dev->type != dev_type)
			score |= 2;
		if (score == 0)
			return t;

		if (score < cand_score) {
			cand = t;
			cand_score = score;
		}
	}

	if (cand != NULL)
		return cand;

	return NULL;
}

static struct ip_tunnel __rcu **__eoip_bucket(struct eoip_net *ign,
		struct ip_tunnel_parm_kern *parms)
{
	__be32 key = parms->i_key;
	unsigned int h = HASH(key);

	return &ign->tunnels[h];
}

static inline struct ip_tunnel __rcu **eoip_bucket(struct eoip_net *ign,
		struct ip_tunnel *t)
{
	return __eoip_bucket(ign, &t->parms);
}

static void eoip_tunnel_link(struct eoip_net *ign, struct ip_tunnel *t)
{
	struct ip_tunnel __rcu **tp = eoip_bucket(ign, t);

	rcu_assign_pointer(t->next, rtnl_dereference(*tp));
	rcu_assign_pointer(*tp, t);
}

static void eoip_tunnel_unlink(struct eoip_net *ign, struct ip_tunnel *t)
{
	struct ip_tunnel __rcu **tp;
	struct ip_tunnel *iter;

	for (tp = eoip_bucket(ign, t);
			(iter = rtnl_dereference(*tp)) != NULL;
			tp = &iter->next) {
		if (t == iter) {
			rcu_assign_pointer(*tp, t->next);
			break;
		}
	}
}

static struct ip_tunnel *eoip_tunnel_find(struct net *net,
						struct ip_tunnel_parm_kern *parms,
						int type)
{
	__be32 remote = parms->iph.daddr;
	__be32 local = parms->iph.saddr;
	__be32 key = parms->i_key;
	int link = parms->link;
	struct ip_tunnel *t;
	struct ip_tunnel __rcu **tp;
	struct eoip_net *ign = net_generic(net, eoip_net_id);

	for (tp = __eoip_bucket(ign, parms);
			(t = rtnl_dereference(*tp)) != NULL;
			tp = &t->next)
		if (local == t->parms.iph.saddr &&
			remote == t->parms.iph.daddr &&
			key == t->parms.i_key &&
			link == t->parms.link &&
			type == t->dev->type)
			break;

	return t;
}

static struct ip_tunnel *eoip_tunnel_locate(struct net *net,
		struct ip_tunnel_parm_kern *parms, int create)
{
	struct ip_tunnel *t, *nt;
	struct net_device *dev;
	char name[IFNAMSIZ];
	struct eoip_net *ign = net_generic(net, eoip_net_id);

	t = eoip_tunnel_find(net, parms, ARPHRD_ETHER);
	if (t || !create)
		return t;

	if (parms->name[0])
		strscpy(name, parms->name, IFNAMSIZ);
	else
		strcpy(name, "eoip%d");

	dev = alloc_netdev(sizeof(*t), name, NET_NAME_UNKNOWN, eoip_setup);
	if (!dev)
		return NULL;

	dev_net_set(dev, net);

	nt = netdev_priv(dev);
	nt->parms = *parms;
	dev->rtnl_link_ops = &eoip_ops;

	dev->mtu = eoip_tunnel_bind_dev(dev);

	if (register_netdevice(dev) < 0)
		goto failed_free;

	dev_hold(dev);
	eoip_tunnel_link(ign, nt);
	return nt;

failed_free:
	free_netdev(dev);
	return NULL;
}

static void eoip_if_uninit(struct net_device *dev)
{
	struct net *net = dev_net(dev);
	struct eoip_net *ign = net_generic(net, eoip_net_id);

	eoip_tunnel_unlink(ign, netdev_priv(dev));
	dev_put(dev);
}


static void eoip_err(struct sk_buff *skb, u32 info)
{

/* All the routers (except for Linux) return only
   8 bytes of packet payload. It means, that precise relaying of
   ICMP in the real Internet is absolutely infeasible.

   Moreover, Cisco "wise men" put GRE key to the third word
   in GRE header. It makes impossible maintaining even soft state for keyed
   GRE tunnels with enabled checksum. Tell them "thank you".

   Well, I wonder, rfc1812 was written by Cisco employee,
   what the hell these idiots break standrads established
   by themself???
 */

	const struct iphdr *iph = (const struct iphdr *)skb->data;
	__be16 *p = (__be16 *)(skb->data+(iph->ihl<<2));
	int grehlen = (iph->ihl<<2) + 4;
	const int type = icmp_hdr(skb)->type;
	const int code = icmp_hdr(skb)->code;
	struct ip_tunnel *t;
	__be16 flags;

	flags = p[0];
	if (flags&(GRE_CSUM|GRE_KEY|GRE_SEQ|GRE_ROUTING|GRE_VERSION)) {
		if (flags&(GRE_VERSION|GRE_ROUTING))
			return;
		if (flags&GRE_KEY) {
			grehlen += 4;
			if (flags&GRE_CSUM)
				grehlen += 4;
		}
	}

	/* If only 8 bytes returned, keyed message will be dropped here */
	if (skb_headlen(skb) < grehlen)
		return;

	switch (type) {
	default:
	case ICMP_PARAMETERPROB:
		return;

	case ICMP_DEST_UNREACH:
		switch (code) {
		case ICMP_SR_FAILED:
		case ICMP_PORT_UNREACH:
			/* Impossible event. */
			return;
		case ICMP_FRAG_NEEDED:
			/* Soft state for pmtu is maintained by IP core. */
			return;
		default:
			/* All others are translated to HOST_UNREACH.
			   rfc2003 contains "deep thoughts" about NET_UNREACH,
			   I believe they are just ether pollution. --ANK
			 */
			break;
		}
		break;
	case ICMP_TIME_EXCEEDED:
		if (code != ICMP_EXC_TTL)
			return;
		break;
	}

	rcu_read_lock();
	t = eoip_tunnel_lookup(skb->dev, iph->daddr, iph->saddr,
				flags & GRE_KEY ?
				*(((__be32 *)p) + (grehlen / 4) - 1) : 0);
	if (t == NULL || t->parms.iph.daddr == 0 ||
			ipv4_is_multicast(t->parms.iph.daddr))
		goto out;

	if (t->parms.iph.ttl == 0 && type == ICMP_TIME_EXCEEDED)
		goto out;

	if (time_before(jiffies, t->err_time + IPTUNNEL_ERR_TIMEO))
		t->err_count++;
	else
		t->err_count = 1;
	t->err_time = jiffies;
out:
	rcu_read_unlock();
}

static int eoip_rcv(struct sk_buff *skb)
{
	const struct iphdr *iph;
	u8 *h;
	__be16 flags;
	__be32 key = 0;
	struct ip_tunnel *tunnel;
	int offset = 8;

	/* ip header (20 bytes) + eoip header (8 bytes) */
	if (!pskb_may_pull(skb, 28))
		goto drop_nolock;

	iph = ip_hdr(skb);
	h = skb->data;
	flags = *(__be16 *)h;

	key = le16_to_cpu(*(__le16 *)(h + 6));

	rcu_read_lock();

	tunnel = eoip_tunnel_lookup(skb->dev, iph->saddr, iph->daddr, key);
	if (tunnel) {
		secpath_reset(skb);

		skb->protocol = ARPHRD_ETHER;

		skb->mac_header = skb->network_header;
		pskb_pull(skb, offset);
		skb_postpull_rcsum(skb, skb_transport_header(skb), offset);
		skb->pkt_type = PACKET_HOST;
#ifdef CONFIG_NET_IPGRE_BROADCAST
		if (ipv4_is_multicast(iph->daddr)) {
			/* Looped back packet, drop it! */
			if (rt_is_output_route(skb_rtable(skb)))
				goto drop;
			tunnel->dev->stats.multicast++;
			skb->pkt_type = PACKET_BROADCAST;
		}
#endif

		/* Warning: All skb pointers will be invalidated! */
		if (!pskb_may_pull(skb, ETH_HLEN)) {
			tunnel->dev->stats.rx_length_errors++;
			tunnel->dev->stats.rx_errors++;
			goto drop;
		}

		iph = ip_hdr(skb);
		skb->protocol = eth_type_trans(skb, tunnel->dev);
		skb_postpull_rcsum(skb, eth_hdr(skb), ETH_HLEN);

		dev_sw_netstats_rx_add(tunnel->dev, skb->len);

		__skb_tunnel_rx(skb, tunnel->dev, dev_net(tunnel->dev));

		skb_reset_network_header(skb);

		netif_rx(skb);

		rcu_read_unlock();
		return 0;
	}
	icmp_send(skb, ICMP_DEST_UNREACH, ICMP_PORT_UNREACH, 0);

drop:
	rcu_read_unlock();
drop_nolock:
	kfree_skb(skb);
	return 0;
}

static netdev_tx_t eoip_if_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct ip_tunnel *tunnel = netdev_priv(dev);
	struct pcpu_sw_netstats *tstats;
	const struct iphdr *old_iph = ip_hdr(skb);
	const struct iphdr *tiph;
	struct flowi4 fl4;
	u8 tos;
	struct rtable *rt;			/* Route to the other host */
	struct net_device *tdev;	/* Device to other host */
	struct iphdr *iph;			/* Our new IP header */
	unsigned int max_headroom;	/* The extra header space needed */
	int gre_hlen;
	__be32 dst;
	int mtu;
	uint16_t frame_size;

	IPCB(skb)->flags = 0;

	gre_hlen = tunnel->hlen;
	tiph = &tunnel->parms.iph;
	dst = tiph->daddr;

	tos = tiph->tos;

	frame_size = skb->len;

	rt = ip_route_output_gre(dev_net(dev), &fl4, dst, tiph->saddr,
			tunnel->parms.o_key, RT_TOS(tos),
			tunnel->parms.link);
	if (IS_ERR(rt)) {
		dev->stats.tx_carrier_errors++;
		goto tx_error;
	}
	tdev = rt->dst.dev;

	if (tdev == dev) {
		ip_rt_put(rt);
		dev->stats.collisions++;
		goto tx_error;
	}

	mtu = skb_dst(skb) ? dst_mtu(skb_dst(skb)) : dev->mtu;

	if (skb_dst(skb))
		skb_dst(skb)->ops->update_pmtu(skb_dst(skb), NULL, skb, mtu, true);

	if (tunnel->err_count > 0) {
		if (time_before(jiffies,
				tunnel->err_time + IPTUNNEL_ERR_TIMEO)) {
			tunnel->err_count--;

			dst_link_failure(skb);
		} else
			tunnel->err_count = 0;
	}

	max_headroom = LL_RESERVED_SPACE(tdev) + gre_hlen + rt->dst.header_len;

	if (skb_headroom(skb) < max_headroom || skb_shared(skb) ||
			(skb_cloned(skb) && !skb_clone_writable(skb, 0))) {
		struct sk_buff *new_skb;

		new_skb = skb_realloc_headroom(skb, max_headroom);
		if (!new_skb) {
			ip_rt_put(rt);
			dev->stats.tx_dropped++;
			dev_kfree_skb(skb);
			return NETDEV_TX_OK;
		}
		if (skb->sk)
			skb_set_owner_w(new_skb, skb->sk);
		dev_kfree_skb(skb);
		skb = new_skb;
		old_iph = ip_hdr(skb);
	}

	skb_reset_transport_header(skb);
	skb_push(skb, gre_hlen);
	skb_reset_network_header(skb);
	memset(&(IPCB(skb)->opt), 0, sizeof(IPCB(skb)->opt));
	IPCB(skb)->flags &= ~(IPSKB_XFRM_TUNNEL_SIZE | IPSKB_XFRM_TRANSFORMED |
			IPSKB_REROUTED);
	skb_dst_drop(skb);
	skb_dst_set(skb, &rt->dst);

	/*
	 *	Push down and install the EOIP header.
	 */

	iph = ip_hdr(skb);
	iph->version = 4;
	iph->ihl = sizeof(struct iphdr) >> 2;
	iph->frag_off = 0;
	iph->protocol = IPPROTO_GRE;
	iph->tos = tos;
	iph->daddr = fl4.daddr;
	iph->saddr = fl4.saddr;
	iph->ttl = tiph->ttl;

	if (iph->ttl == 0)
		iph->ttl = ip4_dst_hoplimit(&rt->dst);

	((u8 *)(iph + 1))[0] = 0x20;
	((u8 *)(iph + 1))[1] = 0x01;
	((u8 *)(iph + 1))[2] = 0x64;
	((u8 *)(iph + 1))[3] = 0x00;

	((__be16 *)(iph + 1))[2] = htons(frame_size);
	((__le16 *)(iph + 1))[3] = cpu_to_le16(tunnel->parms.i_key);

	nf_reset_ct(skb);
	tstats = this_cpu_ptr(dev->tstats);
	{
		int err;
		int pkt_len = skb->len - skb_transport_offset(skb);

		skb->ip_summed = CHECKSUM_NONE;
		ip_select_ident(dev_net(skb->dev), skb, NULL);

		err = ip_local_out(dev_net(skb->dev), NULL, skb);
		if (likely(net_xmit_eval(err) == 0))
			iptunnel_xmit_stats(dev, pkt_len);
		else
			iptunnel_xmit_stats(dev, -1);
	}
	return NETDEV_TX_OK;

tx_error:
	dev->stats.tx_errors++;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static int eoip_tunnel_bind_dev(struct net_device *dev)
{
	struct net_device *tdev = NULL;
	struct ip_tunnel *tunnel;
	const struct iphdr *iph;
	int hlen = LL_MAX_HEADER;
	int mtu = ETH_DATA_LEN;
	int addend = sizeof(struct iphdr) + 8;

	tunnel = netdev_priv(dev);
	iph = &tunnel->parms.iph;

	/* Guess output device to choose reasonable mtu and needed_headroom */

	if (iph->daddr) {
		struct flowi4 fl4;
		struct rtable *rt;

		rt = ip_route_output_gre(dev_net(dev), &fl4,
				iph->daddr, iph->saddr,
				tunnel->parms.o_key,
				RT_TOS(iph->tos),
				tunnel->parms.link);
		if (!IS_ERR(rt)) {
			tdev = rt->dst.dev;
			ip_rt_put(rt);
		}
	}

	if (!tdev && tunnel->parms.link)
		tdev = __dev_get_by_index(dev_net(dev), tunnel->parms.link);

	if (tdev) {
		hlen = tdev->hard_header_len + tdev->needed_headroom;
		mtu = tdev->mtu;
	}
	dev->ifindex = tunnel->parms.link;

	dev->needed_headroom = addend + hlen;
	mtu -= dev->hard_header_len + addend;

	if (mtu < 68)
		mtu = 68;

	tunnel->hlen = addend;

	return mtu;
}

static int eoip_if_change_mtu(struct net_device *dev, int new_mtu)
{
	struct ip_tunnel *tunnel = netdev_priv(dev);
	if (new_mtu < 68 ||
			new_mtu > 0xFFF8 - dev->hard_header_len - tunnel->hlen)
		return -EINVAL;
	dev->mtu = new_mtu;
	return 0;
}

static void eoip_dev_free(struct net_device *dev)
{
	free_percpu(dev->tstats);
	free_netdev(dev);
}

static const struct gre_protocol eoip_protocol = {
	.handler = eoip_rcv,
	.err_handler = eoip_err,
};

static void eoip_destroy_tunnels(struct eoip_net *ign, struct list_head *head)
{
	int h;
	for (h = 0; h < EOIP_HASH_SIZE; h++) {
		struct ip_tunnel *t;

		t = rtnl_dereference(ign->tunnels[h]);

		while (t != NULL) {
			unregister_netdevice_queue(t->dev, head);
			t = rtnl_dereference(t->next);
		}
	}
}

static int __net_init eoip_init_net(struct net *net)
{
	return 0;
}

static void __net_exit eoip_exit_net(struct net *net)
{
	struct eoip_net *ign;
	LIST_HEAD(list);

	ign = net_generic(net, eoip_net_id);
	rtnl_lock();
	eoip_destroy_tunnels(ign, &list);
	unregister_netdevice_many(&list);
	rtnl_unlock();
}

static struct pernet_operations eoip_net_ops = {
	.id = &eoip_net_id,
	.init = eoip_init_net,
	.exit = eoip_exit_net,
	.size = sizeof(struct eoip_net),
};

static int eoip_tunnel_validate(struct nlattr *tb[], struct nlattr *data[])
{
	return 0;
}

static int eoip_validate(struct nlattr *tb[], struct nlattr *data[], struct netlink_ext_ack *extack)
{
	__be32 daddr;

	if (tb[IFLA_ADDRESS]) {
		if (nla_len(tb[IFLA_ADDRESS]) != ETH_ALEN)
			return -EINVAL;
		if (!is_valid_ether_addr(nla_data(tb[IFLA_ADDRESS])))
			return -EADDRNOTAVAIL;
	}

	if (!data)
		goto out;

	if (data[IFLA_GRE_REMOTE]) {
		memcpy(&daddr, nla_data(data[IFLA_GRE_REMOTE]), 4);
		if (!daddr)
			return -EINVAL;
	}

out:
	return eoip_tunnel_validate(tb, data);
}

static void eoip_netlink_parms(struct nlattr *data[],
				struct ip_tunnel_parm_kern *parms)
{
	memset(parms, 0, sizeof(*parms));

	parms->iph.protocol = IPPROTO_GRE;

	if (!data)
		return;

	if (data[IFLA_GRE_LINK])
		parms->link = nla_get_u32(data[IFLA_GRE_LINK]);

	if (data[IFLA_GRE_IKEY])
		parms->i_key = nla_get_be32(data[IFLA_GRE_IKEY]);

	if (data[IFLA_GRE_LOCAL])
		parms->iph.saddr = nla_get_be32(data[IFLA_GRE_LOCAL]);

	if (data[IFLA_GRE_REMOTE])
		parms->iph.daddr = nla_get_be32(data[IFLA_GRE_REMOTE]);

	if (data[IFLA_GRE_TTL])
		parms->iph.ttl = nla_get_u8(data[IFLA_GRE_TTL]);

	if (data[IFLA_GRE_TOS])
		parms->iph.tos = nla_get_u8(data[IFLA_GRE_TOS]);
}

static int eoip_if_init(struct net_device *dev)
{
	struct ip_tunnel *tunnel;

	tunnel = netdev_priv(dev);

	tunnel->dev = dev;
	strcpy(tunnel->parms.name, dev->name);

	eoip_tunnel_bind_dev(dev);

	dev->tstats = alloc_percpu(struct pcpu_sw_netstats);
	if (!dev->tstats)
		return -ENOMEM;

	return 0;
}

static const struct net_device_ops eoip_netdev_ops = {
	.ndo_init = eoip_if_init,
	.ndo_uninit = eoip_if_uninit,
	.ndo_start_xmit = eoip_if_xmit,
	.ndo_set_mac_address = eth_mac_addr,
	.ndo_validate_addr = eth_validate_addr,
	.ndo_change_mtu = eoip_if_change_mtu,
	.ndo_get_stats64 = dev_get_tstats64, // eoip_if_get_stats,
};

static void eoip_setup(struct net_device *dev)
{
	ether_setup(dev);

	dev->netdev_ops = &eoip_netdev_ops;
	dev->priv_destructor = eoip_dev_free;

	dev->ifindex = 0;
	dev->netns_local = true;
}

static int eoip_newlink(struct net *src_net, struct net_device *dev,
			struct nlattr *tb[], struct nlattr *data[], struct netlink_ext_ack *extack)
{
	struct ip_tunnel *nt;
	struct net *net = dev_net(dev);
	struct eoip_net *ign = net_generic(net, eoip_net_id);
	int mtu;
	int err;

	nt = netdev_priv(dev);
	eoip_netlink_parms(data, &nt->parms);

	if (eoip_tunnel_find(net, &nt->parms, dev->type))
		return -EEXIST;

	if (!tb[IFLA_ADDRESS]) {
		uint8_t addr[ETH_ALEN];

		eth_random_addr(addr);
		dev_addr_set(dev,addr);
	}

	mtu = eoip_tunnel_bind_dev(dev);

	/* Can use a lockless transmit, we do not generate output sequences */
	dev->lltx = true;

	err = register_netdevice(dev);
	if (err)
		goto out;

	dev_hold(dev);
	eoip_tunnel_link(ign, nt);

out:
	return err;
}

static int eoip_changelink(struct net_device *dev, struct nlattr *tb[],
					struct nlattr *data[], struct netlink_ext_ack *extack)
{
	struct ip_tunnel *t, *nt;
	struct net *net = dev_net(dev);
	struct eoip_net *ign = net_generic(net, eoip_net_id);
	struct ip_tunnel_parm_kern p;
	int mtu;

	nt = netdev_priv(dev);
	eoip_netlink_parms(data, &p);

	t = eoip_tunnel_locate(net, &p, 0);

	if (t) {
		if (t->dev != dev)
			return -EEXIST;
	} else {
		t = nt;

		eoip_tunnel_unlink(ign, t);
		t->parms.iph.saddr = p.iph.saddr;
		t->parms.iph.daddr = p.iph.daddr;
		t->parms.i_key = p.i_key;
		eoip_tunnel_link(ign, t);
		netdev_state_change(dev);
	}

	t->parms.iph.ttl = p.iph.ttl;
	t->parms.iph.tos = p.iph.tos;
	t->parms.iph.frag_off = p.iph.frag_off;

	if (t->parms.link != p.link) {
		t->parms.link = p.link;
		mtu = eoip_tunnel_bind_dev(dev);
		netdev_state_change(dev);
	}

	return 0;
}

static size_t eoip_get_size(const struct net_device *dev)
{
	return
		/* IFLA_GRE_LINK */
		nla_total_size(4) +
		/* IFLA_GRE_IKEY */
		nla_total_size(4) +
		/* IFLA_GRE_LOCAL */
		nla_total_size(4) +
		/* IFLA_GRE_REMOTE */
		nla_total_size(4) +
		/* IFLA_GRE_TTL */
		nla_total_size(1) +
		/* IFLA_GRE_TOS */
		nla_total_size(1) +
		0;
}

static int eoip_fill_info(struct sk_buff *skb, const struct net_device *dev)
{
	struct ip_tunnel *t = netdev_priv(dev);
	struct ip_tunnel_parm_kern *p = &t->parms;

	if (nla_put_u32(skb, IFLA_GRE_LINK, p->link) ||
		nla_put_be32(skb, IFLA_GRE_IKEY, p->i_key) ||
		nla_put_be32(skb, IFLA_GRE_LOCAL, p->iph.saddr) ||
		nla_put_be32(skb, IFLA_GRE_REMOTE, p->iph.daddr) ||
		nla_put_u8(skb, IFLA_GRE_TTL, p->iph.ttl) ||
		nla_put_u8(skb, IFLA_GRE_TOS, p->iph.tos))
		goto nla_put_failure;

	return 0;

nla_put_failure:
	return -EMSGSIZE;
}

static const struct nla_policy eoip_policy[IFLA_GRE_MAX + 1] = {
	[IFLA_GRE_LINK]		= { .type = NLA_U32 },
	[IFLA_GRE_IKEY]		= { .type = NLA_U32 },
	[IFLA_GRE_LOCAL]	= { .len = sizeof_field(struct iphdr, saddr) },
	[IFLA_GRE_REMOTE]	= { .len = sizeof_field(struct iphdr, daddr) },
	[IFLA_GRE_TTL]		= { .type = NLA_U8 },
	[IFLA_GRE_TOS]		= { .type = NLA_U8 },
};

static struct rtnl_link_ops eoip_ops __read_mostly = {
	.kind		= "eoip",
	.maxtype	= IFLA_GRE_MAX,
	.policy		= eoip_policy,
	.priv_size	= sizeof(struct ip_tunnel),
	.setup		= eoip_setup,
	.validate	= eoip_validate,
	.newlink	= eoip_newlink,
	.changelink	= eoip_changelink,
	.get_size	= eoip_get_size,
	.fill_info	= eoip_fill_info,
};

/*
 *	And now the modules code and kernel interface.
 */

static int __init eoip_init(void)
{
	int err;

	printk(KERN_INFO "EoIP (IPv4) tunneling driver\n");

	err = register_pernet_device(&eoip_net_ops);
	if (err < 0)
		return err;

	err = gre_add_protocol(&eoip_protocol, GREPROTO_NONSTD_EOIP);
	if (err < 0) {
		printk(KERN_INFO "eoip init: can't add protocol\n");
		goto add_proto_failed;
	}

	err = rtnl_link_register(&eoip_ops);
	if (err < 0)
		goto eoip_ops_failed;

out:
	return err;

eoip_ops_failed:
	gre_del_protocol(&eoip_protocol, GREPROTO_NONSTD_EOIP);
add_proto_failed:
	unregister_pernet_device(&eoip_net_ops);
	goto out;
}

static void __exit eoip_fini(void)
{
	rtnl_link_unregister(&eoip_ops);
	if (gre_del_protocol(&eoip_protocol, GREPROTO_NONSTD_EOIP) < 0)
		printk(KERN_INFO "eoip close: can't remove protocol\n");
	unregister_pernet_device(&eoip_net_ops);
}

module_init(eoip_init);
module_exit(eoip_fini);
MODULE_LICENSE("GPL");
MODULE_ALIAS_RTNL_LINK("eoip");
