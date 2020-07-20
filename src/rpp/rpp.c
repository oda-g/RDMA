/* SPDX-License-Identifier: GPLv2
 * Copyright(c) 2020 Itsuro Oda
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>

/* rpp: simplified version of rping.
 *
 * rpp is intended for RDMA programing using only rdma_cm and
 * rdma verbs, not using ibv_*. 
 *
 * ping/pong(same as rping) is done only once:
 * 	client sends source rkey/addr/len
 *      server receives source rkey/add/len
 *      server rdma reads "ping" data from source
 *      server sends "go ahead" on rdma read completion
 *      client sends sink rkey/addr/len
 *      server receives sink rkey/addr/len
 *      server rdma writes "pong" data to sink
 *      server sends "completion" on rdma write completion
 */

static int server = -1;

static int debug;
#define DEBUG_LOG if (debug) printf

struct rpp_rdma_info {
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
};

static struct rpp_rdma_info recv_buf;
static struct ibv_mr *recv_mr;

static struct rpp_rdma_info send_buf;
static struct ibv_mr *send_mr;

#define DATA_SIZE 4096
static char read_data[DATA_SIZE];
static char write_data[DATA_SIZE];

static struct ibv_mr *read_mr;
static struct ibv_mr *write_mr;

static uint32_t rkey;
static uint64_t raddr;
static uint32_t rlen;

static int
rpp_create_qp(struct rdma_cm_id *id)
{
	struct ibv_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = 2;
	init_attr.cap.max_recv_wr = 2;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;
	init_attr.qp_type = IBV_QPT_RC;
	/* NOTE: when sq_sig_all == 0, set IBV_SEND_SIGNALED to
	 * 'flags' of rdma_post_* if you want to get send completion
	 */
	init_attr.sq_sig_all = 1;

	DEBUG_LOG("rdma_create_qp\n");
	ret = rdma_create_qp(id, NULL, &init_attr);
	if (ret != 0) {
		perror("rdma_create_qp");
	}

	return ret;
}

static int
rpp_setup_buffers(struct rdma_cm_id *id)
{
	DEBUG_LOG("rdma_reg_msgs recv_buf\n");
	recv_mr = rdma_reg_msgs(id, &recv_buf, sizeof(recv_buf));
	if (recv_mr == NULL) {
		perror("rdma_reg_msgs recv_buf");
		return 1;
	}

	DEBUG_LOG("rdma_reg_msgs send_buf\n");
	send_mr = rdma_reg_msgs(id, &send_buf, sizeof(send_buf));
	if (send_mr == NULL) {
		perror("rdma_reg_msgs send_buf");
		return 1;
	}

	DEBUG_LOG("rdma_reg_read\n");
	read_mr = rdma_reg_read(id, read_data, sizeof(read_data));
	if (read_mr == NULL) {
		perror("rdma_reg_read");
		return 1;
	}

	DEBUG_LOG("rdma_reg_write\n");
	write_mr = rdma_reg_write(id, write_data, sizeof(write_data));
	if (write_mr == NULL) {
		perror("rdma_reg_write");
		return 1;
	}

	return 0;
}

static void
rpp_free_buffers(void)
{
	if (recv_mr) {
		DEBUG_LOG("rdma_dereg_mr recv_mr\n");
		if (rdma_dereg_mr(recv_mr) != 0) {
			perror("rdma_rereg_mr recv_mr");
		}
	}
	if (send_mr) {
		DEBUG_LOG("rdma_dereg_mr send_mr\n");
		if (rdma_dereg_mr(send_mr) != 0) {
			perror("rdma_rereg_mr send_mr");
		}
	}
	if (read_mr) {
		DEBUG_LOG("rdma_dereg_mr read_mr\n");
		if (rdma_dereg_mr(read_mr) != 0) {
			perror("rdma_rereg_mr read_mr");
		}
	}
	if (write_mr) {
		DEBUG_LOG("rdma_dereg_mr write_mr\n");
		if (rdma_dereg_mr(write_mr) != 0) {
			perror("rdma_rereg_mr write_mr");
		}
	}
}

