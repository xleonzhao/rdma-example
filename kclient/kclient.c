/*
 * An example RDMA client side code. 
 * Author: Animesh Trivedi 
 *         atrivedi@apache.org
 */

/*
* RDMA client as a kernel module
* Author: Leon Zhao
*         xleonzhao@gmail.com 
*/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/inet.h>

#include "krdma.h"

static bool thread_running = false;
static struct krdma_cb *cb = NULL;
#define this_client_token1 "ABCDEFGH"
#define this_client_token2 "4321"

#define IPADDR_LEN 16
static char server[IPADDR_LEN]={0}; // server ip address
module_param_string(server, server, IPADDR_LEN, S_IRUGO);

static int port = 20886; // server port number
module_param(port, int, S_IRUGO);

/* Source and Destination buffers, where RDMA operations source and sink */
// static char *src = NULL, *dst = NULL; 
const char test_string[] = "hello, world";

/* This is our testing function */
static int check_src_dst(void *src, void *dst) 
{
	return memcmp(src, dst, strlen(src));
}

/* This function prepares client side connection resources for an RDMA connection */
static int client_init_rdma(struct krdma_cb *cb)
{
	int ret = -1;

	/* Create cm_id */
	cb->cm_id = rdma_create_id(&init_net, krdma_cma_event_handler, cb,
					RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(cb->cm_id)) {
		ret = PTR_ERR(cb->cm_id);
		rdma_error("rdma_create_id error %d\n", ret);
		goto exit;
	}
	debug("created cm_id %p\n", cb->cm_id);

	ret = krdma_resolve_remote(cb, server, port);
	if (ret)
		goto exit;
	/* 
	* Allocate pd, cq, qp, mr, freed by caller
	*/
	ret = krdma_init_cb(cb);
	if (ret < 0)
		goto exit;

	ret = krdma_setup_buf(cb);
	if (ret < 0)
		goto exit;

	return 0;

exit:
	return ret;
}

static int client_connect_to_server(struct krdma_cb *cb)
{
	struct rdma_conn_param conn_param;
	int ret = -1;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 3;
	conn_param.initiator_depth = 3;
	conn_param.retry_count = 3;

	ret = rdma_connect(cb->cm_id, &conn_param);
	if (ret) {
		rdma_error("rdma_connect error %d\n", ret);
		goto exit;
	}

	wait_for_completion(&cb->cm_done);
	if (cb->state != KRDMA_CONNECTED) {
		rdma_error("wait for KRDMA_CONNECTED state, but get %d\n", cb->state);
		ret = -1;
		goto exit;
	}

	debug("rdma_connect successful\n");
	return 0;

exit:
	return ret;
}

/* Exchange buffer metadata with the server. The client sends its, and then receives
 * from the server. The client-side metadata on the server is _not_ used because
 * this program is client driven. But it shown here how to do it for the illustration
 * purposes
 */
static int client_xchange_metadata_with_server(struct krdma_cb *cb)
{
	int ret = -1;

	struct ib_sge server_recv_sge[1];
	struct ib_recv_wr server_recv_wr;
	const struct ib_recv_wr *bad_recv_wr = NULL;

	// get ready for receiving server info
	memset(server_recv_sge, 0, sizeof(struct ib_sge));
	memset(&server_recv_wr, 0, sizeof(server_recv_wr));

	server_recv_sge[0].addr = cb->recv_dma_addr;
	server_recv_sge[0].length = sizeof(struct krdma_server_info);
	server_recv_sge[0].lkey = cb->pd->local_dma_lkey;
	server_recv_wr.sg_list = server_recv_sge;
	server_recv_wr.num_sge = 1;

	ret = ib_post_recv(cb->qp /* which QP */,
		      &server_recv_wr /* receive work request*/,
		      &bad_recv_wr /* error WRs */);
	if (ret) {
		rdma_error("Failed to pre-post the receive buffer, errno: %d \n", ret);
		return ret;
	}
	debug("Receive buffer posting is successful \n");

	// now preparing data to send to server
	struct ib_sge client_send_sge[1];
	struct ib_send_wr client_send_wr;
	const struct ib_send_wr *bad_send_wr = NULL;

	memset(client_send_sge, 0, sizeof(struct ib_sge));
	memset(&client_send_wr, 0, sizeof(client_send_wr));

	struct krdma_client_info *info = &cb->send_buf;
	info->token1 = string_to_uint64(this_client_token1);
	info->token2 = (uint32_t)(string_to_uint64(this_client_token2) >> 32);
	// request server to allocate a buffer with info->size
	info->size = RDMA_RDWR_BUF_LEN;

	client_send_sge[0].addr = cb->send_dma_addr;
	client_send_sge[0].length = sizeof(struct krdma_client_info);
	client_send_sge[0].lkey = cb->pd->local_dma_lkey;
	client_send_wr.sg_list = client_send_sge;
	client_send_wr.num_sge = 1;
	client_send_wr.opcode = IB_WR_SEND;
	client_send_wr.send_flags = IB_SEND_SIGNALED;

	/* Now we post it */
	ret = ib_post_send(cb->qp, 
		    	&client_send_wr,
	       		&bad_send_wr);
	if (ret) {
		rdma_error("Failed to send client metadata, ret: %d \n", ret);
		return ret;
	}
	debug("Send posting is successful \n");

	/* at this point we are expecting 2 work completion. One for our 
	 * send and one for recv that we will get from the server for 
	 * its buffer information */
	wait_for_completion(&cb->cm_done);

	if (cb->state == KRDMA_ERROR) 
		return -1;
	
	return 0;
}

/* This function does :
 * 1) Prepare memory buffers for RDMA operations 
 * 1) RDMA write from src -> remote buffer 
 * 2) RDMA read from remote bufer -> dst
 */ 
