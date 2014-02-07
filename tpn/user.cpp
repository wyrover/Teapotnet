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

#include "tpn/user.h"
#include "tpn/config.h"
#include "tpn/file.h"
#include "tpn/directory.h"
#include "tpn/sha512.h"
#include "tpn/html.h"
#include "tpn/yamlserializer.h"
#include "tpn/jsonserializer.h"
#include "tpn/byteserializer.h"
#include "tpn/mime.h"

namespace tpn
{

Map<String, User*>	User::UsersByName;
Map<ByteString, User*>	User::UsersByAuth;
Mutex			User::UsersMutex;

unsigned User::Count(void)
{
	 UsersMutex.lock();
	 unsigned count = UsersByName.size();
	 UsersMutex.unlock();
	 return count;
}

void User::GetNames(Array<String> &array)
{
	 UsersMutex.lock();
	 UsersByName.getKeys(array);
	 UsersMutex.unlock();
}

bool User::Exist(const String &name)
{
	return (User::Get(name) != NULL);
}

User *User::Get(const String &name)
{
	User *user = NULL;
	UsersMutex.lock();
	if(UsersByName.get(name, user)) 
	{
	  	UsersMutex.unlock();
		return user;
	}
	UsersMutex.unlock();
	return NULL; 
}

User *User::Authenticate(const String &name, const String &password)
{
	ByteString hash;
	Sha512::RecursiveHash(password, name, hash, Sha512::CryptRounds);
	
	User *user = NULL;
	UsersMutex.lock();
	if(UsersByAuth.get(hash, user))
	{
	  	UsersMutex.unlock();
		return user;
	}
	UsersMutex.unlock();
	LogWarn("User::Authenticate", "Authentication failed for \""+name+"\"");
	return NULL;
}

void User::UpdateAll(void)
{
	Array<String> names;
	UsersMutex.lock();
	UsersByName.getKeys(names);
	UsersMutex.unlock();
	
	for(int i=0; i<names.size(); ++i)
	{
		User *user = NULL;
		UsersMutex.lock();
		UsersByName.get(names[i], user);
		UsersMutex.unlock();
		
		user->addressBook()->update();
	}
}

User::User(const String &name, const String &password, const String &tracker) :
	mName(name),
	mOnline(false),
	mSetOfflineTask(this)
{
	if(mName.empty()) 
		throw Exception("Empty user name");
	
	if(!mName.isAlphanumeric()) 
		throw Exception("User name must be alphanumeric");
	
	// TODO: backward compatibility, should be removed
	if(File::Exist(profilePath()+"password"))
		File::Remove(profilePath()+"password");
	//

	// Auth digest
	if(password.empty())
	{
		File file(profilePath()+"auth", File::Read);
		file.read(mAuth);
		file.close();
	}
	else {
		Sha512::RecursiveHash(password, mName, mAuth, Sha512::CryptRounds);
		
		File file(profilePath()+"auth", File::Truncate);
		file.write(mAuth);
		file.close();
	}
		
	// Token secret
	mTokenSecret.writeRandom(16);
	
	// Secret
	if(File::Exist(profilePath()+"secret"))
	{
		File file(profilePath()+"secret", File::Read);
		file.read(mSecret);
		file.close();
	}
	
	mStore = NULL;
	mAddressBook = NULL;
	mMessageQueue = NULL;
	mProfile = NULL;

	try {
		mStore = new Store(this); // must be created first
		mProfile = new Profile(this, mName, tracker); // must be created before AddressBook
        	mAddressBook = new AddressBook(this);
       	 	mMessageQueue = new MessageQueue(this);
	}
	catch(...)
	{
		delete mStore;
		delete mAddressBook;
		delete mMessageQueue;
		delete mProfile;
		throw;
	}

	try {
		mProfile->load();
	}
	catch(const Exception &e)
	{
		LogWarn("User", String("Unable to load profile: ") + e.what());
	}
	
	UsersMutex.lock();
	UsersByName.insert(mName, this);
	UsersByAuth.insert(mAuth, this);
	UsersMutex.unlock();

	Interface::Instance->add(urlPrefix(), this);
}

User::~User(void)
{
  	UsersMutex.lock();
	UsersByName.erase(mName);
  	UsersByAuth.erase(mAuth);
	UsersMutex.unlock();
	
	Interface::Instance->remove(urlPrefix());
	Scheduler::Global->remove(&mSetOfflineTask);
	
	delete mAddressBook;
	delete mMessageQueue;
	delete mStore;
}

String User::name(void) const
{
	Synchronize(this);
	return mName; 
}

String User::tracker(void) const
{
	Synchronize(this);
        return mProfile->tracker();
}

String User::profilePath(void) const
{
	Synchronize(this);
	if(!Directory::Exist(Config::Get("profiles_dir"))) Directory::Create(Config::Get("profiles_dir"));
	String path = Config::Get("profiles_dir") + Directory::Separator + mName;
	if(!Directory::Exist(path)) Directory::Create(path);
	return path + Directory::Separator;
}

String User::urlPrefix(void) const
{
	Synchronize(this);
	return String("/") + mName;
}

void User::setTracker(const String &tracker)
{
	Synchronize(this);
	mProfile->setTracker(tracker);
}

AddressBook *User::addressBook(void) const
{
	return mAddressBook;
}

MessageQueue *User::messageQueue(void) const
{
	return mMessageQueue;
}

Store *User::store(void) const
{
	return mStore;
}

Profile *User::profile(void) const
{
        return mProfile;
}

bool User::isOnline(void) const
{
	Synchronize(this);
	return mOnline;
}

void User::setOnline(void)
{
	Synchronize(this);
	if(!mOnline) 
	{
		mOnline = true;
		sendStatus();
		
		DesynchronizeStatement(this, mAddressBook->update());
	}
	
	Scheduler::Global->schedule(&mSetOfflineTask, 60.);
}

void User::setOffline(void)
{
	Synchronize(this);
	
	if(mOnline) 
	{
		mOnline = false;
		sendStatus();
	}
}

void User::sendStatus(const Identifier &identifier)
{
	Synchronize(this);
	
	String status = (mOnline ? "online" : "offline");
	
	Notification notification(status);
	notification.setParameter("type", "status");
	
	if(identifier != Identifier::Null)
	{
		DesynchronizeStatement(this, notification.send(identifier));
	}
	else {
		DesynchronizeStatement(this, addressBook()->send(notification));
	}
}

void User::sendSecret(const Identifier &identifier)
{
	Synchronize(this);
	
	if(mSecret.empty()) return;
	
	if(identifier == Identifier::Null)
		throw Exception("Prevented sendSecret() to broadcast");
	
	Notification notification(mSecret.toString());
	notification.setParameter("type", "secret");
	notification.setParameter("time", File::Time(profilePath()+"secret").toString());
	
	DesynchronizeStatement(this, notification.send(identifier));
}

void User::setSecret(const ByteString &secret, const Time &time)
{
	Synchronize(this);
	
	if(secret.empty()) return;
	
	if(mSecret.empty() || !File::Exist(profilePath()+"secret") || time >  File::Time(profilePath()+"secret"))
	{
		mSecret = secret;
		
		File file(profilePath()+"secret", File::Truncate);
		file.write(mSecret);
		file.close();
		
		if(mSecret != secret)
		{
			AddressBook::Contact *self = addressBook()->getSelf();
			if(self) sendSecret(self->peering());
		}
	}
}

ByteString User::getSecretKey(const String &action)
{
	// Create secret if it does not exist
	if(mSecret.empty())
	{
		ByteString secret;
		secret.writeRandom(64);
		setSecret(secret, Time::Now());
	}
	
	// Cache for subkeys
	ByteString key;
	if(!mSecretKeysCache.get(action, key))
	{
		Sha512::DerivateKey(mSecret, action, key, Sha512::CryptRounds);
		mSecretKeysCache.insert(action, key);
	}

	return key;
}

String User::generateToken(const String &action) const
{
	ByteString salt;
	salt.writeRandom(8);

	ByteString plain;
	ByteSerializer splain(&plain);
	splain.output(name());
	splain.output(action);
	splain.output(salt);
	SynchronizeStatement(this, splain.output(mTokenSecret));

	ByteString digest;
	Sha512::Hash(plain, digest);
	
	ByteString key;
	digest.readBinary(key, 8);

	ByteString token;
	token.writeBinary(salt);	// 8 bytes
	token.writeBinary(key);		// 8 bytes
	
	Assert(token.size() == 16);
	return token;
}

bool User::checkToken(const String &token, const String &action) const
{
	if(!token.empty())
	{
		ByteString bs;
		try {
			token.extract(bs);
		}
		catch(const Exception &e)
		{
			LogWarn("User::checkToken", String("Error parsing token: ") + e.what());
			return false;
		}

		if(bs.size() == 16)
		{
			ByteString salt, remoteKey;
			AssertIO(bs.readBinary(salt, 8));
			AssertIO(bs.readBinary(remoteKey, 8));
			
			ByteString plain;
			ByteSerializer splain(&plain);
			splain.output(name());
			splain.output(action);
			splain.output(salt);
			SynchronizeStatement(this, splain.output(mTokenSecret));
			
			ByteString digest;
			Sha512::Hash(plain, digest);
			
			ByteString key;
			digest.readBinary(key, 8);

			if(key == remoteKey) 
				return true;
		}
	}
	
	LogWarn("User::checkToken", String("Invalid token") + (!action.empty() ? " for action \"" + action + "\"" : ""));
	return false;
}

void User::http(const String &prefix, Http::Request &request)
{
	try {
		setOnline();
		
		String url = request.url;
		if(url.empty() || url[0] != '/') throw 404;

		if(url == "/")
		{
			if(request.method == "POST")
			{
				if(!checkToken(request.post["token"], "admin"))
					throw 403;

				String redirect;
				request.post.get("redirect", redirect);
				if(redirect.empty()) redirect = prefix + "/";
				
				String command = request.post["command"];
				
				bool shutdown = false;

				if(command == "update")
				{
					if(!request.sock->getRemoteAddress().isLocal()) throw 403;
					if(!Config::LaunchUpdater()) throw 500;
					
					Http::Response response(request, 200);
					response.send();
					Html page(response.sock);
					page.header("Please wait", true);
					page.open("div", "notification");
					page.image("/loading.png", "Please wait");
					page.br();
					page.open("h1",".huge");
					page.text("Updating and restarting...");
					page.close("h1");
					page.close("div");
					page.javascript("setTimeout(function() {window.location.href = \""+redirect+"\";}, 20000);");
					page.footer();
					response.sock->close();
					
					Thread::Sleep(1.);	// Some time for the browser to load resources
					
					LogInfo("User::http", "Exiting");
					exit(0);
				}
				else if(command == "shutdown")
				{
					if(!request.sock->getRemoteAddress().isLocal()) throw 403;
					shutdown = true;
				}
				else throw 400;
				
				Http::Response response(request, 303);
				response.headers["Location"] = redirect;
				response.send();
				response.sock->close();
				
				if(shutdown)
				{
					LogInfo("User::http", "Shutdown");
					exit(0);
				}

				return;
			}
			
			Http::Response response(request,200);
			response.send();
			
			Html page(response.sock);
			page.header(APPNAME, true);

			// TODO: This is awful
			page.javascript("$('#page').css('max-width','100%');");
			
#if defined(WINDOWS)
                        if(request.sock->getRemoteAddress().isLocal() && Config::IsUpdateAvailable())
                        {
                                page.open("div", "updateavailable.banner");
				page.openForm(prefix+'/', "post", "shutdownAndUpdateForm");
				page.input("hidden", "token", generateToken("admin"));
                        	page.input("hidden", "command", "update");
				page.text("New version available - ");
                                page.link("#", "Update now", "shutdownAndUpdateLink");
				page.closeForm();
				page.javascript("$('#shutdownAndUpdateLink').click(function(event) {\n\
					event.preventDefault();\n\
					document.shutdownAndUpdateForm.submit();\n\
				});");
                                page.close("div");
                        }
#endif
		
#if defined(MACOSX)
                        if(request.sock->getRemoteAddress().isLocal() && Config::IsUpdateAvailable())
                        {
                                page.open("div", "updateavailable.banner");
				page.openForm(prefix+'/', "post", "shutdownAndUpdateForm");
				page.input("hidden", "token", generateToken("admin"));
                        	page.input("hidden", "command", "shutdown");
                        	page.input("hidden", "redirect", String(SECUREDOWNLOADURL) + "?release=osx&update=1");
				page.text("New version available - ");
                                page.link(String(SECUREDOWNLOADURL) + "?release=osx&update=1", "Quit and download now", "shutdownAndUpdateLink");
				page.closeForm();
				page.javascript("$('#shutdownAndUpdateLink').click(function(event) {\n\
					event.preventDefault();\n\
					document.shutdownAndUpdateForm.submit();\n\
				});");
                                page.close("div");
                        }
#endif
		
			page.open("div", "wrapper");
			
			page.open("div","leftcolumn");

			page.open("div", "logo");
			page.openLink("/"); page.image("/logo.png", APPNAME); page.closeLink();
			page.close("div");

			page.open("div","search");
			page.openForm(prefix + "/search", "post", "searchForm");
			page.link(prefix+"/browse/", "Browse", ".button");
			page.input("text","query", "Search for files...");
			//page.button("search","Search");
			page.closeForm();
			//page.javascript("$(document).ready(function() { document.searchForm.query.focus(); });");	// really annoying with touchscreens
			page.javascript("$(document).ready(function() { document.searchForm.query.style.color = 'grey'; });");
			//page.br();
			page.close("div");

			
			page.open("div","contacts.box");
			
			page.link(prefix+"/contacts/","Edit",".button");
		
			page.open("h2");
			page.text("Contacts");
			page.close("h2");
	
			AddressBook::Contact *self = mAddressBook->getSelf();
			Array<AddressBook::Contact*> contacts;
			mAddressBook->getContacts(contacts);
			
			if(contacts.empty() && !self) page.link(prefix+"/contacts/","Add contact / Accept request");
			else {
				page.open("div", "contactsTable");
				page.open("p"); page.text("Loading..."); page.close("p");
				page.close("div");
				
				unsigned refreshPeriod = 5000;
				page.javascript("displayContacts('"+prefix+"/contacts/?json"+"','"+String::number(refreshPeriod)+"','#contactsTable')");
			}
			
			page.close("div");

			page.open("div","files.box");
			
			Array<String> directories;
			mStore->getDirectories(directories);
			
			Array<String> globalDirectories;
			Store::GlobalInstance->getDirectories(globalDirectories);
			directories.append(globalDirectories);
			
			page.link(prefix+"/files/","Edit",".button");
			if(!directories.empty()) page.link(prefix+"/files/?action=refresh&redirect="+String(prefix+url).urlEncode(), "Refresh", "refreshfiles.button");
			
			page.open("h2");
			page.text("Shared folders");
			page.close("h2");
			
			if(directories.empty()) page.link(prefix+"/files/","Add shared folder");
			else {
				page.open("div",".files");
				for(int i=0; i<directories.size(); ++i)
				{	
					const String &directory = directories[i];
					String directoryUrl = prefix + "/files/" + directory + "/";

					page.open("div", ".filestr");
					
					page.span("", ".icon");
					page.image("/dir.png");
					
					page.span("", ".filename");
					page.link(directoryUrl, directory);
					
					page.close("div");
				}
				page.close("div");
			}
			page.close("div");
			
			page.close("div");

// End of leftcolumn

			page.open("div", "rightcolumn");

			page.open("div", "rightheader");
			page.link("/", "Change account", ".button");
			page.open("h1");
			const String instance = Core::Instance->getName().before('.');
			page.openLink(profile()->urlPrefix());
			page.image(profile()->avatarUrl(), "", ".avatar");	// NO alt text for avatars
			page.text(name() + "@" + tracker());
			if(addressBook()->getSelf() && !instance.empty()) page.text(" (" + instance + ")");
			page.closeLink();
			page.close("h1");
			page.close("div");
			
			String broadcastUrl = "/messages";
			
			page.open("div", "statuspanel");
			page.raw("<a class=\"button\" href=\"#\" onclick=\"createFileSelector('/"+name()+"/myself/files/?json', '#fileSelector', 'input.attachment', 'input.attachmentname','"+generateToken("directory")+"'); return false;\"><img src=\"/paperclip.png\" alt=\"File\"></a>");
			page.openForm("#", "post", "statusform");
			page.input("hidden", "attachment");
			page.input("hidden", "attachmentname");
			page.textarea("statusinput");
			//page.button("send","Send");
			//page.br();
			page.closeForm();
			page.div("",".attachedfile");
			page.close("div");

			page.div("", "fileSelector");

			page.open("div", "newsfeed.box");

			page.open("div", "optionsnewsfeed");

			StringMap optionsCount;
			optionsCount["&count=15"] << "Last 15";
			optionsCount["&count=30"] << "Last 30";
			optionsCount[""] << "All";
			page.raw("<span class=\"customselect\">");
			page.select("listCount", optionsCount, "&count=15");
			page.raw("</span>");

			StringMap optionsIncoming;
			optionsIncoming["0"] << "Mine & others";
			optionsIncoming["1"] << "Others only";
			page.raw("<span class=\"customselect\">");
			page.select("listIncoming", optionsIncoming, "0");
			page.raw("</span>");

			page.close("div");

			page.open("h2");
			page.text("Public messages");
			page.close("h2");
			
			page.open("div", "statusmessages");
			page.open("p"); page.text("No public messages yet !"); page.close("p");
			page.close("div");

			page.close("div");
		 
			page.javascript("var TokenMessage = '"+generateToken("message")+"';\n\
					var TokenDirectory = '"+generateToken("directory")+"';\n\
					function postStatus() {\n\
					var message = $(document.statusform.statusinput).val();\n\
					var attachment = $(document.statusform.attachment).val();\n\
					if(!message) return false;\n\
					var fields = {};\n\
					fields['message'] = message;\n\
					fields['public'] = 1;\n\
					fields['token'] = '"+generateToken("message")+"';\n\
					if(attachment) fields['attachment'] = attachment;\n\
					var request = $.post('"+prefix+broadcastUrl+"/"+"', fields);\n\
					request.fail(function(jqXHR, textStatus) {\n\
						alert('The message could not be sent.');\n\
					});\n\
					$(document.statusform.statusinput).val('');\n\
					$(document.statusform.attachment).val('');\n\
					$(document.statusform.attachmentname).val('');\n\
					$('#statuspanel .attachedfile').hide();\n\
				}\n\
				$(document.statusform.attachment).change(function() {\n\
					$('#statuspanel .attachedfile').html('');\n\
					$('#statuspanel .attachedfile').hide();\n\
					var filename = $(document.statusform.attachmentname).val();\n\
					if(filename != '') {\n\
						$('#statuspanel .attachedfile')\n\
							.append('<img class=\"icon\" src=\"/file.png\">')\n\
							.append('<span class=\"filename\">'+filename+'</span>')\n\
							.show();\n\
					}\n\
					var input = $(document.statusform.statusinput);\n\
					input.focus();\n\
					if(input.val() == '') {\n\
						input.val(filename).select();\n\
					}\n\
				});\n\
				document.statusform.onsubmit = function() {\n\
					postStatus();\n\
					return false;\n\
				}\n\
				$(document).ready(function() {\n\
					document.statusform.statusinput.value = 'Click here to post a public message for all your contacts';\n\
					document.statusform.statusinput.style.color = 'grey';\n\
				});\n\
				document.statusform.statusinput.onblur = function() {\n\
					if(document.statusform.statusinput.value == '') {\n\
						document.statusform.statusinput.value = 'Click here to post a public message for all your contacts';\n\
						document.statusform.statusinput.style.color = 'grey';\n\
					}\n\
				}\n\
				document.statusform.statusinput.onfocus = function() {\n\
					if(document.statusform.statusinput.style.color != 'black') {\n\
						document.statusform.statusinput.value = '';\n\
						document.statusform.statusinput.style.color = 'black';\n\
					}\n\
				}\n\
				document.searchForm.query.onfocus = function() {\n\
					document.searchForm.query.value = '';\n\
					document.searchForm.query.style.color = 'black';\n\
				}\n\
				document.searchForm.query.onblur = function() {\n\
					document.searchForm.query.style.color = 'grey';\n\
					document.searchForm.query.value = 'Search for files...';\n\
				}\n\
				$('textarea.statusinput').keypress(function(e) {\n\
					if (e.keyCode == 13 && !e.shiftKey) {\n\
						e.preventDefault();\n\
						postStatus();\n\
					}\n\
				});\n\
				var listCount = document.getElementsByName(\"listCount\")[0];\n\
				listCount.addEventListener('change', function() {\n\
					updateMessagesReceiver('"+prefix+broadcastUrl+"/?json&public=1"+"&incoming='+listIncoming.value.toString()+'"+"'+listCount.value.toString(),'#statusmessages');\n\
				}, true);\n\
				\n\
				var listIncoming = document.getElementsByName(\"listIncoming\")[0];\n\
				listIncoming.addEventListener('change', function() {\n\
					updateMessagesReceiver('"+prefix+broadcastUrl+"/?json&public=1"+"&incoming='+listIncoming.value.toString()+'"+"'+listCount.value.toString(),'#statusmessages');\n\
				}, true);\n\
				setMessagesReceiver('"+prefix+broadcastUrl+"/?json&public=1&incoming=0"+"'+listCount.value.toString(),'#statusmessages');\n\
				$('#newsfeed').on('keypress','textarea', function (e) {\n\
					if (e.keyCode == 13 && !e.shiftKey) {\n\
						$(this).closest('form').submit();\n\
						return false; \n\
					}\n\
				});\n\
				//$('#newsfeed').on('blur','.reply', function (e) {\n\
				//	$(this).hide();\n\
				//});\n\
				$('.attachedfile').hide();\n\
			");
			
			page.open("div", "footer");
			page.text(String("Version ") + APPVERSION + " - ");
			page.link(HELPLINK, "Help", "", true);
			page.text(" - ");
			page.link(SOURCELINK, "Source code", "", true);
			page.text(" - ");
			page.link(BUGSLINK, "Report a bug", "", true);
			page.close("div");

			page.close("div");
			page.close("div");
			
			page.footer();
			return;
		}
		
		String directory = url;
		directory.ignore();		// remove first '/'
		url = "/" + directory.cut('/');
		if(directory.empty()) throw 404;
		
		if(directory == "browse")
		{
			String target(url);
			Assert(!target.empty());
			
			if(request.get.contains("json") || request.get.contains("playlist"))
			{
				// Query resources
				Resource::Query query(store(), target);
				query.setFromSelf(true);
				
				SerializableSet<Resource> resources;
				bool success = query.submitLocal(resources);
				success|= query.submitRemote(resources, Identifier::Null);
				if(!success) throw 404;
				
				if(request.get.contains("json"))
				{
					Http::Response response(request, 200);
					response.headers["Content-Type"] = "application/json";
					response.send();
					JsonSerializer json(response.sock);
					json.output(resources);
				}
				else {
					Http::Response response(request, 200);
					response.headers["Content-Disposition"] = "attachment; filename=\"playlist.m3u\"";
					response.headers["Content-Type"] = "audio/x-mpegurl";
					response.send();
					
					String host;
					request.headers.get("Host", host);
					Resource::CreatePlaylist(resources, response.sock, host);
				}
				return;
			}
			
			Http::Response response(request, 200);
			response.send();
			
			Html page(response.sock);
			if(target == "/") page.header("Browse files");
			else page.header("Browse files: "+target.substr(1));
			page.open("div","topmenu");
			page.link(prefix+"/search/","Search files",".button");
			page.link(prefix+request.url+"?playlist","Play all","playall.button");
			page.close("div");

			page.div("","list.box");
			page.javascript("listDirectory('"+prefix+request.url+"?json','#list',true,false);");
			page.footer();
			return;
		}
		else if(directory == "search")
		{
			if(url != "/") throw 404;
			
			String match;
			if(!request.post.get("query", match))
				request.get.get("query", match);
			match.trim();
			
			if(request.get.contains("json") || request.get.contains("playlist"))
			{
				if(match.empty()) throw 400;
				
				Resource::Query query(store());
				query.setMatch(match);
				
				SerializableSet<Resource> resources;
				if(!query.submit(resources))
					throw 404;

				if(request.get.contains("json"))
				{
					Http::Response response(request, 200);
					response.headers["Content-Type"] = "application/json";
					response.send();
					JsonSerializer json(response.sock);
					json.output(resources);
				}
				else {
					Http::Response response(request, 200);
					response.headers["Content-Disposition"] = "attachment; filename=\"playlist.m3u\"";
					response.headers["Content-Type"] = "audio/x-mpegurl";
					response.send();
					
					String host;
					request.headers.get("Host", host);
					Resource::CreatePlaylist(resources, response.sock, host);
				}
				return;
			}
			
			Http::Response response(request, 200);
			response.send();
				
			Html page(response.sock);
			
			if(match.empty()) page.header("Search");
			else page.header(String("Searching ") + match);
			
			page.open("div","topmenu");
			page.openForm(prefix + "/search", "post", "searchform");
			page.input("text","query", match);
			page.button("search","Search");
			page.closeForm();
			page.javascript("$(document).ready(function() { document.searchForm.query.focus(); });");
			if(!match.empty()) page.link(prefix+request.url+"?query="+match.urlEncode()+"&playlist","Play all",".button");
			page.close("div");
			
			unsigned refreshPeriod = 5000;
			page.javascript("setCallback(\""+prefix+"/?json\", "+String::number(refreshPeriod)+", function(info) {\n\
				transition($('#status'), info.status.capitalize());\n\
				$('#status').removeClass().addClass('button').addClass(info.status);\n\
				if(info.newmessages) playMessageSound();\n\
			});");
			
			if(!match.empty())
			{
				page.div("", "#list.box");
				page.javascript("listDirectory('"+prefix+request.url+"?query="+match.urlEncode()+"&json','#list',true,false);");
				page.footer();
			}
			return;
		}
		else if(directory == "avatar" || request.url == "/myself/avatar")
		{
			Http::Response response(request, 303);	// See other
			response.headers["Location"] = profile()->avatarUrl(); 
			response.send();
			return;
		}
		else if(directory == "myself")
		{
			Http::Response response(request, 303);	// See other
			response.headers["Location"] = prefix + "/files/";
			response.send();
			return;
		}
	}
	catch(const Exception &e)
	{
		LogWarn("User::http", e.what());
		throw 404;	// Httpd handles integer exceptions
	}
			
	throw 404;
}

}
