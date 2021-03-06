/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2010-2016 Intel Corporation. All rights reserved.
 *   Copyright(c) 2016-2019 National Institute of Advanced Industrial
 *                Science and Technology. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/* vim: set noexpandtab ai: */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/queue.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

/*
 * RTE_LIBRTE_RING_DEBUG generates statistics of ring buffers. However, SEGV is occurred. (v16.07）
 * #define RTE_LIBRTE_RING_DEBUG
 */
#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_memzone.h>
#include <rte_eal.h>
#include <rte_per_lcore.h>
#include <rte_launch.h>
#include <rte_atomic.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_interrupts.h>
#include <rte_pci.h>
#include <rte_random.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_ring.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_errno.h>
#include <rte_timer.h>
#include <rte_string_fns.h>

static int64_t loss_random(const char *loss_rate);
static int64_t loss_random_a(double loss_rate);
static bool loss_event(void);
static bool loss_event_random(uint64_t loss_rate);
static bool loss_event_GE(uint64_t loss_rate_n, uint64_t loss_rate_a, uint64_t st_ch_rate_no2ab, uint64_t st_ch_rate_ab2no);
static bool loss_event_4state( uint64_t p13, uint64_t p14, uint64_t p23, uint64_t p31, uint64_t p32);
static bool dup_event(void);
#define RANDOM_MAX 1000000000

static volatile bool force_quit;

#define RTE_LOGTYPE_DEMU RTE_LOGTYPE_USER1

/*
 * Configurable number of RX/TX ring descriptors
 */
#define RTE_TEST_RX_DESC_DEFAULT 128
#define RTE_TEST_TX_DESC_DEFAULT 512
static uint16_t nb_rxd = RTE_TEST_RX_DESC_DEFAULT;
static uint16_t nb_txd = RTE_TEST_TX_DESC_DEFAULT;

/* ethernet addresses of ports */
static struct ether_addr demu_ports_eth_addr[RTE_MAX_ETHPORTS];

struct rte_mempool * demu_pktmbuf_pool = NULL;

static uint32_t demu_enabled_port_mask = 0;

/* Per-port statistics struct */
struct demu_port_statistics {
	uint64_t tx;
	uint64_t rx;
	uint64_t dropped;
	uint64_t rx_worker_dropped;
	uint64_t worker_tx_dropped;
	uint64_t queue_dropped;
	uint64_t discarded;
} __rte_cache_aligned;
struct demu_port_statistics port_statistics[RTE_MAX_ETHPORTS];

/*
 * The maximum number of packets which are processed in burst.
 * Note: do not set PKT_BURST_RX to 1.
 */
#define PKT_BURST_RX 32768
#define PKT_BURST_TX 32768
#define PKT_BURST_WORKER 32768

/*
 * The default mempool size is not enough for bufferijng 64KB of short packets for 1 second.
 * SHORT_PACKET should be enabled in the case of short packet benchmarking.
 * #define SHORT_PACKET
 */
#ifdef SHORT_PACKET
#define DEMU_DELAYED_BUFFER_PKTS 8388608
#define MEMPOOL_BUF_SIZE 1152
#else
#define DEMU_DELAYED_BUFFER_PKTS 4194304
#define MEMPOOL_BUF_SIZE RTE_MBUF_DEFAULT_BUF_SIZE /* 2048 */
#endif

#define MEMPOOL_CACHE_SIZE 512
#define DEMU_SEND_BUFFER_SIZE_PKTS 512


struct port_t {
	uint8_t portid;
	uint64_t delayed_time;
	struct rte_ring *rx_to_workers;
	struct rte_ring *workers_to_tx;
	struct rte_ring *workers_to_tx_other;
};


enum thread_type_t {
	RX = 0,
	TX,
	WORKER
};

struct port_t ports[RTE_MAX_ETHPORTS];
uint8_t nb_lcores;
uint8_t nb_ports;

enum demu_loss_mode {
	LOSS_MODE_NONE,
	LOSS_MODE_RANDOM,
	LOSS_MODE_GE,
	LOSS_MODE_4STATE,
};
static enum demu_loss_mode loss_mode = LOSS_MODE_NONE;

static uint64_t loss_percent_1 = 0;
static uint64_t loss_percent_2 = 0;
// static uint64_t change_percent_1 = 0;
// static uint64_t change_percent_2 = 0;

