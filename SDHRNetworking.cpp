#include "SDHRNetworking.h"
#include "SDHRManager.h"
#include <time.h>
#include <fcntl.h>


ENET_RES socket_bind_and_listen(__SOCKET* server_fd, const sockaddr_in& server_addr)
{
#ifdef __NETWORKING_WINDOWS__
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		std::cerr << "WSAStartup failed" << std::endl;
		return ENET_RES::ERR;
	}
	if ((*server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == INVALID_SOCKET) {
		std::cerr << "Error creating socket" << std::endl;
		WSACleanup();
		return ENET_RES::ERR;
	}
	if (bind(*server_fd, (SOCKADDR*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
		std::cerr << "Error binding socket" << std::endl;
		closesocket(*server_fd);
		WSACleanup();
		return ENET_RES::ERR;
	}
	if (listen(*server_fd, 1) == SOCKET_ERROR) {
		std::cerr << "Error listening on socket" << std::endl;
		closesocket(*server_fd);
		WSACleanup();
		return ENET_RES::ERR;
	}
#else // not __NETWORKING_WINDOWS__
	if ((*server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
		std::cerr << "Error creating socket" << std::endl;
		return ENET_RES::ERR;
	}
	if (bind(*server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
		std::cerr << "Error binding socket" << std::endl;
		return ENET_RES::ERR;
	}
	if (listen(*server_fd, 1) == -1) {
		std::cerr << "Error listening on socket" << std::endl;
		return ENET_RES::ERR;
	}
#endif

	return ENET_RES::OK;
}

int socket_server_thread(uint16_t port, bool* shouldTerminateNetworking)
{
	// commands socket and descriptors
	//__SOCKET server_fd, client_fd;
	//struct sockaddr_in server_addr, client_addr;
	//socklen_t client_len = sizeof(client_addr);

	//server_addr.sin_family = AF_INET;
	//server_addr.sin_port = htons(port);
	//server_addr.sin_addr.s_addr = INADDR_ANY;

#define VLEN 16
#define BUFSZ 2048

#ifdef __NETWORKING_WINDOWS__
#else
	struct mmsghdr msgs[VLEN];
	struct iovec iovecs[VLEN];
	uint8_t bufs[VLEN][BUFSZ];
	for (int i = 0; i < VLEN; ++i) {
		iovecs[i].iov_base = bufs[i];
		iovecs[i].iov_len = BUFSZ;
		msgs[i].msg_hdr.msg_iov = &iovecs[i];
		msgs[i].msg_hdr.msg_iovlen = 1;
	}

	int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if (sockfd < 0) {
		std::cerr << "Error opening socket" << std::endl;
		return 1;
	}

	int optval = 1;
	setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
		(const void*)&optval, sizeof(int));
	struct sockaddr_in serveraddr;
	bzero((char*)&serveraddr, sizeof(serveraddr));
	serveraddr.sin_family = AF_INET;
	serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
	serveraddr.sin_port = htons((unsigned short)8080);
	
	if (bind(sockfd, (struct sockaddr*)&serveraddr,
		sizeof(serveraddr)) < 0) {
		std::cerr << "Error binding socket" << std::endl;
		return 1;
	}

	int flags = fcntl(sockfd, F_GETFL, 0);
	flags |= O_NONBLOCK;
	fcntl(sockfd, F_SETFL, flags);


	auto sdhrMgr = SDHRManager::GetInstance();
	uint8_t* a2mem = sdhrMgr->GetApple2MemPtr();

	std::cout << "Waiting for connection..." << std::endl;
	bool connected = false;

	bool first_drop = true;
	uint32_t prev_seqno = 0;
	uint16_t prev_addr = 0;
	std::vector<SDHREvent> events;
	events.reserve(1000000);
	int64_t last_recv_nsec;

	while (!(*shouldTerminateNetworking)) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		int64_t nsec = ts.tv_sec * 1000000000ll + ts.tv_nsec;
		int retval = recvmmsg(sockfd, msgs, VLEN, 0, NULL);
		if (retval < 0 && errno != EWOULDBLOCK) {
			std::cerr << "Error in recvmmsg" << std::endl;
			return 1;
		}
		if (connected && nsec > last_recv_nsec + 10000000000ll) {
			std::cout << "Client disconnected" << std::endl;
			connected = false;
			first_drop = true;
			continue;
		}	
		if (retval == -1) {
			continue;
		}
		if (!connected) {
			connected = true;
			std::cout << "Client connected" << std::endl;
		}
		last_recv_nsec = nsec;

		events.clear();
		for (int i = 0; i < retval; ++i) {
			SDHRPacketHeader* h = (SDHRPacketHeader*)bufs[i];
			uint32_t seqno = h->seqno[0];
			seqno += (uint32_t)h->seqno[1] << 8;
			seqno += (uint32_t)h->seqno[2] << 16;
			seqno += (uint32_t)h->seqno[3] << 24;
			if (seqno != prev_seqno + 1) {
				if (first_drop) {
					first_drop = false;
				} else {
					std::cerr << "seqno drops: " 
						<< seqno - prev_seqno + 1 << std::endl;
					// this is pretty bad, should probably go into error
				}
			}
			prev_seqno = seqno;
			if (h->cmdtype != 0) {
				std::cerr << "ignoring cmd" << std::endl;
				// currently ignoring anything not a bus event
				continue;
			}
			uint8_t* p = bufs[i] + sizeof(SDHRPacketHeader);
			while (p - bufs[i] < msgs[i].msg_len) {
				SDHRBusChunk* c = (SDHRBusChunk*) p;
				size_t chunk_len = 10;
                                uint32_t addr_count = 0;
				for (int j = 0; j < 8; ++j) {
					bool addr_flag = (c->seqflags & (1 << j)) != 0;
					bool rw = (c->rwflags & (1 << j)) != 0;
					uint16_t addr;
					if (addr_flag) {
						chunk_len += 2;
						addr = c->addrs[addr_count*2+1];
						addr <<= 8;
						addr += c->addrs[addr_count*2];
						++addr_count;
					} else {
						addr = ++prev_addr;
					}
					prev_addr = addr;
					SDHREvent e(rw, addr, c->data[j]);
					events.emplace_back(e);
				}
				p += chunk_len;
			}
		}
		for (const auto& e : events) {
			//std::cout << e.rw << " " << std::hex << e.addr << " " << (uint32_t)e.data << std::endl;
			if (e.rw) {
				// ignoring all read events
				continue;
			}
			if ((e.addr >= 0x200) && (e.addr <= 0xbfff)) {
				a2mem[e.addr] = e.data;
				continue;
			}
			if ((e.addr != CXSDHR_CTRL) && (e.addr != CXSDHR_DATA)) {
				// ignore non-control
				continue;
			}
			//std::cerr << "cmd " << e.addr << " " << (uint32_t) e.data << std::endl;
			SDHRCtrl_e _ctrl;
			switch (e.addr & 0x0f)
			{
			case 0x00:
				// std::cout << "This is a control packet!" << std::endl;
				_ctrl = (SDHRCtrl_e)e.data;
				switch (_ctrl)
				{
				case SDHR_CTRL_DISABLE:
#ifdef DEBUG
					std::cout << "CONTROL: Disable SDHR" << std::endl;
#endif
					sdhrMgr->ToggleSdhr(false);
					break;
				case SDHR_CTRL_ENABLE:
#ifdef DEBUG
					std::cout << "CONTROL: Enable SDHR" << std::endl;
#endif
					sdhrMgr->ToggleSdhr(true);
					break;
				case SDHR_CTRL_RESET:
#ifdef DEBUG
					std::cout << "CONTROL: Reset SDHR" << std::endl;
#endif
					sdhrMgr->ResetSdhr();
					break;
				case SDHR_CTRL_PROCESS:
				{
					/*
					At this point we have a complete set of commands to process.
					Some more data may be in the kernel socket receive buffer, but we don't care.
					They'll be processed in the next batch.
					Wait for the main thread to finish loading the current state (if any), then process
					the commands.
					*/

					while (sdhrMgr->threadState != THREADCOMM_e::SOCKET_LOCK)
					{
						if (sdhrMgr->threadState == THREADCOMM_e::IDLE)
							sdhrMgr->threadState = THREADCOMM_e::SOCKET_LOCK;
					}
#ifdef DEBUG
					std::cout << "CONTROL: Process SDHR" << std::endl;
#endif
					bool processingSucceeded = sdhrMgr->ProcessCommands();
					// Whether or not the processing worked, clear the buffer. If the processing failed,
					// the data was corrupt and shouldn't be reprocessed
					sdhrMgr->ClearBuffer();
					sdhrMgr->dataState = DATASTATE_e::COMMAND_READY;
					sdhrMgr->threadState = THREADCOMM_e::IDLE;
					if (processingSucceeded)
					{
#ifdef DEBUG
						std::cout << "Processing succeeded!" << std::endl;
#endif
					}
					else {
#ifdef DEBUG
						std::cerr << "ERROR: Processing failed!" << std::endl;
#endif
					}
					break;
				}
				default:
					std::cerr << "ERROR: Unknown control packet type: " << std::hex << (uint32_t)e.data << std::endl;
					break;
				}
				break;
			case 0x01:
				// std::cout << "This is a data packet" << std::endl;
				sdhrMgr->AddPacketDataToBuffer(e.data);
				break;
			default:
				std::cerr << "ERROR: Unknown packet type: " << std::hex << e.addr << std::endl;
				break;
			}
		}
	}

	std::cout << "Client Closing" << std::endl;
	close(sockfd);
	std::cout << "    Client Closed" << std::endl;
#endif
	return 0;
}

bool socket_unblock_accept(uint16_t port)
{
#ifdef __NETWORKING_WINDOWS__
	WSADATA wsaData;
	int result = WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (result != 0) {
		std::cerr << "WSAStartup failed: " << result << std::endl;
		return false;
	}
#endif

	struct sockaddr_in server_addr;
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
#ifdef __NETWORKING_WINDOWS__
	InetPton(AF_INET, ("127.0.0.1"), &server_addr.sin_addr.s_addr);
#else
	inet_pton(AF_INET, ("127.0.0.1"), &server_addr.sin_addr.s_addr);
#endif

	auto client_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#ifdef __NETWORKING_WINDOWS__
	if (client_socket == INVALID_SOCKET) {
		std::cerr << "Error creating socket: " << WSAGetLastError() << std::endl;
		WSACleanup();
#else
	if (client_socket == -1) {
		std::cerr << "Error creating socket!" << std::endl;
#endif
		return false;
	}
#ifdef __NETWORKING_WINDOWS__
	if (connect(client_socket, (SOCKADDR*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
		closesocket(client_socket);
		std::cerr << "Error connecting to server: " << WSAGetLastError() << std::endl;
		WSACleanup();
#else
	if (connect(client_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
		close(client_socket);
		std::cerr << "Error connecting to server!" << std::endl;
#endif
		return false;
	}
	// Do nothing, we've already unblocked the server's accept()
	// so it will quit if shouldTerminateNetworking is true
#ifdef __NETWORKING_WINDOWS__
	closesocket(client_socket);
	WSACleanup();
#else
	close(client_socket);
#endif
	return true;
}
