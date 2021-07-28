#define VISIBILITY_PRIVATE

#include "event_capture.h"
#include "sys_call_test.h"
#include "scoped_configuration.h"
#include "feature_manager.h"

#include "Poco/Exception.h"
#include "Poco/Net/FTPStreamFactory.h"
#include "Poco/Net/HTTPSStreamFactory.h"
#include "Poco/Net/HTTPStreamFactory.h"
#include "Poco/NullStream.h"
#include "Poco/Path.h"
#include "Poco/StreamCopier.h"
#include "Poco/URI.h"
#include "Poco/URIStreamOpener.h"

#include <Poco/NumberFormatter.h>
#include <Poco/NumberParser.h>
#include <Poco/PipeStream.h>
#include <Poco/Process.h>
#include <Poco/StringTokenizer.h>

#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <event.h>
#include <fcntl.h>
#include <gtest.h>
#include <list>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>

// For HTTP server
#include "analyzer_thread.h"
#include "configuration_manager.h"
#include "connectinfo.h"
#include "procfs_parser.h"
#include "protostate.h"
#include "sinsp_int.h"

#include "test-helpers/scoped_config.h"
#include "test-helpers/http_server.h"

#include <Poco/Net/HTTPRequestHandler.h>
#include <Poco/Net/HTTPRequestHandlerFactory.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPServer.h>
#include <Poco/Net/HTTPServerRequest.h>
#include <Poco/Net/HTTPServerRequestImpl.h>
#include <Poco/Net/HTTPServerResponse.h>
#include <Poco/Net/SecureServerSocket.h>
#include <Poco/Net/SecureStreamSocket.h>
#include <Poco/Net/ServerSocket.h>

#include <arpa/inet.h>
#include <array>
#include <memory>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace std;
using namespace test_helpers;

using Poco::NumberFormatter;
using Poco::NumberParser;
using Poco::SharedPtr;

using Poco::Exception;
using Poco::NullOutputStream;
using Poco::Path;
using Poco::StreamCopier;
using Poco::URI;
using Poco::URIStreamOpener;
using Poco::Net::FTPStreamFactory;
using Poco::Net::HTTPSStreamFactory;
using Poco::Net::HTTPStreamFactory;

using Poco::Net::HTTPResponse;
using Poco::Net::HTTPServer;
using Poco::Net::HTTPServerParams;
using Poco::Net::HTTPServerRequest;
using Poco::Net::HTTPServerResponse;
using Poco::Net::SecureServerSocket;
using Poco::Net::SecureStreamSocket;
using Poco::Net::ServerSocket;

#define SITE "www.google.com"
#define SITE1 "www.yahoo.com"
#define BUFSIZE 1024
#define N_CONNECTIONS 2
#define N_REQS_PER_CONNECTION 10

/*
 * error - wrapper for perror
 */
void error(char* msg)
{
	perror(msg);
	exit(0);
}

//
// SSL server stuff
//

///
/// Read the SSL certificate and key given as parameters.
///
/// The cert and key are read into the provided SSL context.
///
void load_certs(SSL_CTX* ctx, string cert_fn, string key_fn)
{
	int ret;

	FILE* certf = fopen(cert_fn.c_str(), "r");
	FILE* keyf = fopen(key_fn.c_str(), "r");

	// Read the cert and key
	X509* cert_x509 = PEM_read_X509(certf, NULL, NULL, NULL);
	EVP_PKEY* pkey = PEM_read_PrivateKey(keyf, NULL, NULL, NULL);

	if (cert_x509 == nullptr)
	{
		cerr << "Error reading certificate" << endl;
		ERR_print_errors_fp(stderr);
		goto cleanup;
	}
	if (pkey == nullptr)
	{
		cerr << "Error reading private key" << endl;
		ERR_print_errors_fp(stderr);
		goto cleanup;
	}

	// Set the cert and key in the context
	ret = SSL_CTX_use_certificate(ctx, cert_x509);
	if (ret <= 0)
	{
		cerr << "Error using certificate: " << ret << endl;
		ERR_print_errors_fp(stderr);
		goto cleanup;
	}
	ret = SSL_CTX_use_PrivateKey(ctx, pkey);
	if (ret <= 0)
	{
		cerr << "Error using private key: " << ret << endl;
		ERR_print_errors_fp(stderr);
		goto cleanup;
	}

cleanup:
	fclose(certf);
	fclose(keyf);
}