static int
rpp_rdma_recv(struct rdma_cm_id *id)
{
	int ret;
	struct ibv_wc wc;

	DEBUG_LOG("rdma_get_recv_comp\n");
	ret = rdma_get_recv_comp(id, &wc);
	if (ret < 0) {
		perror("rdma_get_recv_comp");
		return 1;
	} else if (ret == 0) {
		fprintf(stderr, "rdma_get_recv_comp ret 0\n");
		return 1;
	}

	/* NOTE: client send remote buffer info to server.
	 * server's send is to notify only and data has no meaning.
	 */
	if (server) {
		rkey = recv_buf.rkey;
		raddr = recv_buf.buf;
		rlen = recv_buf.size;
		printf("remote rkey %x, addr %lx, len %d\n", rkey, raddr, rlen);
	}

	/* register for next recieve */
	DEBUG_LOG("rdma_post_recv\n");
	ret = rdma_post_recv(id, NULL, &recv_buf, sizeof(recv_buf), recv_mr);
	if (ret != 0) {
		perror("rdma_post_recv");
		return 1;
	}

	return 0;
}

static int
rpp_wait_send_comp(struct rdma_cm_id *id)
{
	int ret;
	struct ibv_wc wc;

	DEBUG_LOG("rdma_get_send_comp\n");
	ret = rdma_get_send_comp(id, &wc);
	if (ret < 0) {
		perror("rdma_get_send_comp");
		return 1;
	} else if (ret == 0) {
		fprintf(stderr, "rdma_get_send_comp ret 0\n");
		return 1;
	}

	return 0;
}

static int
rpp_rdma_send(struct rdma_cm_id *id)
{
	int ret;

	DEBUG_LOG("rdma_post_send\n");
	ret = rdma_post_send(id, NULL, &send_buf, sizeof(send_buf), send_mr, 0);
	if (ret != 0) {
		perror("rdma_post_send");
		return 1;
	}

	return rpp_wait_send_comp(id);
}

static int
run_server(struct sockaddr *addr)
{
	int ret;
	struct rdma_cm_id *listen_id;
	struct rdma_cm_id *id = NULL;

	DEBUG_LOG("rdma_create_id\n");
	ret = rdma_create_id(NULL, &listen_id, NULL, RDMA_PS_TCP);
	if (ret != 0) {
		perror("rdma_create_id");
		return 1;
	}

	DEBUG_LOG("rdma_bind_addr\n");
	ret = rdma_bind_addr(listen_id, addr);
	if (ret != 0) {
		perror("rdma_bind_addr");
		goto out;
	}

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(listen_id, 1);
	if (ret != 0) {
		perror("rdma_listen");
		goto out;
	}

	DEBUG_LOG("rdma_get_request\n");
	ret = rdma_get_request(listen_id, &id);
	if (ret != 0) {
		perror("rdma_get_request");
		goto out;
	}

	ret = rpp_create_qp(id);
	if (ret != 0) {
		goto out;
	}

	ret = rpp_setup_buffers(id);
	if (ret != 0) {
		goto out;
	}

	/* regisger for first recieve */
	DEBUG_LOG("rdma_post_recv\n");
	ret = rdma_post_recv(id, NULL, &recv_buf, sizeof(recv_buf), recv_mr);
	if (ret != 0) {
		perror("rdma_post_recv");
		goto out;
	}

	DEBUG_LOG("rdma_accept\n");
	ret = rdma_accept(id, NULL);
	if (ret != 0) {
		perror("rdma_accept");
		goto out;
	}

	/* recieve remote buffer info from client */
	ret = rpp_rdma_recv(id);
	if (ret != 0) {
		goto out;
	}

	/* RDMA READ */
	DEBUG_LOG("rdma_post_read\n");
	ret = rdma_post_read(id, NULL, read_data, rlen, read_mr, 0, raddr, rkey);
	if (ret != 0) {
		perror("rdma_post_read");
		goto out;
	}

	ret = rpp_wait_send_comp(id);
	if (ret != 0) {
		goto out;
	}

	printf("RDMA READ data: %s\n", read_data);

	/* send go ahead to clinet */
	ret = rpp_rdma_send(id);
	if (ret != 0) {
		goto out;
	}

	/* recieve remote buffer info from client */
	ret = rpp_rdma_recv(id);
	if (ret != 0) {
		goto out;
	}

	/* prepare write data */
	strcpy(write_data, "bbb");

	/* RDMA WRITE */
	DEBUG_LOG("rdma_post_write\n");
	ret = rdma_post_write(id, NULL, write_data, rlen, write_mr, 0, raddr, rkey);
	if (ret != 0) {
		perror("rdma_post_write");
		goto out;
	}

	ret = rpp_wait_send_comp(id);
	if (ret != 0) {
		goto out;
	}

	/* send complete to clinet */
	ret = rpp_rdma_send(id);
	if (ret != 0) {
		goto out;
	}

	printf("done\n");

out:
	rpp_free_buffers();
	if (id) {
		DEBUG_LOG("rdma_destroy_qp\n");
		rdma_destroy_qp(id);
		DEBUG_LOG("rdma_destroy_id id\n");
		if (rdma_destroy_id(id) != 0) {
			perror("rdma_destroy_id id");
		}
	}
	DEBUG_LOG("rdma_destroy_id listen_id\n");
	if (rdma_destroy_id(listen_id) != 0) {
		perror("rdma_destroy_id listen_id");
	}

	return ret;
}

