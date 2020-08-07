#include "funset.hpp"
#include <string.h>
#include <memory>
#include <iostream>
#include <vector>
#include <algorithm>
#include <string>
#include <thread>
#include <chrono>
#include <system_error>
#include <cerrno>

#ifdef _MSC_VER
#include <WinSock2.h>
#include <winsock.h>
#include <ws2tcpip.h>
#else
#include <sys/select.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#endif

// Blog: https://blog.csdn.net/fengbingchun/article/details/107848160
namespace {

const char* server_ip_ = "10.4.96.33"; // 服务器ip
const int server_port_ = 8888; // 服务器端口号,需确保此端口未被占用
			       // linux: $ netstat -nap | grep 6666; kill -9 PID
			       // windows: tasklist | findstr OpenSSL_Test.exe; taskkill /T /F /PID PID
const int server_listen_queue_length_ = 100; // 服务器listen队列支持的最大长度

#ifdef _MSC_VER
// 每一个WinSock应用程序必须在开始操作前初始化WinSock的动态链接库(DLL)，并在操作完成后通知DLL进行清除操作
class WinSockInit {
public:
	WinSockInit()
	{
		WSADATA wsaData;
		// WinSock应用程序在开始时必须要调用WSAStartup函数，结束时调用WSACleanup函数
		// WSAStartup函数必须是WinSock应用程序调用的第一个WinSock函数，否则，其它的WinSock API函数都将会失败并返回错误值
		int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (ret != NO_ERROR)
			fprintf(stderr, "fail to init winsock: %d\n", ret);
	}