static uint64_t dup_rate = 0;

static const struct rte_eth_conf port_conf = {
	.rxmode = {
		.split_hdr_size = 0,
		.header_split   = 0, /**< Header Split disabled */
		.hw_ip_checksum = 0, /**< IP checksum offload disabled */
		.hw_vlan_filter = 0, /**< VLAN filtering disabled */
		.jumbo_frame    = 0, /**< Jumbo Frame Support disabled */
		.hw_strip_crc   = 0, /**< CRC stripped by hardware */
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
};

static struct rte_eth_rxconf rx_conf = {
	.rx_thresh = {                    /**< RX ring threshold registers. */
		.pthresh = 8,             /**< Ring prefetch threshold. */
		.hthresh = 8,             /**< Ring host threshold. */
		.wthresh = 0,             /**< Ring writeback threshold. */
	},
	.rx_free_thresh = 32,             /**< Drives the freeing of RX descriptors. */
	.rx_drop_en = 0,                  /**< Drop packets if no descriptors are available. */
	.rx_deferred_start = 0,           /**< Do not start queue with rte_eth_dev_start(). */
};

static struct rte_eth_txconf tx_conf = {
	.tx_thresh = {                    /**< TX ring threshold registers. */
		.pthresh = 32,
		.hthresh = 0,
		.wthresh = 0,
	},
	.tx_rs_thresh = 32,               /**< Drives the setting of RS bit on TXDs. */
	.tx_free_thresh = 32,             /**< Start freeing TX buffers if there are less free descriptors than this value. */
	.txq_flags = (ETH_TXQ_FLAGS_NOMULTSEGS |
			ETH_TXQ_FLAGS_NOVLANOFFL |
			ETH_TXQ_FLAGS_NOXSUMSCTP |
			ETH_TXQ_FLAGS_NOXSUMUDP |
			ETH_TXQ_FLAGS_NOXSUMTCP),
	.tx_deferred_start = 0,            /**< Do not start queue with rte_eth_dev_start(). */
};

/* #define DEBUG */
/* #define DEBUG_RX */
/* #define DEBUG_TX */

#ifdef DEBUG_RX
#define RX_STAT_BUF_SIZE 3000000
double rx_stat[RX_STAT_BUF_SIZE] = {0};
uint64_t rx_cnt = 0;
#endif

#ifdef DEBUG_TX
#define TX_STAT_BUF_SIZE 3000000
double tx_stat[TX_STAT_BUF_SIZE] = {0};
uint64_t tx_cnt = 0;
#endif


static inline void
pktmbuf_free_bulk(struct rte_mbuf *mbuf_table[], unsigned n)
{
	unsigned int i;

	for (i = 0; i < n; i++)
		rte_pktmbuf_free(mbuf_table[i]);
}

static uint64_t amount_token = 0;
static uint64_t limit_speed = 0;
static uint64_t sub_amount_token = 0;

static void
tx_timer_cb(__attribute__((unused)) struct rte_timer *tmpTime, __attribute__((unused)) void *arg)
{
	double upper_limit_speed = limit_speed * 1.2;
	if (amount_token >= (uint64_t)upper_limit_speed)
		return;

	if (limit_speed >= 1000000)
		amount_token += (limit_speed / 1000000);
	else {
		sub_amount_token += limit_speed;
		if (sub_amount_token > 1000000) {
			amount_token += sub_amount_token / 1000000;
			sub_amount_token %= 1000000;
		}
	}
}

static void
demu_timer_loop(void)
{
	unsigned lcore_id;
	uint64_t hz;
	struct rte_timer timer;

	lcore_id = rte_lcore_id();
	hz = rte_get_timer_hz();

	rte_timer_init(&timer);
	rte_timer_reset(&timer, hz / 1000000, PERIODICAL, lcore_id, tx_timer_cb, NULL);

	RTE_LOG(INFO, DEMU, "Entering timer loop on lcore %u\n", lcore_id);
	RTE_LOG(INFO, DEMU, "  Linit speed is %lu bps\n", limit_speed);

	while (!force_quit)
		rte_timer_manage();
}

static void
demu_tx_loop(struct port_t port)
{
	struct rte_mbuf *send_buf[PKT_BURST_TX];
	unsigned lcore_id;
	uint32_t numdeq = 0;
	uint16_t sent;
	uint16_t pkt_size_bit;
	uint32_t num_send = 0;
	uint16_t prevent_discard = 0;

	lcore_id = rte_lcore_id();

	RTE_LOG(INFO, DEMU, "Entering main tx loop on lcore %u portid %u\n", lcore_id, port.portid);

	while (!force_quit) {
		numdeq = rte_ring_sc_dequeue_burst(port.workers_to_tx,
				(void *)(send_buf + prevent_discard), PKT_BURST_TX, NULL);

		if (unlikely(numdeq == 0))
			continue;

		if (limit_speed) {
			num_send = 0;
			for (uint32_t j = 0; j < numdeq + prevent_discard; j++) {
				pkt_size_bit = send_buf[j]->pkt_len * 8;
				if (amount_token >= pkt_size_bit) {
					amount_token -= pkt_size_bit;
					num_send++;
				} else break;
			}
			rte_prefetch0(rte_pktmbuf_mtod(send_buf[0], void *));
			sent = rte_eth_tx_burst(port.portid, 0, send_buf, num_send);

			if (prevent_discard < PKT_BURST_TX) {
				prevent_discard = numdeq + prevent_discard - num_send;
			}
		} else {
			rte_prefetch0(rte_pktmbuf_mtod(send_buf[0], void *));
			sent = 0;
			while (numdeq > sent)
				sent += rte_eth_tx_burst(port.portid, 0, send_buf + sent, numdeq - sent);
		}

#ifdef DEBUG_TX
		if (tx_cnt < TX_STAT_BUF_SIZE) {
			for (uint32_t i = 0; i < numdeq; i++) {
				tx_stat[tx_cnt] = rte_rdtsc();
				tx_cnt++;
			}
		}
#endif
		if (limit_speed) {
			if (prevent_discard >= (uint16_t)(PKT_BURST_TX * 0.8)) {
				pktmbuf_free_bulk(&send_buf[sent], numdeq - sent);
				prevent_discard = 0;
			}
		}
#ifdef DEBUG
		else {
			// printf("tx:%u %u\n", numdeq, sent);
			port_statistics[port.portid].tx += sent;
			port_statistics[port.portid].dropped += (numdeq - sent);
		}
#endif
	}
}

static void
demu_rx_loop(struct port_t port)
{
	struct rte_mbuf *pkts_burst[PKT_BURST_RX], *rx2w_buffer[PKT_BURST_RX];
	unsigned lcore_id;

	unsigned nb_rx, i;
	unsigned nb_loss;
	unsigned nb_dup;
	uint32_t numenq;

	lcore_id = rte_lcore_id();

	RTE_LOG(INFO, DEMU, "Entering main rx loop on lcore %u portid %u\n", lcore_id, port.portid);

	while (!force_quit) {
		nb_rx = rte_eth_rx_burst((uint8_t) port.portid, 0,
				pkts_burst, PKT_BURST_RX);

		if (likely(nb_rx == 0))
			continue;

#ifdef DEBUG
		port_statistics[port.portid].rx += nb_rx;
#endif
		nb_loss = 0;
		nb_dup = 0;
		for (i = 0; i < nb_rx; i++) {
			struct rte_mbuf *clone;

			if (loss_event()) {
				port_statistics[port.portid].discarded++;
				nb_loss++;
				continue;
			}

			rx2w_buffer[i - nb_loss + nb_dup] = pkts_burst[i];
			rte_prefetch0(rte_pktmbuf_mtod(rx2w_buffer[i - nb_loss + nb_dup], void *));
			rx2w_buffer[i - nb_loss + nb_dup]->udata64 = rte_rdtsc();

			/* FIXME: we do not check the buffer overrun of rx2w_buffer. */
			if (dup_event()) {
				clone = rte_pktmbuf_clone(rx2w_buffer[i - nb_loss + nb_dup], demu_pktmbuf_pool);
				if (clone == NULL)
					RTE_LOG(ERR, DEMU, "cannot clone a packet\n");
				nb_dup++;
				rx2w_buffer[i - nb_loss + nb_dup] = clone;
			}

#ifdef DEBUG_RX
			if (rx_cnt < RX_STAT_BUF_SIZE) {
				rx_stat[rx_cnt] = rte_rdtsc();
				rx_cnt++;
			}
#endif
		}

		numenq = rte_ring_sp_enqueue_burst(port.rx_to_workers,
					(void *)rx2w_buffer, nb_rx - nb_loss + nb_dup, NULL);


		if (unlikely(numenq < (unsigned)(nb_rx - nb_loss + nb_dup))) {
#ifdef DEBUG
			port_statistics[port.portid].rx_worker_dropped += (nb_rx - nb_loss + nb_dup - numenq);
			printf("Delayed Queue Overflow count:%" PRIu64 "\n",
					port_statistics[port.portid].queue_dropped);
#endif
			pktmbuf_free_bulk(&pkts_burst[numenq], nb_rx - nb_loss + nb_dup - numenq);
		}
	}
}

static void
worker_thread(struct port_t port)
{
	uint16_t burst_size = 0;
	struct rte_mbuf *burst_buffer[PKT_BURST_WORKER];
	uint64_t diff_tsc;
	int i;
	unsigned lcore_id;

	lcore_id = rte_lcore_id();
	RTE_LOG(INFO, DEMU, "Entering main worker on lcore %u\n", lcore_id);
	i = 0;

	while (!force_quit) {
			burst_size = rte_ring_sc_dequeue_burst(port.rx_to_workers,
					(void *)burst_buffer, PKT_BURST_WORKER, NULL);
		if (unlikely(burst_size == 0))
			continue;
		rte_prefetch0(rte_pktmbuf_mtod(burst_buffer[0], void *));
		i = 0;
		while (i != burst_size) {
			diff_tsc = rte_rdtsc() - burst_buffer[i]->udata64;
			if (diff_tsc >= port.delayed_time) {
				rte_prefetch0(rte_pktmbuf_mtod(burst_buffer[i], void *));
				rte_ring_sp_enqueue(port.workers_to_tx_other, burst_buffer[i]);
				i++;
			}
		}
	}
}

static int
demu_launch_one_lcore(__attribute__((unused)) void *dummy)
{
	unsigned lcore_id, lcore_idx;
	lcore_id = rte_lcore_id();
	lcore_idx = rte_lcore_index(lcore_id);

	/* each port uses 3 lcores */
	uint8_t port_idx = lcore_idx/3;
	enum thread_type_t thread_type = lcore_idx%3;

	/* last lcore is for timer_loop */
	if (lcore_idx + 1 == nb_lcores) {
		if (limit_speed) demu_timer_loop();
	}

	else if (thread_type == RX) {
		demu_tx_loop(ports[port_idx]);
	}

	else if (thread_type == TX) {
		worker_thread(ports[port_idx]);
	}

	else if (thread_type == WORKER) {
		demu_rx_loop(ports[port_idx]);
	}

	if (force_quit)
		return 0;

	return 0;
}

/* display usage */
static void
demu_usage(const char *prgname)
{
	printf("%s [EAL options] -- "
		" -P (portid,portid,delayed_us)[,(portid,portid,delayed_us)]: link to add effect.\n"
		"                                       required argument.\n"
		" -p PORTMASK: HEXADECIMAL bitmask of ports to configure\n"
		" -r random packet loss %% (default is 0%%)\n"
		" -g XXX\n"
		" -s bandwidth limitation [bps]\n"
		" -D duplicate packet rate\n",
		prgname);
}

static int
demu_parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	/* parse hexadecimal string */
	pm = strtoul(portmask, &end, 16);
	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0'))
		return -1;

	return pm;
}

