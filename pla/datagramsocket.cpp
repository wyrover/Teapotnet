/*************************************************************************
 *   Copyright (C) 2011-2013 by Paul-Louis Ageneau                       *
 *   paul-louis (at) ageneau (dot) org                                   *
 *                                                                       *
 *   This file is part of Plateform.                                     *
 *                                                                       *
 *   Plateform is free software: you can redistribute it and/or modify   *
 *   it under the terms of the GNU Affero General Public License as      *
 *   published by the Free Software Foundation, either version 3 of      *
 *   the License, or (at your option) any later version.                 *
 *                                                                       *
 *   Plateform is distributed in the hope that it will be useful, but    *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the        *
 *   GNU Affero General Public License for more details.                 *
 *                                                                       *
 *   You should have received a copy of the GNU Affero General Public    *
 *   License along with Plateform.                                       *
 *   If not, see <http://www.gnu.org/licenses/>.                         *
 *************************************************************************/

#include "pla/datagramsocket.h"
#include "pla/exception.h"
#include "pla/string.h"
#include "pla/time.h"

namespace pla
{

const size_t DatagramSocket::MaxDatagramSize = 1500;

DatagramSocket::DatagramSocket(int port, bool broadcast) :
		mSock(INVALID_SOCKET)
{
	bind(port, broadcast);
}

DatagramSocket::DatagramSocket(const Address &local, bool broadcast) :
		mSock(INVALID_SOCKET)
{
 	bind(local, broadcast);  
}

DatagramSocket::~DatagramSocket(void)
{
	NOEXCEPTION(close());
}

Address DatagramSocket::getBindAddress(void) const
{
	char hostname[HOST_NAME_MAX];
	if(gethostname(hostname,HOST_NAME_MAX))
		throw NetException("Cannot retrieve hostname");

	sockaddr_storage sa;
	socklen_t sl = sizeof(sa);
	int ret = ::getsockname(mSock, reinterpret_cast<sockaddr*>(&sa), &sl);
	if(ret < 0) throw NetException("Cannot obtain Address of socket");

	return Address(reinterpret_cast<sockaddr*>(&sa), sl);
}

void DatagramSocket::getLocalAddresses(Set<Address> &set) const
{
	set.clear();
  
	Address bindAddr = getBindAddress();
  
#ifdef NO_IFADDRS
	// Retrieve hostname
	char hostname[HOST_NAME_MAX];
	if(gethostname(hostname,HOST_NAME_MAX))
		throw NetException("Cannot retrieve hostname");

	// Resolve hostname
	addrinfo *aiList = NULL;
	addrinfo aiHints;
	memset(&aiHints, 0, sizeof(aiHints));
	aiHints.ai_family = AF_UNSPEC;
	aiHints.ai_socktype = SOCK_DGRAM;
	aiHints.ai_protocol = 0;
	aiHints.ai_flags = 0;
	String service;
	service << mPort;
	if(getaddrinfo(hostname, service.c_str(), &aiHints, &aiList) != 0)
	{
		LogWarn("DatagramSocket", "Local hostname is not resolvable !");
		if(getaddrinfo("localhost", service.c_str(), &aiHints, &aiList) != 0)
		{
			set.insert(bindAddr);
			return;
		}
	}
	
	addrinfo *ai = aiList;  
	while(ai)
	{
		Address addr(ai->ai_addr,ai->ai_addrlen);
		if(addr == bindAddr)
		{
			set.clear();
			set.insert(addr);
			break;
		}
		
		if(ai->ai_family == AF_INET || ai->ai_family == AF_INET6)
			set.insert(addr);
		
		ai = ai->ai_next;
	}
	
	freeaddrinfo(aiList);
#else
	ifaddrs *ifas;
	if(getifaddrs(&ifas) < 0)
		throw NetException("Unable to list network interfaces");

        ifaddrs *ifa = ifas;
	while(ifa)
	{
		sockaddr *sa = ifa->ifa_addr;
		socklen_t len = 0;
		switch(sa->sa_family) 
		{
			case AF_INET:  len = sizeof(sockaddr_in);  break;
			case AF_INET6: len = sizeof(sockaddr_in6); break;
		}
		
		if(len)
		{
			Address addr(sa, len);
			String host = addr.host();
			if(host.substr(0,4) != "fe80")
			{
				addr.set(host, mPort);
				if(addr == bindAddr)
				{
					set.clear();
					set.insert(addr);
					break;
				}
				set.insert(addr);
			}
		}
		
		ifa = ifa->ifa_next;
	}

	freeifaddrs(ifas);
#endif
}

void DatagramSocket::getHardwareAddresses(Set<BinaryString> &set) const
{
	set.clear();
	
#ifdef WINDOWS
	IP_ADAPTER_ADDRESSES adapterInfo[16];   
	DWORD dwBufLen = sizeof(IP_ADAPTER_ADDRESSES)*16;
	
	DWORD dwStatus = GetAdaptersAddresses(getBindAddress().addrFamily(), 0, NULL, adapterInfo, &dwBufLen);
	if(dwStatus != ERROR_SUCCESS)
		throw NetException("Unable to retrive hardware addresses");
	
	IP_ADAPTER_ADDRESSES *pAdapterInfo = adapterInfo;
	while(pAdapterInfo)
	{
		if(pAdapterInfo->PhysicalAddressLength)
			set.insert(BinaryString((char*)pAdapterInfo->PhysicalAddress, pAdapterInfo->PhysicalAddressLength));
		
		pAdapterInfo = pAdapterInfo->Next;
	}
#else
	struct ifreq ifr;
	struct ifconf ifc;
	char buf[BufferSize];
	
	ifc.ifc_len = sizeof(buf);
	ifc.ifc_buf = buf;
	if (ioctl(mSock, SIOCGIFCONF, &ifc) == -1)
		throw NetException("Unable to retrive hardware addresses");
	
	bool success = 0;
	for(	struct ifreq* it = ifc.ifc_req;
		it != ifc.ifc_req + ifc.ifc_len/sizeof(struct ifreq);
		++it)
	{
		strcpy(ifr.ifr_name, it->ifr_name);
		
		if (ioctl(mSock, SIOCGIFFLAGS, &ifr) == 0) 
		{
			if(!(ifr.ifr_flags & IFF_LOOPBACK))
			{
				if (ioctl(mSock, SIOCGIFHWADDR, &ifr) == 0)
				{
					set.insert(BinaryString(reinterpret_cast<char*>(ifr.ifr_hwaddr.sa_data), size_t(IFHWADDRLEN)));	// hwaddr.sa_data is big endian
				}
			}
		}
	}
#endif
}

void DatagramSocket::bind(int port, bool broadcast, int family)
{
	close();
	
	mPort = port;
	
	// Obtain local Address
	addrinfo *aiList = NULL;
	addrinfo aiHints;
	std::memset(&aiHints, 0, sizeof(aiHints));
	aiHints.ai_family = family;
	aiHints.ai_socktype = SOCK_DGRAM;
	aiHints.ai_protocol = 0;
	aiHints.ai_flags = AI_PASSIVE;
	String service;
	service << port;
	if(getaddrinfo(NULL, service.c_str(), &aiHints, &aiList) != 0)
		throw NetException("Local binding address resolution failed for UDP port "+service);
	
	try {
		// Prefer IPv6
		addrinfo *ai = aiList;
		while(ai && ai->ai_family != AF_INET6)
			ai = ai->ai_next;
		if(!ai) ai = aiList;
			
		// Create socket
		mSock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if(mSock == INVALID_SOCKET)
		{
			addrinfo *first = ai;
			ai = aiList;
			while(ai)
			{
				if(ai != first) mSock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
				if(mSock != INVALID_SOCKET) break;
				ai = ai->ai_next;
			}
			if(!ai) throw NetException("Datagram socket creation failed");
		}


		// Set options
		int enabled = 1;
		int disabled = 0;
		setsockopt(mSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&enabled), sizeof(enabled));
		if(broadcast) setsockopt(mSock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&enabled), sizeof(enabled));
		if(ai->ai_family == AF_INET6) 
			setsockopt(mSock, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&disabled), sizeof(disabled)); 
		
		// TODO: Seems necessary for DTLS
                //setsockopt(mSock, IPPROTO_IP, IP_DONTFRAG, reinterpret_cast<char*>(&enabled), sizeof(enabled));
		
		// Bind it
		if(::bind(mSock, ai->ai_addr, ai->ai_addrlen) != 0)
			throw NetException(String("Binding failed on UDP port ") + String::number(port));

		/*
		ctl_t b = 1;
		if(ioctl(mSock,FIONBIO,&b) < 0)
		throw Exception("Cannot use non-blocking mode");
		 */
	}
	catch(...)
	{
		freeaddrinfo(aiList);
		close();
		throw;
	}
	
	freeaddrinfo(aiList);
}

