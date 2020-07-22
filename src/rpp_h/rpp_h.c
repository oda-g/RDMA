/* SPDX-License-Identifier: GPLv2
 * Copyright(c) 2020 Itsuro Oda
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <pthread.h>
#include <signal.h>

/* rpp_h: multi client version of rpp. */

static int server = -1;
static int terminate = 0;

static int debug;
#define DEBUG_LOG if (debug) printf

struct rpp_rdma_info {
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
};

#define DATA_SIZE 4096
struct rpp_context {
	struct rpp_rdma_info recv_buf;
	struct ibv_mr *recv_mr;

	struct rpp_rdma_info send_buf;
	struct ibv_mr *send_mr;

	char *read_data;
	char *write_data;

	struct ibv_mr *read_mr;
	struct ibv_mr *write_mr;

	uint32_t rkey;
	uint64_t raddr;
	uint32_t rlen;
};

static int
rpp_init_context(struct rdma_cm_id *id)
{
	struct rpp_context *ct;

	ct = (struct rpp_context *)malloc(sizeof(*ct));
	if (ct == NULL) {
		perror("malloc rpp_context");
		return 1;
	}
	memset(ct, 0, sizeof(*ct));
	ct->read_data = (char *)malloc(DATA_SIZE);
	if (ct->read_data == NULL) {
		perror("malloc read_data");
		free(ct);
		return 1;
	}
	ct->write_data = (char *)malloc(DATA_SIZE);
	if (ct->write_data == NULL) {
		perror("malloc write_data");
		free(ct->read_data);
		free(ct);
		return 1;
	}

	id->context = ct;

	return 0;
}

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
	struct rpp_context *ct = id->context;

	DEBUG_LOG("rdma_reg_msgs recv_buf\n");
	ct->recv_mr = rdma_reg_msgs(id, &ct->recv_buf, sizeof(ct->recv_buf));
	if (ct->recv_mr == NULL) {
		perror("rdma_reg_msgs recv_buf");
		return 1;
	}

	DEBUG_LOG("rdma_reg_msgs send_buf\n");
	ct->send_mr = rdma_reg_msgs(id, &ct->send_buf, sizeof(ct->send_buf));
	if (ct->send_mr == NULL) {
		perror("rdma_reg_msgs send_buf");
		return 1;
	}

	DEBUG_LOG("rdma_reg_read\n");
	ct->read_mr = rdma_reg_read(id, ct->read_data, DATA_SIZE);
	if (ct->read_mr == NULL) {
		perror("rdma_reg_read");
		return 1;
	}

	DEBUG_LOG("rdma_reg_write\n");
	ct->write_mr = rdma_reg_write(id, ct->write_data, DATA_SIZE);
	if (ct->write_mr == NULL) {
		perror("rdma_reg_write");
		return 1;
	}

	return 0;
}

static void
rpp_free_buffers(struct rdma_cm_id *id)
{
	struct rpp_context *ct = id->context;

	if (ct == NULL) {
		return;
	}
	if (ct->recv_mr) {
		DEBUG_LOG("rdma_dereg_mr recv_mr\n");
		if (rdma_dereg_mr(ct->recv_mr) != 0) {
			perror("rdma_rereg_mr recv_mr");
		}
	}
	if (ct->send_mr) {
		DEBUG_LOG("rdma_dereg_mr send_mr\n");
		if (rdma_dereg_mr(ct->send_mr) != 0) {
			perror("rdma_rereg_mr send_mr");
		}
	}
	if (ct->read_mr) {
		DEBUG_LOG("rdma_dereg_mr read_mr\n");
		if (rdma_dereg_mr(ct->read_mr) != 0) {
			perror("rdma_rereg_mr read_mr");
		}
	}
	if (ct->write_mr) {
		DEBUG_LOG("rdma_dereg_mr write_mr\n");
		if (rdma_dereg_mr(ct->write_mr) != 0) {
			perror("rdma_rereg_mr write_mr");
		}
	}
}

static int
rpp_rdma_recv(struct rdma_cm_id *id)
{
	struct rpp_context *ct = id->context;
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
		ct->rkey = ct->recv_buf.rkey;
		ct->raddr = ct->recv_buf.buf;
		ct->rlen = ct->recv_buf.size;
		printf("remote rkey %x, addr %lx, len %d\n", ct->rkey,
			       ct->raddr, ct->rlen);
	}

	/* register for next recieve */
	DEBUG_LOG("rdma_post_recv\n");
	ret = rdma_post_recv(id, NULL, &ct->recv_buf, sizeof(ct->recv_buf),
		       ct->recv_mr);
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
	struct rpp_context *ct = id->context;
	int ret;

	DEBUG_LOG("rdma_post_send\n");
	ret = rdma_post_send(id, NULL, &ct->send_buf, sizeof(ct->send_buf),
		       ct->send_mr, 0);
	if (ret != 0) {
		perror("rdma_post_send");
		return 1;
	}

	return rpp_wait_send_comp(id);
}

