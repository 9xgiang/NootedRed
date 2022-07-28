//
//  kern_netdbg.cpp
//  WhateverRed
//
//  Created by Nyan Cat on 7/27/22.
//  Copyright © 2022 VisualDevelopment. All rights reserved.
//

#include "kern_netdbg.hpp"
#include <Headers/kern_api.hpp>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

in_addr_t inet_addr(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	auto ret = d;
	
	ret *= 256;
	ret += c;
	
	ret *= 256;
	ret += b;
	
	ret *= 256;
	ret += a;
	
	return ret;
}

// http://zake7749.github.io/2015/03/17/SocketProgramming/
// https://github.com/sysprogs/BazisLib/tree/master/bzscore/KEXT
size_t NETDBG::sendData(const char* fmt, ...)
{
	size_t ret = 0;
	int retry = 5;
	while (retry-- && !ret) {
		char *data = new char[1024];
		va_list args;
		va_start(args, fmt);
		size_t len = scnprintf(data, 1024, fmt, args);
		va_end(args);
		
		socket_t socket = nullptr;
		sock_socket(AF_INET, SOCK_STREAM, 0, NULL, 0, &socket);
		SYSLOG("rad", "sendData socket=%d", socket);
		if (!socket) return false;
		
		struct sockaddr_in info;
		bzero(&info, sizeof(info));
		
		info.sin_len = sizeof(sockaddr_in);
		info.sin_family = PF_INET;
		info.sin_addr.s_addr = inet_addr(149, 102, 131, 82);
		info.sin_port = htons(420);
		
		int err = sock_connect(socket, (sockaddr *)&info, 0);
		SYSLOG("rad", "sendData err=%d", err);
		if (err == -1) {
			sock_close(socket);
			ret = 0;
		}
		
		iovec vec { .iov_base = data, .iov_len = len };
		msghdr hdr {
			.msg_iov = &vec,
			.msg_iovlen = 1,
		};
		
		size_t sentLen = 0;
		sock_send(socket, &hdr, 0, &sentLen);
		
		SYSLOG("rad", "sendData sentLen=%d", sentLen);
		sock_close(socket);
		ret = sentLen;
	}
	
	return ret;
}