///
/// Encapsulates an SSL server socket
///
class ssl_socket
{
	int sock_fd = -1;
	int sock_err = 0;
	SSL_CTX* ctx = nullptr;
	bool run_server = false;

public:
	ssl_socket()
	{
		SSL_load_error_strings();
		SSL_library_init();
		OpenSSL_add_ssl_algorithms();

		// Create the SSL context
		ctx = SSL_CTX_new(SSLv23_server_method());
		if (!ctx)
		{
			cerr << "Unable to build SSL context" << endl;
			ERR_print_errors_fp(stderr);
			sock_err = -1;
			return;
		}

		load_certs(ctx, "certificate.pem", "key.pem");
	}

	~ssl_socket()
	{
		if (run_server)
		{
			stop();
		}
		if (sock_fd > 0)
		{
			close(sock_fd);
			sock_fd = -1;
		}
		SSL_CTX_free(ctx);
		EVP_cleanup();
	}

	bool error() { return sock_err != 0; }

	void start(uint16_t port)
	{
		uint32_t MAX_WAIT_MS = 5 * 1000;
		run_server = true;
		bool server_started = false;
		std::mutex mtx;
		std::condition_variable cv;

		thread t(
		    [this, &server_started, &mtx, &cv](uint16_t port) {
				// Create the socket and begin listening

				struct sockaddr_in addr;
				addr.sin_family = AF_INET;
				addr.sin_addr.s_addr = htonl(INADDR_ANY);
				addr.sin_port = htons(port);

				int s = socket(addr.sin_family, SOCK_STREAM, 0);
				if (s < 0)
				{
					cerr << "Unable to create socket: " << s << endl;
					sock_err = s;
					return;
				}

				if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) < 0)
				{
					cerr << "Unable to bind to address: " << s << endl;
					sock_err = s;
					close(s);
					return;
				}

				int ret = listen(s, 1);
				if (ret < 0)
				{
					cerr << "Unable to listen on socket: " << ret << endl;
					sock_err = ret;
					close(s);
					return;
				}

				sock_fd = s;
				std::unique_lock<std::mutex> lck(mtx);
				server_started = true;
				cv.notify_one();  // Let the parent function know the server is ready to roll
				lck.unlock();

				while (run_server)
				{
					struct sockaddr_in addr;
					uint32_t len = sizeof(addr);
					SSL* ssl = nullptr;

					int conn_fd = accept(sock_fd, (struct sockaddr*)&addr, &len);
					if (conn_fd < 0)
					{
						cerr << "Error while accepting incoming connection: " << conn_fd << endl;
						sock_err = conn_fd;
						run_server = false;
						continue;
					}

					ssl = SSL_new(ctx);
					SSL_set_fd(ssl, conn_fd);
					int ret = SSL_accept(ssl);

					if (ret <= 0)
					{
						cerr << "SSL error accepting incoming connection: " << ret << endl;
						ERR_print_errors_fp(stderr);
						run_server = false;
						continue;
					}
					else
					{
						char buf[128] = {};
						SSL_read(ssl, buf, sizeof(buf));
						SSL_write(ssl, buf, strlen(buf));
						sleep(1);
					}

					close(conn_fd);
					SSL_free(ssl);
				}
		    },
		    port);
		t.detach();

		// Wait for the server to actually start before returning
		std::unique_lock<std::mutex> guard(mtx);
		while (!server_started)
		{
			if (cv.wait_for(guard, std::chrono::milliseconds(MAX_WAIT_MS)) == cv_status::timeout)
			{
				cerr << "Never got notified that the server got started!" << endl;
				ASSERT(false);
			}
		}
	}

	void stop()
	{
		run_server = false;
		close(sock_fd);
		sock_fd = -1;
	}
};

///
/// So that callers don't have to remember all the magic words to get the socket.
///
unique_ptr<SecureServerSocket> get_ssl_socket(uint16_t port)
{
	return unique_ptr<SecureServerSocket>(new SecureServerSocket(port, 64 /* backlog */));
}

