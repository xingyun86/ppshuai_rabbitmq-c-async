/*
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MIT
 *
 * Portions created by Alan Antonuk are Copyright (c) 2012-2013
 * Alan Antonuk. All Rights Reserved.
 *
 * Portions created by VMware are Copyright (c) 2007-2012 VMware, Inc.
 * All Rights Reserved.
 *
 * Portions created by Tony Garnock-Jones are Copyright (c) 2009-2010
 * VMware, Inc. and Tony Garnock-Jones. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * ***** END LICENSE BLOCK *****
 */

#include <string>
#include <stdio.h>
#include "amqp_async.h"

#include <thread>

#define SUMMARY_EVERY_US 1000000
std::string g_queue_name = "";
#include <deque>
#include <mutex>
std::mutex g_msg_locker;
std::deque<std::string> g_msg_queue;

#ifdef _MSC_VER
uint64_t my_amqp_get_monotonic_timestamp(void) {
	static double NS_PER_COUNT = 0;
	LARGE_INTEGER perf_count;

	if (0 == NS_PER_COUNT) {
		LARGE_INTEGER perf_frequency;
		if (!QueryPerformanceFrequency(&perf_frequency)) {
			return 0;
		}
		NS_PER_COUNT = (double)AMQP_NS_PER_S / perf_frequency.QuadPart;
	}

	if (!QueryPerformanceCounter(&perf_count)) {
		return 0;
	}

	return (uint64_t)(perf_count.QuadPart * NS_PER_COUNT);
}
#else
#include <time.h>
uint64_t my_amqp_get_monotonic_timestamp(void) {
#ifdef __hpux
return (uint64_t)gethrtime();
#else
struct timespec tp;
if (-1 == clock_gettime(CLOCK_MONOTONIC, &tp)) {
	return 0;
}

return ((uint64_t)tp.tv_sec * AMQP_NS_PER_S + (uint64_t)tp.tv_nsec);
#endif
}
#endif /* _MSC_VER */