void DatagramSocket::bind(const Address &local, bool broadcast)
{
	close();
	
	try {
		mPort = local.port();

		// Create datagram socket
		mSock = ::socket(local.addrFamily(), SOCK_DGRAM, 0);
		if(mSock == INVALID_SOCKET)
			throw NetException("Datagram socket creation failed");

		// Set options
		int enabled = 1;
		setsockopt(mSock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&enabled), sizeof(enabled));
		if(broadcast) setsockopt(mSock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<char*>(&enabled), sizeof(enabled));
		
		// Bind it
		if(::bind(mSock, local.addr(), local.addrLen()) != 0)
			throw NetException(String("Binding failed on ") + local.toString());

		/*
		ctl_t b = 1;
		if(ioctl(mSock,FIONBIO,&b) < 0)
		throw Exception("Cannot use non-blocking mode");
		 */
	}
	catch(...)
	{
		close();
		throw;
	} 
}

void DatagramSocket::close(void)
{
	MutexLocker lock(&mStreamsMutex);
	
	for(Map<Address, DatagramStream*>::iterator it = mStreams.begin();
		it != mStreams.end();
		++it)
	{
		it->second->mSock = NULL;
		it->second->mBufferSync.notifyAll();
	}

	if(mSock != INVALID_SOCKET)
	{
		::closesocket(mSock);
		mSock = INVALID_SOCKET;
		mPort = 0;
	}
}