	~WinSockInit()
	{
		WSACleanup();
	}
};

static WinSockInit win_sock_init_;

#define close(fd) closesocket(fd)
#define socklen_t int
#else
#define SOCKET int
#endif

int get_error_code()
{
#ifdef _MSC_VER
	auto err_code = WSAGetLastError();
#else
	auto err_code = errno;
#endif
	return err_code;
}

// 服务器端处理来自客户端的数据
void calc_string_length(SOCKET fd)
{
	// 从客户端接收数据
	const int length_recv_buf = 2048;
	char buf_recv[length_recv_buf];
	std::vector<char> recved_data;

	//std::this_thread::sleep_for(std::chrono::seconds(10)); // 为了验证客户端write或send会超时

	while (1) {
		auto num = recv(fd, buf_recv, length_recv_buf, 0);
		if (num <= 0) {
			auto err_code = get_error_code();
			if (num < 0 && err_code == EINTR) {
				continue;
			}

			std::error_code ec(err_code, std::system_category());
			fprintf(stderr, "fail to recv: %d, error code: %d, message: %s\n", num, err_code, ec.message().c_str());
			close(fd);
			return;
		}

		bool flag = false;
		std::for_each(buf_recv, buf_recv + num, [&flag, &recved_data](const char& c) {
			if (c == '\0') flag = true; // 以空字符作为接收结束的标志
			else recved_data.emplace_back(c);
		});

		if (flag == true) break;
	}

	fprintf(stdout, "recved data: ", recved_data.data());
	std::for_each(recved_data.data(), recved_data.data() + recved_data.size(), [](const char& c){
		fprintf(stdout, "%c", c);
	});
	fprintf(stdout, "\n");

	// 向客户端发送数据
	auto str = std::to_string(recved_data.size());
	std::vector<char> vec(str.size() + 1);
	memcpy(vec.data(), str.data(), str.size());
	vec[str.size()] = '\0';
	const char* ptr = vec.data();
	auto left_send = str.size() + 1; // 以空字符作为发送结束的标志

	//std::this_thread::sleep_for(std::chrono::seconds(10)); // 为了验证客户端read或recv会超时

	while (left_send > 0) {
		auto sended_length = send(fd, ptr, left_send, 0); // write
		if (sended_length <= 0) {
			int err_code = get_error_code();
			if (sended_length < 0 && err_code == EINTR) {
				continue;
			}

			std::error_code ec(err_code, std::system_category());
			fprintf(stderr, "fail to send: %d, error code: %d, message: %s\n", sended_length, err_code, ec.message().c_str());
			close(fd);
			return;
		}

		left_send -= sended_length;
		ptr += sended_length;
	}

	close(fd);
}

// 设置套接字为非阻塞的
int set_client_socket_nonblock(SOCKET fd)
{
#ifdef _MSC_VER
	u_long n = 1;
	// ioctlsocket: 通过将第2个参数设置为FIONBIO变更套接字fd的操作模式
	// 当此函数的第3个参数为true时，变更为非阻塞模式；为false时，变更为阻塞模式
	auto ret = ioctlsocket(fd, FIONBIO, &n);
	if (ret != 0) {
		fprintf(stderr, "fail to ioctlsocket: %d\n", ret);
		return -1;
	}
#else
	// fcntl: 向打开的套接字fd发送命令，更改其属性; F_GETFL/F_SETFL: 获得/设置套接字fd状态值; O_NONBLOCK: 设置套接字为非阻塞模式
	auto ret = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	if (ret < 0) {
		fprintf(stderr, "fail to fcntl: %d\n", ret);
	}
#endif

	return 0;
}

// 设置套接字为阻塞的
int set_client_socket_block(SOCKET fd)
{
#ifdef _MSC_VER
	u_long n = 0;
	auto ret = ioctlsocket(fd, FIONBIO, &n);
	if (ret != 0) {
		fprintf(stderr, "fail to ioctlsocket: %d\n", ret);
		return -1;
	}
#else
	auto ret = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
	if (ret < 0) {
		fprintf(stderr, "fail to fcntl: %d\n", ret);
	}
#endif

	return 0;
}

// 设置连接超时
int set_client_connect_time_out(SOCKET fd, const sockaddr* server_addr, socklen_t length, int seconds)
{
#ifdef _MSC_VER
	if (seconds <= 0) {
#else
	if (fd >= FD_SETSIZE || seconds <= 0) {
#endif
		return connect(fd, server_addr, length);
	}

	set_client_socket_nonblock(fd);

	auto ret = connect(fd, server_addr, length);
	if (ret == 0) {
		set_client_socket_block(fd);
		fprintf(stdout, "non block connect return 0\n");
		return 0;
	}
#ifdef _MSC_VER
	else if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
#else
	else if (ret < 0 && errno != EINPROGRESS) {
#endif
		fprintf(stderr, "non block connect fail return: %d\n", ret);
		return -1;
	}

	// 设置超时
	fd_set fdset;
	FD_ZERO(&fdset);
	FD_SET(fd, &fdset);

	struct timeval tv;
	tv.tv_sec = seconds;
	tv.tv_usec = 0;

	// select: 非阻塞方式，返回值：0:表示超时; 1:表示连接成功; -1:表示有错误发生
	// 注：在windows下select函数不作为计时器,在windows下，select的第一个参数可以忽略，可以是任意值
	ret = select(fd + 1, nullptr, &fdset, nullptr, &tv);
	if (ret < 0) {
		fprintf(stderr, "fail to select: %d\n", ret);
		return -1;
	} else if (ret == 0) {
		auto err_code = get_error_code();
		std::error_code ec(err_code, std::system_category());
		fprintf(stderr, "connect time out: error code: %d, message: %s\n", fd, err_code, ec.message().c_str());
		return -1;
	} else {
		int optval;
		socklen_t optlen = sizeof(optval);
#ifdef _MSC_VER
		ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&optval, &optlen);
#else
		// getsockopt: 获得套接字选项设置情况,此函数的第3个参数SO_ERROR表示获取错误
		ret = getsockopt(fd, SOL_SOCKET, SO_ERROR, &optval, &optlen);
#endif
		if (ret == -1 || optval != 0) {
			fprintf(stderr, "fail to getsockopt\n");
			return -1;
		}

		if (optval == 0) {
			set_client_socket_block(fd);
			fprintf(stdout, "connect did not time out\n");
			return 0;
		}
	}

	return 0;
}

// 设置发送数据send超时
int set_client_send_time_out(SOCKET fd, int seconds)
{
	if (seconds <= 0) {
		fprintf(stderr, "seconds should be greater than 0: %d\n", seconds);
		return -1;
	}

#ifdef _MSC_VER
	DWORD timeout = seconds * 1000; // milliseconds
	auto ret = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
#else
	struct timeval timeout;
	timeout.tv_sec = seconds;
	timeout.tv_usec = 0;

	// setsockopt: 设置套接字选项,为了操作套接字层的选项，此函数的第2个参数的值需指定为SOL_SOCKET，第3个参数SO_SNDTIMEO表示发送超时，第4个参数指定超时时间
	// 默认情况下send函数在发送数据的时候是不会超时的，当没有数据的时候会永远阻塞
	auto ret = setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
	if (ret < 0) {
		fprintf(stderr, "fail to setsockopt: send\n");
		return -1;
	}

	return 0;
}

// 设置接收数据recv超时
int set_client_recv_time_out(SOCKET fd, int seconds)
{
	if (seconds <= 0) {
		fprintf(stderr, "seconds should be greater than 0: %d\n", seconds);
		return -1;
	}

#ifdef _MSC_VER
	DWORD timeout = seconds * 1000; // milliseconds
	auto ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
	struct timeval timeout;
	timeout.tv_sec = seconds;
	timeout.tv_usec = 0;

	// setsockopt: 此函数的第3个参数SO_RCVTIMEO表示接收超时，第4个参数指定超时时间
	// 默认情况下recv函数在接收数据的时候是不会超时的，当没有数据的时候会永远阻塞
	auto ret = setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
	if (ret < 0) {
		fprintf(stderr, "fail to setsockopt: recv\n");
		return -1;
	}

	return 0;
}

} // namespace

