#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <termios.h>
#include <pthread.h>
#include <curl/curl.h>
#include <assert.h>
#include <ipmitool/ipmi.h>
#include <ipmitool/log.h>
#include <stdbool.h>

#define SOL_ESCAPE_CHARACTER_DEFAULT '~'

int is_running = 1;
static struct termios _saved_tio;
static int _in_raw_mode = 0;

static int
processSolUserInput(
	char *input,
	u_int16_t buffer_length, CURL *curl)
{
	static int escape_pending = 0;
	static int last_was_cr = 1;
	int length = 0;
	int retval = 0;
	char ch;
	int i;
	char buffer[1024];
	int buffer_count = 0;
	size_t sent;

	/*
	 * Our first order of business is to check the input for escape
	 * sequences to act on.
	 */
	for (i = 0; i < buffer_length; ++i)
	{
		ch = input[i];

		if (escape_pending)
		{
			escape_pending = 0;

			/*
			 * Process a possible escape sequence.
			 */
			switch (ch)
			{
			case '.':
				printf("%c. [terminated ipmitool]\n",
					   SOL_ESCAPE_CHARACTER_DEFAULT);
				retval = 1;
				break;

			default:
				if (ch != SOL_ESCAPE_CHARACTER_DEFAULT)
					buffer[length++] =
						SOL_ESCAPE_CHARACTER_DEFAULT;
				buffer[length++] = ch;
			}
		}

		else
		{
			if (last_was_cr && (ch == SOL_ESCAPE_CHARACTER_DEFAULT))
			{
				escape_pending = 1;
				continue;
			}

			buffer[length++] = ch;
		}

		/*
		 * Normal character.  Record whether it was a newline.
		 */
		last_was_cr = (ch == '\r' || ch == '\n');
	}

	/*
	 * If there is anything left to process we dispatch it to the BMC,
	 * send intf->session->sol_data.max_outbound_payload_size bytes
	 * at a time.
	 */
	if (length)
	{
		buffer_count = length;
		int result = curl_ws_send(curl, buffer, buffer_count, &sent, 0,
								  CURLWS_BINARY);
		if (result != CURLE_OK)
		{
			lprintf(LOG_ERR, "ipmitool: https: failed to send data\n");
			return -1;
		}
	}

	return retval;
}

void https_sol_leave_raw_mode(void)
{
	if (!_in_raw_mode)
		return;
	if (tcsetattr(fileno(stdin), TCSADRAIN, &_saved_tio) == -1)
		perror("tcsetattr");
	else
		_in_raw_mode = 0;
}

void https_sol_enter_raw_mode(void)
{
	struct termios tio;
	if (tcgetattr(fileno(stdin), &tio) == -1)
	{
		perror("tcgetattr");
		return;
	}
	_saved_tio = tio;
	tio.c_iflag |= IGNPAR;
	tio.c_iflag &= ~(ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXANY | IXOFF);
	tio.c_lflag &= ~(ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHONL);
	//	#ifdef IEXTEN
	tio.c_lflag &= ~IEXTEN;
	//	#endif
	tio.c_oflag &= ~OPOST;
	tio.c_cc[VMIN] = 1;
	tio.c_cc[VTIME] = 0;
	if (tcsetattr(fileno(stdin), TCSADRAIN, &tio) == -1)
		perror("tcsetattr");
	else
		_in_raw_mode = 1;
}