static int
demu_parse_port_pairs(const char *q_arg)
{
	char s[256];
	const char *p, *p0 = q_arg;
	char *end;
	enum fieldnames {
		FLD_PORT = 0,
		FLD_PORT_OTHER,
		FLD_DELAYED_TIME,
		_NUM_FLD
	};
	unsigned long int_fld[_NUM_FLD];
	char *str_fld[_NUM_FLD];
	int i;
	unsigned size;

	nb_ports = 0;

	while ((p = strchr(p0,'(')) != NULL) {
		++p;
		if((p0 = strchr(p,')')) == NULL)
			return -1;

		size = p0 - p;
		if(size >= sizeof(s))
			return -1;

		snprintf(s, sizeof(s), "%.*s", size, p);
		if (rte_strsplit(s, sizeof(s), str_fld, _NUM_FLD, ',') != _NUM_FLD)
			return -1;

		for (i = 0; i < _NUM_FLD; i++){
			errno = 0;
			int_fld[i] = strtoul(str_fld[i], &end, 0);
			if (end == str_fld[i])
				return -1;
		}

		if (nb_ports > RTE_MAX_ETHPORTS) {
			printf("exceeded max number of ports: %hu\n",
				nb_ports);
			return -1;
		}

		uint64_t delayed_time = \
			((rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S) * \
			int_fld[FLD_DELAYED_TIME];

		ports[nb_ports].portid = (uint8_t)int_fld[FLD_PORT];
		ports[nb_ports].delayed_time = delayed_time;
		++nb_ports;

		ports[nb_ports].portid = (uint8_t)int_fld[FLD_PORT_OTHER];
		ports[nb_ports].delayed_time = delayed_time;
		++nb_ports;
	}

	return 0;
}

static int64_t
demu_parse_speed(const char *arg)
{
	int64_t speed, base;
	char *end = NULL;

	speed = strtoul(arg, &end, 10);
	if (end != NULL) {
		char unit = *end;

		switch (unit) {
			case 'k':
			case 'K':
				base = 1000;
				break;
			case 'm':
			case 'M':
				base = 1000 * 1000;
				break;
			case 'g':
			case 'G':
				if (speed > 10) return -1;
				base = 1000 * 1000 * 1000;
				break;
			default:
				return -1;
		}
		end++;
	}

	if (arg[0] == '\0' || end == NULL || *end != '\0') {
		return -1;
	}

	speed = speed * base;
	if (speed < 1000 && 10000000000 < speed)
		return -1;

	return speed;
}

/* Parse the argument given in the command line of the application */
static int
demu_parse_args(int argc, char **argv)
{
	int opt, ret;
	char **argvopt;
	char *prgname = argv[0];
	const struct option longopts[] = {
		{0, 0, 0, 0}
	};
	int longindex = 0;
	int64_t val;

	argvopt = argv;

	while ((opt = getopt_long(argc, argvopt, "g:p:P:r:s:D:h:",
					longopts, &longindex)) != EOF) {

		switch (opt) {
			/* portmask */
			case 'p':
				demu_enabled_port_mask = demu_parse_portmask(optarg);
				if (demu_enabled_port_mask <= 0) {
					printf("Invalid value: portmask\n");
					demu_usage(prgname);
					return -1;
				}
				break;

			case 'P':
				val = demu_parse_port_pairs(optarg);
				if (val < 0) {
					printf("Invalid value: port pairs \n");
					demu_usage(prgname);
					return -1;
				}
				break;

			/* random packet loss */
			case 'r':
				val = loss_random(optarg);
				if (val < 0) {
					printf("Invalid value: loss rate\n");
					demu_usage(prgname);
					return -1;
				}
				loss_percent_1 = val;
				loss_mode = LOSS_MODE_RANDOM;
				break;

			case 'g':
				val = loss_random(optarg);
				if (val < 0) {
					printf("Invalid value: loss rate\n");
					demu_usage(prgname);
					return -1;
				}
				loss_percent_2 = val;
				loss_mode = LOSS_MODE_GE;
				break;

			/* duplicate packet */
			case 'D':
				val = loss_random(optarg);
				if (val < 0) {
					printf("Invalid value: loss rate\n");
					demu_usage(prgname);
					return -1;
				}
				dup_rate = val;
				break;

			/* bandwidth limitation */
			case 's':
				val = demu_parse_speed(optarg);
				if (val < 0) {
					RTE_LOG(ERR, DEMU, "Invalid value: speed\n");
					return -1;
				}
				limit_speed = val;
				break;

			/* long options */
			case 0:
				demu_usage(prgname);
				return -1;

			default:
				demu_usage(prgname);
				return -1;
		}
	}

	if (nb_ports == 0) {
		RTE_LOG(ERR, DEMU, "Option -P must be specified\n");
		return -1;
	}

	if (optind >= 0)
		argv[optind-1] = prgname;

	ret = optind-1;
	optind = 0; /* reset getopt lib */
	return ret;
}

static void
check_all_ports_link_status(uint8_t port_num, uint32_t port_mask)
{
#define CHECK_INTERVAL 100 /* 100ms */
#define MAX_CHECK_TIME 90 /* 9s (90 * 100ms) in total */
	uint8_t portid, count, all_ports_up, print_flag = 0;
	struct rte_eth_link link;

	RTE_LOG(INFO, DEMU, "Checking link status\n");
	for (count = 0; count <= MAX_CHECK_TIME; count++) {
		if (force_quit)
			return;
		all_ports_up = 1;
		for (portid = 0; portid < port_num; portid++) {
			if (force_quit)
				return;
			if ((port_mask & (1 << portid)) == 0)
				continue;
			memset(&link, 0, sizeof(link));
			rte_eth_link_get_nowait(portid, &link);
			/* print link status if flag set */
			if (print_flag == 1) {
				if (link.link_status)
					RTE_LOG(INFO, DEMU, "  Port %d Link Up - speed %u "
						"Mbps - %s\n", (uint8_t)portid,
						(unsigned)link.link_speed,
						(link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
						("full-duplex") : ("half-duplex\n"));
				else
					RTE_LOG(INFO, DEMU, "  Port %d Link Down\n",
						(uint8_t)portid);
				continue;
			}
			/* clear all_ports_up flag if any link down */
			if (link.link_status == ETH_LINK_DOWN) {
				all_ports_up = 0;
				break;
			}
		}
		/* after finally printing all link status, get out */
		if (print_flag == 1)
			break;

		if (all_ports_up == 0)
			rte_delay_ms(CHECK_INTERVAL);

		/* set the print_flag if all ports up or timeout */
		if (all_ports_up == 1 || count == (MAX_CHECK_TIME - 1))
			print_flag = 1;
	}
}

static void
signal_handler(int signum)
{
	if (signum == SIGINT || signum == SIGTERM) {
		RTE_LOG(NOTICE, DEMU, "Signal %d received, preparing to exit...\n",
				signum);
		force_quit = true;
	}
}

int
main(int argc, char **argv)
{
	int ret;
	unsigned lcore_id;

	/* init EAL */
	ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid EAL arguments\n");
	argc -= ret;
	argv += ret;

	force_quit = false;
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);

	/* parse application arguments (after the EAL ones) */
	ret = demu_parse_args(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "Invalid DEMU arguments\n");

	nb_lcores = rte_lcore_count();
	uint8_t nb_lcores_required = nb_ports*3 + 1;
	if (nb_lcores != nb_lcores_required)
		rte_exit(EXIT_FAILURE, " %d lcores, %d ports.\n"
				"The number of lcores should be %d (1 + 3*NUMBER_OF_PORTS).\n",
				nb_lcores, nb_ports, nb_lcores_required);

	/* create the mbuf pool */
	demu_pktmbuf_pool = rte_pktmbuf_pool_create("mbuf_pool",
			DEMU_DELAYED_BUFFER_PKTS + DEMU_DELAYED_BUFFER_PKTS + DEMU_SEND_BUFFER_SIZE_PKTS + DEMU_SEND_BUFFER_SIZE_PKTS,
			MEMPOOL_CACHE_SIZE, 0, MEMPOOL_BUF_SIZE,
			rte_socket_id());

	if (demu_pktmbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot init mbuf pool\n");

	/* Initialise each port */
	for (int i = 0; i < nb_ports; i++) {
		/* init port */
		uint8_t portid = ports[i].portid;

		RTE_LOG(INFO, DEMU, "Initializing port %u\n", (unsigned) portid);
		ret = rte_eth_dev_configure(portid, 1, 1, &port_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "Cannot configure device: err=%d, port=%u\n",
					ret, (unsigned) portid);

		rte_eth_macaddr_get(portid,&demu_ports_eth_addr[portid]);

		/* init one RX queue */
		ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
				rte_eth_dev_socket_id(portid),
				&rx_conf,
				demu_pktmbuf_pool);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup:err=%d, port=%u\n",
					ret, (unsigned) portid);

		/* init one TX queue on each port */
		ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
				rte_eth_dev_socket_id(portid),
				&tx_conf);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup:err=%d, port=%u\n",
					ret, (unsigned) portid);

		/* Start device */
		ret = rte_eth_dev_start(portid);
		if (ret < 0)
			rte_exit(EXIT_FAILURE, "rte_eth_dev_start:err=%d, port=%u\n",
					ret, (unsigned) portid);

		rte_eth_promiscuous_enable(portid);

		RTE_LOG(INFO, DEMU, "  Port %u, MAC address: %02X:%02X:%02X:%02X:%02X:%02X\n",
			(unsigned) portid,
			demu_ports_eth_addr[portid].addr_bytes[0],
			demu_ports_eth_addr[portid].addr_bytes[1],
			demu_ports_eth_addr[portid].addr_bytes[2],
			demu_ports_eth_addr[portid].addr_bytes[3],
			demu_ports_eth_addr[portid].addr_bytes[4],
			demu_ports_eth_addr[portid].addr_bytes[5]);

		/* initialize port stats */
		memset(&port_statistics, 0, sizeof(port_statistics));

	}

	check_all_ports_link_status(nb_ports, demu_enabled_port_mask);

	char ring_name[20];
	for (int i = 0; i < nb_ports; i++) {
		sprintf(ring_name, "rx_to_workers_%d", i);
		ports[i].rx_to_workers = rte_ring_create(ring_name, DEMU_DELAYED_BUFFER_PKTS,
			rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (ports[i].rx_to_workers == NULL)
			rte_exit(EXIT_FAILURE, "%s\n", rte_strerror(rte_errno));

		sprintf(ring_name, "workers_to_tx_%d", i);
		ports[i].workers_to_tx = rte_ring_create(ring_name, DEMU_SEND_BUFFER_SIZE_PKTS,
				rte_socket_id(), RING_F_SP_ENQ | RING_F_SC_DEQ);
		if (ports[i].workers_to_tx == NULL)
			rte_exit(EXIT_FAILURE, "%s\n", rte_strerror(rte_errno));
	}

	for (int i = 0; i < nb_ports; i += 2) {
		ports[i+1].workers_to_tx_other = ports[i].workers_to_tx;
		ports[i].workers_to_tx_other = ports[i+1].workers_to_tx;
	}

	ret = 0;
	/* launch per-lcore init on every lcore */
	rte_eal_mp_remote_launch(demu_launch_one_lcore, NULL, CALL_MASTER);
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
			ret = -1;
			break;
		}
	}

	for (int i = 0; i < nb_ports; i++) {
		uint8_t portid = ports[i].portid;
		/* if ((demu_enabled_port_mask & (1 << portid)) == 0) */
		/*  continue; */
		RTE_LOG(INFO, DEMU, "Closing port %d\n", portid);
		rte_eth_dev_stop(portid);
		rte_eth_dev_close(portid);

#ifdef DEBUG
		// saketa
		printf("Stats[%d]. TX %8" PRIu64 " RX %8" PRIu64 " rx-workDrop %" PRIu64 " work-txDrop %" PRIu64 " TXdropped %" PRIu64 "\n",
			portid,
			port_statistics[portid].tx,
			port_statistics[portid].rx,
			port_statistics[portid].rx_worker_dropped,
			port_statistics[portid].worker_tx_dropped,
			port_statistics[portid].dropped);
#endif
	}