int DatagramSocket::read(char *buffer, size_t size, Address &sender, double &timeout)
{
	return recv(buffer, size, sender, timeout, 0);
}

int DatagramSocket::read(char *buffer, size_t size, Address &sender, const double &timeout)
{
	double dummy = timeout;
	return  read(buffer, size, sender, dummy);
}

int DatagramSocket::peek(char *buffer, size_t size, Address &sender, double &timeout)
{
	return recv(buffer, size, sender, timeout, MSG_PEEK);
}

int DatagramSocket::peek(char *buffer, size_t size, Address &sender, const double &timeout)
{
	double dummy = timeout;
	return  peek(buffer, size, sender, dummy);
}

void DatagramSocket::write(const char *buffer, size_t size, const Address &receiver)
{
	send(buffer, size, receiver, 0);
}

bool DatagramSocket::read(Stream &stream, Address &sender, double &timeout)
{
	stream.clear();
	char buffer[MaxDatagramSize];
	int size = MaxDatagramSize;
	size = read(buffer, size, sender, timeout);
	if(size < 0) return false;
	stream.writeData(buffer,size);
	return true;
}

bool DatagramSocket::read(Stream &stream, Address &sender, const double &timeout)
{
	double dummy = timeout;
	return  read(stream, sender, dummy);
}

bool DatagramSocket::peek(Stream &stream, Address &sender, double &timeout)
{
	stream.clear();
	char buffer[MaxDatagramSize];
	int size = MaxDatagramSize;
	size = peek(buffer, size, sender, timeout);
	if(size < 0) return false;
	stream.writeData(buffer,size);
	return true;
}

bool DatagramSocket::peek(Stream &stream, Address &sender, const double &timeout)
{
	double dummy = timeout;
	return peek(stream, sender, dummy);
}

void DatagramSocket::write(Stream &stream, const Address &receiver)
{
	char buffer[MaxDatagramSize];
	size_t size = stream.readData(buffer,MaxDatagramSize);
	write(buffer, size, receiver);
	stream.clear();
}

bool DatagramSocket::wait(double &timeout)
{
	fd_set readfds;
	FD_ZERO(&readfds);
	FD_SET(mSock, &readfds);
	
	struct timeval tv;
	Time::SecondsToStruct(timeout, tv);
	int ret = ::select(SOCK_TO_INT(mSock)+1, &readfds, NULL, NULL, &tv);
	if (ret < 0) throw Exception("Unable to wait on socket");
	if (ret ==  0)
	{
		timeout = 0.;
		return false;
	}
	
	timeout = Time::StructToSeconds(tv);
	return true;
}