int test_socket_tcp_client()
{
	// 1.创建流式套接字
	auto fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		auto err_code = get_error_code();
		std::error_code ec(err_code, std::system_category());
		fprintf(stderr, "fail to socket: %d, error code: %d, message: %s\n", fd, err_code, ec.message().c_str());
		return -1;
	}

	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(server_port_);
	auto ret = inet_pton(AF_INET, server_ip_, &server_addr.sin_addr);
	if (ret != 1) {
		fprintf(stderr, "fail to inet_pton: %d\n", ret);
		return -1;
	}

	set_client_send_time_out(fd, 2); // 设置write或send超时时间
	set_client_recv_time_out(fd, 2); // 设置read或recv超时时间

	// 2.连接
	// connect函数的第二参数是一个指向数据结构sockaddr的指针，其中包括客户端需要连接的服务器的目的端口和IP地址，以及协议类型
	ret = set_client_connect_time_out(fd, (struct sockaddr*)&server_addr, sizeof(server_addr), 2); // 设置连接超时时间
	//ret = connect(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
	if (ret != 0) {
		auto err_code = get_error_code();
		std::error_code ec(err_code, std::system_category());
		fprintf(stderr, "fail to connect: %d, error code: %d, message: %s\n", ret, err_code, ec.message().c_str());
		return -1;
	}

	// 3.接收和发送数据
	// 向服务器端发送数据
	const char* buf_send = "https://blog.csdn.net/fengbingchun";
	const char* ptr = buf_send;
	auto length = strlen(buf_send);
	auto left_send = length + 1; // 以空字符作为发送结束的标志

	// 以下注释掉的code仅用于测试write或send超时时间
	//std::unique_ptr<char> buf_send(new char[1024 * 1024]);
	//int length = 1024 * 1024;
	//long long count = 0;
	//for (;;) {
		//int left_send = length + 1;
		//const char* ptr = buf_send.get();
		//fprintf(stdout, "count: %lld\n", ++count);
		while (left_send > 0) {
			// send: 将缓冲区ptr中大小为left_send的数据，通过套接字文件描述符fd按照第4个参数flags指定的方式发送出去
			// send的返回值是成功发送的字节数.由于用户缓冲区ptr中的数据在通过send函数进行发送的时候，并不一定能够
			// 全部发送出去，所以要检查send函数的返回值，按照与计划发送的字节长度left_send是否相等来判断如何进行下一步操作
			// 当send的返回值小于left_send的时候，表明缓冲区中仍然有部分数据没有成功发送，这是需要重新发送剩余部分的数据
			// send发生错误的时候返回值为-1
			// 注意：send的成功返回并不一定意味着数据已经送到了网络中，只说明协议栈有足够的空间缓存数据，协议栈可能会为了遵循协议的约定推迟传输
			auto sended_length = send(fd, ptr, left_send, 0); // write
			if (sended_length <= 0) {
				auto err_code = get_error_code();
				if (sended_length < 0 && err_code == EINTR) {
					continue;
				}

				std::error_code ec(err_code, std::system_category());
				fprintf(stderr, "fail to send: %d, err code: %d, message: %s\n", sended_length, err_code, ec.message().c_str());
				return -1;
			}
			left_send -= sended_length;
			ptr += sended_length;
		}
	//}

	// 从服务器端接收数据
	const int length_recv_buf = 2048;
	char buf_recv[length_recv_buf];
	std::vector<char> recved_data;
	while (1) {
		// recv: 用于接收数据，从套接字fd中接收数据放到缓冲区buf_recv中，第4个参数用于设置接收数据的方式
		// recv的返回值是成功接收到的字节数，当返回值为-1时错误发生
		auto num = recv(fd, buf_recv, length_recv_buf, 0); // read
		if (num <= 0) {
			auto err_code = get_error_code();
			if (num < 0 && err_code == EINTR) {
				continue;
			}

			std::error_code ec(err_code, std::system_category());
			fprintf(stderr, "fail to recv: %d, err code: %d, message: %s\n", num, err_code, ec.message().c_str());
			return -1;
		}

		bool flag = false;
		std::for_each(buf_recv, buf_recv + num, [&flag, &recved_data](const char& c) {
			if (c == '\0') flag = true; // 以空字符作为接收结束的标志
			else recved_data.emplace_back(c);
		});

		if (flag == true) break;
	}

	// 4.关闭套接字
	close(fd);

	// 验证接收的数据是否是预期的
	fprintf(stdout, "send data: %s\n", buf_send, recved_data.data());
	fprintf(stdout, "recved data: ");
	std::for_each(recved_data.data(), recved_data.data() + recved_data.size(), [](const char& c){
		fprintf(stdout, "%c", c);
	});
	fprintf(stdout, "\n");

	std::string str(recved_data.data());
	auto length2 = std::stoi(str);
	if (length != length2) {
		fprintf(stderr, "received data is wrong: %d, %d\n", length, length2);
		return -1;
	}

	return 0;
}