#if defined(DEBUG_RX) || defined(DEBUG_TX)
	time_t timer;
	struct tm *timeptr;
	timer = time(NULL);
	timeptr = localtime(&timer);
#endif
#ifdef DEBUG_RX
	char filename1[64] = {'\0'};
	strftime(filename1, 64, "/home/aketa/result/rxtime%m%d%H%M%S", timeptr);
	FILE *rxoutput;
	if ((rxoutput = fopen(filename1, "a+")) == NULL) {
		printf("file open error!!\n");
		exit(EXIT_FAILURE);
	}
	for (uint64_t i = 0; i < rx_cnt - 1 ; i++) {
		fprintf(rxoutput, "%lf\n", rx_stat[i]);
	}
	fclose(rxoutput);
#endif
#ifdef DEBUG_TX
	char filename2[64] = {'\0'};
	strftime(filename2, 64, "/home/aketa/result/txtime%m%d%H%M%S", timeptr);
	FILE *txoutput;
	if ((txoutput = fopen(filename2, "a+")) == NULL) {
		printf("file open error!!\n");
		exit(EXIT_FAILURE);
	}
	for (uint64_t i = 0; i < tx_cnt - 1 ; i++) {
		fprintf(txoutput, "%lf\n", tx_stat[i]);
	}
	fclose(txoutput);