int https_sol_send_and_receive(void *curl_handle)
{
	CURL *curl = (CURL *)curl_handle;

	int numRead;
	curl_socket_t sockfd;
	CURLcode res;
	fd_set read_fds;
	struct timeval tv;
	int retval;
	size_t rlen;
	const struct curl_ws_frame *meta;
	char buffer[4096 * 4] = {0}; // 16K

	res = curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &sockfd);
	if (!res && sockfd != CURL_SOCKET_BAD)
	{
		/* operate on sockfd */

		while (is_running)
		{
			FD_ZERO(&read_fds);
			FD_SET(0, &read_fds);
			FD_SET(sockfd, &read_fds);

			/* Wait up to half a second */
			tv.tv_sec = 0;
			tv.tv_usec = 500000;

			retval = select(sockfd + 1, &read_fds, NULL, NULL, &tv);

			if (retval)
			{
				if (retval == -1)
				{
					/* ERROR */
					perror("select");
					return -1;
				}

				/*
				 * Process input from the user
				 */
				if (FD_ISSET(0, &read_fds))
				{
					numRead = read(fileno(stdin),
								   buffer,
								   sizeof(buffer));
					if (numRead > 0)
					{
						int rc = processSolUserInput(buffer, numRead, curl);
						if (rc == 1)
						{
							is_running = 0;
						}
					}
				}
				/*
				 * Process input from the BMC
				 */
				else if (FD_ISSET(sockfd, &read_fds))
				{
					do
					{
						res = curl_ws_recv(curl, buffer, sizeof(buffer), &rlen, &meta);
						if (res == CURLE_OK)
						{
							if (rlen > 0)
							{
								fwrite(buffer, sizeof(char), rlen, stdout);
								fflush(stdout);
							}
							else
							{
								// Connection closed
								is_running = 0;
								break; // Exit the loop
							}
						}
						else if (res == CURLE_AGAIN)
						{
							continue;
						}
						else
						{
							// Handle other errors
							fprintf(stderr, "Error receiving WebSocket data: %s\n", curl_easy_strerror(res));
							return -1;
						}
					} while (rlen == sizeof(buffer)); // Continue if the buffer was full
				}

				/*
				 * ERROR in select
				 */
				else
				{
					lprintf(LOG_ERR, "Error: Select returned with nothing to read");
					is_running = 0;
				}
			}
		}
	}
	return 0;
}

static bool is_full_wss_url(const char* str)
{
	return strncmp(str, "wss://", 6) == 0;
}

int ipmi_https_sol_main(const char *hostname, const char *password, int port)
{
	CURL *curl = NULL;
	CURLcode res;
	struct curl_slist *headers = NULL;
	char token[64];
	char url[256];

	curl_global_init(CURL_GLOBAL_DEFAULT);
	curl = curl_easy_init();
	assert(curl);

	if (port == 0 || port == 443) {
		if (is_full_wss_url(hostname)) {
			/* User provided full wss uri, use it directly and append /console/default to the URI */
			if (snprintf(url, sizeof(url), "%s/console/default", hostname) >= (int)sizeof(url)) {
				lprintf(LOG_ERR, "ipmitool: https: url too long");
				return -1;
			}
		}
		else {
			snprintf(url, sizeof(url), "wss://%s/console/default", hostname);
		}
	}
	else {
		if (is_full_wss_url(hostname)) {
			lprintf(LOG_ERR, "ipmitool: https: invalid port %d when using full wss:// URI, please use port 443", port);
			return -1;
		}
		snprintf(url, sizeof(url), "wss://%s:%d/console/default", hostname, port);
	}

	snprintf(token, sizeof(token), "X-Auth-Token: %s", password);
	headers = curl_slist_append(headers, token);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L); /* websocket style */
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

	res = curl_easy_perform(curl);
	if (res != CURLE_OK)
	{
		long http_code = 0;
		lprintf(LOG_ERR, "ipmitool: https-sol: failed to connect to %s, %s", url, curl_easy_strerror(res));
		curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
		lprintf(LOG_ERR, "HTTP Response Code: %lu", http_code);
		if (res == CURLE_HTTP_RETURNED_ERROR) {
			/* The BMC port connected but http returned error */
			lprintf(LOG_ERR, "Please check if your token expires.");
		}
		curl_slist_free_all(headers);
		curl_easy_cleanup(curl);
		curl_global_cleanup();
		return -1;
	}

	printf("[Type '~.' to exit ipmi https sol]\n");

	https_sol_enter_raw_mode();

	https_sol_send_and_receive(curl);

	https_sol_leave_raw_mode();

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	curl_global_cleanup();
	return 0;
}