static int client_remote_memory_ops(struct krdma_cb *cb) 
{
	int ret = -1;
	struct ib_sge client_rdma_sge[1];
	struct ib_rdma_wr client_rdma_wr;
	const struct ib_send_wr *bad_send_wr = NULL;

	// copy data to start_buf
	memcpy((void *)cb->rdma_write_buf, (const void *)test_string, 
			sizeof(test_string));

	memset(client_rdma_sge, 0, sizeof(struct ib_sge));
	memset(&client_rdma_wr, 0, sizeof(client_rdma_wr));

	/* Step 1: is to copy the local buffer into the remote buffer. We will 
	 * reuse the previous variables. */
	/* now we fill up SGE */
	client_rdma_sge[0].addr = (uint64_t) cb->rdma_wbuf_dma_addr;
	client_rdma_sge[0].length = (uint32_t) sizeof(test_string);
	client_rdma_sge[0].lkey = cb->pd->local_dma_lkey;
	/* now we link to the rdma work request */
	client_rdma_wr.wr.sg_list = client_rdma_sge;
	client_rdma_wr.wr.num_sge = 1;
	client_rdma_wr.wr.opcode = IB_WR_RDMA_WRITE;
	client_rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	/* we have to tell server side info for RDMA */
	client_rdma_wr.rkey = cb->recv_buf.rkey;
	client_rdma_wr.remote_addr = cb->recv_buf.dma_addr;
	/* Now we post it */
	ret = ib_post_send(cb->qp, &client_rdma_wr.wr, &bad_send_wr);
	if (ret) {
		rdma_error("Failed to write client src buffer, ret %d \n", 
				ret);
		return ret;
	}
	/* at this point we are expecting 1 work completion for the write */
	wait_for_completion(&cb->cm_done);

	if (cb->state == KRDMA_ERROR) 
		return -1;

	debug("Client side WRITE is complete \n");

	/* Now we prepare a READ using same variables but for destination */
	client_rdma_sge[0].addr = (uint64_t) cb->rdma_rbuf_dma_addr;
	client_rdma_sge[0].length = (uint32_t) cb->rdma_buf_size;
	client_rdma_sge[0].lkey = cb->pd->local_dma_lkey;
	/* now we link to the rdma work request */
	client_rdma_wr.wr.sg_list = client_rdma_sge;
	client_rdma_wr.wr.num_sge = 1;
	client_rdma_wr.wr.opcode = IB_WR_RDMA_READ;
	client_rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	/* we have to tell server side info for RDMA */
	client_rdma_wr.remote_addr = cb->recv_buf.dma_addr;
	client_rdma_wr.rkey = cb->recv_buf.rkey;
	/* Now we post it */
	ret = ib_post_send(cb->qp, 
		       &client_rdma_wr.wr, &bad_send_wr);
	if (ret) {
		rdma_error("Failed to read client dst buffer from the master, ret %d\n", 
				ret);
		return ret;
	}
	/* at this point we are expecting 1 work completion for the write */
	wait_for_completion(&cb->cm_done);

	if (cb->state == KRDMA_ERROR) 
		return -1;

	debug("Client side READ is complete \n");

	// memcpy(dst, cb->rdma_read_buf, sizeof(test_string));
	return 0;
}

/* This function disconnects the RDMA connection from the server and cleans up 
 * all the resources.
 */
static void client_disconnect_and_clean(struct krdma_cb *cb)
{
	// free up pd, cq, qp, and mr
	if (cb)
		krdma_free_cb(cb);
}

static int client_main(void * data) {
	int ret = -1;

	thread_running = true;

	cb = krdma_alloc_cb();
	if (!cb) {
		rdma_error("alloc cb fail\n");
		goto exit;
	}

	ret = client_init_rdma(cb);
	if (ret) { 
		rdma_error("Failed to initialize RDMA resources\n");
		goto exit;
	}

	ret = client_connect_to_server(cb);
	if (ret) { 
		rdma_error("Failed to setup client connection , ret = %d \n", ret);
		goto exit;
	}

	ret = client_xchange_metadata_with_server(cb);
	if (ret) {
		rdma_error("Failed to setup client connection , ret = %d \n", ret);
		goto exit;
	}

	ret = client_remote_memory_ops(cb);
	if (ret) {
		rdma_error("Failed to finish remote memory ops, ret = %d \n", ret);
		goto exit;
	}
	
	if (check_src_dst(cb->rdma_write_buf, cb->rdma_read_buf)) {
		rdma_error("src and dst buffers do not match \n");
	} else {
		debug("...\nSUCCESS, source and destination buffers match \n");
	}

	ret = 0;

exit:
	client_disconnect_and_clean(cb);
	if (ret) {
		debug("error occurred, abort");
	} else {
		debug("quit normally");
	}

	thread_running = false;
	return ret;
}

/////////////////////////////////////////////////////////////

static struct task_struct *thread = NULL;

static int __init kclient_init(void) {
	int ret;

	thread = kthread_run(client_main, NULL, "krdma_client");
	if (IS_ERR(thread)) {
		rdma_error("start thead failed.\n");
		ret = PTR_ERR(thread);
		return ret;
	}
    return 0;
}

static void __exit kclient_exit(void) {
	int ret;
	if (thread_running) {
		send_sig(SIGKILL, thread, 1);
		ret = kthread_stop(thread);
		if (ret < 0) {
			rdma_error("kill thread failed.\n");
		}
	}
}

module_init(kclient_init);
module_exit(kclient_exit);
MODULE_AUTHOR("Leon Zhao <xleonzhao@gmail.com>");
MODULE_DESCRIPTION("RDMA client kernel module");
MODULE_LICENSE("GPLv2");