#endif


	/* rte_ring_dump(stdout, rx_to_workers); */
	/* rte_ring_dump(stdout, workers_to_tx); */

	RTE_LOG(INFO, DEMU, "Bye...\n");

	return ret;
}

static int64_t
loss_random_a(double loss_rate)
{
	double percent;
	uint64_t percent_u64;

	percent = loss_rate;
	percent *= RANDOM_MAX / 100;
	percent_u64 = (uint64_t)percent;

	return percent_u64;
}

static int64_t
loss_random(const char *loss_rate)
{
	double percent;
	uint64_t percent_u64;

	if (sscanf(loss_rate, "%lf", &percent) == 0)
		return -1;
	percent *= RANDOM_MAX / 100;
	percent_u64 = (uint64_t)percent;

	return percent_u64;
}

static bool
loss_event(void)
{
	bool lost = false;

	switch (loss_mode) {
	case LOSS_MODE_NONE:
		break;

	case LOSS_MODE_RANDOM:
		if (unlikely(loss_event_random(loss_percent_1) == true))
			lost = true;
		break;

	case LOSS_MODE_GE:
		if (unlikely(loss_event_GE(loss_random_a(0), loss_random_a(100),
			loss_percent_1, loss_percent_2) == true))
			lost = true;
		break;

	case LOSS_MODE_4STATE: /* FIX IT */
		if (unlikely(loss_event_4state(loss_random_a(100), loss_random_a(0),
			loss_random_a(100), loss_random_a(0), loss_random_a(1)) == true))
			lost = true;
		break;
	}

	return lost;
}