///
/// Send an ssl request to the given localhost port.
///
/// This will establish an SSL connection, send a string over that connection,
/// and then continue reading replies until the socket is closed.
///
/// Yeah, I know all this OpenSSL API code is gross. But the Poco version
/// was crashing in mysterious ways.
///
/// @param[in]  port  The server port to connect to
///
/// @return  true  The request was made successfully and a response was received
/// @return false  The request encountered an error
///
bool localhost_ssl_request(uint16_t port)
{
	BIO* server = nullptr;
	SSL_CTX* ctx = nullptr;
	SSL* ssl = nullptr;

	// Build the context
	ctx = SSL_CTX_new(SSLv23_method());
	if (ctx == nullptr)
	{
		cerr << "Unable to build SSL context for client" << endl;
		ERR_print_errors_fp(stderr);
		return false;
	}

	// Set up the context
	SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
	const long flags = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION;
	SSL_CTX_set_options(ctx, flags);
	int res = SSL_CTX_load_verify_locations(ctx, "certificate.pem", nullptr);
	if (res != 1)
	{
		cerr << "Couldn't load certificate: " << res << endl;
		ERR_print_errors_fp(stderr);
		return false;
	}

	// Create the server IO stream and SSL object
	server = BIO_new_ssl_connect(ctx);
	if (server == nullptr)
	{
		cerr << "Couldn't create SSL BIO object" << endl;
		ERR_print_errors_fp(stderr);
		return false;
	}

	stringstream ss;
	ss << "127.0.0.1:" << port;

	BIO_set_conn_hostname(server, ss.str().c_str());
	BIO_get_ssl(server, &ssl);

	if (ssl == nullptr)
	{
		cerr << "Couldn't create SSL object" << endl;
		ERR_print_errors_fp(stderr);
		return false;
	}

	SSL_set_tlsext_host_name(ssl, "127.0.0.1");

	// Connect the IO stream to the server
	res = BIO_do_connect(server);
	if (res != 1)
	{
		cerr << "Client connect failed: " << res << endl;
		return false;
	}

	res = BIO_do_handshake(server);
	if (res != 1)
	{
		cerr << "Client handshake failed: " << res << endl;
		return false;
	}

	// Send the payload
	BIO_puts(server, "Hello from Sysdig test SSL client!");

	// Read the responses until the socket is shut down
	int len = 0;
	char buf[256] = {};

	while (true)
	{
		len = BIO_read(server, buf, sizeof(buf));
		if (len <= 0)
		{
			break;
		}
	}

	// Cleanup
	if (server != nullptr)
	{
		BIO_free_all(server);
	}

	if (ctx != nullptr)
	{
		SSL_CTX_free(ctx);
	}

	return true;
}