int test_socket_tcp_server()
{
	// 1.创建流式套接字
	// socket:参数依次为协议族、协议类型、协议编号. AF_INET: 以太网；
	// SOCK_STREAM：流式套接字，TCP连接，提供序列化的、可靠的、双向连接的字节流
	auto fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		auto err_code = get_error_code();
		std::error_code ec(err_code, std::system_category());
		fprintf(stderr, "fail to socket: %d, error code: %d, message: %s\n", fd, err_code, ec.message().c_str());
		return -1;
	}

	// 2.绑定地址端口
	// sockaddr_in: 以太网套接字地址数据结构，与结构sockaddr大小完全一致
	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	// htons: 网络字节序转换函数，还包括htonl, ntohs, ntohl等，
	// 其中s是short数据类型的意思，l是long数据类型的意思,而h是host,即主机的意思，n是network，即网络的意思,
	// htons: 表示对于short类型的变量，从主机字节序转换为网络字节序
	server_addr.sin_port = htons(server_port_);
	// inet_xxx: 字符串IP地址和二进制IP地址转换函数
	// inet_pton: 将字符串类型的IP地址转换为二进制类型,第1个参数表示网络类型的协议族
	auto ret = inet_pton(AF_INET, server_ip_, &server_addr.sin_addr);
	if (ret != 1) {
		fprintf(stderr, "fail to inet_pton: %d\n", ret);
		return -1;
	}

	// sockaddr: 通用的套接字地址数据结构，它可以在不同协议族之间进行转换,包含了地址、端口和IP地址的信息
	ret = bind(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
	if (ret != 0) {
		auto err_code = get_error_code();
		std::error_code ec(err_code, std::system_category());
		fprintf(stderr, "fail to bind: %d, error code: %d, message: %s\n", ret, err_code, ec.message().c_str());
		return -1;
	}

	//std::this_thread::sleep_for(std::chrono::seconds(30)); // 为了验证客户端连接会超时

	// 3.监听端口
	// listen: 用来初始化服务器可连接队列，服务器处理客户端连接请求的时候是顺序处理的，同一时间仅能处理一个客户端连接.
	// 当多个客户端的连接请求同时到来的时候，服务器并不是同时处理，而是将不能处理的客户端连接请求放到等待队列中，这个队列的长度有listen函数来定义
	// listen的第二个参数表示在accept函数处理之前在等待队列中的客户端的长度，如果超过这个长度，客户端会返回一个错误
	ret = listen(fd, server_listen_queue_length_);
	if (ret != 0) {
		auto err_code = get_error_code();
		std::error_code ec(err_code, std::system_category());
		fprintf(stderr, "fail to listen: %d, error code: %d, message: %s\n", ret, err_code, ec.message().c_str());
		return -1;
	}

	while (1) {
		struct sockaddr_in client_addr;
		socklen_t length = sizeof(client_addr);
		// 4.接收客户端的连接，在这个过程中客户端与服务器进行三次握手，建立TCP连接
		// accept成功执行后，会返回一个新的套接字文件描述符来表示客户端的连接，客户端连接的信息可以通过这个新描述符来获得
		// 当服务器成功处理客户端的请求连接后，会有两个文件描述符，老的文件描述符表示正在监听的socket，新产生的文件描述符表示客户端的连接
		auto fd2 = accept(fd, (struct sockaddr*)&client_addr, &length);
		if (fd2 < 0) {
			auto err_code = get_error_code();
			std::error_code ec(err_code, std::system_category());
			fprintf(stderr, "fail to accept: %d, error code: %d, message: %s\n", fd2, err_code, ec.message().c_str());
			continue;
		}
		struct in_addr addr;
		addr.s_addr = client_addr.sin_addr.s_addr;
		// inet_ntoa: 将二进制类型的IP地址转换为字符串类型
		fprintf(stdout, "client ip: %s\n", inet_ntoa(addr));

		// 5.接收和发送数据,处理完后关闭此连接套接字
		// 连接上的每一个客户都有单独的线程来处理
		std::thread(calc_string_length, fd2).detach();
	}

	// 关闭套接字
	close(fd);
	return 0;
}

