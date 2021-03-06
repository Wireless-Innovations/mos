#include "net.h"

#include <fs/sockfs/sockfs.h>
#include <fs/vfs.h>
#include <include/errno.h>
#include <include/if_ether.h>
#include <include/sockios.h>
#include <memory/vmm.h>
#include <net/arp.h>
#include <net/ethernet.h>
#include <net/icmp.h>
#include <net/ip.h>
#include <net/neighbour.h>
#include <net/sk_buff.h>
#include <proc/task.h>
#include <utils/printf.h>
#include <utils/string.h>

extern volatile uint32_t scheduler_lock_counter;

struct thread *backup_thread;
struct process *net_process;
struct thread *net_thread;
struct list_head lrx_skb;
struct list_head lsocket;
struct net_device *current_netdev;

// NOTE: MQ 2020-06-04
// network card DMA might add padding at the each packet to make it word align
// -> size might be bigger than its actual size
void push_rx_queue(uint8_t *data, uint32_t size)
{
	struct sk_buff *skb = skb_alloc(0, size);

	skb_put(skb, size);
	memcpy(skb->data, data, size);
	list_add_tail(&skb->sibling, &lrx_skb);
}

void sock_setup(struct socket *sock, int32_t family)
{
	struct sock *sk = sock->sk;
	sk = kcalloc(1, sock->ops->obj_size);
	sk->dev = get_current_net_device();
	sk->owner_thread = current_thread;
	sk->sock = sock;
	INIT_LIST_HEAD(&sk->rx_queue);
	INIT_LIST_HEAD(&sk->tx_queue);

	sock->sk = sk;
}

void socket_setup(int32_t family, enum socket_type type, int32_t protocal, struct vfs_file *file)
{
	struct socket *sock = SOCKET_I(file->f_dentry->d_inode);
	sock->protocol = protocal;
	sock->file = file;
	sock->type = type;
	sock->state = SS_UNCONNECTED;

	if (family == PF_INET)
	{
		if (type == SOCK_STREAM)
			sock->ops = &tcp_proto_ops;
		else if (type == SOCK_DGRAM)
			sock->ops = &udp_proto_ops;
		else if (type == SOCK_RAW)
			sock->ops = &raw_proto_ops;
	}
	else if (family == PF_PACKET)
		sock->ops = &packet_proto_ops;

	list_add_tail(&sock->sibling, &lsocket);
	sock_setup(sock, family);
}

int socket_shutdown(struct socket *sock)
{
	sock->state = SS_DISCONNECTED;
	list_del(&sock->sibling);
	return 0;
}

struct socket *sockfd_lookup(uint32_t sockfd)
{
	struct vfs_file *file = current_process->files->fd[sockfd];
	return SOCKET_I(file->f_dentry->d_inode);
}

uint32_t packet_checksum_start(void *packet, uint16_t size)
{
	uint32_t checksum = 0;

	uint16_t ibytes = size;
	uint16_t *chunk = (uint16_t *)packet;
	while (ibytes > 1)
	{
		checksum += *chunk;

		ibytes -= 2;
		chunk += 1;
	}
	if (ibytes == 1)
		checksum += *(uint8_t *)chunk;

	while (checksum > CHECKSUM_MASK)
		checksum = (checksum & CHECKSUM_MASK) + (checksum >> 16);

	return checksum;
}

uint16_t singular_checksum(void *packet, uint16_t size)
{
	uint32_t checksum = packet_checksum_start(packet, size);
	return ~checksum & CHECKSUM_MASK;
}

