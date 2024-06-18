// credit: https://github.com/snake0/krdma/blob/nopost/krdma.h

#ifndef __KVM_X86_KRDMA_H
#define __KVM_X86_KRDMA_H
/*
 * Copyright (C) 2019, Trusted Cloud Group, Shanghai Jiao Tong University.
 *
 * Authors:
 *   Yubin Chen <binsschen@sjtu.edu.cn>
 *   Jin Zhang <jzhang3002@sjtu.edu.cn>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */
#include <linux/pci.h>
#include <linux/list.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>

/* Error Macro*/
#define rdma_error(msg, args...) do {\
	printk(KERN_ERR "%s : %d : ERROR : "msg, __FILE__, __LINE__, ## args);\
}while(0);

#define KCLIENT_DEBUG

#ifdef KCLIENT_DEBUG 
/* Debug Macro */
#define debug(msg, args...) do {\
    printk(KERN_INFO "DEBUG: "msg, ## args);\
}while(0);

#else 

#define debug(msg, args...) 

#endif /* KCLIENT_DEBUG */

#define RDMA_RESOLVE_TIMEOUT 2000
#define RDMA_CONNECT_RETRY_MAX 3

#define RDMA_SEND_QUEUE_DEPTH 1
#define RDMA_RECV_QUEUE_DEPTH 32
#define RDMA_CQ_QUEUE_DEPTH (RDMA_SEND_QUEUE_DEPTH + RDMA_RECV_QUEUE_DEPTH)

#define RDMA_RDWR_BUF_LEN (PAGE_SIZE * 1)

struct krdma_server_info {
	uint64_t dma_addr;
	uint32_t size;
	uint32_t rkey;
};

// simulating sending server client's secret
struct krdma_client_info {
	uint64_t token1;
	uint32_t size;
	uint32_t token2;
};

/* control block that supports both RDMA send/recv and read/write */
struct krdma_cb {
	enum {
		KRDMA_INIT = 0,
		KRDMA_ADDR_RESOLVED,
		KRDMA_ROUTE_RESOLVED,
		KRDMA_CONNECTED,
		KRDMA_FLUSHING,
		KRDMA_CLOSING,
		KRDMA_CLOSED,
		KRDMA_SEND_DONE,
		KRDMA_RECV_DONE,
		KRDMA_WRITE_COMPLETE,
		KRDMA_READ_COMPLETE,
		KRDMA_ERROR,
		KRDMA_CONNECT_REJECTED,
		KRDMA_DISCONNECTED,
	} state;

	/* Communication Manager id */
	struct rdma_cm_id *cm_id;
	// struct sockaddr_in server_sockaddr;

	/* Completion Queue */
	struct ib_cq *send_cq;
	struct ib_cq *recv_cq;
	/* Protection Domain */
	struct ib_pd *pd;
	/* Queue Pair */
	struct ib_qp *qp;

	// for RDMA SEND/RECV operations
	struct krdma_client_info send_buf __aligned(16); /* single send buf */
	dma_addr_t send_dma_addr;
	struct krdma_server_info recv_buf __aligned(16); /* single recv buf */
	dma_addr_t recv_dma_addr;

	// for RDMA READ/WRITE operations
	uint32_t rdma_buf_size;
	char *rdma_write_buf;			/* local rdma write ops buffer */
	u64  rdma_wbuf_dma_addr;
	char *rdma_read_buf;			/* local rdma read ops buffer */
	u64  rdma_rbuf_dma_addr;

	struct completion cm_done;

	int retry_count;
};

#define DYNAMIC_POLLING_INTERVAL

struct krdma_cb *krdma_alloc_cb(void);
int krdma_init_cb(struct krdma_cb *cb);
int krdma_free_cb(struct krdma_cb *cb);
int krdma_cma_event_handler(struct rdma_cm_id *cm_id,
		struct rdma_cm_event *event);
int krdma_set_addr(struct sockaddr_in *addr, const char *host, const int port);
int krdma_resolve_remote(struct krdma_cb *cb, const char *host, const int port);
int krdma_setup_buf(struct krdma_cb *cb);
void krdma_free_buf(struct krdma_cb *cb);
uint64_t string_to_uint64(const char *str);

#endif /* __KVM_X86_KRDMA_H */