#include "preinclude.h"

#include <map>
#include <vector>
#include <thread>
#include <chrono>
#include <mutex>

#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>

#define BITCOIN_UA_LENGTH 23
#define BITCOIN_UA {'/', 'R', 'e', 'l', 'a', 'y', 'N', 'e', 't', 'w', 'o', 'r', 'k', 'S', 'e', 'r', 'v', 'e', 'r', ':', '4', '2', '/'}

#include "crypto/sha2.h"
#include "flaggedarrayset.h"
#include "relayprocess.h"
#include "utils.h"
#include "p2pclient.h"
#include "connection.h"
#include "rpcclient.h"
#include "relayconnection.h"




static const char* HOST_SPONSOR;



class RelayNetworkClient;
class RelayNetworkCompressor : public RelayNodeCompressor {
public:
	RelayNetworkCompressor() : RelayNodeCompressor(false, true) {}
	RelayNetworkCompressor(bool useFlagsAndSmallerMax, bool freezeIndexesDuringBlock) : RelayNodeCompressor(useFlagsAndSmallerMax, freezeIndexesDuringBlock) {}
	void relay_node_connected(RelayNetworkClient* client, int token);
};

static_assert(COMPRESSOR_TYPES == 3, "There are two compressor types init'd in server");
static const std::map<std::string, int16_t> compressor_types = {{std::string("sponsor printer"), 2}, {std::string("spammy memeater"), 1}, {std::string("the blocksize"), 2}, {std::string("what i should have done"), 0}};
static RelayNetworkCompressor compressors[COMPRESSOR_TYPES];
class CompressorInit {
public:
	CompressorInit() {
		compressors[0] = RelayNetworkCompressor(false, true);
		compressors[1] = RelayNetworkCompressor(false, false);
		compressors[2] = RelayNetworkCompressor(true, false);
	}
};
static CompressorInit init;



/***********************************************
 **** Relay network client processing class ****
 ***********************************************/
class RelayNetworkClient : public Connection, public RelayConnectionProcessor {
private:
	DECLARE_ATOMIC_INT(int, connected);

	bool sendSponsor = false;
	uint8_t tx_sent = 0;

	const std::function<std::tuple<uint64_t, std::chrono::steady_clock::time_point> (RelayNetworkClient*, RelayNodeCompressor::DecompressState&)> provide_block;
	const std::function<void (RelayNetworkClient*, std::shared_ptr<std::vector<unsigned char> >&)> provide_transaction;
	const std::function<void (RelayNetworkClient*, int)> connected_callback;

public:
	time_t lastDupConnect = 0;
	DECLARE_ATOMIC_INT(int16_t, compressor_type);

	RelayNetworkClient(int sockIn, std::string hostIn,
						const std::function<std::tuple<uint64_t, std::chrono::steady_clock::time_point> (RelayNetworkClient*, RelayNodeCompressor::DecompressState&)>& provide_block_in,
						const std::function<void (RelayNetworkClient*, std::shared_ptr<std::vector<unsigned char> >&)>& provide_transaction_in,
						const std::function<void (RelayNetworkClient*, int)>& connected_callback_in)
			: Connection(sockIn, hostIn), connected(0), provide_block(provide_block_in), provide_transaction(provide_transaction_in),
			connected_callback(connected_callback_in), compressor_type(-1)
	{ construction_done(); }

private:
	void send_sponsor(int token=0) {
		if (!sendSponsor || tx_sent != 0)
			return;
		relay_msg_header sponsor_header = { RELAY_MAGIC_BYTES, SPONSOR_TYPE, htonl(strlen(HOST_SPONSOR)) };
		Connection::do_send_bytes((char*)&sponsor_header, sizeof(sponsor_header), token);
		Connection::do_send_bytes(HOST_SPONSOR, strlen(HOST_SPONSOR), token);
	}

	bool readable() { return true; }


