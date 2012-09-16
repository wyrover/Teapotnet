/*************************************************************************
 *   Copyright (C) 2011-2012 by Paul-Louis Ageneau                       *
 *   paul-louis (at) ageneau (dot) org                                   *
 *                                                                       *
 *   This file is part of Arcanet.                                       *
 *                                                                       *
 *   Arcanet is free software: you can redistribute it and/or modify     *
 *   it under the terms of the GNU Affero General Public License as      *
 *   published by the Free Software Foundation, either version 3 of      *
 *   the License, or (at your option) any later version.                 *
 *                                                                       *
 *   Arcanet is distributed in the hope that it will be useful, but      *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the        *
 *   GNU Affero General Public License for more details.                 *
 *                                                                       *
 *   You should have received a copy of the GNU Affero General Public    *
 *   License along with Arcanet.                                         *
 *   If not, see <http://www.gnu.org/licenses/>.                         *
 *************************************************************************/

#include "core.h"
#include "html.h"
#include "sha512.h"
#include "aescipher.h"

namespace arc
{

Core *Core::Instance = NULL;


Core::Core(int port) :
		mSock(port),
		mLastRequest(0)
{
	Interface::Instance->add("/peers", this);
}

Core::~Core(void)
{
	Interface::Instance->remove("/peers");
	mSock.close();	// useless
}

void Core::registerPeering(	const Identifier &peering,
				const Identifier &remotePeering,
		       		const ByteString &secret,
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

void Core::addPeer(Socket *sock, const Identifier &peering)
{
	assert(sock != NULL);
	Synchronize(this);
	
	if(peering != Identifier::Null && !mPeerings.contains(peering))
		throw Exception("Added peer with unknown peering");
	
	//Log("Core", "Spawning new handler");
	Handler *handler = new Handler(this,sock);
	if(peering != Identifier::Null) handler->setPeering(peering);
	handler->start(true); // handler will destroy itself
}

bool Core::hasPeer(const Identifier &peering)
{
	Synchronize(this);
	return mHandlers.contains(peering);
}

void Core::run(void)
{
	Log("Core", "Starting");
	
	try {
		while(true)
		{
			Socket *sock = new Socket;
			mSock.accept(*sock);
			Log("Core", "Incoming connexion from " + sock->getRemoteAddress().toString());
			addPeer(sock, Identifier::Null);
		}
	}
	catch(const NetException &e)
	{

	}
	
	Log("Core", "Finished");
}

void Core::sendMessage(const Message &message)
{
	Synchronize(this);
	
	if(message.mReceiver == Identifier::Null)
	{
		for(Map<Identifier,Handler*>::iterator it = mHandlers.begin();
				it != mHandlers.end();
				++it)
		{
			Handler *handler = it->second;
			handler->sendMessage(message);
		}
	}
	else {
		Handler *handler;
		if(!mHandlers.get(message.mReceiver, handler)) 
			throw Exception("Message receiver is not connected");
		
		handler->sendMessage(message);
	}
}

unsigned Core::addRequest(Request *request)
{
	Synchronize(this);
  
	++mLastRequest;
	request->mId = mLastRequest;
	mRequests.insert(mLastRequest, request);

	if(request->mReceiver == Identifier::Null)
	{
		for(Map<Identifier,Handler*>::iterator it = mHandlers.begin();
				it != mHandlers.end();
				++it)
		{
			Handler *handler = it->second;
			handler->addRequest(request);
		}
	}
	else {
		Handler *handler;
		if(!mHandlers.get(request->mReceiver, handler)) 
			throw Exception("Request receiver is not connected");
		
		handler->addRequest(request);
	}

	return mLastRequest;
}

void Core::removeRequest(unsigned id)
{
	Synchronize(this);
  
	for(Map<Identifier,Handler*>::iterator it = mHandlers.begin();
				it != mHandlers.end();
				++it)
	{
		Handler *handler = it->second;
		handler->removeRequest(id);
	}
}

void Core::http(const String &prefix, Http::Request &request)
{
	Synchronize(this);
	
	if(prefix == "/peers")
	{
		if(request.url == "/")
		{
			Http::Response response(request, 200);
			response.send();

			Html page(response.sock);
			page.header("Peers");
			page.open("h1");
			page.text("Peers");
			page.close("h1");

			if(mHandlers.empty()) page.text("No peer...");
			else for(Map<Identifier,Handler*>::iterator it = mHandlers.begin();
							it != mHandlers.end();
							++it)
			{
				String str(it->first.toString());
				page.link(str,String("/peers/")+str);
				page.br();
			}

			page.footer();
		}
		else {
			//Identifier ident;
			//list.front() >> ident;

			// TODO
		}
	}
	else throw 404;
}

void Core::addHandler(const Identifier &peer, Core::Handler *handler)
{
	Assert(handler != NULL);
	Synchronize(this);
	
	if(!mHandlers.contains(peer)) mHandlers.insert(peer, handler);
	else {
		if(mHandlers[peer] != handler)
			throw Exception("Another handler is already registered");
	}
}

void Core::removeHandler(const Identifier &peer, Core::Handler *handler)
{
	Synchronize(this);
  
	if(mHandlers.contains(peer))
	{
		Handler *h = mHandlers.get(peer);
		if(h != handler) return;
		delete h;
		mHandlers.erase(peer);
	}
}

void Core::Handler::sendCommand(Stream *stream, const String &command, const String &args, const StringMap &parameters)
{
	String line;
	line << command << " " << args << Stream::NewLine;
  	*stream << line;

	for(	StringMap::const_iterator it = parameters.begin();
		it != parameters.end();
		++it)
	{
	  	line.clear();
		line << it->first << ": " << it->second << Stream::NewLine;
		*stream << line;
	}
	*stream << Stream::NewLine;
}
		
bool Core::Handler::recvCommand(Stream *stream, String &command, String &args, StringMap &parameters)
{
	command.clear();
	if(!stream->readLine(command)) return false;
	args = command.cut(' ');
	command = command.toUpper();
	
	parameters.clear();
	while(true)
	{
		String name;
		AssertIO(stream->readLine(name));
		if(name.empty()) break;

		String value = name.cut(':');
		name.trim();
		value.trim();
		parameters.insert(name,value);
	}
	
	return true;
}

Core::Handler::Handler(Core *core, Socket *sock) :
	mCore(core),
	mSock(sock),
	mStream(sock)
{

}

Core::Handler::~Handler(void)
{	
	if(mStream != mSock) delete mStream;
	delete mSock;
	delete mHandler;
}

void Core::Handler::setPeering(const Identifier &peering)
{
	Synchronize(this);
	mPeering = peering;
}

void Core::Handler::sendMessage(const Message &message)
{
	Synchronize(this);
	
	Log("Core::Handler", "New message");
	
	mSender.lock();
	mSender.mMessagesQueue.push(message);
	mSender.unlock();
	mSender.notify();
}

void Core::Handler::addRequest(Request *request)
{
	Synchronize(this);
	
	Log("Core::Handler", "New request " + String::number(request->id()));
	
	mSender.lock();
	request->addPending();
	mSender.mRequestsQueue.push(request);
	mSender.unlock();
	mSender.notify();
	
	mRequests.insert(request->id(),request);
}

void Core::Handler::removeRequest(unsigned id)
{
	Synchronize(this);
	mRequests.erase(id);
}

void Core::Handler::run(void)
{
	try {
		lock();
		Log("Core::Handler", "Starting");
	  
		ByteString nonce_a, salt_a;
		for(int i=0; i<16; ++i)
		{
			char c = char(rand()%256);	// TODO
			nonce_a.push_back(c);
			c = char(rand()%256);		// TODO
			salt_a.push_back(c);  
		}
	  
		String command, args;
		StringMap parameters;
	  
		if(mPeering != Identifier::Null)
		{
		  	mCore->lock();
			if(!mCore->mPeerings.get(mPeering, mRemotePeering))
				throw Exception("Unknown peering: " + mPeering.toString());
			mCore->unlock();
			
			args.clear();
			args << mRemotePeering;
			parameters.clear();
			parameters["Application"] << APPNAME;
			parameters["Version"] << APPVERSION;
			parameters["Nonce"] << nonce_a;
			sendCommand(mStream, "H", args, parameters);
		}
	  
		if(!recvCommand(mStream, command, args, parameters)) return;
		if(command != "H") throw Exception("Unexpected command: " + command);
		
		String appname, appversion;
		ByteString peering, nonce_b;
		args.read(peering);
		parameters["Application"] >> appname;
		parameters["Version"] >> appversion;
		parameters["Nonce"] >> nonce_b;

		if(mPeering != Identifier::Null && mPeering != peering) 
				throw Exception("Peering in response does not match: " + peering.toString());
		
		ByteString secret;
		if(peering.size() != 64) throw Exception("Invalid peering: " + peering.toString());	// TODO: useless
		
		{
			Synchronize(mCore);
			if(!mCore->mSecrets.get(peering, secret)) 
				throw Exception("No secret for peering: " + peering.toString());
		}
		
		if(mPeering == Identifier::Null)
		{
			mPeering = peering;
			if(!mCore->mPeerings.get(mPeering, mRemotePeering)) 
				throw Exception("Unknown peering: " + mPeering.toString());
			
			args.clear();
			args << mRemotePeering;
			parameters.clear();
			parameters["Application"] << APPNAME;
			parameters["Version"] << APPVERSION;
			parameters["Nonce"] << nonce_a;
			sendCommand(mStream, "H", args, parameters);
		}
		
		String agregate_a;
		agregate_a.writeLine(secret);
		agregate_a.writeLine(salt_a);
		agregate_a.writeLine(nonce_b);
		agregate_a.writeLine(mPeering);
		
		ByteString hash_a;
		Sha512::Hash(agregate_a, hash_a, Sha512::CryptRounds);
		
		parameters.clear();
		parameters["Digest"] << hash_a;
		parameters["Salt"] << salt_a;
		sendCommand(mStream, "A", "DIGEST AES256", parameters);
		
		AssertIO(recvCommand(mStream, command, args, parameters));
		if(command != "A") throw Exception("Unexpected command: " + command);
		if(args.toUpper() != "DIGEST AES256") throw Exception("Unknown authentication method " + args.toString());
		
		ByteString salt_b, test_b;
		parameters["Digest"] >> test_b;
		parameters["Salt"] >> salt_b;
		
		String agregate_b;
		agregate_b.writeLine(secret);
		agregate_b.writeLine(salt_b);
		agregate_b.writeLine(nonce_a);
		agregate_b.writeLine(mRemotePeering);
		
		ByteString hash_b;
		Sha512::Hash(agregate_b, hash_b, Sha512::CryptRounds);
		
		if(test_b != hash_b) throw Exception("Authentication failed");
		
		Log("Core::Handler", "Authentication finished");
		
		AesCipher *cipher = new AesCipher(mSock);
		
		// Set encryption key and IV
		ByteString key_a;
		agregate_a.writeLine(nonce_a);
		Sha512::Hash(agregate_a, key_a, Sha512::CryptRounds);
		ByteString iv_a(key_a);
		key_a.resize(32);	// 256 bits
		iv_a.ignore(32);
		cipher->setEncryptionKey(key_a);
		cipher->setEncryptionInit(iv_a);
		
		// Set decryption key and IV
		ByteString key_b;
		agregate_b.writeLine(nonce_b);
		Sha512::Hash(agregate_b, key_b, Sha512::CryptRounds);
		ByteString iv_b(key_b);
		key_b.resize(32);	// 256 bits
		iv_b.ignore(32);
		cipher->setDecryptionKey(key_b);
		cipher->setDecryptionInit(iv_b);
		
		mStream = cipher;
		
		mCore->addHandler(mPeering,this);
		mSender.mStream = mStream;
		mSender.start();
		
		Log("Core::Handler", "Entering main loop");
		unlock();
		while(recvCommand(mStream, command, args, parameters))
		{
			lock();

			if(command == "R")
			{
				unsigned id;
				String status;
				unsigned channel;
				args.read(id);
				args.read(status);
				args.read(channel);
				
				Request *request;
				if(mRequests.get(id,request))
				{
				  	Request::Response *response;
					if(channel)
					{
						Log("Core::Handler", "Received response for request "+String::number(id)+", receiving on channel "+String::number(channel));
	
						ByteStream *sink = request->mContentSink; 	// TODO
						if(!sink) sink = new ByteString;		// TODO
						
						response = new Request::Response(status, parameters, sink);
						mResponses.insert(channel,response);
					}
					else {
						Log("Core::Handler", "Received response for request "+String::number(id)+", no data");
						response = new Request::Response(status, parameters);
					}

					response->mPeering = mPeering;
					request->addResponse(response);

					// TODO: Only one response expected for now
					mRequests.erase(id);

					request->removePending();	// TODO
				}
				else Log("Core::Handler", "WARNING: Received response for unknown request "+String::number(id));
			}
			else if(command == "D")
			{
				unsigned channel, size;
				args.read(channel);
				args.read(size);

				Request::Response *response;
				if(mResponses.get(channel,response))
				{
				 	Assert(response->content() != NULL);
					if(size) mStream->readData(*response->content(),size);
					else {
						Log("Core::Handler", "Finished receiving on channel "+String::number(channel));
						response->content()->close();
						mRequests.erase(channel);
					}
				}
				else {
				  Log("Core::Handler", "WARNING: Received data for unknown channel "+String::number(channel));
				  mStream->ignore(size);
				}
			}
			else if(command == "I" || command == "G")
			{
			  	unsigned id;
				args.read(id);
				String &target = args;
			  	Log("Core::Handler", "Received request "+String::number(id)+" for \""+target+"\"");

				Listener *listener;
				mCore->lock();
				if(!mCore->mListeners.get(mPeering, listener)) listener = NULL;
				mCore->unlock();
				
				if(!listener) Log("Core::Handler", "WARNING: No listener, dropping request " + String::number(id));
				else {
					Request *request = new Request;
					request->setTarget(target, (command == "G"));
					request->setParameters(parameters);
					request->mId = id;
				
					listener->request(request);

					mSender.lock();
					mSender.mRequestsToRespond.push_back(request);
					request->mResponseSender = &mSender;
					mSender.unlock();
					mSender.notify();
				}
			}
			else if(command == "M")
			{
				unsigned size;
				args.read(size);
				Log("Core::Handler", "Received message");
				
				Message message;
				message.mReceiver = mPeering;
				message.mParameters = parameters;
				message.mContent.reserve(size);
				
				mStream->read(message.mContent,size);
				
				Listener *listener;
				mCore->lock();
				if(!mCore->mListeners.get(mPeering, listener)) listener = NULL;
				mCore->unlock();
				
				if(listener) listener->message(&message);
				else Log("Core::Handler", "WARNING: No listener, dropping message");
			}

			unlock();
		}

		Log("Core::Handler", "Finished");
	}
	catch(std::exception &e)
	{
		unlock();
		Log("Core::Handler", String("Stopping: ") + e.what()); 
	}
	
	mSock->close();
	mCore->removeHandler(mPeering, this);
}

const size_t Core::Handler::Sender::ChunkSize = 4096;	// TODO

Core::Handler::Sender::Sender(void) :
		mLastChannel(0)
{

}

Core::Handler::Sender::~Sender(void)
{
	for(int i=0; i<mRequestsToRespond.size(); ++i)
		delete mRequestsToRespond[i];
}

void Core::Handler::Sender::run(void)
{
	try {
		Log("Core::Handler::Sender", "Starting");
		Assert(mStream);
		
		while(true)
		{
			lock();
			if(mMessagesQueue.empty()
				&& mRequestsQueue.empty()
			  	&& mTransferts.empty())
			{
				//Log("Core::Handler::Sender", "No pending tasks, waiting");
				wait();
			}
			
			for(int i=0; i<mRequestsToRespond.size(); ++i)
			{
				Request *request = mRequestsToRespond[i];
				Synchronize(request);
				
				for(int j=0; j<request->responsesCount(); ++j)
				{
					Request::Response *response = request->response(j);
					if(!response->mIsSent)
					{
						Log("Core::Handler::Sender", "Sending response");
						
						String status = "OK";
						unsigned channel = 0;

						if(response->content())
						{
							++mLastChannel;
							channel = mLastChannel;
							
							
							Log("Core::Handler::Sender", "Start sending channel "+String::number(channel));
							mTransferts.insert(channel,response->content());
						}
						
						String args;
						args << request->id() <<" "<<status<<" "<<channel;
						Handler::sendCommand(mStream, "R", args, response->mParameters);
						
						response->mIsSent = true;
					}
				}
			}
			
			if(!mMessagesQueue.empty())
			{
				const Message &message = mMessagesQueue.front();
				Log("Core::Handler::Sender", "Sending message");
				
				String args;
				args << message.mContent.size();
				Handler::sendCommand(mStream, "M", args, message.parameters());
				
				mStream->write(message.mContent);
				
				mMessagesQueue.pop();
			}
			  
			if(!mRequestsQueue.empty())
			{
				Request *request = mRequestsQueue.front();
				Log("Core::Handler::Sender", "Sending request "+String::number(request->id()));
				
				String command;
				if(request->mIsData) command = "G";
				else command = "I";
				
				String args;
				args << request->id() << " " << request->target();
				Handler::sendCommand(mStream, command, args, request->mParameters);
				mRequestsQueue.pop();
			}

			char buffer[ChunkSize];
			Map<unsigned, ByteStream*>::iterator it = mTransferts.begin();
			while(it != mTransferts.end())
			{
				size_t size = it->second->readData(buffer,ChunkSize);
				
				String args;
				args << it->first << " " << size;
				Handler::sendCommand(mStream, "D", args, StringMap());

				if(size == 0)
				{
					Log("Core::Handler::Sender", "Finished sending on channel "+String::number(it->first));
					mTransferts.erase(it++);
				}
				else {
				 	mStream->writeData(buffer, size);
					++it;
				}
			}
			
			int i=0;
			while(i<mRequestsToRespond.size())
			{
				Request *request = mRequestsToRespond[i];
				if(!request->isPending())
				{
					mRequestsToRespond.erase(i);
					delete request; 
				}
				else ++i;
			}
			
			unlock();
		}
		
		Log("Core::Handler::Sender", "Finished");
	}
	catch(std::exception &e)
	{
		//mStream->close();	// TODO
		unlock();
		Log("Core::Handler::Sender", String("Stopping: ") + e.what()); 
	}
	
}

}