__inline static
bool send_heart_beat(struct amqp_connection_state_t_ * conn)
{
	int err = 0;
	bool ret = true;
	uint64_t past = 0;
	amqp_frame_t hb = { 0 };
	if (conn->heartbeat == 0)
	{
		return ret;
	}
	uint64_t dst = conn->next_send_heartbeat.time_point_ns + (conn->heartbeat / 2);
	past = my_amqp_get_monotonic_timestamp();
	if (past > dst)
	{
		hb.channel = 0;
		hb.frame_type = AMQP_FRAME_HEARTBEAT;
		ret = ((err = amqp_send_frame(conn, &hb)) == AMQP_STATUS_OK);
		if (ret == false)
		{
			printf("Error is: %s\n", amqp_error_string(err));
		}
		else
		{
			printf("Heartbeat sent(%ldd)\n", conn->next_send_heartbeat.time_point_ns / 1000000);
		}
	}
	else
	{
		ret = true;
	}
	return ret;
}
__inline static
void recv_run_loop(amqp_connection_state_t conn, void * state) {
	std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
	int received = 0;
	int previous_received = 0;
	std::chrono::steady_clock::time_point previous_report_time = start_time;
	std::chrono::steady_clock::time_point next_summary_time = start_time + std::chrono::microseconds(SUMMARY_EVERY_US);

	amqp_frame_t frame;
	std::chrono::steady_clock::time_point now;
	std::chrono::steady_clock::duration delay;
	struct timeval timeout = { 0, 15000 };
	while((*((int*)state) == 1))
	{
		amqp_rpc_reply_t ret_consume_message;
		amqp_envelope_t envelope;

		now = std::chrono::steady_clock::now();
		delay = now - next_summary_time;
		if (delay > std::chrono::steady_clock::duration::zero()) 
		{
			int countOverInterval = received - previous_received;
			double intervalRate = (double)(countOverInterval * SUMMARY_EVERY_US) / (std::chrono::duration_cast<std::chrono::microseconds>(now - previous_report_time).count());
			printf("%d ms: Received %d - %d since last report (%.2f Hz)\n",
				std::chrono::duration_cast<std::chrono::microseconds>(delay).count()/1000, received, countOverInterval,
				intervalRate);

			previous_received = received;
			previous_report_time = now;
			next_summary_time = start_time + std::chrono::microseconds(SUMMARY_EVERY_US);
		}

		amqp_maybe_release_buffers(conn);
		ret_consume_message = amqp_consume_message(conn, &envelope, &timeout, 0);

		if (AMQP_RESPONSE_NORMAL != ret_consume_message.reply_type)
		{
			if (AMQP_RESPONSE_LIBRARY_EXCEPTION == ret_consume_message.reply_type &&
				AMQP_STATUS_UNEXPECTED_STATE == ret_consume_message.library_error) {
				if (AMQP_STATUS_OK != amqp_simple_wait_frame(conn, &frame)) {
					return;
				}

				if (AMQP_FRAME_METHOD == frame.frame_type) {
					switch (frame.payload.method.id) {
					case AMQP_BASIC_ACK_METHOD:
						/* if we've turned publisher confirms on, and we've published a
						 * message here is a message being confirmed.
						 */
						break;
					case AMQP_BASIC_RETURN_METHOD:
						/* if a published message couldn't be routed and the mandatory
						 * flag was set this is what would be returned. The message then
						 * needs to be read.
						 */
					{
						amqp_message_t message;
						ret_consume_message = amqp_read_message(conn, frame.channel, &message, 0);
						if (AMQP_RESPONSE_NORMAL != ret_consume_message.reply_type) {
							return;
						}
						std::string message_body((const char *)message.body.bytes, message.body.len);
						amqp_destroy_message(&message);
						printf("message=%s\n");
					}

					break;

					case AMQP_CHANNEL_CLOSE_METHOD:
						/* a channel.close method happens when a channel exception occurs,
						 * this can happen by publishing to an exchange that doesn't exist
						 * for example.
						 *
						 * In this case you would need to open another channel redeclare
						 * any queues that were declared auto-delete, and restart any
						 * consumers that were attached to the previous channel.
						 */
						return;

					case AMQP_CONNECTION_CLOSE_METHOD:
						/* a connection.close method happens when a connection exception
						 * occurs, this can happen by trying to use a channel that isn't
						 * open for example.
						 *
						 * In this case the whole connection must be restarted.
						 */
						return;

					default:
						fprintf(stderr, "An unexpected method was received %u\n",
							frame.payload.method.id);
						return;
					}
				}
			}

		}
		else {
			printf("channel=%d,consumer_tag=%s,delivery_tag=%lld,redelivered=%d,exchange=%s,routing_key=%s:\n",
				envelope.channel,
				std::string((const char *)envelope.consumer_tag.bytes, envelope.consumer_tag.len).c_str(),
				envelope.delivery_tag,
				envelope.redelivered,
				std::string((const char*)envelope.exchange.bytes, envelope.exchange.len).c_str(),
				std::string((const char*)envelope.routing_key.bytes, envelope.routing_key.len).c_str()
				);
			printf("message:body=%s\n", std::string((const char*)envelope.message.body.bytes, envelope.message.body.len).c_str());
			printf("property:timestamp=%lld,_flags=%d,content_type=%s,content_encoding=%s,correlation_id=%s,reply_to=%s,expiration=%s,type=%s,"
				"user_id=%s,app_id=%s,cluster_id=%s"
				"\n",
				envelope.message.properties.timestamp,
				envelope.message.properties._flags,
				std::string((const char*)envelope.message.properties.content_type.bytes, envelope.message.properties.content_type.len).c_str(),
				std::string((const char*)envelope.message.properties.content_encoding.bytes, envelope.message.properties.content_encoding.len).c_str(),
				std::string((const char*)envelope.message.properties.correlation_id.bytes, envelope.message.properties.correlation_id.len).c_str(),
				std::string((const char*)envelope.message.properties.reply_to.bytes, envelope.message.properties.reply_to.len).c_str(),
				std::string((const char*)envelope.message.properties.expiration.bytes, envelope.message.properties.expiration.len).c_str(),
				std::string((const char*)envelope.message.properties.message_id.bytes, envelope.message.properties.message_id.len).c_str(),
				std::string((const char*)envelope.message.properties.type.bytes, envelope.message.properties.type.len).c_str(),
				std::string((const char*)envelope.message.properties.user_id.bytes, envelope.message.properties.user_id.len).c_str(),
				std::string((const char*)envelope.message.properties.app_id.bytes, envelope.message.properties.app_id.len).c_str(),
				std::string((const char*)envelope.message.properties.cluster_id.bytes, envelope.message.properties.cluster_id.len).c_str()
			);
			amqp_destroy_envelope(&envelope);
		}

		received++;

		//std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
}
__inline static
int recv_rmq_test(void* state)
{
	std::string hostname = "10.0.3.252";// "10.0.1.20";
	std::string vhost = "/"; //AMQP_DEFAULT_VHOST;//"order";
	std::string username = "guest";// "order_producer";
	std::string password = "guest";// "order_producer123";
	int port = 5672;
	std::string exchange = "ppstest.amq.direct";
	std::string exchange_type = "direct";
	std::string routingkey = "ppstest.test.queue";
	std::string queuename = "";
	int channel_max = AMQP_DEFAULT_MAX_CHANNELS;
	int frame_max = AMQP_DEFAULT_FRAME_SIZE;
	int heart_beat = 60;// AMQP_DEFAULT_HEARTBEAT;
	struct timeval timeout = { 0,15000 };
	amqp_channel_t channel = 1;
	amqp_boolean_t passive = false;
	amqp_boolean_t durable = false;
	amqp_boolean_t exclusive = false;
	amqp_boolean_t auto_delete = true;

	amqp_boolean_t no_local = false;
	amqp_boolean_t no_ack = true;

	amqp_boolean_t internal = false;

	amqp_connection_state_t conn = amqp_new_connection();

	amqp_socket_t* socket = amqp_tcp_socket_new(conn);
	if (socket == nullptr)
	{
		die("creating TCP socket");
	}

	int status = amqp_socket_open_noblock(socket, hostname.c_str(), port, &timeout);
	if (status != AMQP_STATUS_OK)
	{
		die_on_error(status, "opening TCP socket");
	}
	amqp_rpc_reply_t amqp_rpc_reply_login = amqp_login(conn, vhost.c_str(), channel_max, frame_max, heart_beat, AMQP_SASL_METHOD_PLAIN, username.c_str(), password.c_str());
	die_on_amqp_error(amqp_rpc_reply_login, "Logging in");

	amqp_channel_open_ok_t* amqp_channel_open_ok = amqp_channel_open(conn, channel);
	amqp_rpc_reply_t amqp_rpc_reply_channel_open = amqp_get_rpc_reply(conn);
	die_on_amqp_error(amqp_rpc_reply_channel_open, "Opening channel");

	amqp_exchange_declare_ok_t* amqp_exchange_declare_ok = amqp_exchange_declare(conn, channel, amqp_cstring_bytes(exchange.c_str()), amqp_cstring_bytes(exchange_type.c_str()), passive, durable, auto_delete, internal, amqp_empty_table);
	amqp_rpc_reply_t amqp_rpc_reply_exchange_declare = amqp_get_rpc_reply(conn);
	die_on_amqp_error(amqp_rpc_reply_exchange_declare, "Declaring exchange");

	/*amqp_queue_declare_ok_t* amqp_queue_declare_ok = amqp_queue_declare(conn, channel, amqp_empty_bytes, passive, durable, exclusive, auto_delete, amqp_empty_table);
	amqp_rpc_reply_t amqp_rpc_reply_queue_declare = amqp_get_rpc_reply(conn);
	die_on_amqp_error(amqp_rpc_reply_queue_declare, "Declaring queue");
	
	if (amqp_queue_declare_ok->queue.bytes == nullptr)
	{
		fprintf(stderr, "Out of memory while copying queue name");
		return 1;
	}
	queuename = std::string((const char*)amqp_queue_declare_ok->queue.bytes, amqp_queue_declare_ok->queue.len);

	amqp_queue_bind_ok_t* amqp_queue_bind_ok = amqp_queue_bind(conn, channel, amqp_cstring_bytes(queuename.c_str()), amqp_cstring_bytes(exchange.c_str()), amqp_cstring_bytes(routingkey.c_str()), amqp_empty_table);
	amqp_rpc_reply_t amqp_rpc_reply_queue_bind = amqp_get_rpc_reply(conn);
	die_on_amqp_error(amqp_rpc_reply_queue_bind, "Binding queue");*/

	while (g_queue_name.length() == 0 && (*((int*)state) == 1))
	{
		std::this_thread::sleep_for(std::chrono::microseconds(100000));
	}
	queuename = g_queue_name;
	if (queuename.length() > 0)
	{
		amqp_basic_consume_ok_t* amqp_basic_consume_ok = amqp_basic_consume(conn, channel, amqp_cstring_bytes(queuename.c_str()), amqp_empty_bytes, no_local, no_ack, exclusive, amqp_empty_table);
		amqp_rpc_reply_t amqp_rpc_reply_basic_consume = amqp_get_rpc_reply(conn);
		die_on_amqp_error(amqp_rpc_reply_basic_consume, "Consuming");

		recv_run_loop(conn, state);

		amqp_rpc_reply_t amqp_rpc_reply_channel_close = amqp_channel_close(conn, channel, AMQP_REPLY_SUCCESS);
		die_on_amqp_error(amqp_rpc_reply_channel_close, "Closing channel");
		amqp_rpc_reply_t amqp_rpc_reply_connection_close = amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
		die_on_amqp_error(amqp_rpc_reply_connection_close, "Closing connection");
		status = amqp_destroy_connection(conn);
		die_on_error(status, "Ending connection");

		printf("Recv rmq msg finished!\n");
	}
	else
	{
		printf("Recv rmq msg not finished, queuename is empty!\n");
	}
	return 0;
}
__inline static 
void send_run_loop(void * state, amqp_connection_state_t conn, amqp_channel_t channel, char const* exchange_name, char const* queue_name,	int rate_limit) {
	std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
	int sent = 0;
	int previous_sent = 0;
	std::chrono::steady_clock::time_point previous_report_time = start_time;
	std::chrono::steady_clock::time_point next_summary_time = start_time + std::chrono::microseconds(SUMMARY_EVERY_US);

	std::string message = "";
		amqp_boolean_t mandatory = false;
	amqp_boolean_t immediate = false;
	struct amqp_basic_properties_t_* properties = nullptr;

	while(*((int*)state) == 1)
	{
		g_msg_locker.lock();
		if (g_msg_queue.empty())
		{
			g_msg_locker.unlock();
			std::this_thread::sleep_for(std::chrono::microseconds(1000000));
			if (send_heart_beat(conn) == false)
			{
				break;
			}
			continue;
		}
		else
		{
			message = g_msg_queue.front();
			g_msg_queue.pop_front();
			g_msg_locker.unlock();

			std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
			std::chrono::steady_clock::duration delay = now - start_time;

			int status = amqp_basic_publish(conn, channel,
				amqp_cstring_bytes(exchange_name),
				amqp_cstring_bytes(queue_name), mandatory, immediate, properties,
				amqp_cstring_bytes(message.c_str()));
			die_on_error(status, "Publishing");
			sent++;
			if (delay != std::chrono::steady_clock::duration::zero())
			{
				int countOverInterval = sent - previous_sent;
				double intervalRate = (double)(countOverInterval * SUMMARY_EVERY_US) / (std::chrono::duration_cast<std::chrono::microseconds>(now - previous_report_time).count());
				printf("%d ms: Sent %d - %d since last report (%.2f Hz)\n",
					std::chrono::duration_cast<std::chrono::microseconds>(delay).count() / 1000, sent, countOverInterval,
					intervalRate);

				previous_sent = sent;
				previous_report_time = now;
				next_summary_time += std::chrono::microseconds(SUMMARY_EVERY_US);
			}
			else
			{
				delay = std::chrono::microseconds(SUMMARY_EVERY_US);
			}
			while (((sent * SUMMARY_EVERY_US) / std::chrono::duration_cast<std::chrono::microseconds>(delay).count()) > rate_limit && (*((int*)state) == 1))
			{
				std::this_thread::sleep_for(std::chrono::microseconds(2000));
				now = std::chrono::steady_clock::now();
				delay = now - start_time;
				if (delay == std::chrono::steady_clock::duration::zero())
				{
					delay = std::chrono::microseconds(SUMMARY_EVERY_US);
				}
			}
		}
		
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}

	{
		std::chrono::steady_clock::time_point stop_time = std::chrono::steady_clock::now();
		std::chrono::steady_clock::duration total_delta = std::chrono::duration_cast<std::chrono::microseconds>(stop_time - start_time);

		printf("PRODUCER - Message count: %d\n", sent);
		printf("Total time, milliseconds: %d\n", total_delta.count() / 1000);
		printf("Overall messages-per-second: %g\n",	(sent / (total_delta.count() / 1000000.0)));
	}
}
__inline static 
int send_rmq_test(void* state)
{
	std::string hostname = "10.0.3.252";// "10.0.1.20";
	std::string vhost = "/";// AMQP_DEFAULT_VHOST;//"order";
	std::string username = "guest";// "order_producer";
	std::string password = "guest";// "order_producer123";
	int port = 5672;
	std::string exchange = "ppstest.amq.direct";
	std::string exchange_type = "direct";
	std::string routingkey = "ppstest.test.queue";
	std::string queuename = "";
	int channel_max = AMQP_DEFAULT_MAX_CHANNELS;
	int frame_max = AMQP_DEFAULT_FRAME_SIZE;
	int heart_beat = 60;// AMQP_DEFAULT_HEARTBEAT;
	struct timeval timeout = { 0,15000 };
	amqp_channel_t channel = 1;
	amqp_boolean_t passive = false;
	amqp_boolean_t durable = false;
	amqp_boolean_t exclusive = false;
	amqp_boolean_t auto_delete = true;

	amqp_boolean_t no_local = false;
	amqp_boolean_t no_ack = true;

	amqp_boolean_t internal = false;

	amqp_connection_state_t conn = amqp_new_connection();

	amqp_socket_t* socket = amqp_tcp_socket_new(conn);
	if (socket == nullptr)
	{
		die("creating TCP socket");
	}

	int status = amqp_socket_open_noblock(socket, hostname.c_str(), port, &timeout);
	if (status != AMQP_STATUS_OK)
	{
		die_on_error(status, "opening TCP socket");
	}
	amqp_rpc_reply_t amqp_rpc_reply_login = amqp_login(conn, vhost.c_str(), channel_max, frame_max, heart_beat, AMQP_SASL_METHOD_PLAIN, username.c_str(), password.c_str());
	die_on_amqp_error(amqp_rpc_reply_login, "Logging in");

	amqp_channel_open_ok_t* amqp_channel_open_ok = amqp_channel_open(conn, channel);
	amqp_rpc_reply_t amqp_rpc_reply_channel_open = amqp_get_rpc_reply(conn);
	die_on_amqp_error(amqp_rpc_reply_channel_open, "Opening channel");

	amqp_exchange_declare_ok_t* amqp_exchange_declare_ok = amqp_exchange_declare(conn, channel, amqp_cstring_bytes(exchange.c_str()), amqp_cstring_bytes(exchange_type.c_str()), passive, durable, auto_delete, internal, amqp_empty_table);
	amqp_rpc_reply_t amqp_rpc_reply_exchange_declare = amqp_get_rpc_reply(conn);
	die_on_amqp_error(amqp_rpc_reply_exchange_declare, "Declaring exchange");

	amqp_queue_declare_ok_t* amqp_queue_declare_ok = amqp_queue_declare(conn, channel, amqp_empty_bytes, passive, durable, exclusive, auto_delete, amqp_empty_table);
	amqp_rpc_reply_t amqp_rpc_reply_queue_declare = amqp_get_rpc_reply(conn);
	die_on_amqp_error(amqp_rpc_reply_queue_declare, "Declaring queue");

	if (amqp_queue_declare_ok->queue.bytes == nullptr)
	{
		fprintf(stderr, "Out of memory while copying queue name");
		return 1;
	}
	queuename = std::string((const char *)amqp_queue_declare_ok->queue.bytes, amqp_queue_declare_ok->queue.len);
	g_queue_name = queuename;
	printf("g_queue_name = %s\n", g_queue_name.c_str());

	amqp_queue_bind_ok_t* amqp_queue_bind_ok = amqp_queue_bind(conn, channel, amqp_cstring_bytes(queuename.c_str()), amqp_cstring_bytes(exchange.c_str()), amqp_cstring_bytes(routingkey.c_str()), amqp_empty_table);
	amqp_rpc_reply_t amqp_rpc_reply_queue_bind = amqp_get_rpc_reply(conn);
	die_on_amqp_error(amqp_rpc_reply_queue_bind, "Binding queue");

	send_run_loop(state, conn, channel, exchange.c_str(), routingkey.c_str(), 1000);
	/*amqp_basic_consume_ok_t* amqp_basic_consume_ok = amqp_basic_consume(conn, channel, queuename, amqp_empty_bytes, no_local, no_ack, exclusive, amqp_empty_table);
	amqp_rpc_reply_t amqp_rpc_reply_basic_consume = amqp_get_rpc_reply(conn);
	die_on_amqp_error(amqp_rpc_reply_basic_consume, "Consuming");

	run_loop(conn);*/

	amqp_rpc_reply_t amqp_rpc_reply_channel_close = amqp_channel_close(conn, channel, AMQP_REPLY_SUCCESS);
	die_on_amqp_error(amqp_rpc_reply_channel_close, "Closing channel");
	amqp_rpc_reply_t amqp_rpc_reply_connection_close = amqp_connection_close(conn, AMQP_REPLY_SUCCESS);
	die_on_amqp_error(amqp_rpc_reply_connection_close, "Closing connection");
	status = amqp_destroy_connection(conn);
	die_on_error(status, "Ending connection");

	printf("Send rmq msg finished!\n");

	return 0;
}
int main(int argc, char** argv)
{
	int state = 1;
	/*std::thread recv_rmq_thread = std::thread([](void* p) {
		recv_rmq_test(p);
		}, &state);*/
	std::thread send_rmq_thread = std::thread([](void* p) {
		send_rmq_test(p);
		}, &state);
	std::thread make_msg_thread = std::thread([](void* p) {
		while (*((int*)p) == 1)
		{
			g_msg_locker.lock();
			g_msg_queue.push_back("");// "msg_" + std::to_string(time(0)));
			printf("push msg %s\n", g_msg_queue.back().c_str());
			g_msg_locker.unlock();
			std::this_thread::sleep_for(std::chrono::seconds(120));
		}
		}, &state);
	getchar();
	state = 0;
	make_msg_thread.join();
	send_rmq_thread.join();
	//recv_rmq_thread.join();
}