	const char* handle_peer_version(const std::string& their_version) {
		if (their_version != VERSION_STRING) {
			relay_msg_header version_header = { RELAY_MAGIC_BYTES, MAX_VERSION_TYPE, htonl(strlen(VERSION_STRING)) };
			Connection::do_send_bytes((char*)&version_header, sizeof(version_header));
			Connection::do_send_bytes(VERSION_STRING, strlen(VERSION_STRING));
		}

		std::map<std::string, int16_t>::const_iterator it = compressor_types.find(their_version);
		if (it == compressor_types.end())
			return "unknown version string";

		compressor_type = it->second;

		if (their_version == "what i should have done")
			compressor = RelayNodeCompressor(false, true);
		else if (their_version == "spammy memeater")
			compressor = RelayNodeCompressor(false, false);
		else
			compressor = RelayNodeCompressor(true, false);

		if (their_version != "the blocksize")
			sendSponsor = true;

		relay_msg_header version_header = { RELAY_MAGIC_BYTES, VERSION_TYPE, uint32_t(their_version.length()) };
		Connection::do_send_bytes((char*)&version_header, sizeof(version_header));
		Connection::do_send_bytes(their_version.c_str(), their_version.length());

		printf("%s Connected to relay node with protocol version %s\n", host.c_str(), their_version.c_str());
		int token = get_send_mutex();
		connected = 2;
		do_throttle_outbound();
		connected_callback(this, token); // Called with send_mutex!
		release_send_mutex(token);

		return NULL;
	}

	const char* handle_max_version(const std::string& max_version) {
		if (std::string(VERSION_STRING) != max_version)
			printf("%s peer sent us a MAX_VERSION message\n", host.c_str());
		else
			return "got MAX_VERSION of same version as us";

		return NULL;
	}

	const char* handle_sponsor(const std::string& sponsor) { return NULL; }
	void handle_pong(uint64_t nonce) {}

	void handle_block(RelayNodeCompressor::DecompressState& current_block,
			std::chrono::system_clock::time_point& read_finish_time,
			std::chrono::steady_clock::time_point& read_finish,
			std::chrono::steady_clock::time_point& current_block_read_start) {

		auto res = provide_block(this, current_block);

		if (std::get<0>(res))
			printf(HASH_FORMAT" BLOCK %lu %s UNTRUSTEDRELAY %u / %lu / %u TIMES: %lf %lf\n", HASH_PRINT(&(*current_block.fullhashptr)[0]),
											epoch_millis_lu(read_finish_time), host.c_str(),
											current_block.wire_bytes, std::get<0>(res), current_block.block_bytes,
											to_millis_double(read_finish - current_block_read_start), to_millis_double(std::get<1>(res) - read_finish));
	}

	void handle_transaction(std::shared_ptr<std::vector<unsigned char> >& tx) {
		provide_transaction(this, tx);
	}

	void do_send_bytes(const char *buf, size_t nbyte) {
		Connection::do_send_bytes(buf, nbyte);
	}

	void disconnect(const char* reason) {
		Connection::disconnect(reason);
	}

	void recv_bytes(char* buf, size_t len) {
		RelayConnectionProcessor::recv_bytes(buf, len);
	}

public:
	void receive_transaction(const std::shared_ptr<std::vector<unsigned char> >& tx, int token=0) {
		if (connected != 2)
			return;

		Connection::do_send_bytes(tx, token);
		tx_sent++;
		if (!token)
			send_sponsor(token);
	}

	void receive_block(const std::shared_ptr<std::vector<unsigned char> >& block) {
		if (connected != 2)
			return;

		int token = get_send_mutex();
		Connection::do_send_bytes(block, token);
		struct relay_msg_header header = { RELAY_MAGIC_BYTES, END_BLOCK_TYPE, 0 };
		Connection::do_send_bytes((char*)&header, sizeof(header), token);
		release_send_mutex(token);
	}
};