/////////////////////////////////////////////////////////////////////
int test_get_hostname_ip()
{
	char host_name[4096];
	int ret = gethostname(host_name, sizeof(host_name));
	if ( ret != 0) {
		fprintf(stderr, "fail to gethostname: %d\n", ret);
		return -1;
	}
	fprintf(stdout, "host name: %s\n", host_name);

	struct hostent* ptr = gethostbyname(host_name);
	if (ptr == nullptr) {
		fprintf(stderr, "fail to gethostbyname\n");
		return -1;
	}

	char** pptr = ptr->h_aliases;
	for (; *pptr != nullptr; pptr++)
		fprintf(stdout, "host alias: %s\n", *pptr);
	
	char str[INET_ADDRSTRLEN];
	switch (ptr->h_addrtype) {
	case AF_INET:
		pptr = ptr->h_addr_list;
		for (; *pptr != nullptr; pptr++)
			fprintf(stdout, "ip: %s\n", inet_ntop(ptr->h_addrtype, *pptr, str, sizeof(str)));
		break;

	default:
		fprintf(stderr, "unknown address type\n");
		break;
	}

	return 0;
}

///////////////////////////////////////////////////////////////
// Blog: https://blog.csdn.net/fengbingchun/article/details/100834902
int test_select_1()
{
#ifdef _MSC_VER
	fd_set fds;
	FD_ZERO(&fds);

	timeval tv;
	tv.tv_sec = 2;
	tv.tv_usec = 0;

	int ret = select(0, &fds, nullptr, nullptr, &tv);
	if (ret == SOCKET_ERROR) {
		fprintf(stderr, "fail to select, error: %d\n", WSAGetLastError());
		return -1;
	} else if (ret == 0) {
		fprintf(stderr, "select timeout\n");
		return -1;
	} else {
		fprintf(stdout, "success to select\n");
	}
#else
	const char* path = "/dev/video0";
	int fd = open(path, O_RDWR);
	if (fd == -1) {
		fprintf(stderr, "fail to open device: %s\n", path);
	}

	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(fd, &fds);

	struct timeval tv;
	tv.tv_sec = 2;
	tv.tv_usec = 0;

	int ret = select(fd+1, &fds, nullptr, nullptr, &tv);
	if (ret == -1) {
		fprintf(stderr, "fail to select, error: %d, %s\n", errno, strerror(errno));
		return -1;
	} else if (ret == 0) {
		fprintf(stderr, "select timeout\n");
		return -1;
	} else {
		fprintf(stdout, "success to select\n");
	}

	close(fd);
#endif

	return 0;
}