static bool
loss_event_random(uint64_t loss_rate)
{
	bool flag = false;
	uint64_t temp;

	temp = rte_rand() % (RANDOM_MAX + 1);
	if (loss_rate >= temp)
		flag = true;

	return flag;
}

/*
 * Gilbert Elliott loss model
 * 0: S_NOR (normal state, low loss ratio)
 * 1: S_ABN (abnormal state, high loss ratio)
 */
static bool
loss_event_GE(uint64_t loss_rate_n, uint64_t loss_rate_a, uint64_t st_ch_rate_no2ab, uint64_t st_ch_rate_ab2no)
{
#define S_NOR 0
#define S_ABN 1
	static bool state = S_NOR;
	uint64_t rnd_loss, rnd_tran;
	uint64_t loss_rate, state_ch_rate;
	bool flag = false;

	if (state == S_NOR) {
		loss_rate = loss_rate_n;
		state_ch_rate = st_ch_rate_no2ab;
	} else { // S_ABN
		loss_rate = loss_rate_a;
		state_ch_rate = st_ch_rate_ab2no;
	}

	rnd_loss = rte_rand() % (RANDOM_MAX + 1);
	if (rnd_loss < loss_rate) {
		flag = true;
	}

	rnd_tran = rte_rand() % (RANDOM_MAX + 1);
	if (rnd_tran < state_ch_rate) {
		state = !state;
	}

	return flag;
}

