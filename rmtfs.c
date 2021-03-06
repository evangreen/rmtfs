#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libqrtr.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "qmi_rmtfs.h"
#include "util.h"
#include "rmtfs.h"

#define RMTFS_QMI_SERVICE	14
#define RMTFS_QMI_VERSION	1
#define RMTFS_QMI_INSTANCE	0

static struct rmtfs_mem *rmem;

static bool dbgprintf_enabled;
static void dbgprintf(const char *fmt, ...)
{
	va_list ap;

	if (!dbgprintf_enabled)
		return;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static void qmi_result_error(struct rmtfs_qmi_result *result, unsigned error)
{
	/* Only propagate initial error */
	if (result->result == QMI_RMTFS_RESULT_FAILURE)
		return;

	result->result = QMI_RMTFS_RESULT_FAILURE;
	result->error = error;
}

static void rmtfs_open(int sock, const struct qrtr_packet *pkt)
{
	struct rmtfs_open_resp resp = {};
	struct rmtfs_open_req req = {};
	DEFINE_QRTR_PACKET(resp_buf, 256);
	unsigned int txn;
	ssize_t len;
	int caller_id = -1;
	int ret;

	ret = qmi_decode_message(&req, &txn, pkt, QMI_REQUEST,
				 QMI_RMTFS_OPEN, rmtfs_open_req_ei);
	if (ret < 0) {
		qmi_result_error(&resp.result, QMI_RMTFS_ERR_MALFORMED_MSG);
		goto respond;
	}

	caller_id = storage_get(pkt->node, req.path);
	if (caller_id < 0) {
		qmi_result_error(&resp.result, QMI_RMTFS_ERR_INTERNAL);
		goto respond;
	}

	resp.caller_id = caller_id;
	resp.caller_id_valid = true;

respond:
	dbgprintf("[RMTFS] open %s => %d (%d:%d)\n",
		  req.path, caller_id, resp.result.result, resp.result.error);

	len = qmi_encode_message(&resp_buf,
				 QMI_RESPONSE, QMI_RMTFS_OPEN, txn, &resp,
				 rmtfs_open_resp_ei);
	if (len < 0) {
		fprintf(stderr, "[RMTFS] failed to encode open-response: %s\n",
			strerror(-len));
		return;
	}

	ret = qrtr_sendto(sock, pkt->node, pkt->port,
			  resp_buf.data, resp_buf.data_len);
	if (ret < 0)
		fprintf(stderr, "[RMTFS] failed to send open-response: %s\n",
			strerror(-ret));
}

static void rmtfs_close(int sock, const struct qrtr_packet *pkt)
{
	struct rmtfs_close_resp resp = {};
	struct rmtfs_close_req req = {};
	DEFINE_QRTR_PACKET(resp_buf, 256);
	unsigned int txn;
	ssize_t len;
	int ret;

	ret = qmi_decode_message(&req, &txn, pkt, QMI_REQUEST,
				 QMI_RMTFS_CLOSE, rmtfs_close_req_ei);
	if (ret < 0) {
		qmi_result_error(&resp.result, QMI_RMTFS_ERR_MALFORMED_MSG);
		goto respond;
	}

	ret = storage_put(pkt->node, req.caller_id);
	if (ret < 0) {
		qmi_result_error(&resp.result, QMI_RMTFS_ERR_INTERNAL);
	}

	rmtfs_mem_free(rmem);

respond:
	dbgprintf("[RMTFS] close %s => %d (%d:%d)\n",
		  req.caller_id, resp.result.result, resp.result.error);

	len = qmi_encode_message(&resp_buf,
				 QMI_RESPONSE, QMI_RMTFS_CLOSE, txn, &resp,
				 rmtfs_close_resp_ei);
	if (len < 0) {
		fprintf(stderr, "[RMTFS] failed to encode close-response: %s\n",
			strerror(-len));
		return;
	}

	ret = qrtr_sendto(sock, pkt->node, pkt->port,
			  resp_buf.data, resp_buf.data_len);
	if (ret < 0)
		fprintf(stderr, "[RMTFS] failed to send close-response: %s\n",
			strerror(-ret));
}

static void rmtfs_iovec(int sock, struct qrtr_packet *pkt)
{
	struct rmtfs_iovec_entry *entries;
	struct rmtfs_iovec_resp resp = {};
	struct rmtfs_iovec_req req = {};
	DEFINE_QRTR_PACKET(resp_buf, 256);
	unsigned long phys_offset;
	uint32_t caller_id = 0;
	size_t num_entries = 0;
	uint8_t is_write;
	uint8_t force = 0;
	unsigned txn;
	ssize_t len;
	ssize_t n;
	char buf[SECTOR_SIZE];
	int ret;
	int fd;
	int i;
	int j;

	ret = qmi_decode_message(&req, &txn, pkt, QMI_REQUEST,
				 QMI_RMTFS_RW_IOVEC, rmtfs_iovec_req_ei);
	if (ret < 0) {
		qmi_result_error(&resp.result, QMI_RMTFS_ERR_MALFORMED_MSG);
		goto respond;
	}

	caller_id = req.caller_id;
	is_write = req.direction;
	entries = req.iovec;
	num_entries = req.iovec_len;
	force = req.is_force_sync;

	fd = storage_get_handle(pkt->node, caller_id);
	if (fd < 0) {
		fprintf(stderr, "[RMTFS] iovec request for non-existing caller\n");
		qmi_result_error(&resp.result, QMI_RMTFS_ERR_INTERNAL);
		goto respond;
	}

	for (i = 0; i < num_entries; i++) {
		phys_offset = entries[i].phys_offset;

		n = lseek(fd, entries[i].sector_addr * SECTOR_SIZE, SEEK_SET);
		if (n < 0) {
			fprintf(stderr, "[RMTFS] failed to seek sector %d\n", entries[i].sector_addr);
			qmi_result_error(&resp.result, QMI_RMTFS_ERR_INTERNAL);
			goto respond;
		}

		for (j = 0; j < entries[i].num_sector; j++) {
			if (is_write) {
				n = rmtfs_mem_read(rmem, phys_offset, buf, SECTOR_SIZE);
				n = write(fd, buf, n);
			} else {
				n = read(fd, buf, SECTOR_SIZE);
				n = rmtfs_mem_write(rmem, phys_offset, buf, n);
			}

			if (n != SECTOR_SIZE) {
				fprintf(stderr, "[RMTFS] failed to %s sector %d\n",
					is_write ? "write" : "read", entries[i].sector_addr + j);
				qmi_result_error(&resp.result, QMI_RMTFS_ERR_INTERNAL);
				goto respond;
			}

			phys_offset += SECTOR_SIZE;
		}
	}

respond:
	dbgprintf("[RMTFS] iovec %d, %sforced => (%d:%d)\n", caller_id, force ? "" : "not ",
							     resp.result.result, resp.result.error);
	for (i = 0; i < num_entries; i++) {
		dbgprintf("[RMTFS]       %s %d:%d 0x%x\n", is_write ? "write" : "read",
							   entries[i].sector_addr,
							   entries[i].num_sector,
							   entries[i].phys_offset);
	}

	len = qmi_encode_message(&resp_buf,
				 QMI_RESPONSE, QMI_RMTFS_RW_IOVEC, txn, &resp,
				 rmtfs_iovec_resp_ei);
	if (len < 0) {
		fprintf(stderr, "[RMTFS] failed to encode iovec-response: %s\n",
			strerror(-len));
		return;
	}

	ret = qrtr_sendto(sock, pkt->node, pkt->port,
			  resp_buf.data, resp_buf.data_len);
	if (ret < 0)
		fprintf(stderr, "[RMTFS] failed to send iovec-response: %s\n",
			strerror(-ret));
}

static void rmtfs_alloc_buf(int sock, struct qrtr_packet *pkt)
{
	struct rmtfs_alloc_buf_resp resp = {};
	struct rmtfs_alloc_buf_req req = {};
	DEFINE_QRTR_PACKET(resp_buf, 256);
	uint32_t alloc_size = 0;
	uint32_t caller_id = 0;
	int64_t address = 0;
	unsigned txn;
	ssize_t len;
	int ret;

	ret = qmi_decode_message(&req, &txn, pkt, QMI_REQUEST,
				 QMI_RMTFS_ALLOC_BUFF, rmtfs_alloc_buf_req_ei);
	if (ret < 0) {
		qmi_result_error(&resp.result, QMI_RMTFS_ERR_MALFORMED_MSG);
		goto respond;
	}

	caller_id = req.caller_id;
	alloc_size = req.buff_size;

	address = rmtfs_mem_alloc(rmem, alloc_size);
	if (address < 0) {
		qmi_result_error(&resp.result, QMI_RMTFS_ERR_INTERNAL);
		goto respond;
	}

	resp.buff_address = address;
	resp.buff_address_valid = true;
respond:
	dbgprintf("[RMTFS] alloc %d, %d => 0x%lx (%d:%d)\n", caller_id, alloc_size, address, resp.result.result, resp.result.error);

	len = qmi_encode_message(&resp_buf,
				 QMI_RESPONSE, QMI_RMTFS_ALLOC_BUFF, txn, &resp,
				 rmtfs_alloc_buf_resp_ei);
	if (len < 0) {
		fprintf(stderr, "[RMTFS] failed to encode alloc-buf-response: %s\n",
			strerror(-len));
		return;
	}

	ret = qrtr_sendto(sock, pkt->node, pkt->port,
			  resp_buf.data, resp_buf.data_len);
	if (ret < 0)
		fprintf(stderr, "[RMTFS] failed to send alloc-buf-response: %s\n",
			strerror(-ret));
}

static void rmtfs_get_dev_error(int sock, struct qrtr_packet *pkt)
{
	struct rmtfs_dev_error_resp resp = {};
	struct rmtfs_dev_error_req req = {};
	DEFINE_QRTR_PACKET(resp_buf, 256);
	int dev_error = 0;
	unsigned txn;
	ssize_t len;
	int ret;

	ret = qmi_decode_message(&req, &txn, pkt, QMI_REQUEST,
				 QMI_RMTFS_GET_DEV_ERROR,
				 rmtfs_dev_error_req_ei);
	if (ret < 0) {
		qmi_result_error(&resp.result, QMI_RMTFS_ERR_MALFORMED_MSG);
		goto respond;
	}

	dev_error = storage_get_error(pkt->node, req.caller_id);
	if (dev_error < 0) {
		qmi_result_error(&resp.result, QMI_RMTFS_ERR_INTERNAL);
		goto respond;
	}

	resp.status = dev_error;
	resp.status_valid = true;

respond:
	dbgprintf("[RMTFS] dev_error %d => %d (%d:%d)\n", req.caller_id, dev_error, resp.result.result, resp.result.error);

	len = qmi_encode_message(&resp_buf,
				 QMI_RESPONSE, QMI_RMTFS_GET_DEV_ERROR, txn,
				 &resp, rmtfs_dev_error_resp_ei);
	if (len < 0) {
		fprintf(stderr, "[RMTFS] failed to encode dev-error-response: %s\n",
			strerror(-len));
		return;
	}

	ret = qrtr_sendto(sock, pkt->node, pkt->port,
			  resp_buf.data, resp_buf.data_len);
	if (ret < 0)
		fprintf(stderr, "[RMTFS] failed to send dev-error-response: %s\n",
			strerror(-ret));
}

static int rmtfs_bye(uint32_t node)
{
	dbgprintf("[RMTFS] bye from %d\n", node);

	return 0;
}

static int rmtfs_del_client(uint32_t node, uint32_t port)
{
	dbgprintf("[RMTFS] del_client %d:%d\n", node, port);

	return 0;
}

static int handle_rmtfs(int sock)
{
	struct sockaddr_qrtr sq;
	struct qrtr_packet pkt;
	unsigned int msg_id;
	socklen_t sl;
	char buf[4096];
	int ret;

	sl = sizeof(sq);
	ret = recvfrom(sock, buf, sizeof(buf), 0, (void *)&sq, &sl);
	if (ret < 0) {
		ret = -errno;
		if (ret != -ENETRESET)
			fprintf(stderr, "[RMTFS] recvfrom failed: %d\n", ret);
		return ret;
	}

	dbgprintf("[RMTFS] packet; from: %d:%d\n", sq.sq_node, sq.sq_port);

	ret = qrtr_decode(&pkt, buf, ret, &sq);
	if (ret < 0) {
		fprintf(stderr, "[RMTFS] unable to decode qrtr packet\n");
		return ret;
	}

	switch (pkt.type) {
	case QRTR_TYPE_BYE:
		return rmtfs_bye(pkt.node);
	case QRTR_TYPE_DEL_CLIENT:
		return rmtfs_del_client(pkt.node, pkt.port);
	case QRTR_TYPE_DATA:
		ret = qmi_decode_header(&pkt, &msg_id);
		if (ret < 0)
			return ret;

		switch (msg_id) {
		case QMI_RMTFS_OPEN:
			rmtfs_open(sock, &pkt);
			break;
		case QMI_RMTFS_CLOSE:
			rmtfs_close(sock, &pkt);
			break;
		case QMI_RMTFS_RW_IOVEC:
			rmtfs_iovec(sock, &pkt);
			break;
		case QMI_RMTFS_ALLOC_BUFF:
			rmtfs_alloc_buf(sock, &pkt);
			break;
		case QMI_RMTFS_GET_DEV_ERROR:
			rmtfs_get_dev_error(sock, &pkt);
			break;
		default:
			fprintf(stderr, "[RMTFS] Unknown request: %d\n", msg_id);
			break;
		}

		return 0;
	}

	return ret;
}

static int run_rmtfs(void)
{
	int rmtfs_fd;
	int ret;

	rmtfs_fd = qrtr_open(RMTFS_QMI_SERVICE);
	if (rmtfs_fd < 0) {
		fprintf(stderr, "failed to create qrtr socket\n");
		return rmtfs_fd;
	}

	dbgprintf("registering services\n");

	ret = qrtr_publish(rmtfs_fd, RMTFS_QMI_SERVICE,
			   RMTFS_QMI_VERSION, RMTFS_QMI_INSTANCE);
	if (ret < 0) {
		fprintf(stderr, "failed to publish rmtfs service");
		return ret;
	}

	for (;;) {
		ret = qrtr_poll(rmtfs_fd, -1);
		if (ret < 0 && errno != EINTR)
			break;
		else if (ret < 0 && errno == EINTR)
			continue;

		ret = handle_rmtfs(rmtfs_fd);
		if (ret == -ENETRESET)
			break;
	}

	close(rmtfs_fd);

	return ret;
}

int main(int argc, char **argv)
{
	int ret;

	if (argc == 2 && strcmp(argv[1], "-v") == 0)
		dbgprintf_enabled = true;

	rmem = rmtfs_mem_open();
	if (!rmem)
		return 1;

	ret = storage_open();
	if (ret) {
		fprintf(stderr, "failed to initialize storage system\n");
		goto close_rmtfs_mem;
	}

	for (;;) {
		ret = run_rmtfs();

		if (ret <= 0 && ret != -ENETRESET)
			break;
	}

	do {
		ret = run_rmtfs();
	} while (ret == -ENETRESET);

	storage_close();
close_rmtfs_mem:
	rmtfs_mem_close(rmem);

	return 0;
}
