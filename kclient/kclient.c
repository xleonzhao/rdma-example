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

#define IPADDR_LEN 16
static char server[IPADDR_LEN]={0}; // server ip address
module_param_string(server, server, IPADDR_LEN, S_IRUGO);

static int port = 20886; // server port number
module_param(port, int, S_IRUGO);

// static struct sockaddr_in server_sockaddr = {0};
/* Source and Destination buffers, where RDMA operations source and sink */
static char *src = NULL, *dst = NULL; 
const char test_string[] = "hello, world";

/* This is our testing function */
static int check_src_dst(void) 
{
	return memcmp((void*) src, (void*) dst, strlen(src));
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

	ret = krdma_setup_mr(cb);
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
		return ret;
	}

	wait_for_completion(&cb->cm_done);
	if (cb->state != KRDMA_CONNECTED) {
		rdma_error("wait for KRDMA_CONNECTED state, but get %d\n", cb->state);
		goto exit;
	}

	debug("rdma_connect successful\n");
	return 0;

exit:
	return ret;
}

static int client_init(void) {
	src = kzalloc(strlen(test_string), GFP_KERNEL);
	if (!src) {
		rdma_error("Failed to allocate memory : -ENOMEM\n");
		return -ENOMEM;
	}
	/* Copy the passes arguments */
	strncpy(src, test_string, strlen(test_string));
	dst = kzalloc(strlen(test_string), GFP_KERNEL);
	if (!dst) {
		rdma_error("Failed to allocate destination memory, -ENOMEM\n");
		kfree(src);
		return -ENOMEM;
	}

	return 0;
}

/* Exchange buffer metadata with the server. The client sends its, and then receives
 * from the server. The client-side metadata on the server is _not_ used because
 * this program is client driven. But it shown here how to do it for the illustration
 * purposes
 */