int DatagramSocket::recv(char *buffer, size_t size, Address &sender, double &timeout, int flags)
{
	int result = 0;
	size = std::min(size, MaxDatagramSize);

	while(true)
	{
		if(timeout >= 0.)
		{
			if(!wait(timeout))
				return -1;
		}
  
		if(mStreams.empty())
		{
			sockaddr_storage sa;
			socklen_t sl = sizeof(sa);
			result = ::recvfrom(mSock, buffer, size, flags, reinterpret_cast<sockaddr*>(&sa), &sl);
			if(result < 0) throw NetException("Unable to read from socket (error " + String::number(sockerrno) + ")");
			sender.set(reinterpret_cast<sockaddr*>(&sa),sl);
			break;
		}
		else {
			char datagramBuffer[MaxDatagramSize];
			sockaddr_storage sa;
			socklen_t sl = sizeof(sa);
			result = ::recvfrom(mSock, datagramBuffer, MaxDatagramSize, flags, reinterpret_cast<sockaddr*>(&sa), &sl);
			if(result < 0) throw NetException("Unable to read from socket (error " + String::number(sockerrno) + ")");
			sender.set(reinterpret_cast<sockaddr*>(&sa),sl);

			MutexLocker lock(&mStreamsMutex);
			Map<Address, DatagramStream*>::iterator it = mStreams.find(sender);
			if(it == mStreams.end())
			{
				mStreamsMutex.unlock();
				result = std::min(result, int(size));
				std::memcpy(buffer, datagramBuffer, result);
				break;
			}

			DatagramStream *stream = it->second;
			Assert(stream);

			if(result > 0)
			{
				Synchronize(&stream->mBufferSync);
				
				// Wait for last datagram to be processed
				while(!stream->mBuffer.empty())
					stream->mBufferSync.wait();
				
				stream->mBuffer.assign(datagramBuffer, result);
				stream->mBufferSync.notifyAll();
			}		
		}
	}
	
	return result;
}

void DatagramSocket::send(const char *buffer, size_t size, const Address &receiver, int flags)
{
	int result = ::sendto(mSock, buffer, size, flags, receiver.addr(), receiver.addrLen());
	if(result < 0) throw NetException("Unable to write to socket (error " + String::number(sockerrno) + ")");
}

void DatagramSocket::accept(DatagramStream &stream)
{
	BinaryString buffer;
	Address sender;
	read(buffer, sender);
	
	unregisterStream(&stream);
	
	stream.mSock = this;
	stream.mAddr = sender;
	std::swap(stream.mBuffer, buffer);
}

void DatagramSocket::registerStream(const Address &addr, DatagramStream *stream)
{
	Assert(stream);
	MutexLocker lock(&mStreamsMutex);
	Map<Address, DatagramStream*>::iterator it = mStreams.find(stream->mAddr);
	if(it != mStreams.end() && it->second != stream) it->second->mSock = NULL;
	mStreams.insert(addr, stream);
}

bool DatagramSocket::unregisterStream(DatagramStream *stream)
{
	Assert(stream);
	MutexLocker lock(&mStreamsMutex);
	Map<Address, DatagramStream*>::iterator it = mStreams.find(stream->mAddr);
	if(it == mStreams.end() || it->second != stream) return false;
	mStreams.erase(it);
	return true;
}

double DatagramStream::ReadTimeout = 60.; // 1 min

DatagramStream::DatagramStream(void) :
	mSock(NULL)
{
	
}

DatagramStream::DatagramStream(DatagramSocket *sock, const Address &addr) :
	mSock(sock),
	mAddr(addr)
{
	Assert(mSock);
	mSock->registerStream(addr, this);
}

DatagramStream::~DatagramStream(void)
{
	if(mSock) mSock->unregisterStream(this);
}

Address DatagramStream::getLocalAddress(void) const
{
	// TODO: this is actually different from local address
	return mSock->getBindAddress();
}

Address DatagramStream::getRemoteAddress(void) const
{
	return mAddr;
}

size_t DatagramStream::readData(char *buffer, size_t size)
{
	Synchronize(&mBufferSync);
	
	double timeout = ReadTimeout;
	while(mBuffer.empty())
	{
		if(!mSock) return 0;
		if(!mBufferSync.wait(timeout)) return false;
	}

	size = std::min(size, size_t(mBuffer.size()));
	std::memcpy(buffer, mBuffer.data(), size);
	mBuffer.clear();
	mBufferSync.notifyAll();	
	return size;
}

void DatagramStream::writeData(const char *data, size_t size)
{
	if(!mSock) throw Exception("Datagram socket closed");
	mSock->write(data, size, mAddr);
}

bool DatagramStream::waitData(double &timeout)
{
	Synchronize(&mBufferSync);
	
	while(mBuffer.empty())
	{
		if(!mSock) return true;	// readData will return 0
		if(!mBufferSync.wait(timeout)) return false;
	}
	
	return true;
}

}
