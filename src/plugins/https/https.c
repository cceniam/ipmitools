/*
 * Copyright (c) 2024 ByteDance
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from this
 *    software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <curl/curl.h>

#include <assert.h>
#include <stdbool.h>

#include <ipmitool/log.h>
#include <ipmitool/ipmi.h>
#include <ipmitool/ipmi_intf.h>

#define IPMI_HTTPS_PORT 443
#define IPMI_HTTPS_TIMEOUT 5
#define MAX_RESPONSE_BYTES 256
#define CCODE_LEN 1
#define IPMI_MSG_RSP_HEADER_LEN 7
#define CHECKSUM_LEN 1

static CURL *curl;
static bool curl_global_init_done;

static void ipmi_https_init(void)
{
	if (!curl_global_init_done) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		curl_global_init_done = true;
	}
	if (!curl) {
		curl = curl_easy_init();
	}
	assert(curl);
}

static void ipmi_https_release(void)
{
	if (curl) {
		curl_easy_cleanup(curl);
		curl = NULL;
	}
}

struct curl_write_info {
	char *buf; // pointer to the response data buffer
	size_t size; // size of the response data buffer
};

struct ipmi_rq_header {
	uint8_t netfn:6;
	uint8_t lun:2;
	uint8_t cmd;
	uint8_t target_cmd;
	uint16_t data_len;
};

static int ipmi_https_send_request(struct ipmi_intf *intf, struct ipmi_rq * req, struct ipmi_rs * rsp)
{
	size_t sent;
	uint8_t req_data[256];
	uint8_t bridge_req_data[256];
	size_t rlen;
	const struct curl_ws_frame *meta;
	uint8_t buffer[256] = {0};
	CURLcode result;
	int len = 0;
	int cs = 0;
	int tmp = 0;
	int is_bridge_command = 0;

	lprintf(LOG_DEBUG, "ipmitool: https: sending request: netfn=0x%02x, lun=0x%02x, cmd=0x%02x, target_cmd=0x%02x, data_len=0x%04x",
			req->msg.netfn, req->msg.lun, req->msg.cmd, req->msg.target_cmd, req->msg.data_len);

	uint8_t ourAddress = intf->my_addr;

	if (ourAddress == 0)
		ourAddress = IPMI_BMC_SLAVE_ADDR;

	if (intf->target_addr != ourAddress)
	{
		is_bridge_command = 1;
	}

	if (!is_bridge_command)
	{
		if (sizeof(req_data) < sizeof(struct ipmi_rq_header) + req->msg.data_len)
		{
			return -1;
		}
		memcpy(&req_data[0], req, sizeof(struct ipmi_rq_header));
		memcpy(&req_data[sizeof(struct ipmi_rq_header)], req->msg.data, req->msg.data_len);
	}
	else
	{
		// bridge command
		bridge_req_data[len++] = (0x40 | intf->target_channel);
		cs = len;
		bridge_req_data[len++] = intf->target_addr;
		bridge_req_data[len++] = req->msg.netfn << 2 | (req->msg.lun & 3);

		/* checksum */
		tmp = len - cs;
		bridge_req_data[len++] = ipmi_csum(bridge_req_data + cs, tmp);
		cs = len;

		bridge_req_data[len++] = IPMI_REMOTE_SWID;
		bridge_req_data[len++] = 0; // rqSeq / rqLUN,  Https interface don't need this.

		/* cmd */
		bridge_req_data[len++] = req->msg.cmd;

		/* message data */
		if (req->msg.data_len)
		{
			if (sizeof(bridge_req_data) - len < req->msg.data_len)
			{
				return -1;
			}
			memcpy(bridge_req_data + len, req->msg.data, req->msg.data_len);
			len += req->msg.data_len;
		}

		/* second checksum */
		tmp = len - cs;
		bridge_req_data[len++] = ipmi_csum(bridge_req_data + cs, tmp);

		req->msg.target_cmd = req->msg.cmd;
		req->msg.netfn = IPMI_NETFN_APP;
		req->msg.cmd = 0x34; /* Send Message rqst */
		req->msg.data_len = len;

		if (sizeof(req_data) < sizeof(struct ipmi_rq_header) + req->msg.data_len)
		{
			return -1;
		}
		memcpy(&req_data[0], req, sizeof(struct ipmi_rq_header));
		memcpy(&req_data[sizeof(struct ipmi_rq_header)], bridge_req_data, len);
	}

	lprintf(LOG_DEBUG, "ipmitool: wss raw data: %s", buf2str(req_data, sizeof(struct ipmi_rq_header) + req->msg.data_len));

	result = curl_ws_send(curl, req_data, sizeof(struct ipmi_rq_header) + req->msg.data_len, &sent, 0,
					CURLWS_BINARY);
	if (result != CURLE_OK) {
		lprintf(LOG_ERR, "ipmitool: https: failed to send data");
		return -1;
	}

	// TODO: retry until we get the response, timeout is 1 second
	while (1) {

		result = curl_ws_recv(curl, buffer, sizeof(buffer), &rlen, &meta);
		if (result == CURLE_OK) {
			break;
		}

		if (result == CURLE_AGAIN) {
			continue;
		} else {
			// TODO: Timeout
			lprintf(LOG_ERR, "ipmitool: https: failed to receive data");
			return -1;
		}
	}

	lprintf(LOG_DEBUG, "ipmitool: wss raw data: %s", buf2str(buffer, rlen));

	rsp->ccode = buffer[0];

	if (!is_bridge_command)
	{
		if (sizeof(rsp->data) < rlen - CCODE_LEN)
		{
			return -1;
		}
		memcpy(rsp->data, &buffer[CCODE_LEN], rlen - CCODE_LEN);
		rsp->data_len = rlen - CCODE_LEN;
	}
	else
	{
		if (sizeof(rsp->data) < rlen - CCODE_LEN - IPMI_MSG_RSP_HEADER_LEN - CHECKSUM_LEN)
		{
			return -1;
		}
		memcpy(rsp->data, &buffer[CCODE_LEN + IPMI_MSG_RSP_HEADER_LEN], rlen - CCODE_LEN - IPMI_MSG_RSP_HEADER_LEN - CHECKSUM_LEN);
		rsp->data_len = rlen - CCODE_LEN - IPMI_MSG_RSP_HEADER_LEN - CHECKSUM_LEN;
	}

	return 0;
}

