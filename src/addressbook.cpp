/*************************************************************************
 *   Copyright (C) 2011-2012 by Paul-Louis Ageneau                       *
 *   paul-louis (at) ageneau (dot) org                                   *
 *                                                                       *
 *   This file is part of TeapotNet.                                     *
 *                                                                       *
 *   TeapotNet is free software: you can redistribute it and/or modify   *
 *   it under the terms of the GNU Affero General Public License as      *
 *   published by the Free Software Foundation, either version 3 of      *
 *   the License, or (at your option) any later version.                 *
 *                                                                       *
 *   TeapotNet is distributed in the hope that it will be useful, but    *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of          *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the        *
 *   GNU Affero General Public License for more details.                 *
 *                                                                       *
 *   You should have received a copy of the GNU Affero General Public    *
 *   License along with TeapotNet.                                       *
 *   If not, see <http://www.gnu.org/licenses/>.                         *
 *************************************************************************/

#include "addressbook.h"
#include "user.h"
#include "core.h"
#include "sha512.h"
#include "config.h"
#include "file.h"
#include "directory.h"
#include "html.h"
#include "yamlserializer.h"
#include "jsonserializer.h"
#include "portmapping.h"

namespace tpot
{

AddressBook::AddressBook(User *user) :
	mUser(user)
{
	Assert(mUser != NULL);
	mFileName = mUser->profilePath() + "contacts";
	
	Interface::Instance->add("/"+mUser->name()+"/contacts", this);
	
	if(File::Exist(mFileName))
	{
		try {
			File file(mFileName, File::Read);
			load(file);
			file.close();
		}
		catch(const Exception &e)
		{
			Log("AddressBook", String("Loading failed: ") + e.what());
		}
	}
}

AddressBook::~AddressBook(void)
{
  	Interface::Instance->remove("/"+mUser->name()+"/contacts");
	
	clear();
}

User *AddressBook::user(void) const
{
 	return mUser; 
}

String AddressBook::userName(void) const
{
 	return mUser->name(); 
}

int AddressBook::unreadMessagesCount(void) const
{
	Synchronize(this);
  
	int count = 0;
	for(Map<Identifier, Contact*>::const_iterator it = mContacts.begin();
		it != mContacts.end();
		++it)
	{
		const Contact *contact = it->second;
		count+= contact->unreadMessagesCount();
	}
	return count;
}

const Identifier &AddressBook::addContact(String name, const ByteString &secret)
{
	Synchronize(this);

	String tracker = name.cut('@');
	if(tracker.empty()) tracker = Config::Get("tracker");
	
	String uname = name;
	unsigned i = 1;
	while(mContactsByUniqueName.contains(uname) || uname == userName())	// userName reserved for self
	{
		uname = name;
		uname << ++i;
	}
	
	Contact *contact = new Contact(this, uname, name, tracker, secret);
	if(mContacts.contains(contact->peering()))
	{
		delete contact;
		throw Exception("The contact already exists");
	}
	
	mContacts.insert(contact->peering(), contact);
	mContactsByUniqueName.insert(contact->uniqueName(), contact);
	Interface::Instance->add(contact->urlPrefix(), contact);
	
	save();
	start();
	return contact->peering();
}

void AddressBook::removeContact(const Identifier &peering)
{
	Synchronize(this);
	
	Contact *contact;
	if(mContacts.get(peering, contact))
	{
		Core::Instance->unregisterPeering(peering);
		mContactsByUniqueName.erase(contact->uniqueName());
 		mContacts.erase(peering);
		Interface::Instance->remove(contact->urlPrefix());
		delete contact;
		save();
		start();
	}
}

AddressBook::Contact *AddressBook::getContact(const Identifier &peering)
{
	Synchronize(this);
  
	Contact *contact;
	if(mContacts.get(peering, contact)) return contact;
	else return NULL;
}

const AddressBook::Contact *AddressBook::getContact(const Identifier &peering) const
{
	Synchronize(this);
  
	Contact *contact;
	if(mContacts.contains(peering)) return mContacts.get(peering);
	else return NULL;
}

void AddressBook::getContacts(Array<AddressBook::Contact *> &array)
{
	Synchronize(this); 
  
	mContactsByUniqueName.getValues(array);
	Contact *self = getSelf();
	if(self) array.remove(self);
}

const Identifier &AddressBook::setSelf(const ByteString &secret)
{
	Synchronize(this);
  
	const String tracker = Config::Get("tracker");
	
	Contact *self = getSelf();
	if(self) removeContact(self->peering());
	
	self = new Contact(this, userName(), userName(), tracker, secret);
	if(mContacts.contains(self->peering())) 
	{
		delete self;
		throw Exception("a contact with the same peering already exists");
	}
	
	mContacts.insert(self->peering(), self);
	mContactsByUniqueName.insert(userName(), self);
	
	save();
	start();
	return self->peering();
}

AddressBook::Contact *AddressBook::getSelf(void)
{
	Synchronize(this);
	
	Contact *contact;
	if(mContactsByUniqueName.get(userName(), contact)) return contact;
	else return NULL;
}

const AddressBook::Contact *AddressBook::getSelf(void) const
{
	Synchronize(this);
	
	Contact *contact;
	if(mContactsByUniqueName.get(userName(), contact)) return contact;
	else return NULL;
}

void AddressBook::clear(void)
{
	Synchronize(this);
  
	for(Map<Identifier, Contact*>::const_iterator it = mContacts.begin();
		it != mContacts.end();
		++it)
	{
		const Contact *contact = it->second;
		Interface::Instance->remove(contact->urlPrefix());
		delete contact;
	} 
	
	mContacts.clear();
	mContactsByUniqueName.clear();
}

void AddressBook::load(Stream &stream)
{
	Synchronize(this);

	bool changed = false;
	
	YamlSerializer serializer(&stream);
	Contact *contact = new Contact(this);
	while(serializer.input(*contact))
	{
		Contact *oldContact = NULL;
		if(mContactsByUniqueName.get(contact->uniqueName(), oldContact))
		{
			if(oldContact->time() >= contact->time()) continue;
			contact->addAddresses(oldContact->addresses());
			delete oldContact;
		}

		mContacts.insert(contact->peering(), contact);
		mContactsByUniqueName.insert(contact->uniqueName(), contact);
		Interface::Instance->add(contact->urlPrefix(), contact);
		changed = true;
		
		contact = new Contact(this);
	}
	delete contact;
	
	if(changed) start();
}

void AddressBook::save(Stream &stream) const
{
	Synchronize(this);
  
	YamlSerializer serializer(&stream);
	for(Map<Identifier, Contact*>::const_iterator it = mContacts.begin();
		it != mContacts.end();
		++it)
	{
		const Contact *contact = it->second;
		serializer.output(*contact);
	}	
}

void AddressBook::save(void) const
{
	Synchronize(this);
  
	String data;
	save(data);
	
	SafeWriteFile file(mFileName);
	file.write(data);
	file.close();
	
	const Contact *self = getSelf();
	if(self && self->isConnected())
	{
		try {
			Message message(data);
			message.setParameter("type", "contacts");
			message.send(self->peering());
		}
		catch(Exception &e)
		{
			Log("AddressBook::Save", String("Contacts synchronization failed: ") + e.what()); 
		}
	}
}

void AddressBook::update(void)
{
	Synchronize(this);
	//Log("AddressBook::update", "Updating " + String::number(unsigned(mContacts.size())) + " contacts");
	
	for(Map<Identifier, Contact*>::iterator it = mContacts.begin();
		it != mContacts.end();
		++it)
	{
		Contact *contact = it->second;
		contact->update();
	}
		
	//Log("AddressBook::update", "Finished");
	save();
}

void AddressBook::http(const String &prefix, Http::Request &request)
{
	Synchronize(this);

	try {
		if(request.url.empty() || request.url == "/")
		{
			if(request.method == "POST")
			{
				try {
			  		String command = request.post["command"];
			  		if(command == "delete")
					{
				  		Identifier peering;
						request.post["argument"] >> peering;
						
						removeContact(peering);
					}
					else {
						String name, csecret;
						name = request.post["name"];
						csecret = request.post["secret"];
					  
						if(name.empty() || csecret.empty()) throw 400;
						
						ByteString secret;
						Sha512::Hash(csecret, secret, Sha512::CryptRounds);
						
						if(request.post.contains("self")) setSelf(secret);
						else addContact(name, secret);
					}
				}
				catch(const Exception &e)
				{
					Log("AddressBook::http", String("Error: ") + e.what());
					throw 400;
				}				
				
				Http::Response response(request, 303);
				response.headers["Location"] = prefix + "/";
				response.send();
				return;
			}
			
			if(request.get.contains("json"))
			{
				Http::Response response(request, 200);
				response.headers["Content-Type"] = "application/json";
				response.send();

				JsonSerializer json(response.sock);
				json.outputMapBegin();
				for(Map<Identifier, Contact*>::iterator it = mContacts.begin();
					it != mContacts.end();
					++it)
				{
					Contact *contact = it->second;
					
					StringMap map;
					map["name"] << contact->name();
					map["tracker"] << contact->tracker();
					map["status"] << contact->status();
					map["messages"] << contact->unreadMessagesCount();
					
					json.outputMapElement(contact->uniqueName(), map);
				}
				json.outputMapEnd();
				return;
			}
			
			Http::Response response(request,200);
			response.send();
			
			Html page(response.sock);
			page.header("Contacts");
			
			Array<Contact*> contacts;
			getContacts(contacts);
			if(!contacts.empty())
			{
				page.open("div",".box");
				
				page.openForm(prefix+"/", "post", "executeForm");
				page.input("hidden", "command");
				page.input("hidden", "argument");
				page.closeForm();
				
				page.javascript("function deleteContact(name, identifier) {\n\
					if(confirm('Do you really want to delete '+name+' ?')) {\n\
						document.executeForm.command.value = 'delete';\n\
						document.executeForm.argument.value = identifier;\n\
						document.executeForm.submit();\n\
					}\n\
				}");
				
				page.open("table",".contacts");
				for(int i=0; i<contacts.size(); ++i)
				{
					Contact *contact = contacts[i];
					String contactUrl = prefix + '/' + contact->uniqueName() + '/';
					
					page.open("tr");
					page.open("td");
					page.open("span",".contact");
					page.link(contactUrl, contact->name() + "@" + contact->tracker());
					page.close("span");
					page.close("td");
					page.open("td");
					page.text(" "+String::hexa(contact->peeringChecksum(),8));
					page.close("td");
					page.open("td",".delete");
					page.openLink("javascript:deleteContact('"+contact->name()+"','"+contact->peering().toString()+"')");
					page.image("/delete.png", "Delete");
					page.closeLink();
					page.close("td");
					page.close("tr");
				}
				page.close("table");
				page.close("div");
			}
			
			page.openForm(prefix+"/","post");
			page.openFieldset("New contact");
			page.label("name","Name"); page.input("text","name"); page.br();
			page.label("secret","Secret"); page.input("text","secret"); page.br();
			page.label("add"); page.button("add","Add contact");
			page.closeFieldset();
			page.closeForm();
			
			page.openForm(prefix+"/","post");
			page.openFieldset("Personal secret");
			page.input("hidden","name",userName());
			page.input("hidden","self","true");
			if(getSelf()) page.text("Your personal secret is already set, but you can change it here.");
			else page.text("Set the same username and the same personal secret on multiple devices to enable automatic synchronization.");
			page.br();
			page.br();
			page.label("secret","Secret"); page.input("text","secret"); page.br();
			page.label("add"); page.button("add","Set secret");
			page.closeFieldset();
			page.closeForm();
			
			page.footer();
			return;
		}
	}
	catch(const Exception &e)
	{
		Log("AddressBook::http",e.what());
		throw 500;	// Httpd handles integer exceptions
	}
	
	throw 404;
}

void AddressBook::run(void)
{
	update();
}

bool AddressBook::publish(const Identifier &remotePeering)
{
	try {
		String url("http://" + Config::Get("tracker") + "/tracker?id=" + remotePeering.toString());
		
		List<Address> list;
		Config::GetExternalAddresses(list);
		
		String addresses;
		for(	List<Address>::iterator it = list.begin();
			 it != list.end();
			++it)
		{
			if(!addresses.empty()) addresses+= ',';
			addresses+= it->toString();
		}
		
		StringMap post;
		post["instance"] = Core::Instance->getName();
		post["port"] = Config::Get("port");
		post["addresses"] = addresses;
		
		if(!Core::Instance->isPublicConnectable())
		{
			list.clear();
			Core::Instance->getKnownPublicAdresses(list);
			
			String altAddresses;
			for(	List<Address>::iterator it = list.begin();
				it != list.end();
				++it)
			{
				if(!altAddresses.empty()) altAddresses+= ',';
				altAddresses+= it->toString();
			}
			
			post["alternate"] = altAddresses;
		}
		
		if(Http::Post(url, post) != 200) return false;
	}
	catch(const std::exception &e)
	{
		Log("AddressBook::publish", e.what()); 
		return false;
	}
	return true;
}

bool AddressBook::query(const Identifier &peering, const String &tracker, AddressMap &output, bool alternate)
{
	try {
	  	String url;
	  	if(tracker.empty()) url = "http://" + Config::Get("tracker") + "/tracker?id=" + peering.toString();
		else url = "http://" + tracker + "/tracker?id=" + peering.toString();
  		if(alternate) url+= "&alternate=1";
		  
		String tmp;
		if(Http::Get(url, &tmp) != 200) return false;
		tmp.trim();
		if(tmp.empty()) return false;
	
		YamlSerializer serializer(&tmp);
		serializer.input(output);
	}
	catch(const std::exception &e)
	{
		Log("AddressBook::query", e.what()); 
		return false;
	}
	return true;
}

AddressBook::Contact::Contact(	AddressBook *addressBook, 
				const String &uname,
				const String &name,
			        const String &tracker,
			        const ByteString &secret) :
	mAddressBook(addressBook),
	mUniqueName(uname),
	mName(name),
	mTracker(tracker),
	mSecret(secret),
	mTime(Time::Now()),
	mFound(false)
{	
	Assert(addressBook != NULL);
	Assert(!uname.empty());
	Assert(!name.empty());
	Assert(!tracker.empty());
	Assert(!secret.empty());
	
	// Compute peering
	String agregate;
	agregate.writeLine(mSecret);
	agregate.writeLine(mAddressBook->userName());
	agregate.writeLine(mName);
	Sha512::Hash(agregate, mPeering, Sha512::CryptRounds);
	
	// Compute Remote peering
	agregate.clear();
	agregate.writeLine(mSecret);
	agregate.writeLine(mName);
	agregate.writeLine(mAddressBook->userName());
	Sha512::Hash(agregate, mRemotePeering, Sha512::CryptRounds);
}

AddressBook::Contact::Contact(AddressBook *addressBook) :
  	mAddressBook(addressBook),
	mMessagesCount(0)
{
  
}

AddressBook::Contact::~Contact(void)
{

}

const String &AddressBook::Contact::uniqueName(void) const
{
	return mUniqueName;
}

const String &AddressBook::Contact::name(void) const
{
	return mName;
}

const String &AddressBook::Contact::tracker(void) const
{
	return mTracker;
}

const Identifier &AddressBook::Contact::peering(void) const
{
	return mPeering;
}

const Identifier &AddressBook::Contact::remotePeering(void) const
{
	return mRemotePeering;
}

const Time &AddressBook::Contact::time(void) const
{
	return mTime; 
}

uint32_t AddressBook::Contact::peeringChecksum(void) const
{
	return mPeering.getDigest().checksum32() + mRemotePeering.getDigest().checksum32(); 
}

String AddressBook::Contact::urlPrefix(void) const
{
	if(mUniqueName.empty()) return "";
	return String("/")+mAddressBook->userName()+"/contacts/"+mUniqueName;
}

int AddressBook::Contact::unreadMessagesCount(void) const
{
	int count = 0;
	for(int i=mMessages.size()-1; i>=0; --i)
	{
		if(mMessages[i].isRead()) break;
		++count;
	}
	return count;
}

bool AddressBook::Contact::isFound(void) const
{
	return mFound;
}

bool AddressBook::Contact::isConnected(void) const
{
	return Core::Instance->hasPeer(mPeering); 
}

bool AddressBook::Contact::isConnected(const String &instance) const
{
	return Core::Instance->hasPeer(Identifier(mPeering, instance)); 
}

String AddressBook::Contact::status(void) const
{
	if(isConnected()) return "connected";
	else if(isFound()) return "found";
	else return "disconnected";
}

const AddressBook::AddressMap &AddressBook::Contact::addresses(void) const
{
	return mAddrs;
}

bool AddressBook::Contact::addAddress(const Address &addr, const String &instance)
{
  	Synchronize(this);

	if(addr.isNull()) return false;
	if(instance == Core::Instance->getName()) return false;
	
	bool isNew = !(mAddrs.contains(instance) && mAddrs[instance].contains(addr));
	if((!isConnected(instance) || isNew) && connectAddress(addr, instance))
	{
		if(isNew) mAddrs[instance].push_back(addr);
		return true;
	}
	
	return false;
}

bool AddressBook::Contact::addAddresses(const AddressMap &map)
{
	Synchronize(this);
  
	bool success = false;
	for(AddressMap::const_iterator it = map.begin();
		it != map.end();
		++it)
	{
		const String &instance = it->first;
		const AddressArray &addrs = it->second;
		for(int i=0; i<addrs.size(); ++i)
			success|= addAddress(addrs[i], instance);
	}
	
	return success;
}

bool AddressBook::Contact::connectAddress(const Address &addr, const String &instance)
{
 	Synchronize(this);
	
	if(addr.isNull()) return false;
	if(instance == Core::Instance->getName()) return false;
	
	Identifier identifier(mPeering, instance);
	try {
		Desynchronize(this);
		Socket *sock = new Socket(addr, 1000);	// TODO: timeout
		return Core::Instance->addPeer(sock, identifier);
	}
	catch(...)
	{

	}

	return false; 
}

bool AddressBook::Contact::connectAddresses(const AddressMap &map)
{
	Synchronize(this);
  
	bool success = false;
	for(AddressMap::const_iterator it = map.begin();
		it != map.end();
		++it)
	{
		const String &instance = it->first;
		const AddressArray &addrs = it->second;
		for(int i=0; i<addrs.size(); ++i)
			if(connectAddress(addrs[i], instance))
			{
				success = true;
				break;
			}
	}
	return success;
}

void AddressBook::Contact::removeAddress(const Address &addr)
{
	Synchronize(this);
	mAddrs.erase(addr);
}

void AddressBook::Contact::update(void)
{
	Synchronize(this);

	if(!mMessages.empty())
        {
                time_t t = Time::Now();
                while(!mMessages.front().isRead()
                        && mMessages.front().time() >= t + 7200)        // 2h
                {
                                 mMessages.pop_front();
                }
        }

	//Log("AddressBook::Contact", "Looking for " + mUniqueName);
	Core::Instance->registerPeering(mPeering, mRemotePeering, mSecret, this);
		
	if(mPeering != mRemotePeering && Core::Instance->hasRegisteredPeering(mRemotePeering))	// the user is local
	{
		if(!Core::Instance->hasPeer(Identifier(mPeering, Core::Instance->getName())))
		{
			Log("AddressBook::Contact", mUniqueName + " found locally");
			  
			Address addr("127.0.0.1", Config::Get("port"));
			try {
				Socket *sock = new Socket(addr);
				Core::Instance->addPeer(sock, mPeering);
			}
			catch(...)
			{
				Log("AddressBook::Contact", "Warning: Unable to connect the local core");	 
			}
		}
	}
	
	//Log("AddressBook::Contact", "Publishing to tracker " + mTracker + " for " + mUniqueName);
	AddressBook::publish(mRemotePeering);
		  
	//Log("AddressBook::Contact", "Querying tracker " + mTracker + " for " + mUniqueName);	
		
	AddressMap newAddrs;
	if(mFound = AddressBook::query(mPeering, mTracker, newAddrs, false))
	{
		if(addAddresses(newAddrs)) return;
	}
	else {
		if(connectAddresses(mAddrs)) return;
	}
	
	AddressMap altAddrs;
	if(mFound = AddressBook::query(mPeering, mTracker, altAddrs, false))
		connectAddresses(altAddrs);
}

void AddressBook::Contact::message(Message *message)
{
	Synchronize(this);
	
	Assert(message);
	Assert(message->receiver() == mPeering);
	
	String type;
	message->parameters().get("type",type);
	
	if(type.empty() || type == "text")
	{
		mMessages.push_back(*message);
		++mMessagesCount;
		notifyAll();
	}
	else if(type == "contacts")
	{
		if(mUniqueName != mAddressBook->userName())
		{
			Log("AddressBook::Contact::message", "Warning: received contacts update from other than self, dropping");
			return;
		}
		
		String data = message->content();
		mAddressBook->load(data);
	}
}

void AddressBook::Contact::request(Request *request)
{
	Assert(request);
	request->execute(mAddressBook->user());
}

void AddressBook::Contact::http(const String &prefix, Http::Request &request)
{
  	Synchronize(this);
	
	String base(prefix+request.url);
	base = base.substr(base.lastIndexOf('/')+1);
	if(!base.empty()) base+= '/';
	
	try {
		if(request.url.empty() || request.url == "/")
		{
			Http::Response response(request,200);
			response.send();

			Html page(response.sock);
			page.header("Contact: "+mName);
				
			page.open("div",".menu");
				
			page.span("Status:", ".title");
			page.open("span", "status.status");
			page.span(status().capitalized(), String(".")+status());
			page.close("span");
			page.br();
			page.br();
			
			page.link(prefix+"/files/","Files");
			page.br();
			page.link(prefix+"/search/","Search");
			page.br();
			page.openLink(prefix+"/chat/");
			page.text("Chat");
			page.open("span", "messagescount.messagescount");
			int msgcount = unreadMessagesCount();
			if(msgcount) page.text(String(" (")+String::number(msgcount)+String(")"));
			page.close("span");
			page.closeLink();
			page.br();
			page.close("div");
			
			unsigned refreshPeriod = 5000;
			page.javascript("function updateContact() {\n\
				$.getJSON('/"+mAddressBook->userName()+"/contacts/?json', function(data) {\n\
					var info = data."+uniqueName()+";\n\
			  		transition($('#status'),\n\
						'<span class=\"'+info.status+'\">'+info.status.capitalize()+'</span>\\n');\n\
					var msg = '';\n\
					if(info.messages != 0) msg = ' ('+info.messages+')';\n\
					transition($('#messagescount'), msg);\n\
					setTimeout('updateContact()',"+String::number(refreshPeriod)+");\n\
  				});\n\
			}\n\
			updateContact();");
			
			page.footer();
			return;
		}
		else {
			String url = request.url;
			String directory = url;
			directory.ignore();		// remove first '/'
			url = "/" + directory.cut('/');
			if(directory.empty()) throw 404;
			  
			if(directory == "files")
			{
				String target(url);
				if(target.empty() || target[target.size()-1] != '/') 
					target+= '/';
				
				Request trequest(target, false);
				
				// Self
				if(mUniqueName == mAddressBook->userName())
				{
					trequest.execute(mAddressBook->user());
				}
				
				Synchronize(&trequest);
				try {
					trequest.submit(mPeering);
					trequest.wait();
				}
				catch(const Exception &e)
				{
					// If not self
					if(mUniqueName != mAddressBook->userName())
					{
						Log("AddressBook::Contact::http", "Cannot send request, peer not connected");
						
						Http::Response response(request,200);
						response.send();

						Html page(response.sock);
						page.header(mName+": Files");
						page.open("div",".box");
						page.text("Not connected...");
						page.close("div");
						page.footer();
						return;
					}
				}
				
				if(trequest.responsesCount() == 0) throw Exception("No responses");
				if(!trequest.isSuccessful()) throw 404;
					
				try {
					Http::Response response(request,200);
					response.send();	
				
					Html page(response.sock);
					if(target.empty() || target == "/") page.header(mName+": Browse files");
					else page.header(mName+": "+target.substr(1));
					page.link(prefix+"/search/","Search files");
					
					Set<String> instances;
					Map<String, StringMap> files;
					for(int i=0; i<trequest.responsesCount(); ++i)
					{
					  	Request::Response *tresponse = trequest.response(i);
						if(tresponse->error()) continue;
					
						const StringMap &params = tresponse->parameters();
						instances.insert(tresponse->instance());

						// Check info
						if(!params.contains("type")) continue;
						if(!params.contains("name")) continue;
						if(params.get("type") != "directory" && !params.contains("hash")) continue;

						// Sort
						// Directories with the same name appears only once
						// Files with identical content appears only once
						if(params.get("type") == "directory") files.insert("0"+params.get("name"),params);
						else files.insert("1"+params.get("name")+params.get("hash").toString(), params);
					}

					page.open("div",".box");
					if(files.empty()) page.text("No shared files");
					else {
					  	String desc;
						if(instances.size() == 1) desc << files.size() << " files";
						else desc << files.size() << " files on " << instances.size() << " instances";
						page.text(desc);
						page.br();
						
						page.open("table",".files");
						for(Map<String, StringMap>::iterator it = files.begin();
							it != files.end();
							++it)
						{
							StringMap &map = it->second;
								
							page.open("tr");
							page.open("td",".filename"); 
							if(map.get("type") == "directory") page.link(base + map.get("name"), map.get("name"));
							else page.link("/" + map.get("hash"), map.get("name"));
							page.close("td");
							page.open("td",".size"); 
							if(map.get("type") == "directory") page.text("directory");
							else if(map.contains("size")) page.text(String::hrSize(map.get("size")));
							page.close("td");
							page.close("tr");
						}
						page.close("table");
					}
					page.close("div");
					page.footer();
					return;
				}
				catch(const Exception &e)
				{
					Log("AddressBook::Contact::http", String("Unable to access remote file or directory: ") + e.what());
					throw;
				}
			}
			else if(directory == "search")
			{
				if(url != "/") throw 404;
				
				String query;
				if(request.post.contains("query"))
				{
					query = request.post.get("query");
					query.trim();
				}
				
				Http::Response response(request,200);
				response.send();
				
				Html page(response.sock);
				page.header(mName+": Search");
				page.openForm(prefix + "/search", "post", "searchForm");
				page.input("text","query",query);
				page.button("search","Search");
				page.closeForm();
				page.javascript("document.searchForm.query.focus();");
				page.br();
				
				if(query.empty())
				{
					page.footer();
					return;
				}
				
				Request trequest("search:"+query, false);	// no data
				try {
					trequest.submit(mPeering);
				}
				catch(const Exception &e)
				{
					Log("AddressBook::Contact::http", "Cannot send request, peer not connected");
					page.open("div",".box");
					page.text("Not connected...");
					page.close("div");
					page.footer();
					return;
				}
				
				const unsigned timeout = Config::Get("request_timeout").toInt();
				
				{
					Desynchronize(this);
					trequest.lock();
					trequest.wait(timeout);
				}
				
				page.open("div",".box");
				if(!trequest.isSuccessful()) page.text("No files found");
				else try {
					
					page.open("table",".files");
					for(int i=0; i<trequest.responsesCount(); ++i)
					{
						Request::Response *tresponse = trequest.response(i);
						if(tresponse->error()) continue;
					
						// Check info
						StringMap map = tresponse->parameters();
						if(!map.contains("type")) continue;
						if(!map.contains("path")) continue;
						if(map.get("type") != "directory" && !map.contains("hash")) continue;
						if(!map.contains("name")) map["name"] = map["path"].afterLast('/');
						
						page.open("tr");
						page.open("td",".filename"); 
						if(map.get("type") == "directory") page.link(urlPrefix() + "/files" + map.get("path"), map.get("name"));
						else page.link("/" + map.get("hash"), map.get("name"));
						page.close("td");
						page.open("td",".size"); 
						if(map.get("type") == "directory") page.text("directory");
						else if(map.contains("size")) page.text(String::hrSize(map.get("size")));
						page.close("td");
						page.close("tr");
					}
					page.close("table");
					
				}
				catch(const Exception &e)
				{
					Log("AddressBook::Contact::http", String("Unable to list files: ") + e.what());
					page.close("table");
					page.text("Error, unable to list files");
				}
				page.close("div");
				
				trequest.unlock();
				page.footer();
				return;
			}
			else if(directory == "chat")
			{
				if(url != "/")
				{
				  	url.ignore();
					unsigned count = 0;
					try { url>>count; }
					catch(...) { throw 404; }
					
					Http::Response response(request,200);
					response.send();
					
					if(count == mMessagesCount)
						wait(120000);
					
					if(count < mMessagesCount && mMessagesCount-count <= mMessages.size())
					{
						Html html(response.sock);
						int i = mMessages.size() - (mMessagesCount-count);
						messageToHtml(html, mMessages[i], false);
						mMessages[i].markRead();
					}
					
					return;
				}
			  
				if(request.method == "POST")
				{
					if(request.post.contains("message") && !request.post["message"].empty())
					{
						try {
							Message message(request.post["message"]);
						  	message.send(mPeering);	// send to other
							
							Contact *self = mAddressBook->getSelf();
							if(self || self->isConnected()) message.send(self->peering());
							
							mMessages.push_back(Message(request.post["message"]));	// thus receiver is null
							++mMessagesCount;
							notifyAll();
							
							if(request.post.contains("ajax") && request.post["ajax"].toBool())	//ajax
							{
								Http::Response response(request, 200);
								response.send();
								/*Html html(response.sock);
								messageToHtml(html, mMessages.back(), false);
								mMessages.back().markRead();*/
							}
							else {	// form submit
							 	Http::Response response(request, 303);
								response.headers["Location"] = prefix + "/chat";
								response.send();
							}
						}
						catch(const Exception &e)
						{
							Log("AddressBook::Contact::http", String("Cannot post message: ") + e.what());
							throw 409;
						}
						
						return;
					}
				}
			  
				bool isPopup = request.get.contains("popup");
			  
				Http::Response response(request,200);
				response.send();	
				
				Html page(response.sock);
				page.header("Chat with "+mName, isPopup);
				if(isPopup)
				{
					page.open("b");
					page.text("Chat with "+mName+" - ");
					page.open("span", "status.status");
					page.span(status().capitalized(), String(".")+status());
					page.close("span");
					page.close("b");
				}
				else {
					page.open("div", "chat.box");
					page.open("span", "status.status");
					page.span(status().capitalized(), String(".")+status());
					page.close("span");
				}
				
				page.openForm(prefix + "/chat", "post", "chatForm");
				page.input("text","message");
				page.button("send","Send");
				page.space();
				
				if(!isPopup)
				{
					String popupUrl = prefix + "/chat?popup=1";
					page.raw("<a href=\""+popupUrl+"\" target=\"_blank\" onclick=\"return popup('"+popupUrl+"','/');\">Popup</a>");
				}
				
				page.br();
				page.br();
				page.closeForm();
				page.javascript("document.chatForm.message.focus();");
				
				
				unsigned refreshPeriod = 5000;
				page.javascript("function updateContact() {\n\
					$.getJSON('/"+mAddressBook->userName()+"/contacts/?json', function(data) {\n\
						var info = data."+uniqueName()+";\n\
						transition($('#status'),\n\
							'<span class=\"'+info.status+'\">'+info.status.capitalize()+'</span>\\n');\n\
						setTimeout('updateContact()',"+String::number(refreshPeriod)+");\n\
					});\n\
				}\n\
				updateContact();");
				
				page.open("div", "chatmessages");
				for(int i=mMessages.size()-1; i>=0; --i)
				{
	  				messageToHtml(page, mMessages[i], mMessages[i].isRead());
					mMessages[i].markRead();
				}
				page.close("div");
				if(!isPopup) page.close("div");
				
				page.javascript("var count = "+String::number(mMessagesCount)+";\n\
					var title = document.title;\n\
					var hasFocus = true;\n\
					var nbNewMessages = 0;\n\
					$(window).blur(function() {\n\
						hasFocus = false;\n\
						$('span.message').attr('class', 'oldmessage');\n\
					});\n\
					$(window).focus(function() {\n\
						hasFocus = true;\n\
						nbNewMessages = 0;\n\
						document.title = title;\n\
					});\n\
					function update()\n\
					{\n\
						var request = $.ajax({\n\
							url: '"+prefix+"/chat/'+count,\n\
							dataType: 'html',\n\
							timeout: 300000\n\
						});\n\
						request.done(function(html) {\n\
							if($.trim(html) != '')\n\
							{\n\
								$(\"#chatmessages\").prepend(html);\n\
								var text = $('#chatmessages span.text:first');\n\
								if(text) text.html(text.html().linkify());\n\
								if(!hasFocus)\n\
								{\n\
									nbNewMessages+= 1;\n\
									document.title = title+' ('+nbNewMessages+')';\n\
								}\n\
								count+= 1;\n\
							}\n\
							setTimeout('update()', 100);\n\
						});\n\
						request.fail(function(jqXHR, textStatus) {\n\
							setTimeout('update()', 10000);\n\
						});\n\
					}\n\
					function post()\n\
					{\n\
						var message = document.chatForm.message.value;\n\
						if(!message) return false;\n\
						document.chatForm.message.value = '';\n\
						var request = $.post('"+prefix+"/chat',\n\
							{ 'message': message, 'ajax': 1 });\n\
						request.fail(function(jqXHR, textStatus) {\n\
							alert('The message could not be sent. Is this user online ?');\n\
						});\n\
					}\n\
					setTimeout('update()', 1000);\n\
					document.chatForm.onsubmit = function() {post(); return false;}");
				
				page.footer();
				return;
			}
		}
	}
	catch(const NetException &e)
	{
		throw;
	}
	catch(const Exception &e)
	{
		Log("AddressBook::Contact::http", e.what());
		throw 500;
	}
	
	throw 404;
}

void AddressBook::Contact::messageToHtml(Html &html, const Message &message, bool old) const
{
	char buffer[64];
	time_t t = message.time();
	std::strftime (buffer, 64, "%x %X", localtime(&t));
	if(old) html.open("span",".oldmessage");
	else html.open("span",".message");
	html.open("span",".date");
	html.text(buffer);
	html.close("span");
	html.text(" ");
	html.open("span",".user");
	if(message.receiver() == Identifier::Null) html.text(mAddressBook->userName());
	else html.text(mAddressBook->getContact(message.receiver())->name());
	html.close("span");
	html.text(": ");
	html.open("span",".text");
	html.text(message.content());
	html.close("span");
	html.close("span");
	html.br(); 
}

void AddressBook::Contact::serialize(Serializer &s) const
{
	Synchronize(this);
	
	StringMap map;
	map["uname"] << mUniqueName;
	map["name"] << mName;
	map["tracker"] << mTracker;
	map["secret"] << mSecret;
	map["peering"] << mPeering;
	map["remote"] << mRemotePeering;
	map["time"] << mTime;
	
	s.outputMapBegin(2);
	s.outputMapElement(String("info"),map);
	s.outputMapElement(String("addrs"),mAddrs);
	s.outputMapEnd();
}

bool AddressBook::Contact::deserialize(Serializer &s)
{
	Synchronize(this);
	
	if(!mUniqueName.empty())
		Interface::Instance->remove(urlPrefix());
	
	mUniqueName.clear();
  	mName.clear();
	mTracker.clear();
	mSecret.clear();
	mPeering.clear();
	mRemotePeering.clear();
	
	StringMap map;
	
	String key;
	AssertIO(s.inputMapBegin());
	AssertIO(s.inputMapElement(key, map) && key == "info");
	
	// TEMPORARY : try/catch block should be removed
	try {
		AssertIO(s.inputMapElement(key, mAddrs) && key == "addrs");
	}
	catch(...) {
		// HACK
		String hack;
		do s.input(hack);
		while(!hack.empty());
	}
	//
	
	map["uname"] >> mUniqueName;
	map["name"] >> mName;
	map["tracker"] >> mTracker;
	map["secret"] >> mSecret;
	map["peering"] >> mPeering;
	map["remote"] >> mRemotePeering;
	
	if(map.contains("time")) map["time"] >> mTime;
	else mTime = Time::Now();
	
	// TODO: checks
	
	return true;
}

bool AddressBook::Contact::isInlineSerializable(void) const
{
	return false; 
}

}