static void *
exec_rpp(void *arg)
{
	struct rdma_cm_id *id = (struct rdma_cm_id *)arg;
	int ret;
	struct rpp_context *ct;

	ret = rpp_init_context(id);
	if (ret != 0) {
		goto out;
	}
	ct = id->context;

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
	ret = rdma_post_recv(id, NULL, &ct->recv_buf, sizeof(ct->recv_buf),
		       ct->recv_mr);
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
	ret = rdma_post_read(id, NULL, ct->read_data, ct->rlen, ct->read_mr,
		       0, ct->raddr, ct->rkey);
	if (ret != 0) {
		perror("rdma_post_read");
		goto out;
	}

	ret = rpp_wait_send_comp(id);
	if (ret != 0) {
		goto out;
	}

	printf("RDMA READ data: %s\n", ct->read_data);

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
	strcpy(ct->write_data, "bbb");

	/* RDMA WRITE */
	DEBUG_LOG("rdma_post_write\n");
	ret = rdma_post_write(id, NULL, ct->write_data, ct->rlen, ct->write_mr,
		       0, ct->raddr, ct->rkey);
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
	rpp_free_buffers(id);
	DEBUG_LOG("rdma_destroy_qp\n");
	rdma_destroy_qp(id);
	DEBUG_LOG("rdma_destroy_id id\n");
	if (rdma_destroy_id(id) != 0) {
		perror("rdma_destroy_id id");
	}

	return NULL;
}

static void handle_sigint(int sig)
{
	terminate = 1;
}

static int
run_server(struct sockaddr *addr)
{
	int ret;
	struct rdma_event_channel *ch;
	struct rdma_cm_id *listen_id;
	struct rdma_cm_event *event;
	struct rdma_cm_id *id = NULL;
	pthread_t th;
	struct sigaction act;

	DEBUG_LOG("rdma_create_event_channel\n");
	ch = rdma_create_event_channel();
	if (ch == NULL) {
		perror("rdma_create_event_channel");
		return 1;
	}

	DEBUG_LOG("rdma_create_id\n");
	ret = rdma_create_id(ch, &listen_id, NULL, RDMA_PS_TCP);
	if (ret != 0) {
		perror("rdma_create_id");
		rdma_destroy_event_channel(ch);
		return 1;
	}

	/* NOTE: rdma_bind_addr is synchronous.
	 * rdma_get_cm_event/rdma_ack_cm_event is not necessary. */
	DEBUG_LOG("rdma_bind_addr\n");
	ret = rdma_bind_addr(listen_id, addr);
	if (ret != 0) {
		perror("rdma_bind_addr");
		goto out;
	}

	DEBUG_LOG("rdma_listen\n");
	ret = rdma_listen(listen_id, 3);
	if (ret != 0) {
		perror("rdma_listen");
		goto out;
	}

	/* NOTE: use sigaction(2) to wake blocked system call by EINTR
	 * after signal catched. signal(2) implies SA_RESTART. */
	memset(&act, 0, sizeof(act));
	act.sa_handler = handle_sigint;
	ret = sigaction(SIGINT, &act, NULL);
	if (ret != 0) {
		perror("sigaction");
		goto out;
	}

	while (terminate == 0) {
		DEBUG_LOG("rdma_get_cm_event\n");
		ret = rdma_get_cm_event(ch, &event);
		if (ret != 0) {
			perror("rdma_get_cm_event");
			goto out;
		}
		if (event->status != 0) {
			fprintf(stderr, "event status == %d\n", event->status);
			goto out;
		}
		if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
			fprintf(stderr, "unexpected event %d != %d(expected)\n",
				event->event, RDMA_CM_EVENT_CONNECT_REQUEST);
			goto out;
		}
		id = event->id;
		DEBUG_LOG("rdma_ack_cm_event\n");
		ret = rdma_ack_cm_event(event);
		if (ret != 0) {
			perror("rdma_ack_cm_event");
			goto out;
		}

		/* set new id to synchronous */
		DEBUG_LOG("rdma_migrate_id\n");
		ret = rdma_migrate_id(id, NULL);
		if (ret != 0) {
			perror("rdma_migrate_id");
			goto out;
		}

		ret = pthread_create(&th, NULL, exec_rpp, (void *)id);
		if (ret != 0) {
			perror("pthread_create");
			goto out;
		}
		id = NULL;

		ret = pthread_detach(th);
		if (ret != 0) {
			perror("pthread_detach");
			goto out;
		}
	}

out:
	if (id) {
		rpp_free_buffers(id);
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
	DEBUG_LOG("rdma_destroy_event_channel\n");
	rdma_destroy_event_channel(ch);  /* void */

	return ret;
}

static int
run_client(struct sockaddr *addr)
{
	int ret;
	struct rdma_cm_id *id;
	struct ibv_wc wc;
	struct rpp_context *ct;

	DEBUG_LOG("rdma_create_id\n");
	ret = rdma_create_id(NULL, &id, NULL, RDMA_PS_TCP);
	if (ret != 0) {
		perror("rdma_create_id");
		return 1;
	}
	ret = rpp_init_context(id);
	if (ret != 0) {
		goto out;
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
	ct = id->context;

	/* regisger for first recieve */
	DEBUG_LOG("rdma_post_recv\n");
	ret = rdma_post_recv(id, NULL, &ct->recv_buf, sizeof(ct->recv_buf),
		       ct->recv_mr);
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
	strcpy(ct->read_data, "aaa");
	ct->send_buf.buf = (uint64_t)ct->read_data;
	ct->send_buf.rkey = ct->read_mr->rkey;
	ct->send_buf.size = DATA_SIZE;

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
	ct->send_buf.buf = (uint64_t)ct->write_data;
	ct->send_buf.rkey = ct->write_mr->rkey;
	ct->send_buf.size = DATA_SIZE;

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

	printf("RDMA WRITE data: %s\n", ct->write_data);

	printf("done\n");

out:
	rpp_free_buffers(id);
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
	fprintf(stderr, "usage: rpp_h {-s|-c} [-d] server-ip-address\n");
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