static
struct ipmi_rs *
ipmi_https_sendrecv(struct ipmi_intf *intf,
                   struct ipmi_rq *req)
{
	static struct ipmi_rs rsp;
	struct ipmi_rs *ipmi_response = NULL;

	if (!intf)
		return NULL;

	if (!intf->opened && intf->open && intf->open(intf) < 0)
		return NULL;

	rsp.ccode = IPMI_CC_UNSPECIFIED_ERROR;
	rsp.data_len = 0;
	memset(rsp.data, 0, sizeof(rsp.data));

	if (ipmi_https_send_request(intf, req, &rsp) == 0)
    {
        ipmi_response = &rsp;
    }

	return ipmi_response;
}

static
int
ipmi_https_setup(struct ipmi_intf *intf)
{
	intf->fd = 0;
	return 0;
}

static bool is_full_wss_url(const char* str)
{
	return strncmp(str, "wss://", 6) == 0;
}

static
int
ipmi_https_open(struct ipmi_intf * intf)
{
	struct ipmi_session_params *params;
	struct ipmi_session *session;

	if (!intf)
		return -1;

	if (intf->opened)
		return intf->fd;

	params = &intf->ssn_params;

	if (!params->port)
		params->port = IPMI_HTTPS_PORT;
	if (!params->privlvl)
		params->privlvl = IPMI_SESSION_PRIV_ADMIN;
	if (!params->timeout)
		params->timeout = IPMI_HTTPS_TIMEOUT;

	if (!params->hostname || strlen((const char *)params->hostname) == 0) {
		lprintf(LOG_ERR, "No hostname specified!");
		goto fail;
	}

	ipmi_https_init();

	session = (struct ipmi_session *)malloc(sizeof (struct ipmi_session));
	if (!session) {
		lprintf(LOG_ERR, "ipmitool: malloc failure");
		goto fail;
	}

	intf->session = session;

	/* Setup our lanplus session state */
	memset(session, 0, sizeof(struct ipmi_session));
	session->timeout = params->timeout;
	session->sol_data.sequence_number = 1;

	intf->opened = 1;
	intf->abort = 0;

	lprintf(LOG_DEBUG, "IPMI HTTPS SESSION OPENED SUCCESSFULLY to %s:%d\n", params->hostname, params->port);

	CURLcode res;
	struct curl_slist *headers = NULL;
	char url[256];
	char token[64];
	long http_code = 0;

	if (curl == NULL) {
		lprintf(LOG_ERR, "ipmitool: https: not initialized");
		goto fail;
	}

	if (params->port == 443) {
		if (is_full_wss_url(params->hostname)) {
			/* User provided full wss uri, use it directly and append /ipmi to the URI */
			if (snprintf(url, sizeof(url), "%s/ipmi", params->hostname) >= (int)sizeof(url)) {
				lprintf(LOG_ERR, "ipmitool: https: url too long");
				goto fail;
			}
		}
		else {
			snprintf(url, sizeof(url), "wss://%s/ipmi", params->hostname);
		}
	}
	else {
		if (is_full_wss_url(params->hostname)) {
			lprintf(LOG_ERR, "ipmitool: https: invalid port %d when using full wss:// URI, please use port 443", params->port);
			goto fail;
		}
		snprintf(url, sizeof(url), "wss://%s:%d/ipmi", params->hostname, params->port);
	}

	snprintf(token, sizeof(token), "X-Auth-Token: %s", params->authcode_set);
	headers = curl_slist_append(headers, token);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L); /* websocket style */

	res = curl_easy_perform(curl);

	if (res != CURLE_OK) {
		lprintf(LOG_ERR, "ipmitool: https: failed to connect to %s, %s", url, curl_easy_strerror(res));
		if (res == CURLE_HTTP_RETURNED_ERROR) {
			/* The BMC port connected but http returned error */
			curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
			lprintf(LOG_ERR, "HTTP Response Code: %lu", http_code);
			lprintf(LOG_ERR, "Please check if your token expires.");
		}
		goto fail;
	}

	return intf->fd;

 fail:
	intf->close(intf);
	return -1;
}

static
void
ipmi_https_close(struct ipmi_intf *intf)
{
	ipmi_https_release();
	intf->opened = 0;
}

struct ipmi_intf ipmi_https_intf = {
	.name = "https",
	.desc = "OpenBMC HTTPS interface",
	.setup = ipmi_https_setup,
	.open = ipmi_https_open,
	.close = ipmi_https_close,
	.sendrecv = ipmi_https_sendrecv,
	.target_addr = IPMI_BMC_SLAVE_ADDR,
};