TEST_F(sys_call_test, net_web_requests)
{
	int nconns = 0;
	int mytid = getpid();

	//
	// FILTER
	//
	event_filter_t filter = [&](sinsp_evt* evt) { return m_tid_filter(evt); };

	//
	// TEST CODE
	//
	run_callback_t test = [&](sinsp* inspector) {
		int sockfd, n, j, k;
		struct sockaddr_in serveraddr;
		struct hostent* server;
		char* hostname = (char*)SITE;
		int portno = 80;
		string reqstr;
		char reqbody[BUFSIZE] = "GET / HTTP/1.0\n\n";

		// get the server's DNS entry
		server = gethostbyname(hostname);
		ASSERT_TRUE(server) << "ERROR, no such host as " << hostname;

		for (j = 0; j < N_CONNECTIONS; j++)
		{
			// socket: create the socket
			sockfd = socket(AF_INET, SOCK_STREAM, 0);
			if (sockfd < 0)
			{
				error((char*)"ERROR opening socket");
			}

			// build the server's Internet address
			bzero((char*)&serveraddr, sizeof(serveraddr));
			serveraddr.sin_family = AF_INET;
			bcopy((char*)server->h_addr, (char*)&serveraddr.sin_addr.s_addr, server->h_length);
			serveraddr.sin_port = htons(portno);

			// create a connection with the server
			if (connect(sockfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
			{
				error((char*)"ERROR connecting");
			}

			for (k = 0; k < N_REQS_PER_CONNECTION; k++)
			{
				reqstr = string("GET ") + "/dfw" + NumberFormatter::format(k) + " HTTP/1.0\n\n";

				// send the request
				n = write(sockfd, reqstr.c_str(), reqstr.length());
				if (n < 0)
				{
					error((char*)"ERROR writing to socket");
				}

				// get the server's reply
				bzero(reqbody, BUFSIZE);
				while (true)
				{
					n = read(sockfd, reqbody, BUFSIZE);
					if (n == 0)
					{
						break;
					}
					if (n < 0)
					{
						error((char*)"ERROR reading from socket");
					}
				}
				// printf("Echo from server: %s", reqbody);
			}

			close(sockfd);
		}

		// We use a random call to tee to signal that we're done
		tee(-1, -1, 0, 0);
	};

	//
	// OUTPUT VALIDATION
	//
	captured_event_callback_t callback = [&](const callback_param& param) {
		sinsp_evt* evt = param.m_evt;

		if (evt->get_type() == PPME_GENERIC_E)
		{
			if (NumberParser::parse(evt->get_param_value_str("ID", false)) == PPM_SC_TEE)
			{
				unordered_map<ipv4tuple, sinsp_connection, ip4t_hash, ip4t_cmp>::iterator cit;
				for (cit = param.m_analyzer->m_ipv4_connections->m_connections.begin();
				     cit != param.m_analyzer->m_ipv4_connections->m_connections.end();
				     ++cit)
				{
					if (cit->second.m_stid == mytid && cit->first.m_fields.m_dport == 80)
					{
						SCOPED_TRACE(nconns);
						nconns++;
					}
				}
				SCOPED_TRACE("evaluating assertions");
				thread_analyzer_info* ti =
				    dynamic_cast<thread_analyzer_info*>(evt->get_thread_info());
				ASSERT_EQ((uint64_t)0, ti->m_transaction_metrics.get_counter()->m_count_in);
				ASSERT_EQ((uint64_t)0, ti->m_transaction_metrics.get_counter()->m_time_ns_in);
				ASSERT_EQ((uint64_t)0, ti->m_transaction_metrics.get_max_counter()->m_count_in);
				ASSERT_EQ((uint64_t)0, ti->m_transaction_metrics.get_max_counter()->m_time_ns_in);
				// Note: +1 is because of the DNS lookup
				ASSERT_GE(ti->m_transaction_metrics.get_counter()->m_count_out,
				          (uint64_t)N_CONNECTIONS);
				ASSERT_LE(ti->m_transaction_metrics.get_counter()->m_count_out,
				          (uint64_t)N_CONNECTIONS + 1);
				ASSERT_NE((uint64_t)0, ti->m_transaction_metrics.get_counter()->m_time_ns_out);
				ASSERT_EQ((uint64_t)1, ti->m_transaction_metrics.get_max_counter()->m_count_out);
				ASSERT_NE((uint64_t)0, ti->m_transaction_metrics.get_max_counter()->m_time_ns_out);
			}
		}
	};

	//
	// Set a very long sample time, so we're sure no connection is removed
	//
	sinsp_configuration configuration;

	// Set DNS port, /etc/services is read only from dragent context
	// port 80 is not needed, because it's http protocol and is autodiscovered
	scoped_configuration config(R"(
known_ports:
  - 53
)");
	feature_manager::instance().initialize(feature_manager::AGENT_VARIANT_TRADITIONAL);

	ASSERT_NO_FATAL_FAILURE({ event_capture::run(test, callback, filter, configuration); });

	ASSERT_EQ(N_CONNECTIONS, nconns);
}

/**
 * This test is a bit fragile, unfortunately. OpenSSL does not provide any
 * guarantees about how many syscalls (and in what direction) a given call
 * to SSL_read or SSL_write will produce. In addition, changing the version
 * of OpenSSL can change the results of this test.
 *
 * So long as the results we get are consistent across runs and look
 * "reasonable" (where reasonableness is in the eye of the beholder), I call
 * it a pass and move on.
 */
TEST_F(sys_call_test, net_ssl_requests)
{
	//
	// FILTER
	//
	event_filter_t filter = [&](sinsp_evt* evt) {
		auto tinfo = evt->get_thread_info(false);
		return (tinfo != nullptr && tinfo->m_comm == "tests") || m_tid_filter(evt);
	};

	//
	// TEST CODE
	//
	run_callback_t test = [&](sinsp* inspector) {
		ssl_socket sock;

		sock.start(443);

		if (!localhost_ssl_request(443))
		{
			cerr << "A bad thing happened attempting to connect to the SSL server." << endl;
		}
		sock.stop();

		// We use a random call to tee to signal that we're done
		tee(-1, -1, 0, 0);

		return 0;
	};

	//
	// OUTPUT VALIDATION
	//
	captured_event_callback_t callback = [&](const callback_param& param) {
		sinsp_evt* evt = param.m_evt;

		if (evt->get_type() == PPME_GENERIC_E &&
		    NumberParser::parse(evt->get_param_value_str("ID", false)) == PPM_SC_TEE)
		{
			auto threadtable = param.m_inspector->m_thread_manager->get_threads();
			sinsp_transaction_counters transaction_metrics;
			transaction_metrics.clear();
			threadtable->loop([&](sinsp_threadinfo& tinfo) {
				if (tinfo.m_comm == "tests")
				{
					transaction_metrics.add(
					    &dynamic_cast<thread_analyzer_info*>(&tinfo)->m_transaction_metrics);
				}
				return true;
			});

			EXPECT_EQ((uint64_t)2, transaction_metrics.get_counter()->m_count_in);
			EXPECT_LT((uint64_t)0, transaction_metrics.get_counter()->m_time_ns_in);
			EXPECT_EQ((uint64_t)1, transaction_metrics.get_max_counter()->m_count_in);
			EXPECT_LT((uint64_t)0, transaction_metrics.get_max_counter()->m_time_ns_in);

			EXPECT_EQ((uint64_t)1, transaction_metrics.get_counter()->m_count_out);
			EXPECT_NE((uint64_t)0, transaction_metrics.get_counter()->m_time_ns_out);
			EXPECT_EQ((uint64_t)1, transaction_metrics.get_max_counter()->m_count_out);
			EXPECT_NE((uint64_t)0, transaction_metrics.get_max_counter()->m_time_ns_out);
		}
	};

	sinsp_configuration configuration;

	scoped_configuration config(R"(
known_ports:
  - 443
)");
	test_helpers::scoped_config<uint64_t> interval("flush_interval", 100 * ONE_SECOND_IN_NS);
	feature_manager::instance().initialize(feature_manager::AGENT_VARIANT_TRADITIONAL);

	ASSERT_NO_FATAL_FAILURE({ event_capture::run(test, callback, filter, configuration); });
}

//
// This test checks the fact that connect can be called on a UDP socket
// so that read/write/send/recv can then be used on the socket, without the overhead
// of specifying the address with every IO operation.
//
TEST_F(sys_call_test, net_double_udp_connect)
{
	int nconns = 0;
	int mytid = getpid();

	//
	// FILTER
	//
	event_filter_t filter = [&](sinsp_evt* evt) { return m_tid_filter(evt); };

	//
	// TEST CODE
	//
	run_callback_t test = [&](sinsp* inspector) {
		int sockfd, n;
		struct sockaddr_in serveraddr;
		struct sockaddr_in serveraddr1;
		struct hostent* server;
		struct hostent* server1;
		char* hostname = (char*)SITE;
		char* hostname1 = (char*)SITE1;
		int portno = 80;
		string reqstr;

		// get the first server's DNS entry
		server = gethostbyname(hostname);
		if (server == NULL)
		{
			fprintf(stderr, (char*)"ERROR, no such host as %s\n", hostname);
			exit(0);
		}

		// get the second server's DNS entry
		server1 = gethostbyname(hostname1);
		if (server1 == NULL)
		{
			fprintf(stderr, (char*)"ERROR, no such host as %s\n", hostname1);
			exit(0);
		}

		// create the socket
		sockfd = socket(2, 2, 0);
		if (sockfd < 0)
		{
			error((char*)"ERROR opening socket");
		}

		// build the server's Internet address
		bzero((char*)&serveraddr, sizeof(serveraddr));
		serveraddr.sin_family = AF_INET;
		bcopy((char*)server->h_addr, (char*)&serveraddr.sin_addr.s_addr, server->h_length);
		serveraddr.sin_port = 0;

		// create a connection with google
		if (connect(sockfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
		{
			error((char*)"ERROR connecting");
		}

		// create a SECOND connection with google
		if (connect(sockfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0)
		{
			error((char*)"ERROR connecting");
		}

		// build the server's Internet address
		bzero((char*)&serveraddr1, sizeof(serveraddr1));
		serveraddr1.sin_family = AF_INET;
		bcopy((char*)server1->h_addr, (char*)&serveraddr1.sin_addr.s_addr, server1->h_length);
		serveraddr1.sin_port = htons(portno);

		// create a connection with yahoo
		if (connect(sockfd, (struct sockaddr*)&serveraddr1, sizeof(serveraddr1)) < 0)
		{
			error((char*)"ERROR connecting");
		}

		//
		// Send a datagram
		//
		reqstr = "GET /dfw HTTP/1.0\n\n";

		// send the request
		n = write(sockfd, reqstr.c_str(), reqstr.length());
		if (n < 0)
		{
			error((char*)"ERROR writing to socket");
		}

		//
		// Close the socket
		//
		close(sockfd);

		// We use a random call to tee to signal that we're done
		tee(-1, -1, 0, 0);

		return 0;
	};

	//
	// OUTPUT VALIDATION
	//
	captured_event_callback_t callback = [&](const callback_param& param) {
		sinsp_evt* evt = param.m_evt;

		if (evt->get_type() == PPME_GENERIC_E)
		{
			if (NumberParser::parse(evt->get_param_value_str("ID", false)) == PPM_SC_TEE)
			{
				unordered_map<ipv4tuple, sinsp_connection, ip4t_hash, ip4t_cmp>::iterator cit;
				for (cit = param.m_analyzer->m_ipv4_connections->m_connections.begin();
				     cit != param.m_analyzer->m_ipv4_connections->m_connections.end();
				     ++cit)
				{
					if (cit->second.m_stid == mytid && cit->first.m_fields.m_dport == 80)
					{
						nconns++;
					}
				}
			}
		}
	};

	//
	// Set a very long sample time, so we're sure no connection is removed
	//
	sinsp_configuration configuration;

	ASSERT_NO_FATAL_FAILURE({ event_capture::run(test, callback, filter, configuration); });

	ASSERT_EQ(1, nconns);
}

TEST_F(sys_call_test, net_connection_table_limit)
{
	int nconns = 0;
	//	int mytid = getpid();

	//
	// FILTER
	//
	event_filter_t filter = [&](sinsp_evt* evt) { return m_tid_filter(evt); };

	//
	// TEST CODE
	//
	run_callback_t test = [&](sinsp* inspector) {
		const int REQUESTS_TO_SEND = 5;
		int num_requests = 0;
		test_helpers::scoped_http_server srv(9090);

		try
		{
			HTTPStreamFactory::registerFactory();

			NullOutputStream ostr;

			URI uri("http://127.0.0.1:9090");

			// Sleep to give the server time to start up
			std::this_thread::sleep_for(chrono::milliseconds(500));

			std::unique_ptr<std::istream> pStrs[REQUESTS_TO_SEND];
			for (int i = 0; i < REQUESTS_TO_SEND; ++i)
			{
				pStrs[i] = std::move(
				    std::unique_ptr<std::istream>(URIStreamOpener::defaultOpener().open(uri)));
				StreamCopier::copyStream(*pStrs[i].get(), ostr);
				++num_requests;
			}
			// We use a random call to tee to signal that we're done
			tee(-1, -1, 0, 0);
		}
		catch (Exception& exc)
		{
			std::cerr << exc.displayText() << std::endl;
			FAIL();
		}

		while (num_requests < REQUESTS_TO_SEND)
		{
			std::this_thread::sleep_for(chrono::milliseconds(250));
		}
		return;
	};

	//
	// OUTPUT VALIDATION
	//
	captured_event_callback_t callback = [&](const callback_param& param) {
		sinsp_evt* evt = param.m_evt;

		if (evt->get_type() == PPME_GENERIC_E)
		{
			if (NumberParser::parse(evt->get_param_value_str("ID", false)) == PPM_SC_TEE)
			{
				unordered_map<ipv4tuple, sinsp_connection, ip4t_hash, ip4t_cmp>::iterator cit;
				for (cit = param.m_analyzer->m_ipv4_connections->m_connections.begin();
				     cit != param.m_analyzer->m_ipv4_connections->m_connections.end();
				     ++cit)
				{
					nconns++;
				}

				ASSERT_EQ(3, nconns);
			}
		}
	};

	//
	// Set a very long sample time, so we're sure no connection is removed
	//
	sinsp_configuration configuration;

	//
	// Set a very low connection table size
	//
	configuration_manager::instance().get_mutable_config<uint32_t>("connection.max_count")->set(3);

	ASSERT_NO_FATAL_FAILURE({ event_capture::run(test, callback, filter, configuration); });
}

TEST_F(sys_call_test, DISABLED_net_connection_aggregation)
{
	int nconns = 0;

	//
	// FILTER
	//
	event_filter_t filter = [&](sinsp_evt* evt) { return m_tid_filter(evt); };

	//
	// TEST CODE
	//
	run_callback_t test = [&](sinsp* inspector) {
		try
		{
			HTTPStreamFactory::registerFactory();

			NullOutputStream ostr;

			URI uri1("http://www.google.com");
			std::unique_ptr<std::istream> pStr1(URIStreamOpener::defaultOpener().open(uri1));
			StreamCopier::copyStream(*pStr1.get(), ostr);

			URI uri2("http://www.yahoo.com");
			std::unique_ptr<std::istream> pStr2(URIStreamOpener::defaultOpener().open(uri2));
			StreamCopier::copyStream(*pStr2.get(), ostr);

			URI uri3("http://www.bing.com");
			std::unique_ptr<std::istream> pStr3(URIStreamOpener::defaultOpener().open(uri3));
			StreamCopier::copyStream(*pStr3.get(), ostr);

			// We use a random call to tee to signal that we're done
			tee(-1, -1, 0, 0);
			//			sleep(5);
		}
		catch (Exception& exc)
		{
			std::cerr << exc.displayText() << std::endl;
			FAIL();
		}

		return;
	};

	//
	// OUTPUT VALIDATION
	//
	captured_event_callback_t callback = [&](const callback_param& param) {
		return;
		sinsp_evt* evt = param.m_evt;

		if (evt->get_type() == PPME_GENERIC_E)
		{
			if (NumberParser::parse(evt->get_param_value_str("ID", false)) == PPM_SC_TEE)
			{
				unordered_map<ipv4tuple, sinsp_connection, ip4t_hash, ip4t_cmp>::iterator cit;
				for (cit = param.m_analyzer->m_ipv4_connections->m_connections.begin();
				     cit != param.m_analyzer->m_ipv4_connections->m_connections.end();
				     ++cit)
				{
					nconns++;
				}

				ASSERT_EQ(3, nconns);
			}
		}
	};

	//
	// Set a very low connection table size
	//
	sinsp_configuration configuration;

	ASSERT_NO_FATAL_FAILURE({ event_capture::run(test, callback, filter, configuration); });
}

TEST(sinsp_procfs_parser, DISABLED_test_read_network_interfaces_stats)
{
	// cpu, mem, live, ttl cpu, ttl mem
	sinsp_procfs_parser parser(1, 1024, true, 0, 0);

	auto stats = parser.read_network_interfaces_stats();
	EXPECT_EQ(stats.first, 0U);
	EXPECT_EQ(stats.second, 0U);
	ASSERT_TRUE(system("curl https://google.com > /dev/null 2> /dev/null") == 0);
	stats = parser.read_network_interfaces_stats();
	EXPECT_GT(stats.first, 0U);
	EXPECT_GT(stats.second, 0U);
}

TEST(sinsp_procfs_parser, test_add_ports_from_proc_fs)
{
	const char* filename = "resources/procfs.tcp";
	set<uint16_t> oldports = {2379};
	set<uint16_t> newports;
	// These inodes should match local ports 42602, 2379, 2380 and 59042
	// Port 59042 is a connection to a remote host and not a listening port
	set<uint64_t> inodes = {17550, 18661, 18655, 128153, 12345};

	// Since oldports already has 2379 the expected ports added in newports should be 42602 and 2380
	EXPECT_EQ(sinsp_procfs_parser::add_ports_from_proc_fs(filename, oldports, newports, inodes), 2);
	EXPECT_EQ(newports.size(), 2);
	EXPECT_TRUE(newports.find(42602) != newports.end());
	EXPECT_TRUE(newports.find(2380) != newports.end());
}

TEST(sinsp_procfs_parser, test_read_process_serverports)
{
	const uint16_t port = 999;
	set<uint16_t> oldports;
	set<uint16_t> newports;
	pid_t pid = getpid();

	// Populate oldports with current listening ports
	sinsp_procfs_parser::read_process_serverports(pid, newports, oldports);
	// Make sure we're not listening to our port yet
	ASSERT_TRUE(oldports.find(port) == oldports.end());
	// Create socket, bind and listen
	ServerSocket sock(port);

	// Check listening ports
	EXPECT_EQ(sinsp_procfs_parser::read_process_serverports(pid, oldports, newports), 1);
	// Should have found our new port
	EXPECT_EQ(newports.size(), 1);
	EXPECT_TRUE(newports.find(port) != newports.end());
}