class P2PClient : public P2PRelayer {
public:
	P2PClient(const char* serverHostIn, uint16_t serverPortIn,
				const std::function<void (std::vector<unsigned char>&, const std::chrono::system_clock::time_point&)>& provide_block_in,
				const std::function<void (std::shared_ptr<std::vector<unsigned char> >&)>& provide_transaction_in,
				const std::function<void (std::vector<unsigned char>&)>& provide_headers_in,
				bool check_block_msghash_in) :
			P2PRelayer(serverHostIn, serverPortIn, 10000, provide_block_in, provide_transaction_in, provide_headers_in, check_block_msghash_in)
		{ construction_done(); }

private:
	std::vector<unsigned char> generate_version() {
		struct bitcoin_version_with_header version_msg;
		version_msg.version.start.timestamp = htole64(time(0));
		version_msg.version.start.user_agent_length = BITCOIN_UA_LENGTH; // Work around apparent gcc bug
		return std::vector<unsigned char>((unsigned char*)&version_msg, (unsigned char*)&version_msg + sizeof(version_msg));
	}
};


void RelayNetworkCompressor::relay_node_connected(RelayNetworkClient* client, int token) {
	for_each_sent_tx([&] (const std::shared_ptr<std::vector<unsigned char> >& tx) {
		client->receive_transaction(tx_to_msg(tx, false, false), token);
		client->receive_transaction(tx, token);
	});
}


class MempoolClient : public OutboundPersistentConnection {
private:
	std::function<void(std::vector<unsigned char>)> on_hash;
	std::vector<unsigned char> curhash;
	size_t hashpos;
public:
	MempoolClient(std::string serverHostIn, uint16_t serverPortIn, std::function<void(std::vector<unsigned char>)> on_hash_in)
		: OutboundPersistentConnection(serverHostIn, serverPortIn), on_hash(on_hash_in), curhash(32), hashpos(0) { construction_done(); }

	void on_disconnect() {}
	void on_connect() {}

	bool readable() { return true; }
	void recv_bytes(char* buf, size_t len) {
		while (len) {
			size_t bytes_read = std::min(32 - hashpos, len);
			memcpy(&curhash[hashpos], buf, bytes_read);
			hashpos += bytes_read;
			if (hashpos == 32) {
				on_hash(curhash);
				hashpos = 0;
				len -= bytes_read;
				buf += bytes_read;
			}
		}
	}

	void keep_alive_ping() {
		char byte = 0x42;
		maybe_do_send_bytes(&byte, 1);
	}
};