static int client_xchange_metadata_with_server(struct krdma_cb *cb)
{
	int ret = -1;
	// struct ibv_wc wc[2];
	// struct scatterlist sg[2];

	struct ib_sge server_recv_sge[1];
	struct ib_recv_wr server_recv_wr;
	const struct ib_recv_wr *bad_recv_wr = NULL;
	// dma_addr_t dma_addr;

	// get ready for receiving server info
	memset(server_recv_sge, 0, sizeof(struct ib_sge));
	memset(&server_recv_wr, 0, sizeof(server_recv_wr));

	// server_recv_sge[0].addr = cb->recv_dma_addr;
	// server_recv_sge[0].length = sizeof(cb->recv_buf);
	server_recv_sge[0].addr = cb->recv_dma_addr;
	server_recv_sge[0].length = sizeof(struct krdma_buffer_info);
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

	struct krdma_buffer_info *info = &cb->send_buf;
	info->dma_addr = htonll(cb->rdma_dma_addr);
	info->size = htonl(cb->size);
	info->rkey = htonl(cb->mr->rkey);

	// unsigned char * p = (unsigned char *)cb->local_info.buf;
	// for(int i = 0; i <sizeof(struct krdma_buffer_info); i++) {
	// 	printk(KERN_INFO "%02x ", p[i]);
	// }

	client_send_sge[0].addr = cb->send_dma_addr;
	client_send_sge[0].length = sizeof(struct krdma_buffer_info);
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
static int client_remote_memory_ops(void) 
{
#ifdef _OLD_CODE
	struct ibv_wc wc;
	int ret = -1;
	client_dst_mr = rdma_buffer_register(pd,
			dst,
			strlen(src),
			(IBV_ACCESS_LOCAL_WRITE | 
			 IBV_ACCESS_REMOTE_WRITE | 
			 IBV_ACCESS_REMOTE_READ));
	if (!client_dst_mr) {
		rdma_error("We failed to create the destination buffer, -ENOMEM\n");
		return -ENOMEM;
	}
	/* Step 1: is to copy the local buffer into the remote buffer. We will 
	 * reuse the previous variables. */
	/* now we fill up SGE */
	client_send_sge.addr = (uint64_t) client_src_mr->addr;
	client_send_sge.length = (uint32_t) client_src_mr->length;
	client_send_sge.lkey = client_src_mr->lkey;
	/* now we link to the send work request */
	bzero(&client_send_wr, sizeof(client_send_wr));
	client_send_wr.sg_list = &client_send_sge;
	client_send_wr.num_sge = 1;
	client_send_wr.opcode = IBV_WR_RDMA_WRITE;
	client_send_wr.send_flags = IBV_SEND_SIGNALED;
	/* we have to tell server side info for RDMA */
	client_send_wr.wr.rdma.rkey = server_metadata_attr.stag.remote_stag;
	client_send_wr.wr.rdma.remote_addr = server_metadata_attr.address;
	/* Now we post it */
	ret = ibv_post_send(client_qp, 
		       &client_send_wr,
	       &bad_client_send_wr);
	if (ret) {
		rdma_error("Failed to write client src buffer, errno: %d \n", 
				-errno);
		return -errno;
	}
	/* at this point we are expecting 1 work completion for the write */
	ret = process_work_completion_events(io_completion_channel, 
			&wc, 1);
	if(ret != 1) {
		rdma_error("We failed to get 1 work completions , ret = %d \n",
				ret);
		return ret;
	}
	debug("Client side WRITE is complete \n");
	/* Now we prepare a READ using same variables but for destination */
	client_send_sge.addr = (uint64_t) client_dst_mr->addr;
	client_send_sge.length = (uint32_t) client_dst_mr->length;
	client_send_sge.lkey = client_dst_mr->lkey;
	/* now we link to the send work request */
	bzero(&client_send_wr, sizeof(client_send_wr));
	client_send_wr.sg_list = &client_send_sge;
	client_send_wr.num_sge = 1;
	client_send_wr.opcode = IBV_WR_RDMA_READ;
	client_send_wr.send_flags = IBV_SEND_SIGNALED;
	/* we have to tell server side info for RDMA */
	client_send_wr.wr.rdma.rkey = server_metadata_attr.stag.remote_stag;
	client_send_wr.wr.rdma.remote_addr = server_metadata_attr.address;
	/* Now we post it */
	ret = ibv_post_send(client_qp, 
		       &client_send_wr,
	       &bad_client_send_wr);
	if (ret) {
		rdma_error("Failed to read client dst buffer from the master, errno: %d \n", 
				-errno);
		return -errno;
	}
	/* at this point we are expecting 1 work completion for the write */
	ret = process_work_completion_events(io_completion_channel, 
			&wc, 1);
	if(ret != 1) {
		rdma_error("We failed to get 1 work completions , ret = %d \n",
				ret);
		return ret;
	}
	debug("Client side READ is complete \n");
#endif // _OLD_CODE
	return 0;
}

/* This function disconnects the RDMA connection from the server and cleans up 
 * all the resources.
 */
static int client_disconnect_and_clean(void)
{
#ifdef _OLD_CODE
	struct rdma_cm_event *cm_event = NULL;
	int ret = -1;
	/* active disconnect from the client side */
	ret = rdma_disconnect(cm_client_id);
	if (ret) {
		rdma_error("Failed to disconnect, errno: %d \n", -errno);
		//continuing anyways
	}
	ret = process_rdma_cm_event(cm_event_channel, 
			RDMA_CM_EVENT_DISCONNECTED,
			&cm_event);
	if (ret) {
		rdma_error("Failed to get RDMA_CM_EVENT_DISCONNECTED event, ret = %d\n",
				ret);
		//continuing anyways 
	}
	ret = rdma_ack_cm_event(cm_event);
	if (ret) {
		rdma_error("Failed to acknowledge cm event, errno: %d\n", 
			       -errno);
		//continuing anyways
	}
	/* Destroy QP */
	rdma_destroy_qp(cm_client_id);
	/* Destroy client cm id */
	ret = rdma_destroy_id(cm_client_id);
	if (ret) {
		rdma_error("Failed to destroy client id cleanly, %d \n", -errno);
		// we continue anyways;
	}
	/* Destroy CQ */
	ret = ibv_destroy_cq(client_cq);
	if (ret) {
		rdma_error("Failed to destroy completion queue cleanly, %d \n", -errno);
		// we continue anyways;
	}
	/* Destroy completion channel */
	ret = ibv_destroy_comp_channel(io_completion_channel);
	if (ret) {
		rdma_error("Failed to destroy completion channel cleanly, %d \n", -errno);
		// we continue anyways;
	}
	/* Destroy memory buffers */
	rdma_buffer_deregister(server_metadata_mr);
	rdma_buffer_deregister(client_metadata_mr);	
	rdma_buffer_deregister(client_src_mr);	
	rdma_buffer_deregister(client_dst_mr);	
	/* We free the buffers */
	free(src);
	free(dst);
	/* Destroy protection domain */
	ret = ibv_dealloc_pd(pd);
	if (ret) {
		rdma_error("Failed to destroy client protection domain cleanly, %d \n", -errno);
		// we continue anyways;
	}
	rdma_destroy_event_channel(cm_event_channel);
	printf("Client resource clean up is complete \n");
#endif // _OLD_CODE
	return 0;
}

static int client_main(void * data) {
	int ret = -1;
	src = dst = NULL; 
	struct krdma_cb *cb;

	if (client_init() != 0) {
		goto exit;
	}

	thread_running = true;

	ret = krdma_alloc_cb(&cb, KRDMA_CLIENT_CONN);
	if (ret) {
		rdma_error("alloc cb fail, ret %d\n", ret);
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

	ret = client_remote_memory_ops();
	if (ret) {
		rdma_error("Failed to finish remote memory ops, ret = %d \n", ret);
		goto exit;
	}
	
	if (check_src_dst()) {
		rdma_error("src and dst buffers do not match \n");
	} else {
		debug("...\nSUCCESS, source and destination buffers match \n");
	}

	ret = client_disconnect_and_clean();
	if (ret) {
		rdma_error("Failed to cleanly disconnect and clean up resources \n");
	}
	return ret;

	// free up pd, cq, qp, and mr
	krdma_free_cb(cb);
	debug("quit");
	return 0;

exit:
	if (cb)
		krdma_free_cb(cb);

	debug("error occurred, abort");
	thread_running = false;
	return -EINVAL;

#ifdef TEMP_DISABLED
	ret = client_disconnect_and_clean();
	if (ret) {
		rdma_error("Failed to cleanly disconnect and clean up resources \n");
	}
	return ret;
#endif
	return 0;
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
