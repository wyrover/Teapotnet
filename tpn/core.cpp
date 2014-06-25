/*************************************************************************
 *   Copyright (C) 2011-2013 by Paul-Louis Ageneau                       *
 *   paul-louis (at) ageneau (dot) org                                   *
 *                                                                       *
 *   This file is part of Teapotnet.                                     *
 *                                                                       *
 *   Teapotnet is free software: you can redistribute it and/or modify   *
 *   it under the terms of the GNU Affero General Public License as      *
 *   published by the Free Software Foundation, either version 3 of      *
 *   the License, or (at your option) any later version.                 *
 *                                                                       *
 *   Teapotnet is distributed in the hope that it will be useful, but    *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the        *
 *   GNU Affero General Public License for more details.                 *
 *                                                                       *
 *   You should have received a copy of the GNU Affero General Public    *
 *   License along with Teapotnet.                                       *
 *   If not, see <http://www.gnu.org/licenses/>.                         *
 *************************************************************************/

#include "tpn/core.h"
#include "tpn/config.h"
#include "tpn/scheduler.h"
#include "tpn/binaryserializer.h"
#include "tpn/crypto.h"
#include "tpn/random.h"
#include "tpn/securetransport.h"
#include "tpn/httptunnel.h"
#include "tpn/user.h"