int main(const int argc, const char** argv) {
	if (argc < 5) {
		printf("USAGE: %s trusted_host trusted_port trusted_port_2 \"Sponsor String\" (::ffff:whitelisted prefix string)*\n", argv[0]);
		return -1;
	}

	HOST_SPONSOR = argv[4];

	int listen_fd;
	struct sockaddr_in6 addr;

	if ((listen_fd = socket(AF_INET6, SOCK_STREAM, 0)) < 0) {
		printf("Failed to create socket\n");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_addr = in6addr_any;
	addr.sin6_port = htons(8336);

	int reuse = 1;
	if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) ||
			bind(listen_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0 ||
			listen(listen_fd, 3) < 0) {
		printf("Failed to bind 8336: %s\n", strerror(errno));
		return -1;
	}

	std::mutex map_mutex;
	std::map<std::string, RelayNetworkClient*> clientMap;
	DECLARE_NON_ATOMIC_PTR(P2PClient, trustedP2P);

	// You'll notice in the below callbacks that we have to do some header adding/removing
	// This is because the things are setup for the relay <-> p2p case (both to optimize
	// the client and because that is the case we want to optimize for)

	std::mutex txn_mutex;
	vectormruset txnWaitingToBroadcast(MAX_FAS_TOTAL_SIZE);

	const std::function<std::pair<const char*, size_t> (const std::vector<unsigned char>&, const std::vector<unsigned char>&, bool)> do_relay =
		[&](const std::vector<unsigned char>& fullhash, const std::vector<unsigned char>& bytes, bool checkMerkle) {
			std::lock_guard<std::mutex> lock(map_mutex);
			size_t ret;
			for (uint16_t i = 0; i < COMPRESSOR_TYPES; i++) {
				auto tuple = compressors[i].maybe_compress_block(fullhash, bytes, checkMerkle);
				const char* insane = std::get<1>(tuple);
				if (!insane) {
					auto block = std::get<0>(tuple);
					for (const auto& client : clientMap) {
						if (!client.second->disconnectStarted() && client.second->compressor_type == i)
							client.second->receive_block(block);
					}
					if (i == 0)
						ret = block->size();
				} else
					return std::make_pair(insane, (size_t)0);
			}
			return std::make_pair((const char*)0, ret);
		};

	trustedP2P = new P2PClient(argv[1], std::stoul(argv[2]),
					[&](std::vector<unsigned char>& bytes,  const std::chrono::system_clock::time_point& read_start) {
						if (bytes.size() < sizeof(struct bitcoin_msg_header) + 80)
							return;

						std::chrono::system_clock::time_point send_start(std::chrono::system_clock::now());

						std::vector<unsigned char> fullhash(32);
						getblockhash(fullhash, bytes, sizeof(struct bitcoin_msg_header));

						std::pair<const char*, size_t> relay_res = do_relay(fullhash, bytes, false);
						if (relay_res.first) {
							printf(HASH_FORMAT" INSANE %s TRUSTEDP2P\n", HASH_PRINT(&fullhash[0]), relay_res.first);
							return;
						}

						std::chrono::system_clock::time_point send_end(std::chrono::system_clock::now());
						printf(HASH_FORMAT" BLOCK %lu %s TRUSTEDP2P %lu / %lu / %lu TIMES: %lf %lf\n", HASH_PRINT(&fullhash[0]), epoch_millis_lu(send_start), argv[1],
														bytes.size(), relay_res.second, bytes.size(),
														to_millis_double(send_start - read_start), to_millis_double(send_end - send_start));
					},
					[&](std::shared_ptr<std::vector<unsigned char> >& bytes) {
						std::vector<unsigned char> hash(32);
						double_sha256(&(*bytes)[0], &hash[0], bytes->size());
						{
							std::lock_guard<std::mutex> lock(txn_mutex);
							if (txnWaitingToBroadcast.find(hash) == txnWaitingToBroadcast.end())
								return;
						}
						std::lock_guard<std::mutex> lock(map_mutex);
						for (uint16_t i = 0; i < COMPRESSOR_TYPES; i++) {
							auto tx = compressors[i].get_relay_transaction(bytes);
							if (tx.use_count()) {
								for (const auto& client : clientMap) {
									if (!client.second->disconnectStarted() && client.second->compressor_type == i)
										client.second->receive_transaction(tx);
								}
							}
						}
					},
					[&](std::vector<unsigned char>& headers) {
						try {
							std::vector<unsigned char>::const_iterator it = headers.begin();
							uint64_t count = read_varint(it, headers.end());

							for (uint64_t i = 0; i < count; i++) {
								move_forward(it, 81, headers.end());

								if (*(it - 1) != 0)
									return;

								std::vector<unsigned char> fullhash(32);
								getblockhash(fullhash, headers, it - 81 - headers.begin());
								compressors[0].block_sent(fullhash);
							}

							printf("Added headers from trusted peers, seen %u blocks\n", compressors[0].blocks_sent());
						} catch (read_exception) { }
					}, true);

	MempoolClient mempoolClient(argv[1], std::stoul(argv[3]),
					[&](std::vector<unsigned char> txn) {
						std::lock_guard<std::mutex> lock(map_mutex);
						if (!compressors[0].was_tx_sent(&txn[0])) {
							std::lock_guard<std::mutex> lock(txn_mutex);
							txnWaitingToBroadcast.insert(txn);
							((P2PClient*)trustedP2P)->request_transaction(txn);
						}
					});

	std::function<std::tuple<uint64_t, std::chrono::steady_clock::time_point> (RelayNetworkClient*, RelayNodeCompressor::DecompressState&)> relayBlock =
		[&](RelayNetworkClient* from, RelayNodeCompressor::DecompressState& block) {
			assert(block.is_finished());

			std::chrono::steady_clock::time_point first_compressor_sends_queued;
			uint64_t first_compressor_block_size;

			std::lock_guard<std::mutex> lock(map_mutex);
			for (uint16_t i = 0; i < COMPRESSOR_TYPES; i++) {
				auto compressed = compressors[i].recompress_block(block);
				if (compressed->size() > 80) {
					for (const auto& client : clientMap)
						if (!client.second->disconnectStarted() && client.second->compressor_type == i)
							client.second->receive_block(compressed);
				} else {
					printf(HASH_FORMAT" INSANE %s UNTRUSTEDRELAY %s\n", HASH_PRINT(&(*block.fullhashptr)[0]), (const char*)&(*compressed)[0], from->host.c_str());
					return std::make_tuple((uint64_t)0, std::chrono::steady_clock::now());
				}
				if (i == 0) {
					first_compressor_sends_queued = std::chrono::steady_clock::now();
					first_compressor_block_size = compressed->size();
				}
			}
			return std::make_tuple(first_compressor_block_size, first_compressor_sends_queued);
		};

	std::function<void (RelayNetworkClient*, std::shared_ptr<std::vector<unsigned char> >&)> relayTx =
		[&](RelayNetworkClient* from, std::shared_ptr<std::vector<unsigned char>> & bytes) {
			((P2PClient*)trustedP2P)->receive_transaction(bytes);
		};

	std::function<void (RelayNetworkClient*, int token)> connected =
		[&](RelayNetworkClient* client, int token) {
			assert(client->compressor_type >= 0 && client->compressor_type < COMPRESSOR_TYPES);
			compressors[client->compressor_type].relay_node_connected(client, token);
		};

	std::thread([&](void) {
		while (true) {
			std::this_thread::sleep_for(std::chrono::seconds(10)); // Implicit new-connection rate-limit
			{
				std::lock_guard<std::mutex> lock(map_mutex);
				for (auto it = clientMap.begin(); it != clientMap.end();) {
					if (it->second->disconnectComplete()) {
						fprintf(stderr, "%lld: Culled %s, have %lu relay clients\n", (long long) time(NULL), it->first.c_str(), clientMap.size() - 1);
						delete it->second;
						clientMap.erase(it++);
					} else
						it++;
				}
			}
			mempoolClient.keep_alive_ping();
		}
	}).detach();

	std::string droppostfix(".uptimerobot.com");
	std::vector<std::string> whitelistprefix;
	for (int i = 5; i < argc; i++)
		whitelistprefix.push_back(argv[i]);
	socklen_t addr_size = sizeof(addr);
	while (true) {
		int new_fd;
		if ((new_fd = accept(listen_fd, (struct sockaddr *) &addr, &addr_size)) < 0) {
			printf("Failed to select (%d: %s)\n", new_fd, strerror(errno));
			return -1;
		}

		std::string host = gethostname(&addr);
		std::lock_guard<std::mutex> lock(map_mutex);

		bool whitelist = false;
		for (const std::string& s : whitelistprefix)
			if (host.compare(0, s.length(), s) == 0)
				whitelist = true;

		if ((clientMap.count(host) && !whitelist) ||
				(host.length() > droppostfix.length() && !host.compare(host.length() - droppostfix.length(), droppostfix.length(), droppostfix))) {
			if (clientMap.count(host)) {
				const auto& client = clientMap[host];
				if (client->lastDupConnect < (time(NULL) - 60)) {
					client->lastDupConnect = time(NULL);
					fprintf(stderr, "%lld: Got duplicate connection from %s (original's disconnect status: %s)\n", (long long) time(NULL), host.c_str(), client->getDisconnectDebug().c_str());
				}
			}
			close(new_fd);
		} else {
			if (whitelist)
				host += ":" + std::to_string(addr.sin6_port);
			assert(clientMap.count(host) == 0);
			clientMap[host] = new RelayNetworkClient(new_fd, host, relayBlock, relayTx, connected);
			fprintf(stderr, "%lld: New connection from %s, have %lu relay clients\n", (long long) time(NULL), host.c_str(), clientMap.size());
		}
	}
}