uint16_t transport_calculate_checksum(void *segment, uint16_t segment_len, uint8_t protocal, uint32_t source_ip, uint32_t dest_ip)
{
	struct ip4_pseudo_header *ip4_pseudo_header = kcalloc(1, sizeof(struct ip4_pseudo_header));
	ip4_pseudo_header->source_ip = htonl(source_ip);
	ip4_pseudo_header->dest_ip = htonl(dest_ip);
	ip4_pseudo_header->zeros = 0;
	ip4_pseudo_header->protocal = protocal;
	ip4_pseudo_header->transport_length = htons(segment_len);

	uint32_t ip4_checksum_start = packet_checksum_start(ip4_pseudo_header, sizeof(struct ip4_pseudo_header));
	uint32_t udp_checksum_start = packet_checksum_start(segment, segment_len);
	uint32_t checksum = ip4_checksum_start + udp_checksum_start;

	while (checksum > CHECKSUM_MASK)
		checksum = (checksum & CHECKSUM_MASK) + (checksum >> 16);

	kfree(ip4_pseudo_header);
	return ~checksum & CHECKSUM_MASK;
}

void register_net_device(struct net_device *ndev)
{
	current_netdev = ndev;
}

struct net_device *get_current_net_device()
{
	return current_netdev;
}

bool is_broadcast_mac_address(uint8_t *maddr)
{
	if (memcmp(current_netdev->broadcast_addr, maddr, 6) == 0 || memcmp(current_netdev->zero_addr, maddr, 6) == 0)
		return true;
	else
		return false;
}

char *inet_ntop(uint32_t src, char *dst, uint16_t len)
{
	const char fmt[] = "%u.%u.%u.%u";
	char tmp[sizeof "255.255.255.255"];
	uint8_t *sp = (uint8_t *)&src;
	if (sprintf(tmp, fmt, sp[3], sp[2], sp[1], sp[0]) >= len)
		return NULL;

	return strcpy(dst, tmp);
}

int inet_ioctl(struct socket *sock, unsigned int cmd, unsigned long arg)
{
	struct ifreq *ifr = (struct ifreq *)arg;
	struct sockaddr_in *sin = (struct sockaddr_in *)&ifr->ifr_addr;
	struct net_device *dev = get_current_net_device();

	switch (cmd)
	{
	case SIOCGIFADDR:
		sin->sin_addr = dev->local_ip;
		break;

	case SIOCGIFDNSADDR:
		sin->sin_addr = dev->dns_server_ip;
		break;

	default:
		break;
	}

	return 0;
}