static int
run_client(struct sockaddr *addr)
{
	int ret;
	struct rdma_cm_id *id;
	struct ibv_wc wc;

	DEBUG_LOG("rdma_create_id\n");
	ret = rdma_create_id(NULL, &id, NULL, RDMA_PS_TCP);
	if (ret != 0) {
		perror("rdma_create_id");
		return 1;
	}

	DEBUG_LOG("rdma_resolve_addr\n");
	ret = rdma_resolve_addr(id, NULL, addr, 2000);
	if (ret != 0) {
		perror("rdma_resolve_addr");
		goto out;
	}

	DEBUG_LOG("rdma_resolve_route\n");
	ret = rdma_resolve_route(id, 2000);
	if (ret != 0) {
		perror("rdma_resolve_route");
		goto out;
	}

	ret = rpp_create_qp(id);
	if (ret != 0) {
		goto out;
	}

	ret = rpp_setup_buffers(id);
	if (ret != 0) {
		goto out;
	}

	/* regisger for first recieve */
	DEBUG_LOG("rdma_post_recv\n");
	ret = rdma_post_recv(id, NULL, &recv_buf, sizeof(recv_buf), recv_mr);
	if (ret != 0) {
		perror("rdma_post_recv");
		goto out;
	}

	DEBUG_LOG("rdma_connect\n");
	ret = rdma_connect(id, NULL);
	if (ret != 0) {
		perror("rdma_connect");
		goto out;
	}

	/* prepare data for RDMA READ */
	strcpy(read_data, "aaa");
	send_buf.buf = (uint64_t)read_data;
	send_buf.rkey = read_mr->rkey;
	send_buf.size = sizeof(read_data);

	/* send buffer info to server */
	ret = rpp_rdma_send(id);
	if (ret != 0) {
		goto out;
	}

	/* recieve go ahead from server */
	ret = rpp_rdma_recv(id);
	if (ret != 0) {
		goto out;
	}

	/* prepare data for RDMA WRITE */
	send_buf.buf = (uint64_t)write_data;
	send_buf.rkey = write_mr->rkey;
	send_buf.size = sizeof(write_data);

	/* send buffer info to server */
	ret = rpp_rdma_send(id);
	if (ret != 0) {
		goto out;
	}

	/* recieve complete from server */
	ret = rpp_rdma_recv(id);
	if (ret != 0) {
		goto out;
	}

	printf("RDMA WRITE data: %s\n", write_data);

	printf("done\n");

out:
	rpp_free_buffers();
	DEBUG_LOG("rdma_destroy_qp\n");
	rdma_destroy_qp(id);
	DEBUG_LOG("rdma_destroy_id\n");
	if (rdma_destroy_id(id) != 0) {
		perror("rdma_destroy_id");
	}

	return ret;
}

static void
usage(void)
{
	fprintf(stderr, "usage: rpp {-s|-c} [-d] server-ip-address\n");
}

int main(int argc, char *argv[])
{
	int opt;
	struct sockaddr_in addr;
	int ret = 0;

	while ((opt = getopt(argc, argv, "csd")) != -1) {
		switch (opt) {
		case 'c':
			if (server == 1) {
				usage();
				return 1;
			}
			server = 0;
			break;
		case 's':
			if (server == 0) {
				usage();
				return 1;
			}
			server = 1;
			break;
		case 'd':
			debug = 1;
			break;
		default:
			usage();
			return 1;
		}
	}
	
	if (server == -1) {
		usage();
		return 1;
	}

	if (optind != argc - 1) {
		usage();
		return 1;
	}

	addr.sin_family = AF_INET;
	addr.sin_port = htons(7999);
	if (inet_aton(argv[optind], &addr.sin_addr) == 0) {
		fprintf(stderr, "Invalid IP address: %s\n", argv[optind]);
		return 1;
	}

	if (server) {
		ret = run_server((struct sockaddr *)&addr);
	} else {
		ret = run_client((struct sockaddr *)&addr);
	}

	return ret;
}