namespace tpn
{

Core *Core::Instance = NULL;


Core::Core(int port) :
		mThreadPool(4, 16, Config::Get("max_connections").toInt()),
		mLastRequest(0),
		mLastPublicIncomingTime(0)
{
	// Define name
	mName = Config::Get("instance_name");
	if(mName.empty())
	{
		char hostname[HOST_NAME_MAX];
		if(!gethostname(hostname,HOST_NAME_MAX)) 
			mName = hostname;
		
		if(mName.empty() || mName == "localhost")
		{
		#ifdef ANDROID
			mName = String("android.") + String::number(unsigned(pseudorand()%1000), 4);
		#else
			mName = String(".") + String::random(6);
		#endif
			Config::Put("instance_name", mName);
			
			const String configFileName = "config.txt";
			Config::Save(configFileName);
		}
	}
	
	// Create backends
	mTunnelBackend = new TunnelBackend();
	mBackends.push_back(new StreamBackend(port));
	mBackends.push_back(new DatagramBackend(port));
	mBackends.push_back(mTunnelBackend);
	
	// Start backends
	for(List<Backend*>::iterator it = mBackends.begin();
		it != mBackends.end();
		++it)
	{
		Backend *backend = *it;
		backend->start();
	}
}

Core::~Core(void)
{
	// Delete backends
	for(List<Backend*>::iterator it = mBackends.begin();
		it != mBackends.end();
		++it)
	{
		Backend *backend = *it;
		backend->wait();
		delete backend;
	}
}

String Core::getName(void) const
{
	Synchronize(this);
	return mName;
}

void Core::getAddresses(List<Address> &list) const
{
	Synchronize(this);
	mSock.getLocalAddresses(list);
}

void Core::getKnownPublicAdresses(List<Address> &list) const
{
	Synchronize(this);
	list.clear();
	for(	Map<Address, int>::const_iterator it = mKnownPublicAddresses.begin();
		it != mKnownPublicAddresses.end();
		++it)
	{
		list.push_back(it->first);
	}
}

bool Core::isPublicConnectable(void) const
{
	return (Time::Now()-mLastPublicIncomingTime <= 3600.); 
}

void Core::registerPeering(	const Identifier &peering,
				const Identifier &remotePeering,
		       		const BinaryString &secret,
				Core::Listener *listener)
{
	Synchronize(this);
	
	mPeerings[peering] = remotePeering;
	mSecrets[peering] = secret;
	if(listener) mListeners[peering] = listener;
	else mListeners.erase(peering);
}

void Core::unregisterPeering(const Identifier &peering)
{
	Synchronize(this);
	
	mPeerings.erase(peering);
	mSecrets.erase(peering);
}

bool Core::hasRegisteredPeering(const Identifier &peering)
{
	Synchronize(this);
	return mPeerings.contains(peering);
}

void Core::registerCaller(const BinaryString &target, Caller *caller)
{
	mCallers[target].insert(caller);
}

void Core::unregisterCaller(const BinaryString &target, Caller *caller)
{
	Map<BinaryString, Set<Caller*> >::iterator it = mCallers.find(target);
	if(it != mCallers.end())
	{
		it->second.erase(caller);
		if(it->second.empty())   
			mCallers.erase(it);
	}
}

void unregisterAllCallers(const BinaryString &target)
{
	  mCallers.erase(target);
}

void Core::route(Missive &missive, const Identifier &from)
{
	Synchronize(this);
	
	// 1st case: neighbour
	// TODO

	// 2nd case: routing table entry exists
	Identifier route;
	if(mRoutes.get(missive.destination, route))
	{
		// TODO
		return;
	}

	// 3rd case: no routing table entry
	broadcast(missive, from);
}

void Core::broadcast(Missive &missive, const Identifier &from)
{
	Synchronize(this);
	
	Array<Identifier> identifiers;
	mHandlers.getKeys(identifiers);
	
	for(int i=0; i<identifiers.size(); ++i)
	{
		if(identifier == from) continue;
		
		Handler *handler;
		if(mHandlers.get(identifiers[i], handler))
		{
			Desynchronize(this);
			handler->send(missive);
		}
	}
}

bool Core::addRoute(const Identifier &id, const Identifier &route)
{
	Synchronize(this);
	
	mRoutes.insert(id, route);
	return true;
}

bool Core::getRoute(const Identifier &id, Identifier &route)
{
	Synchronize(this);
	
	Map<Identifier, Identifier>::iterator it = mRoutes.find(id);
	if(it == mRoutes.end()) return false;
	route = it->second;
	return true;
}

void Core::addPeer(Stream *bs, const Address &remoteAddr, const Identifier &peering)
{
	Assert(bs);
	Synchronize(this);
	
	bool hasPeering = (peering != Identifier::Null);

	if(hasPeering && !mPeerings.contains(peering))
		throw Exception("Added peer with unknown peering");
	
	{
		Desynchronize(this);
		//LogDebug("Core", "Spawning new handler");
		Handler *handler = new Handler(this, bs, remoteAddr);
		if(hasPeering) handler->setPeering(peering);
		mThreadPool.launch(handler);
	}
}

bool Core::hasPeer(const Identifier &peering)
{
	Synchronize(this);
	return mHandlers.contains(peering);
}

bool Core::getInstancesNames(const Identifier &peering, Array<String> &array)
{
	array.clear();
	
	Map<Identifier,Handler*>::iterator it = mHandlers.lower_bound(peering);
	if(it == mHandlers.end() || it->first != peering) return false;
		
	while(it != mHandlers.end() && it->first == peering)
	{
		String name = it->first.getName();
		if(name.empty()) name = "default";
		array.push_back(name);
		++it;
	}
	
	return true;
}

void Core::run(void)
{
	LogDebug("Core", "Starting...");
	
	try {
		while(true)
		{
			Thread::Sleep(0.01);

			Socket *sock = new Socket;
			mSock.accept(*sock);
			
			try {
				Address addr;
				const size_t peekSize = 5;	
				char peekData[peekSize];
				std::memset(peekData, 0, peekSize);
				
				try {
					addr = sock->getRemoteAddress();
					LogDebug("Core::run", "Incoming connection from " + addr.toString());
					
					if(addr.isPublic() && addr.isIpv4()) // TODO: isPublicConnectable() currently reports state for ipv4 only
						mLastPublicIncomingTime = Time::Now();
					
					sock->setTimeout(milliseconds(Config::Get("tpot_timeout").toInt()));
					sock->peekData(peekData, peekSize);
					
					sock->setTimeout(milliseconds(Config::Get("tpot_read_timeout").toInt()));
				}
				catch(const std::exception &e)
				{
					delete sock;
					throw;
				}
			
				Stream *bs = NULL;
				
				if(std::memcmp(peekData, "GET ", 4) == 0
					|| std::memcmp(peekData, "POST ", 5) == 0)
				{
					// This is HTTP, forward connection to HttpTunnel
					bs = HttpTunnel::Incoming(sock);
					if(!bs) continue;
				}
				else {
					bs = sock;
				}
				
				LogInfo("Core", "Incoming peer from " + addr.toString() + " (tunnel=" + (bs != sock ? "true" : "false") + ")");
				addPeer(bs, addr, Identifier::Null, true);	// async
			}
			catch(const std::exception &e)
			{
				LogDebug("Core::run", String("Processing failed: ") + e.what());
			}
		}
	}
	catch(const NetException &e)
	{
		LogDebug("Core::run", e.what());
	}
	
	LogDebug("Core", "Finished");
}

bool Core::addHandler(const Identifier &peer, Core::Handler *handler)
{
	Assert(handler != NULL);
	Synchronize(this);
	
	Handler *h = NULL;
	if(mHandlers.get(peer, h))
		return (h == handler);

	mHandlers.insert(peer, handler);
	return true;
}

bool Core::removeHandler(const Identifier &peer, Core::Handler *handler)
{
	Assert(handler != NULL);
	Synchronize(this);
  
	Handler *h = NULL;
	if(!mHandlers.get(peer, h) || h != handler)
		return false;
	
	mHandlers.erase(peer);
	return true;
}

Core::Missive::Missive(void) :
	data(1024)
{
	
}

Core::Missive::~Missive(void)
{
	
}

void Core::Missive::prepare(const Identifier &source, const Identifier &destination)
{
	this->source = source;
	this->destination = destination;
	data.clear();
}

void Core::Missive::clear(void)
{
	source.clear();
	destination.clear();
	data.clear();
}

void Core::Missive::serialize(Serializer &s) const
{
	// TODO
	s.output(source);
	s.output(destination);
	s.output(data);
}

bool Core::Missive::deserialize(Serializer &s)
{
	// TODO
	if(!s.input(source)) return false;
	AssertIO(s.input(destination));
	AssertIO(s.input(data));
}

Core::Locator::Locator(const Identifier &id)
{
	identifier = id;
}

Core::Locator::Locator(const Address &addr)
{
	addresses.push_back(addr);
}

Core::Locator::~Locator(void)
{

}

Core::Publisher::Publisher(void)
{

}

Core::Publisher::~Publisher(void)
{
	for(StringSet::iterator it = mPublishedPrefixes.begin();
		it != mPublishedPrefixes.end();
		++it)
	{
		Core::Instance->unpublish(*it, this);
	}
}

void Core::Publisher::publish(const String &prefix)
{
	if(!mPublishedPrefixes.contains(path))
	{
		Core::Instance->publish(path, this);
		mPublishedPrefixes.insert(path);
	}
}

void Core::Publisher::unpublish(const String &prefix)
{
	if(mPublishedPrefixes.contains(path))
	{
		Core::Instance->unpublish(path, this);
		mPublishedPrefixes.erase(path);
	}
}

Core::Subscriber::Subscriber(void)
{
	
}

Core::Subscriber::~Subscriber(void)
{
	for(StringSet::iterator it = mSubscribedPrefixes.begin();
		it != mSubscribedPrefixes.end();
		++it)
	{
		Core::Instance->unsubscribe(*it, this);
	}
}

void Core::Subscriber::subscribe(const String &prefix)
{
	if(!mSubscribedPrefixes.contains(path))
	{
		Core::Instance->subscribe(path, this);
		mSubscribedPrefixes.insert(path);
	}
}

void Core::Subscriber::unsubscribe(const String &prefix)
{
	if(mSubscribedPrefixes.contains(path))
	{
		Core::Instance->unsubscribe(path, this);
		mSubscribedPrefixes.erase(path);
	}
}

Caller::Caller(void)
{
	
}

Caller::Caller(const BinaryString &target)
{
	Assert(!target.empty());
	startCalling(target);
}

Caller::~Caller(void)
{
	stopCalling();
}
	
void Caller::startCalling(const BinaryString &target)
{
	if(target != mTarget)
	{
		stopCalling();
		
		mTarget = target;
		if(!mTarget.empty()) Core::Instance->registerCaller(mTarget, this);
	}
}

void Caller::stopCalling(void)
{
	if(!mTarget.empty())
	{
		Core::Instance->unregisterCaller(mTarget, this);
		mTarget.clear();
	}
}

Core::Backend::Backend(Core *core) :
	mCore(core)
{
	Assert(mCore);
}

Core::Backend::~Backend(void)
{
	
}

void Core::Backend::addIncoming(Stream *stream)
{
	// TODO
	mCore->addPeer(stream);
}

void Core::Backend::run(void)
{
	class MyVerifier : public SecureTransportServer::Verifier
	{
	public:
		Core *core;
		User *user;
		BinaryString peering;
		BinaryString identifier;
		Rsa::PublicKey publicKey;
		
		MyVerifier(Core *core) { this->core = core; this->user = NULL; }
	
		bool verifyName(const String &name, SecureTransport *transport)
		{
			user = User::Get(name);
			if(user)
			{
				SecureTransport::Credentials *creds = getCertificateCredentials(user);
				if(creds) transport->addCredentials(creds);
			}
			
			return true;	// continue handshake anyway
		}
	
		bool verifyPrivateSharedKey(const String &name, BinaryString &key)
		{
			try {
				peering.fromString(name);
			}
			catch(...)
			{
				return false;
			}
			
			// TODO: set identifier
			
			BinaryString secret;
			if(SynchronizeTest(core, core->mSecrets.get(peering, secret)))
			{
				key = secret;
				return true;
			}
			else {
				return false;
			}
		}
		
		bool verifyCertificate(const Rsa::PublicKey &pub)
		{
			if(!user) return false;
			
			// TODO: Compute identifier and check against user
			publicKey = pub;
		}
	};
	
	try {
		while(true)
		{
			SecureTransport *transport = listen();
			if(!transport) break;
			
			// TODO: should allocate to threadpool
			
			// set verifier
			MyVerifier verifier(mCore);
			transport->setVerifier(&verifier);
			
			// set credentials (certificate added on name verification)
			transport->addCredentials(getAnonymousCredentials());
			transport->addCredentials(getPrivateSharedKeyCredentials());
			
			// do handshake
			transport->handshake();
			
			// TODO: read credentials and feed them to addIncoming
			// Identifier::Null if transport->isAnonymous()
			// verifier.identifier otherwise
			
			if(transport) addIncoming(transport);
		}
	}
	catch(const std::exception &e)
	{
		LogError("Core::Backend::run", e.what());
	}
	
	LogWarn("Core::Backend::run", "Closing backend");
}

Core::StreamBackend::StreamBackend(int port) :
	mSock(port)
{

}

Core::StreamBackend::~StreamBackend(void)
{
	
}

SecureTransport *Core::StreamBackend::connect(const Locator &locator)
{
	for(List<Address>::iterator it = locator.addresses.begin();
		it != locator.addresses.end();
		++it)
	{
		try {
			return connect(*it);
		}
		catch(const NetException &e)
		{
			LogDebug("Core::StreamBackend::connect", e.what());
		}
	}
}

SecureTransport *Core::StreamBackend::connect(const Address &addr)
{
	Socket *sock = new Socket(addr);
	try {
		SecureTransport *transport = new SecureTransportClient(sock, NULL, false);			// stream mode
		addIncoming(transport);
	}
	catch(...)
	{
		delete sock;
		throw;
	}
}

SecureTransport *Core::StreamBackend::listen(void)
{
	while(true)
	{
		SecureTransport *transport = new SecureTransportServer::Listen(mSock);
		if(transport) return transport;
	}
}

Core::DatagramBackend::DatagramBackend(int port) :
	mSock(port)
{
	
}

Core::DatagramBackend::~DatagramBackend(void)
{
	
}

SecureTransport *Core::DatagramBackend::connect(const Locator &locator)
{
	for(List<Address>::iterator it = locator.addresses.begin();
		it != locator.addresses.end();
		++it)
	{
		try {
			return connect(*it);
		}
		catch(const NetException &e)
		{
			LogDebug("Core::DatagramBackend::connect", e.what());
		}
	}
}

SecureTransport *Core::DatagramBackend::connect(const Address &addr)
{
	DatagramStream *stream = new DatagramStream(&mSock, addr);
	try {
		SecureTransport *transport = new SecureTransportClient(stream, NULL, true);		// datagram mode
		return transport;
	}
	catch(...)
	{
		delete stream;
		throw;
	}
}

SecureTransport *Core::DatagramBackend::listen(void)
{
	while(true)
	{
		SecureTransport *transport = new SecureTransportServer::Listen(mSock);
		if(transport) return transport;
	}
}

TunnelBackend::TunnelBackend(void) :
	Subscriber(Identifier::Null)	// subscribe to everything delegated
{

}

TunnelBackend::~TunnelBackend(void)
{
	
}

SecureTransport *TunnelBackend::connect(const Locator &locator)
{
	Identifier remote = locator.identifier;
	Identifier local = Identifier::Random();

	TunnelWrapper *wrapper = NULL;
	SecureTransport *transport = NULL;
	try {
		wrapper = new TunnelWrapper(local, remote);
		transport = new SecureTransportServer(wrapper, NULL, true);	// datagram mode
	}
	catch(...)
	{
		delete wrapper;
		throw;
	}
	
	return transport;
}

SecureTransport *TunnelBackend::listen(void)
{
	Synchronizable(&mQueueSync);
	while(mQueue.empty()) mQueueSync.wait();
	
	Missive &missive = mQueue.front();
	Assert(missive.type() == Missive::Tunnel);
	
	TunnelWrapper *wrapper = NULL;
	SecureTransport *transport = NULL;
	try {
		wrapper = new TunnelWrapper(missive.destination, missive.source);
		transport = new SecureTransportServer(sock, NULL, true);	// datagram mode
	}
	catch(...)
	{
		delete wrapper;
		mQueue.pop();
		throw;
	}
	
	mQueue.pop();
	return transport;
}

bool Core::TunnelBackend::incoming(Missive &missive)
{
	if(missive.type() == Missive::Tunnel)
	{
		Synchronizable(&mQueueSync);
		mQueue.push(missive);
		return true;
	}
	
	return false;
}

Core::Handler::Handler(Core *core, Stream *stream) :
	mCore(core),
	mStream(stream),
	mIsIncoming(true),
	mIsRelay(false),
	mIsRelayEnabled(Config::Get("relay_enabled").toBool()),
	mThreadPool(0, 1, 8),
	mStopping(false)
{

}

Core::Handler::~Handler(void)
{
	mRunner.clear();
	delete mStream;
}

void Core::Handler::publish(const String &prefix, Publisher *publisher)
{
	Synchronize(this);
	
	if(!prefix.empty() && prefix[prefix.size()-1] == '/')
		prefix.resize(prefix.size()-1);

	mPublishers[prefix].insert(publisher);
}

void Core::Handler::unpublish(const String &prefix, Publisher *publisher)
{
	Synchronize(this);
  
	if(!prefix.empty() && prefix[prefix.size()-1] == '/')
		prefix.resize(prefix.size()-1);

	Map<String, Set<Publisher*> >::iterator it = mPublishers.find(prefix);
	if(it != mPublishers.end())
	{
		it->second.erase(publisher);
		if(it->second.empty()) 
			mPublishers.erase(it);
	}
}

void Core::Handler::subscribe(const String &prefix, Subscriber *subscriber)
{
	Synchronize(this);
	
	if(!prefix.empty() && prefix[prefix.size()-1] == '/')
		prefix.resize(prefix.size()-1);

	mSubscribers[prefix].insert(subscriber);
}

void Core::Handler::unsubscribe(const String &prefix, Subscriber *subscriber)
{
	Synchronize(this);
  
	if(!prefix.empty() && prefix[prefix.size()-1] == '/')
		prefix.resize(prefix.size()-1);

	Map<String, Set<Subscriber*> >::iterator it = mSubscribers.find(prefix);
	if(it != mSubscribers.end())
	{
		it->second.erase(subscriber);
		if(it->second.empty())
			mSubscribers.erase(it);
	}
}

bool Core::Handler::recv(Missive &missive)
{
	Synchronize(this);
	
	// TODO: remove serializer
	BinarySerializer serializer(mStream);
	
	{
		Desynchronize(this);
		MutexLocker(&mStreamReadMutex);
		return serializer.input(missive);
	}
}

void Core::Handler::send(const Missive &missive)
{
	Synchronize(this);
	
	// TODO: remove serializer
	BinarySerializer serializer(mStream);
	
	{
		Desynchronize(this);
		MutexLocker(&StreamWriteMutex);
		return serializer.output(missive);
	}
}

bool Core::Handler::incoming(const Identifier &source, uint8_t content, Stream &payload)
{
	Synchronize(this);
  
	switch(content)
	{
		case Missive::Notify:
		{
			Sender *sender;
			if(!mSenders.contains(source)) mSenders[source] = new Sender(this, source);
			mSenders[source]->ack(payload); 
			mCore->incomingNotification(source, payload);
			break;
		}
		
		case Missive::Ack:
		{
			Sender *sender;
			if(mSenders.get(source, sender))
				sender->acked(payload);
			break;
		}
		
		case Missive::Call:
		{
			BinaryString target;
			uint16_t tokens;
			AssertIO(payload.readBinary(target));
			AssertIO(payload.readBinary(tokens));
			
			if(!mSenders.contains(source)) mSenders[source] = new Sender(this, source);
			mSenders[source]->addTarget(target, tokens);
			break;
		}
		
		case Missive::Cancel:
		{
			BinaryString target;
			AssertIO(payload.readBinary(target));
			
			Map<BinaryString, Sender*>::iterator it = mSenders.find(source);
			if(it != mSenders.end())
			{
				it->second->removeTarget(target);
				
				// TODO
				/*if(it->second->empty())
				{
					delete it->second;
					mSenders.erase(it);
				}*/
			}
			break;
		}
		
		case Missive::Data:
		{
			BinaryString target;
			AssertIO(payload.readBinary(target));
			
			if(mStore::Instance->push(target, payload))
			{
				unregisterAllCallers(target);
				outgoing(source, Missive::Cancel, target);
			}
			break;
		}

		case Missive::Publish:
		case Missive::Subscribe:
		{
			String path;
			AssertIO(payload.readBinary(path));
	
			List<String> list;
			path.explode(list,'/');
			if(list.empty()) return;		// TODO
	
			// First item should be empty because path begins with /
			if(list.front().empty()) 
				list.pop_front();
	
			// Match prefixes, longest first
			while(!list.empty())
			{
				String prefix;
				prefix.implode(list, '/');
				prefix = "/" + prefix;
				list.pop_back();
			
				if(content == Missive::Publish)
				{
					BinaryString target;
					while(payload.readBinary(target))
					{
						// Pass to local subscribers
						Map<String, Set<Subscriber*> >::iterator it = mSubscribers.find(prefix);
						if(it != mSubscribers.end())
						{
							for(Set<Subscriber*>::iterator jt = it->second.begin();
								jt != it->second.end();
								++jt)
							{
								if(jt->incoming(prefix, target))
									return;					
							}
						}
					}
				}
				else {
					BinaryString response;
				 	response.writeBinary(path);
					
					// Pass to local publishers
					Map<String, Set<Publisher*> >::iterator it = mPublishers.find(prefix);
					if(it != mPublishers.end())
					{
						for(Set<Publisher*>::iterator jt = it->second.begin();
							jt != it->second.end();
							++jt)
						{
							BinaryString target;
							if(jt->anounce(prefix, target))
								response.writeBinary(target);
						}
					}
					
					outgoing(source, Missive::Publish, response);
				}
			}

			break;
		}
	}
	
	return true;
}

void Core::Handler::outgoing(const Identifier &dest, uint8_t content, Stream &payload)
{
	Missive missive;
	missive.prepare(mLocal, dest);
	missive.writeBinary(content);
	missive.writeBinary(payload);
	send(missive);
}

void Core::Handler::process(void)
{
	String command, args;
	StringMap parameters;
  
	Synchronize(this);
	LogDebug("Core::Handler", "Starting...");
	
	Missive missive;
	missive.prepare(mLocal, mRemote);
	// TODO
	
	while(recv(missive))
	{
		try {
			switch(missive.type)
			{
				case Missive::Forward:
					if(missive.destination == mLocal) incoming(missive);
					else route(missive);
					break;
					
				case Missive::Broadcast:
					incoming(missive);
					route(missive);
					break;
					
				case Missive::Lookup:
					if(missive.destination == mLocal) incoming(missive);
					else if(!incoming(missive))
						route(missive);
					
				default:
					// Drop
					break;
			}
		}
		catch(const std::exception &e)
		{
			LogWarn("Core::Handler", e.what()); 
			return;
		}
	}
	
	try {
		Synchronize(mCore);
		
		mCore->removeHandler(mPeering, this);
		 
		if(mCore->mKnownPublicAddresses.contains(mRemoteAddr))
		{
			mCore->mKnownPublicAddresses[mRemoteAddr]-= 1;
			if(mCore->mKnownPublicAddresses[mRemoteAddr] == 0)
				mCore->mKnownPublicAddresses.erase(mRemoteAddr);
		}
	}
	catch(const std::exception &e)
	{
		LogError("Core::Handler", e.what()); 
	}
	
	Listener *listener = NULL;
	if(SynchronizeTest(mCore, mCore->mListeners.get(peering, listener)))
	{
		try {
			listener->disconnected(peering);
		}
		catch(const Exception &e)
		{
			LogWarn("Core::Handler", String("Listener disconnected callback failed: ") + e.what());
		}
	}
}

void Core::Handler::run(void)
{
	try {
		process();
	}
	catch(const std::exception &e)
	{
		LogWarn("Core::Handler::run", String("Unhandled exception: ") + e.what()); 
	}
	catch(...)
	{
		LogWarn("Core::Handler::run", String("Unhandled unknown exception")); 
	}
		
	notifyAll();
	Thread::Sleep(5.);	// TODO
	delete this;		// autodelete
}

Core::Handler::Sender::Sender(Handler *handler, const BinaryString &destination) :
	mHandler(handler),
	mDestination(destination),
	mCurrentSequence(0)
{
	
}

Core::Handler::Sender::~Sender(void)
{
	handler->mRunner.cancel(this);
}

void Core::Handler::Sender::addTarget(const BinaryString &target, unsigned tokens)
{
	Synchronize(this);
	mTargets.insert(target, tokens);
	handler->mRunner.schedule(this);
}

void Core::Handler::Sender::removeTarget(const BinaryString &target)
{
	Synchronize(this);
	mTargets.erase(target);
}

void Core::Handler::Sender::addTokens(unsigned tokens)
{
	Synchronize(this);
	mTokens+= tokens;
	handler->mRunner.schedule(this);
}

void Core::Handler::Sender::removeTokens(unsigned tokens)
{
	Synchronize(this);
	if(mTokens > tokens) mTokens-= tokens;
	else mTokens = 0;
}

bool Core::Handler::Sender::empty(void) const
{
	Synchronize(this);
	return mTargets.empty() && mUnacked.empty();
}

void Core::Handler::Sender::notify(Stream &payload, bool ack)
{
	uint32_t sequence = 0;
	if(ack)
	{
		++mCurrentSequence;
		if(!mCurrentSequence) ++mCurrentSequence;
		sequence = mCurrentSequence;
	}

	// TODO: Move to SendTask
	Missive missive;
	missive.prepare(mLocal, dest);
	missive.writeBinary(uint8_t(Missive::Notify));
	missive.writeBinary(uint32_t(sequence));
	missive.writeBinary(payload);
	handler->send(missive);
	
	const double delay = 0.5;	// TODO
	const int count = 5;		// TODO
	mUnacked.insert(sequence, SendTask(this, sequence, missive, delay, count));
}

void Core::Handler::Sender::ack(Stream &payload)
{
	uint32_t sequence;
	AssertIO(payload.readBinary(sequence));
	
	BinaryString ack;
	ack.writeBinary(sequence);
	
	handler->outgoing(dest, Missive::Ack, payload);
}

void Core::Handler::Sender::acked(Stream &payload)
{
	uint32_t sequence;
	AssertIO(payload.readBinary(sequence));
	mUnacked.erase(sequence);
}

void Core::Handler::Sender::run(void)
{
	Synchronize(this);
	
	// TODO: tokens
	
	if(/*!mTokens ||*/ mTargets.empty()) 
		return;
	
	Map<BinaryString, unsigned>::iterator it = mTargets.find(mNextTarget);
	if(it == mTargets.end()) it = mTargets.begin();
	mNextTarget.clear();
	
	if(it->second)
	{
		BinaryString data;
		mStore::Instance->pull(it->first, data);
		
		//--mTokens;
		--it->second;
		
		if(it->second) ++it;
		else mTargets.erase(it++);
		if(it != mTargets.end()) mNextTarget = it->first;
		
		BinaryString dest(mDest);
		DesynchronizeStatement(this, mHandler->outgoing(dest, Missive::Data, data));
		
		// Warning: iterator is not valid anymore here
	}
	else {
		mTargets.erase(it++);
		if(it != mTargets.end()) mNextTarget = it->first;
	}
	
	handler->mRunner.schedule(this);
}

Core::Handler::Sender::SendTask::ResendTask(Sender *sender, uint32_t sequence, Missive missive, double delay, int count) :
	mSender(sender),
	mMissive(missive),
	mLeft(count),
	mSequence(sequence)
{
	if(mLeft > 0)
	{
		Synchronize(sender);
		sender->mScheduler.schedule(this);
		sender->mScheduler.repeat(missive, delay);
	}
}

Core::Handler::Sender::SendTask::~ResendTask(void)
{
	Synchronize(sender);
	sender->mScheduler.cancel(this);
}

void Core::Handler::Sender::SendTask::run(void)
{  
	sender->mHandler->send(missive);
	
	--mLeft;
	if(mLeft <= 0)
	{
		Synchronize(sender);
		sender->mScheduler.cancel(this);
		sender->mUnacked.erase(mSequence);
	}
}

}