// 1. Check icmp request to local ip -> send ICMP reply
// 2. Check arp probe asking our mac address -> send arp reply
// 3. Check arp annoucement -> update neighbour arp
int net_default_rx_handler(struct sk_buff *skb)
{
	int ret = ethernet_rcv(skb);
	if (ret < 0)
		return ret;

	if (skb->mac.eh->type == htons(ETH_P_IP))
	{
		ret = ip4_rcv(skb);
		if (ret < 0)
			return ret;

		if (skb->nh.iph->protocal == IP4_PROTOCAL_ICMP)
		{
			ret = icmp_rcv(skb);
			if (ret < 0)
				return ret;

			if (skb->h.icmph->code == ICMP_ECHO && skb->h.icmph->type == ICMP_REQUEST &&
				skb->nh.iph->dest_ip == htonl(current_netdev->local_ip))
			{
				if (DEBUG)
				{
					char dest_ip_text[sizeof "255.255.255.255"];
					inet_ntop(htonl(skb->nh.iph->dest_ip), dest_ip_text, sizeof(dest_ip_text));
					char source_ip_text[sizeof "255.255.255.255"];
					inet_ntop(htonl(skb->nh.iph->source_ip), source_ip_text, sizeof(source_ip_text));
					debug_println(DEBUG_INFO, "Ping: %s <-> %s", dest_ip_text, source_ip_text);
				}

				uint32_t payload_len = ntohs(skb->nh.iph->total_length) - sizeof(struct ip4_packet) - sizeof(struct icmp_packet);
				icmp_reply(
					current_netdev->local_ip,
					skb->mac.eh->source_mac, ntohl(skb->nh.iph->source_ip),
					ntohl(skb->nh.iph->identification),
					ntohl(skb->h.icmph->un.rest_of_header),
					skb->h.icmph->payload, payload_len);
			}
		}
	}
	else if (skb->mac.eh->type == ntohs(ETH_P_ARP))
	{
		ret = arp_rcv(skb);
		if (ret < 0)
			return ret;

		if (skb->nh.arph->tpa == htonl(current_netdev->local_ip))
		{
			if (DEBUG)
			{
				char dest_ip_text[sizeof "255.255.255.255"];
				inet_ntop(htonl(skb->nh.arph->tpa), dest_ip_text, sizeof(dest_ip_text));
				char source_ip_text[sizeof "255.255.255.255"];
				inet_ntop(htonl(skb->nh.arph->spa), source_ip_text, sizeof(source_ip_text));
				debug_println(DEBUG_INFO,
							  "ARP: %s at %x:%x:%x:%x:%x:%x, tell %s",
							  dest_ip_text,
							  current_netdev->dev_addr[0], current_netdev->dev_addr[1], current_netdev->dev_addr[2], current_netdev->dev_addr[3], current_netdev->dev_addr[4], current_netdev->dev_addr[5],
							  source_ip_text);
			}

			arp_send(current_netdev->dev_addr, current_netdev->local_ip, skb->nh.arph->sha, ntohl(skb->nh.arph->spa), ARP_REPLY);
		}
		else if (skb->nh.arph->tpa == skb->nh.arph->spa && is_broadcast_mac_address(skb->nh.arph->tha))
		{
			if (DEBUG)
			{
				char source_ip_text[sizeof "255.255.255.255"];
				inet_ntop(htonl(skb->nh.arph->spa), source_ip_text, sizeof(source_ip_text));
				debug_println(DEBUG_INFO,
							  "ARP: %s at %x:%x:%x:%x:%x:%x",
							  source_ip_text,
							  current_netdev->dev_addr[0], current_netdev->dev_addr[1], current_netdev->dev_addr[2], current_netdev->dev_addr[3], current_netdev->dev_addr[4], current_netdev->dev_addr[5]);
			}

			neighbour_update_mapping(skb->nh.arph->sha, skb->nh.arph->spa);
		}
	}
	return 0;
}

void net_rx_loop()
{
	// explain in kernel_init#unlock_scheduler
	unlock_scheduler();

	while (true)
	{
		lock_scheduler();

		struct sk_buff *skb;
		struct sk_buff *prev_skb = NULL;
		list_for_each_entry(skb, &lrx_skb, sibling)
		{
			if (prev_skb)
			{
				list_del(&prev_skb->sibling);
				skb_free(prev_skb);
			}

			struct socket *sock;
			list_for_each_entry(sock, &lsocket, sibling)
			{
				struct sk_buff *skb_new = skb_clone(skb);
				int ret = sock->ops->handler(sock, skb_new);
				if (ret < 0)
					skb_free(skb_new);
			}
			if (current_netdev->state & NETDEV_STATE_CONNECTED)
				net_default_rx_handler(skb);

			prev_skb = skb;
		}
		if (prev_skb)
		{
			list_del(&prev_skb->sibling);
			skb_free(prev_skb);
		}

		update_thread(net_thread, THREAD_WAITING);
		unlock_scheduler();
		schedule();
	}
}

void net_switch()
{
	if (list_empty(&lrx_skb))
		return;

	if (net_thread != current_thread)
	{
		update_thread(net_thread, THREAD_READY);
		if (current_thread->state == THREAD_RUNNING)
			update_thread(current_thread, THREAD_READY);
		schedule();
	}
	else
		update_thread(net_thread, THREAD_READY);
}

void net_init()
{
	INIT_LIST_HEAD(&lsocket);
	INIT_LIST_HEAD(&lrx_skb);

	DEBUG &&debug_println(DEBUG_INFO, "Net: Setup neighbour");
	neighbour_init();

	DEBUG &&debug_println(DEBUG_INFO, "Net: Setup net process");
	net_process = create_system_process("net", net_rx_loop, 0);
	net_thread = net_process->thread;
}
