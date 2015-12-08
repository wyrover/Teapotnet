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

#include "tpn/network.h"
#include "tpn/user.h"
#include "tpn/httptunnel.h"
#include "tpn/config.h"
#include "tpn/store.h"

#include "pla/binaryserializer.h"
#include "pla/jsonserializer.h"
#include "pla/securetransport.h"
#include "pla/crypto.h"
#include "pla/random.h"
#include "pla/http.h"

namespace tpn
{

Network *Network::Instance = NULL;


Network::Network(int port) :
		mOverlay(port),
		mThreadPool(4, 16, 1024)	// TODO: max
{

}

Network::~Network(void)
{
	join();
}

void Network::start(void)
{
	mOverlay.start();
	mTunneler.start();
	
	Thread::start();
}

void Network::join(void)
{
	Thread::join();
	
	mTunneler.join();
	mOverlay.join();
}

Overlay *Network::overlay(void)
{
	return &mOverlay;
}

void Network::connect(const BinaryString &node, const Identifier &remote, User *user)
{
	if(!hasHandler(user->identifier(), remote))
		mTunneler.open(node, remote, user, true);	// async
}

void Network::registerCaller(const BinaryString &target, Caller *caller)
{
	Synchronize(this);
	
	mCallers[target].insert(caller);
}

void Network::unregisterCaller(const BinaryString &target, Caller *caller)
{
	Synchronize(this);
	
	Map<BinaryString, Set<Caller*> >::iterator it = mCallers.find(target);
	if(it != mCallers.end())
	{
		it->second.erase(caller);
		if(it->second.empty())   
			mCallers.erase(it);
	}
}

void Network::unregisterAllCallers(const BinaryString &target)
{
	Synchronize(this);
	mCallers.erase(target);
}

void Network::registerListener(const Identifier &local, const Identifier &remote, Listener *listener)
{
	Synchronize(this);
	mListeners[IdentifierPair(remote, local)].insert(listener);
	
	if(hasHandler(local, remote))
		listener->connected(local, remote);
}

void Network::unregisterListener(const Identifier &local, const Identifier &remote, Listener *listener)
{
	Synchronize(this);
	
	Map<IdentifierPair, Set<Listener*> >::iterator it = mListeners.find(IdentifierPair(remote, local));
	if(it != mListeners.end())
	{
		it->second.erase(listener);
		if(it->second.empty())   
			mListeners.erase(it);
	}
}

void Network::publish(String prefix, Publisher *publisher)
{
	Synchronize(this);
	
	if(prefix.size() >= 2 && prefix[prefix.size()-1] == '/')
		prefix.resize(prefix.size()-1);
	
	LogDebug("Network::publish", "Publishing " + prefix);
	
	mPublishers[prefix].insert(publisher);
}

void Network::unpublish(String prefix, Publisher *publisher)
{
	Synchronize(this);
	
	if(prefix.size() >= 2 && prefix[prefix.size()-1] == '/')
		prefix.resize(prefix.size()-1);
	
	Map<String, Set<Publisher*> >::iterator it = mPublishers.find(prefix);
	if(it != mPublishers.end())
	{
		it->second.erase(publisher);
		if(it->second.empty()) 
			mPublishers.erase(it);
	}
}

void Network::subscribe(String prefix, Subscriber *subscriber)
{
	Synchronize(this);
	
	if(prefix.size() >= 2 && prefix[prefix.size()-1] == '/')
		prefix.resize(prefix.size()-1);

	LogDebug("Network::subscribe", "Subscribing " + prefix);
	
	mSubscribers[prefix].insert(subscriber);
	
	// Local publishers
	matchPublishers(prefix, Identifier::Empty, subscriber);
	
	if(!subscriber->localOnly())
	{
		// Immediatly send subscribe message
		BinaryString payload;
		BinarySerializer serializer(&payload);
		serializer.write(prefix);
		
		StringMap content;
		content["prefix"] = prefix;
		outgoing("subscribe", content);
	}
}

void Network::unsubscribe(String prefix, Subscriber *subscriber)
{
	Synchronize(this);
	
	if(prefix.size() >= 2 && prefix[prefix.size()-1] == '/')
		prefix.resize(prefix.size()-1);
	
	Map<String, Set<Subscriber*> >::iterator it = mSubscribers.find(prefix);
	if(it != mSubscribers.end())
	{
		it->second.erase(subscriber);
		if(it->second.empty())
			mSubscribers.erase(it);
	}
}

void Network::advertise(String prefix, const String &path, const Identifier &source, Publisher *publisher)
{
	Synchronize(this);
	
	if(prefix.size() >= 2 && prefix[prefix.size()-1] == '/')
		prefix.resize(prefix.size()-1);
	
	LogDebug("Network::publish", "Advertising " + prefix + path);
	
	matchSubscribers(prefix, source, publisher); 
}

void Network::addRemoteSubscriber(const Identifier &peer, const String &path, bool publicOnly)
{
	Synchronize(this);
	
	mRemoteSubscribers.push_front(RemoteSubscriber(peer, publicOnly));
	mRemoteSubscribers.begin()->subscribe(path);
}

bool Network::broadcast(const Identifier &local, const Notification &notification)
{
	Synchronize(this);	
	return outgoing(local, Identifier::Empty, "notif", notification);
}

bool Network::send(const Identifier &local, const Identifier &remote, const Notification &notification)
{
	Synchronize(this);
	return outgoing(local, remote, "notif", notification);
}

void Network::storeValue(const BinaryString &key, const BinaryString &value)
{
	mOverlay.store(key, value);
}

bool Network::retrieveValue(const BinaryString &key, Set<BinaryString> &values)
{
	return mOverlay.retrieve(key, values);
}

bool Network::addHandler(Stream *stream, const Identifier &local, const Identifier &remote)
{
	// Not synchronized
	Assert(stream);
	
	LogDebug("Network", "New handler");
	Handler *handler = new Handler(stream, local, remote);
	mThreadPool.launch(handler);
	return true;
}

bool Network::hasHandler(const Identifier &local, const Identifier &remote)
{
	Synchronize(this);
	return mHandlers.contains(IdentifierPair(local, remote));
}

void Network::run(void)
{
	unsigned loops = 0;
	while(true)
	{
		try {
			double timeout = 1.;	// TODO
			
			// Receive messages
			Overlay::Message message;
			while(mOverlay.recv(message, timeout))
			{
				//LogDebug("Network::incoming", "Processing message, type: " + String::hexa(unsigned(message.type)));
				
				switch(message.type)
				{
				// Value
				case Overlay::Message::Value:
					{
						Synchronize(this);
						
						if(mCallers.contains(message.source))
							mOverlay.send(Overlay::Message(Overlay::Message::Call, message.source, message.content));	// TODO: tokens
						
						Map<IdentifierPair, Set<Listener*> >::iterator it = mListeners.lower_bound(IdentifierPair(message.source, Identifier::Empty));	// pair is (remote, local)
						while(it != mListeners.end() && it->first.first == message.source)
						{
							for(Set<Listener*>::iterator jt = it->second.begin();
								jt != it->second.end();
								++jt)
							{
								(*jt)->seen(it->first.second, it->first.first, message.content);
							}
							++it;
						}
						
						break;
					}
					
				// Call
				case Overlay::Message::Call:
					{
						Fountain::Combination combination;
						Store::Instance->pull(message.source, combination);
						
						Overlay::Message data(Overlay::Message::Data, "", message.source);
						BinarySerializer serializer(&data.content);
						serializer.write(combination);
						
						mOverlay.send(data);
						break;
					}
					
				// Data
				case Overlay::Message::Data:
					{
						BinarySerializer serializer(&message.content);
						Fountain::Combination combination;
						serializer.read(combination);
						Store::Instance->push(message.source, combination);
						break;
					}
					
				// Tunnel
				case Overlay::Message::Tunnel:
					{
						mTunneler.incoming(message);
						break;
					}
				}
			}
			
			// Send beacons
			{
				Synchronize(this);
				
				for(Map<BinaryString, Set<Caller*> >::iterator it = mCallers.begin();
					it != mCallers.end();
					++it)
				{
					Desynchronize(this);
					mOverlay.send(Overlay::Message(Overlay::Message::Retrieve, "", it->first));
				}
				
				if(loops % 10 == 0)
				{
					Set<Identifier> localIds;
					Set<Identifier> remoteIds;
					
					for(Map<IdentifierPair, Set<Listener*> >::iterator it = mListeners.begin();
						it != mListeners.end();
						++it)
					{
						localIds.insert(it->first.second);
						remoteIds.insert(it->first.first);
					}
					
					BinaryString node(mOverlay.localNode());
					for(Set<Identifier>::iterator it = localIds.begin();
						it != localIds.end();
						++it)
					{
						storeValue(*it, node);
					}
					
					for(Set<Identifier>::iterator it = remoteIds.begin();
						it != remoteIds.end();
						++it)
					{
						mOverlay.send(Overlay::Message(Overlay::Message::Retrieve, "", *it));
					}
				}
			}
		}
		catch(const std::exception &e)
		{
			LogWarn("Network::run", e.what());
		}
		
		++loops;
	}
}

bool Network::registerHandler(const Identifier &local, const Identifier &remote, Handler *handler)
{
	Synchronize(this);
	
	if(!handler)
		return false;
	
	IdentifierPair pair(local, remote);
	
	Handler *l = NULL;
	if(mHandlers.get(pair, l))
		return (l == handler);
	
	mHandlers.insert(pair, handler);
	mThreadPool.launch(handler);
	return true;
}

bool Network::unregisterHandler(const Identifier &local, const Identifier &remote, Handler *handler)
{
	Synchronize(this);
	
	if(!handler)
		return false;
	
	IdentifierPair pair(local, remote);
	
	Handler *l = NULL;
	if(!mHandlers.get(pair, l) || l != handler)
		return false;
		
	mHandlers.erase(pair);
		return true;	
}

bool Network::outgoing(const String &type, const Serializable &content)
{
	Synchronize(this);
	//LogDebug("Network::outgoing", "Outgoing, type: "+type);
	
	bool success = false;
	for(Map<IdentifierPair, Handler*>::iterator it = mHandlers.begin();
		it != mHandlers.end();
		++it)
	{
		it->second->write(type, content);
		success = true;
	}
	
	return success;
}

bool Network::outgoing(const Identifier &local, const Identifier &remote, const String &type, const Serializable &content)
{
	Synchronize(this);
	//LogDebug("Network::outgoing", "Outgoing, type: "+type);
	
	if(!remote.empty())
	{
		Map<IdentifierPair, Handler*>::iterator it = mHandlers.find(IdentifierPair(local, remote));
		if(it != mHandlers.end())
		{
			it->second->write(type, content);
			return true;
		}
		
		return false;
	}
	else {
		bool success = false;
		for(Map<IdentifierPair, Handler*>::iterator it = mHandlers.lower_bound(IdentifierPair(local, Identifier::Empty));
			it->first.first == local;
			++it)
		{
			it->second->write(type, content);
			success = true;
		}
		
		return success;
	}
}

bool Network::incoming(const Identifier &local, const Identifier &remote, const String &type, Serializer &serializer)
{
	LogDebug("Network::incoming", "Incoming, type: "+type);
	
	// TODO
	return false;
}

bool Network::matchPublishers(const String &path, const Identifier &source, Subscriber *subscriber)
{
	Synchronize(this);
	
	List<String> list;
	path.before('?').explode(list,'/');
	if(list.empty()) return false;
	
	// First item should be empty because path begins with /
	if(list.front().empty()) 
		list.pop_front();
	
	// Match prefixes, longest first
	while(true)
	{
		String prefix;
		prefix.implode(list, '/');
		prefix = "/" + prefix;
		
		String truncatedPath(path.substr(prefix.size()));
		if(truncatedPath.empty()) truncatedPath = "/";
		
		SerializableList<BinaryString> targets;
		Map<String, Set<Publisher*> >::iterator it = mPublishers.find(prefix);
		if(it != mPublishers.end())
		{
			Set<Publisher*> set = it->second;
			Desynchronize(this);
			
			for(Set<Publisher*>::iterator jt = set.begin();
				jt != set.end();
				++jt)
			{
				List<BinaryString> result;
				if((*jt)->anounce(source, prefix, truncatedPath, result))
				{
					Assert(!result.empty());
					if(subscriber) 	// local
					{
						for(List<BinaryString>::iterator it = result.begin(); it != result.end(); ++it)
							subscriber->incoming(Identifier::Empty, path, "/", *it);
					}
					else targets.splice(targets.end(), result);	// remote
				}
			}
			
			if(!targets.empty()) 
			{
				LogDebug("Network::Handler::incoming", "Anouncing " + path);
				
				String response;
				JsonSerializer(&response).outputObject(Serializer::ConstObject()
					.insert("path", &path)
					.insert("targets", &targets));
				
				outgoing(overlay()->localNode(), source, "publish", response);
			}
		}
		
		if(list.empty()) break;
		list.pop_back();
	}
	
	return true;
}

bool Network::matchSubscribers(const String &path, const Identifier &source, Publisher *publisher)
{
	Synchronize(this);
	
	List<String> list;
	path.before('?').explode(list,'/');
	if(list.empty()) return false;
	
	// First item should be empty because path begins with /
	if(list.front().empty()) 
		list.pop_front();
	
	// Match prefixes, longest first
	while(true)
	{
		String prefix;
		prefix.implode(list, '/');
		prefix = "/" + prefix;
		
		String truncatedPath(path.substr(prefix.size()));
		if(truncatedPath.empty()) truncatedPath = "/";
		
		// Pass to subscribers
		Map<String, Set<Subscriber*> >::iterator it = mSubscribers.find(prefix);
		if(it != mSubscribers.end())
		{
			Set<Subscriber*> set = it->second;
			Desynchronize(this);
			
			for(Set<Subscriber*>::iterator jt = set.begin();
				jt != set.end();
				++jt)
			{
				Subscriber *subscriber = *jt;
				List<BinaryString> targets;
				if(publisher->anounce(subscriber->remote(), prefix, truncatedPath, targets))
				{
					for(List<BinaryString>::const_iterator kt = targets.begin();
						kt != targets.end();
						++kt)
					{
						// TODO: should prevent forwarding in case we want to republish another content
						subscriber->incoming(source, prefix, truncatedPath, *kt);
					}
				}
			}
		}
	
		if(list.empty()) break;
		list.pop_back();
	}
	
	return true;
}

void Network::onConnected(const Identifier &local, const Identifier &remote)
{
	Synchronize(this);
	
	Map<IdentifierPair, Set<Listener*> >::iterator it = mListeners.find(IdentifierPair(remote, local));
	if(it != mListeners.end())
	{
		for(Set<Listener*>::iterator jt = it->second.begin();
			jt != it->second.end();
			++jt)
		{
			(*jt)->connected(local, remote); 
		}
	}
}

void Network::onRecv(const Identifier &local, const Identifier &remote, const Notification &notification)
{
	Synchronize(this);
	
	Map<IdentifierPair, Set<Listener*> >::iterator it = mListeners.find(IdentifierPair(remote, local));
	if(it != mListeners.end())
	{
		for(Set<Listener*>::iterator jt = it->second.begin();
			jt != it->second.end();
			++jt)
		{
			(*jt)->recv(local, remote, notification); 
		}
	}
}

bool Network::onAuth(const Identifier &local, const Identifier &remote, const Rsa::PublicKey &pubKey)
{
	Synchronize(this);
	
	Map<IdentifierPair, Set<Listener*> >::iterator it = mListeners.find(IdentifierPair(remote, local));
	if(it != mListeners.end())
	{
		for(Set<Listener*>::iterator jt = it->second.begin();
			jt != it->second.end();
			++jt)
		{
			if((*jt)->auth(local, remote, pubKey))
				return true;
		}
	}
	
	return false;
}

Network::Publisher::Publisher(const Identifier &peer) :
	mPeer(peer)
{

}

Network::Publisher::~Publisher(void)
{
	for(StringSet::iterator it = mPublishedPrefixes.begin();
		it != mPublishedPrefixes.end();
		++it)
	{
		Network::Instance->unpublish(*it, this);
	}
}

void Network::Publisher::publish(const String &prefix, const String &path)
{
	if(!mPublishedPrefixes.contains(prefix))
	{
		Network::Instance->publish(prefix, this);
		mPublishedPrefixes.insert(prefix);
	}
	
	Network::Instance->advertise(prefix, path, mPeer, this);
}

void Network::Publisher::unpublish(const String &prefix)
{
	if(mPublishedPrefixes.contains(prefix))
	{
		Network::Instance->unpublish(prefix, this);
		mPublishedPrefixes.erase(prefix);
	}
}

Network::Subscriber::Subscriber(const Identifier &peer) :
	mPeer(peer),
	mThreadPool(0, 1, 8)
{
	
}

Network::Subscriber::~Subscriber(void)
{
	unsubscribeAll();
}

void Network::Subscriber::subscribe(const String &prefix)
{
	if(!mSubscribedPrefixes.contains(prefix))
	{
		Network::Instance->subscribe(prefix, this);
		mSubscribedPrefixes.insert(prefix);
	}
}

void Network::Subscriber::unsubscribe(const String &prefix)
{
	if(mSubscribedPrefixes.contains(prefix))
	{
		Network::Instance->unsubscribe(prefix, this);
		mSubscribedPrefixes.erase(prefix);
	}
}

void Network::Subscriber::unsubscribeAll(void)
{
	for(StringSet::iterator it = mSubscribedPrefixes.begin();
		it != mSubscribedPrefixes.end();
		++it)
	{
		Network::Instance->unsubscribe(*it, this);
	}
}

Identifier Network::Subscriber::remote(void) const
{
	return Identifier::Empty;
}

bool Network::Subscriber::localOnly(void) const
{
	return false;
}

bool Network::Subscriber::fetch(const Identifier &peer, const String &prefix, const String &path, const BinaryString &target)
{
	// Test local availability
	if(Store::Instance->hasBlock(target))
	{
		Resource resource(target, true);	// local only
		if(resource.isLocallyAvailable())
			return true;
	}
	
	class PrefetchTask : public Task
	{
	public:
		PrefetchTask(Network::Subscriber *subscriber, const Identifier &peer, const String &prefix, const String &path, const BinaryString &target)
		{
			this->subscriber = subscriber;
			this->peer = peer;
			this->target = target;
			this->prefix = prefix;
			this->path = path;
		}
		
		void run(void)
		{
			try {
				Resource resource(target);
				Resource::Reader reader(&resource, "", true);	// empty password + no check
				reader.discard();				// read everything
				
				subscriber->incoming(peer, prefix, path, target);
			}
			catch(const Exception &e)
			{
				LogWarn("Network::Subscriber::fetch", "Fetching failed for " + target.toString() + ": " + e.what());
			}
			
			delete this;	// autodelete
		}
	
	private:
		Network::Subscriber *subscriber;
		Identifier peer;
		BinaryString target;
		String prefix;
		String path;
	};
	
	PrefetchTask *task = new PrefetchTask(this, peer, prefix, path, target);
	mThreadPool.launch(task);
	return false;
}

Network::RemotePublisher::RemotePublisher(const List<BinaryString> targets):
	mTargets(targets)
{
  
}

Network::RemotePublisher::~RemotePublisher(void)
{
  
}

bool Network::RemotePublisher::anounce(const Identifier &peer, const String &prefix, const String &path, List<BinaryString> &targets)
{
	targets = mTargets;
	return !targets.empty();
}

Network::RemoteSubscriber::RemoteSubscriber(const Identifier &remote, bool publicOnly) :
	mRemote(remote),
	mPublicOnly(publicOnly)
{

}

Network::RemoteSubscriber::~RemoteSubscriber(void)
{

}

bool Network::RemoteSubscriber::incoming(const Identifier &peer, const String &prefix, const String &path, const BinaryString &target)
{
	if(mRemote != Identifier::Empty)
	{
		SerializableArray<BinaryString> targets;
		targets.append(target);
		
		String payload;
		JsonSerializer(&payload).outputObject(Serializer::ConstObject()
			.insert("prefix", &prefix)
			.insert("targets", &targets));
				
		Network::Instance->outgoing(Network::Instance->overlay()->localNode(), mRemote, "publish", payload);
	}
}

Identifier Network::RemoteSubscriber::remote(void) const
{
	if(!mPublicOnly) return mRemote;
	else return Identifier::Empty;
}

bool Network::RemoteSubscriber::localOnly(void) const
{
	return true;
}

Network::Caller::Caller(void)
{
	
}

Network::Caller::Caller(const BinaryString &target)
{
	Assert(!target.empty());
	startCalling(target);
}

Network::Caller::~Caller(void)
{
	stopCalling();
}
	
void Network::Caller::startCalling(const BinaryString &target)
{
	if(target != mTarget)
	{
		stopCalling();
		
		mTarget = target;
		if(!mTarget.empty()) Network::Instance->registerCaller(mTarget, this);
	}
}

void Network::Caller::stopCalling(void)
{
	if(!mTarget.empty())
	{
		Network::Instance->unregisterCaller(mTarget, this);
		mTarget.clear();
	}
}

Network::Listener::Listener(void)
{
	
}

Network::Listener::~Listener(void)
{
	for(Set<IdentifierPair>::iterator it = mPairs.begin();
		it != mPairs.end();
		++it)
	{
		Network::Instance->unregisterListener(it->second, it->first, this);
	}
}

void Network::Listener::listen(const Identifier &local, const Identifier &remote)
{
	mPairs.insert(IdentifierPair(remote, local));
	Network::Instance->registerListener(local, remote, this);
}

Network::Tunneler::Tunneler(void)
{

}

Network::Tunneler::~Tunneler(void)
{
	
}

bool Network::Tunneler::open(const BinaryString &node, const Identifier &remote, User *user, bool async)
{
	Assert(user);
	
	if(remote.empty())
		return false;
	
	if(Network::Instance->overlay()->connectionsCount() == 0)
		return false;
	
	// TODO: multi-instance
	if(Network::Instance->hasHandler(user->identifier(), remote))
		return false;
	
	LogDebug("Network::Tunneler::open", "Opening tunnel to " + remote.toString());
	
	uint64_t tunnelId;
	Random().readBinary(tunnelId);	// Generate random tunnel ID
	BinaryString local = user->identifier();
	
	Tunneler::Tunnel *tunnel = NULL;
	SecureTransport *transport = NULL;
	try {
		tunnel = new Tunneler::Tunnel(this, tunnelId, node);
		transport = new SecureTransportClient(tunnel, NULL);
	}
	catch(...)
	{
		delete tunnel;
		throw;
	}
	
	LogDebug("Network::Tunneler::open", "Setting certificate credentials: " + user->name());
		
	// Set remote name
	transport->setHostname(remote.toString());
	
	// Add certificates
	transport->addCredentials(user->certificate(), false);
	
	return handshake(transport, local, remote, async);
}

SecureTransport *Network::Tunneler::listen(void)
{
	Synchronize(&mQueueSync);
	
	while(true)
	{
		while(mQueue.empty()) mQueueSync.wait();
		
		Overlay::Message &datagram = mQueue.front();

		// Read tunnel ID
		uint64_t tunnelId;
		datagram.content.readBinary(tunnelId);

		Map<uint64_t, Tunnel*>::iterator it = mTunnels.find(tunnelId);
		if(it == mTunnels.end())
		{
			LogDebug("Network::Tunneler::listen", "Incoming tunnel from " + datagram.source.toString());
			
			Tunneler::Tunnel *tunnel = NULL;
			SecureTransport *transport = NULL;
			try {
				tunnel = new Tunneler::Tunnel(this, tunnelId, datagram.source);
				transport = new SecureTransportServer(tunnel, NULL, true);	// ask for certificate
			}
			catch(...)
			{
				delete tunnel;
				mQueue.pop();
				throw;
			}
		
			mTunnels.insert(tunnelId, tunnel);
			tunnel->incoming(datagram);
			mQueue.pop();
			return transport;
		}
		
		it->second->incoming(datagram);
		mQueue.pop();
	}
}

bool Network::Tunneler::incoming(const Overlay::Message &datagram)
{
	Synchronize(&mQueueSync);
	mQueue.push(datagram);
	mQueueSync.notifyAll();
	return true;
}

bool Network::Tunneler::registerTunnel(Tunnel *tunnel)
{
	Synchronize(this);
	
	if(!tunnel)
		return false;
	
	Tunneler::Tunnel *t = NULL;
	if(mTunnels.get(tunnel->id(), t))
		return (t == tunnel);
	
	mTunnels.insert(tunnel->id(), tunnel);
	return true;
}

bool Network::Tunneler::unregisterTunnel(Tunnel *tunnel)
{
	Synchronize(this);
	
	if(!tunnel)
		return false;
	
	Tunneler::Tunnel *t = NULL;
	if(!mTunnels.get(tunnel->id(), t) || t != tunnel)
		return false;
	
	mTunnels.erase(tunnel->id());
		return true;	
}

bool Network::Tunneler::handshake(SecureTransport *transport, const Identifier &local, const Identifier &remote, bool async)
{
	class MyVerifier : public SecureTransport::Verifier
	{
	public:
		Identifier local, remote;
		Rsa::PublicKey publicKey;
		
		bool verifyName(const String &name, SecureTransport *transport)
		{
			LogDebug("Network::Tunneler::handshake", String("Verifying user: ") + name);
			
			try {
				local.fromString(name);
			}
			catch(...)
			{
				LogDebug("Network::Tunneler::handshake", String("Invalid identifier: ") + name);
				return false;
			}
			
			User *user = User::GetByIdentifier(local);
			if(user)
			{
				transport->addCredentials(user->certificate(), false);
			}
			else {
				 LogDebug("Network::Tunneler::handshake", String("User does not exist: ") + name);
			}
			
			return true;	// continue handshake anyway
		}
		
		bool verifyPublicKey(const Array<Rsa::PublicKey> &chain)
		{
			if(chain.empty()) return false;
			publicKey = chain[0];
			remote = Identifier(publicKey.digest());
			
			LogDebug("Network::Tunneler::handshake", String("Verifying remote certificate: ") + remote.toString());
			if(Network::Instance->onAuth(local, remote, publicKey))
				return true;
			
			LogDebug("Network::Tunneler::handshake", "Certificate verification failed");
			return false;
		}
	};
	
	class HandshakeTask : public Task
	{
	public:
		HandshakeTask(SecureTransport *transport, const Identifier &local, const Identifier &remote)
		{ 
			this->transport = transport;
			this->local = local;
			this->remote = remote;
		}
		
		bool handshake(void)
		{
			LogDebug("Network::Tunneler::handshake", "HandshakeTask starting...");
			
			try {
				// Set verifier
				MyVerifier verifier;
				verifier.local = local;
				transport->setVerifier(&verifier);
				
				// Do handshake
				transport->handshake();
				Assert(transport->hasCertificate());
				
				if(!local.empty() && local != verifier.local)
					return false;
				
				if(!remote.empty() && remote != verifier.remote)
					return false;
				
				// Handshake succeeded
				LogDebug("Network::Tunneler::handshake", "Handshake succeeded, spawning new handler");
				Handler *handler = new Handler(transport, verifier.local, verifier.remote);
				return true;
			}
			catch(const std::exception &e)
			{
				LogInfo("Network::Tunneler::handshake", String("Handshake failed: ") + e.what());
				delete transport;
				return false;
			}
		}
		
		void run(void)
		{
			handshake();
			delete this;	// autodelete
		}
		
	private:
		SecureTransport *transport;
		Identifier local, remote;
	};
	
	HandshakeTask *task = NULL;
	try {
		if(async)
		{
			task = new HandshakeTask(transport, local, remote);
			mThreadPool.launch(task);
			return true;
		}
		else {
			HandshakeTask stask(transport, local, remote);
			return stask.handshake();
		}
	}
	catch(const std::exception &e)
	{
		LogError("Network::Tunneler::handshake", e.what());
		delete task;
		delete transport;
		return false;
	}
}

void Network::Tunneler::run(void)
{
	try {
		while(true)
		{
			SecureTransport *transport = listen();
			if(!transport) break;
			
			LogDebug("Network::Backend::run", "Incoming tunnel");
			
			handshake(transport, Identifier::Empty, Identifier::Empty, true); // async
		}
	}
	catch(const std::exception &e)
	{
		LogError("Network::Tunneler::run", e.what());
	}
	
	LogWarn("Network::Backend::run", "Closing tunneler");
}

Network::Tunneler::Tunnel::Tunnel(Tunneler *tunneler, uint64_t id, const BinaryString &node) :
	mTunneler(tunneler),
	mId(id),
	mNode(node),
	mTimeout(DefaultTimeout)
{
	mTunneler->registerTunnel(this);
}

Network::Tunneler::Tunnel::~Tunnel(void)
{
	mTunneler->unregisterTunnel(this);
}

uint64_t Network::Tunneler::Tunnel::id(void) const
{
	return mId;
}

void Network::Tunneler::Tunnel::setTimeout(double timeout)
{
	mTimeout = timeout;
}

size_t Network::Tunneler::Tunnel::readData(char *buffer, size_t size)
{
	Synchronize(&mQueueSync);
	
	double timeout = mTimeout;
        while(mQueue.empty())
		if(!mQueueSync.wait(timeout))
			throw Timeout();
	
	const Overlay::Message &message = mQueue.front();
	size = std::min(size, size_t(message.content.size()));
	std::copy(message.content.data(), message.content.data() + size, buffer);
        mQueue.pop();
        return size;
}

void Network::Tunneler::Tunnel::writeData(const char *data, size_t size)
{
	BinaryString content;
	content.writeBinary(mId);
	content.writeBinary(data, size);
	Network::Instance->overlay()->send(Overlay::Message(Overlay::Message::Tunnel, content, mNode));
}

bool Network::Tunneler::Tunnel::waitData(double &timeout)
{
	Synchronize(&mQueueSync);
	
	while(mQueue.empty())
	{
		if(timeout == 0.)
			return false;
		
		if(!mQueueSync.wait(timeout))
			return false;
	}
	
	return true;
}

bool Network::Tunneler::Tunnel::waitData(const double &timeout)
{
	double dummy = timeout;
	return waitData(dummy);
}

bool Network::Tunneler::Tunnel::isDatagram(void) const
{
	return true; 
}

bool Network::Tunneler::Tunnel::incoming(const Overlay::Message &datagram)
{
	Synchronize(&mQueueSync);
	
	if(datagram.type != Overlay::Message::Tunnel)
		return false;
	
	mQueue.push(datagram);
	mQueueSync.notifyAll();
	return true;
}

Network::Handler::Handler(Stream *stream, const Identifier &local, const Identifier &remote) :
	mStream(stream),
	mLocal(local),
	mRemote(remote),
	mTokens(0.),
	mRedundancy(1.1)	// TODO
{
	Network::Instance->registerHandler(mLocal, mRemote, this);
}

Network::Handler::~Handler(void)
{
	Network::Instance->unregisterHandler(mLocal, mRemote, this);	// should be done already
	
	delete mStream;
}

void Network::Handler::write(const String &type, const String &content)
{
	Synchronize(this);
	mSource.write(type.c_str(), type.size()+1);
	mSource.write(content.c_str(), content.size()+1);
	
	// TODO: tokens
}

bool Network::Handler::read(String &type, String &content)
{
	Synchronize(this);
	
	if(!readString(type)) return false;
	if(!readString(content))
		throw Exception("Unexpected end of stream");
	
	return true;
}

bool Network::Handler::readString(String &str)
{
	Synchronize(this);
	
	str.clear();
	while(true)
	{
		// Try to read
		char chr;
		while(mSink.read(&chr, 1))
		{
			if(chr == '\0')
			{
				// Finished
				return true;
			}
			
			str+= chr;
		}
		
		// We need more combinations
		BinaryString temp;
		DesynchronizeStatement(this, if(!mStream->readBinary(temp)) return false);
		
		Fountain::Combination combination;
		BinarySerializer serializer(&temp);
		serializer.read(combination);
		combination.setData(temp);
		mSink.solve(combination);
	}
}

void Network::Handler::process(void)
{
	Network::Instance->onConnected(mLocal, mRemote);
	
	Synchronize(this);
	
	String type, content;
	while(read(type, content))
	{
		JsonSerializer serializer(&content);
		Network::Instance->incoming(mLocal, mRemote, type, serializer);
	}
}

void Network::Handler::run(void)
{
	try {
		LogDebug("Network::Handler", "Starting handler");
	
		process();
		
		LogDebug("Network::Handler", "Closing handler");
	}
	catch(const std::exception &e)
	{
		LogDebug("Network::Handler", String("Closing handler: ") + e.what());
	}
	
	Network::Instance->unregisterHandler(mLocal, mRemote, this);
	
	notifyAll();
	Thread::Sleep(5.);	// TODO
	delete this;		// autodelete
}

}