/*
 * Four-state Markov model
 * State 1 - Packet is received successfully in gap period
 * State 2 - Packet is received within a burst period
 * State 3 - Packet is lost within a burst period
 * State 4 - Isolated packet lost within a gap period
 * p13 is the probability of state change from state1 to state3.
 * https://www.gatesair.com/documents/papers/Parikh-K130115-Network-Modeling-Revised-02-05-2015.pdf
 */
static bool
loss_event_4state( uint64_t p13, uint64_t p14, uint64_t p23, uint64_t p31, uint64_t p32)
{
	static char state = 1;
	bool flag = false;
	uint64_t rnd = rte_rand() % (RANDOM_MAX + 1);

	switch (state) {
	case 1:
		if (rnd < p13) {
			state = 3;
		} else if (rnd < p13 + p14) {
			state = 4;
		}
		break;

	case 2:
		if (rnd < p23) {
			state = 3;
		}
		break;

	case 3:
		if (rnd < p31) {
			state = 1;
		} else if (rnd < p31 + p32) {
			state = 2;
		}
		break;

	case 4:
		state = 1;
		break;
	}

	if (state == 2 || state == 4) {
		flag = true;
	}

	return flag;
}

static bool
dup_event(void)
{
	if (unlikely(loss_event_random(dup_rate) == true))
		return true;
	else
		return false;
}
