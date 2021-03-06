/*************************************************************************
 *   Copyright (C) 2011-2017 by Paul-Louis Ageneau                       *
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

#include "tpn/indexer.hpp"
#include "tpn/user.hpp"
#include "tpn/config.hpp"
#include "tpn/html.hpp"
#include "tpn/request.hpp"
#include "tpn/cache.hpp"
#include "tpn/addressbook.hpp"

#include "pla/crypto.hpp"
#include "pla/random.hpp"
#include "pla/file.hpp"
#include "pla/directory.hpp"
#include "pla/jsonserializer.hpp"
#include "pla/binaryserializer.hpp"
#include "pla/object.hpp"
#include "pla/time.hpp"
#include "pla/mime.hpp"

namespace tpn
{

const String Indexer::UploadDirectoryName = "_upload";

Indexer::Indexer(User *user) :
	Publisher(Network::Link(user->identifier(), Identifier::Empty)),
	mUser(user),
	mRunning(false)
{
	Assert(mUser);
	mDatabase = new Database(mUser->profilePath() + "files.db");

	// Files database
	mDatabase->execute("CREATE TABLE IF NOT EXISTS resources\
		(id INTEGER PRIMARY KEY AUTOINCREMENT,\
		name_rowid INTEGER,\
		path TEXT,\
		digest BLOB,\
		time INTEGER(8),\
		seen INTEGER(1))");
	mDatabase->execute("CREATE UNIQUE INDEX IF NOT EXISTS path ON resources (path)");
	mDatabase->execute("CREATE INDEX IF NOT EXISTS digest ON resources (digest)");
	mDatabase->execute("CREATE VIRTUAL TABLE IF NOT EXISTS names USING FTS3(name)");

	// Fix: "IF NOT EXISTS" is not available for virtual tables with old sqlite3 versions
	//Database::Statement statement = mDatabase->prepare("select DISTINCT tbl_name from sqlite_master where tbl_name = 'names'");
	//if(!statement.step()) mDatabase->execute("CREATE VIRTUAL TABLE names USING FTS3(name)");
	//statement.finalize();
	//

	mFileName = mUser->profilePath() + "directories";

	const String sharedDirectory = Config::Get("shared_dir");
	if(!Directory::Exist(sharedDirectory))
		Directory::Create(sharedDirectory);

	mBaseDirectory = sharedDirectory + Directory::Separator + mUser->name();
	if(!Directory::Exist(mBaseDirectory))
		Directory::Create(mBaseDirectory);

	if(File::Exist(mFileName))
	{
		try {
			File file(mFileName, File::Read);
			JsonSerializer serializer(&file);
			serializer >> mDirectories;
			file.close();
		}
		catch(const Exception &e)
		{
			LogWarn("Indexer", String("Unable to load directories: ") + e.what());
		}

		Map<String,Entry>::iterator it = mDirectories.begin();
		while(it != mDirectories.end())
		{
			String &path = it->second.path;
			if(path.empty() || path == "/")
			{
				mDirectories.erase(it++);
				continue;
			}

			if(path[path.size()-1] == '/')
				path.resize(path.size()-1);

			++it;
		}
	}

	// Special upload directory
	addDirectory(UploadDirectoryName, "", Resource::Personal, true);	// don't commit

	// Publisher
	publish(prefix());
	publish("/files");

	// Interface
	Interface::Instance->add(mUser->urlPrefix()+"/files", this);
	Interface::Instance->add(mUser->urlPrefix()+"/explore", this);

	// Save and run
	save();
	start(seconds(60.));
}

Indexer::~Indexer(void)
{
	unpublish(prefix());

	Interface::Instance->remove(mUser->urlPrefix()+"/files");
	Interface::Instance->remove(mUser->urlPrefix()+"/explore");
}
User *Indexer::user(void) const
{
	return mUser;
}

String Indexer::userName(void) const
{
	return mUser->name();
}

String Indexer::prefix(void) const
{
	return "/files/" + mUser->identifier().toString();
}

void Indexer::addDirectory(const String &name, String path, Resource::AccessLevel access, bool nocommit)
{
	Assert(!name.empty());
	Assert(!name.contains('/') && !name.contains('\\'));

	{
		std::unique_lock<std::mutex> lock(mMutex);
		if(path.empty())
		{
			String dirname = name;
			dirname.replace(' ','_');
			path = mBaseDirectory + Directory::Separator + dirname;
		}

		if(path[path.size()-1] == Directory::Separator) path.ignore(1);

		if(!Directory::Exist(path))
			Directory::Create(path);

		Directory test(path);
		test.close();

		auto it = mDirectories.find(name);
		if(it != mDirectories.end())
		{
			it->second.path = path;
			it->second.access = access;
		}
		else {
			mDirectories.insert(name, Entry(path, access));
		}
	}

	if(!nocommit)
	{
		save();
		start();
	}
}

void Indexer::removeDirectory(const String &name, bool nocommit)
{
	{
		std::unique_lock<std::mutex> lock(mMutex);
		if(mDirectories.contains(name))
			mDirectories.erase(name);
	}

	if(!nocommit)
	{
		save();
		start();
	}
}

void Indexer::getDirectories(Array<String> &array) const
{
	{
		std::unique_lock<std::mutex> lock(mMutex);
		mDirectories.getKeys(array);
	}

	array.remove(UploadDirectoryName);
}

Resource::AccessLevel Indexer::directoryAccessLevel(const String &name) const
{
	Map<String, Entry>::const_iterator it = mDirectories.find(name);
	if(it == mDirectories.end()) throw Exception("Unknown directory: " + name);
	return it->second.access;
}

void Indexer::save(void) const
{
  	std::unique_lock<std::mutex> lock(mMutex);

	File file(mFileName, File::Write);
	JsonSerializer serializer(&file);
	serializer << mDirectories;
	file.close();
}

void Indexer::start(duration delay)
{
	// TODO: do not reschedule if longer delay
	mRunAlarm.schedule(Alarm::clock::now() + delay, [this]()
	{
		run();
	});
}

bool Indexer::query(const Query &q, List<BinaryString> &targets)
{
	targets.clear();

	// Special case for different access levels on root
	if(q.mPath == "/" && q.mAccess != Resource::Public)
	{
		if(q.mOffset > 0)
			return false;

		String tempFileName = File::TempName();
		File tempFile(tempFileName, File::Truncate);

		BinarySerializer serializer(&tempFile);
		Array<String> names;

		{
			std::unique_lock<std::mutex> lock(mMutex);
			mDirectories.getKeys(names);
		}

		for(int i=0; i<names.size(); ++i)
		{
			String name = names[i];
			String subPath = "/" + name;

			if(pathAccessLevel(subPath) > q.mAccess)
				continue;

			Resource subResource;
			Time time(0);
			if(!get(subPath, subResource, &time))
				continue;

			serializer << subResource.getDirectoryRecord(time);
		}

		tempFile.close();

		Resource resource;
		resource.cache(tempFileName, "/", "directory");
		targets.push_back(resource.digest());
		return true;
	}

	Database::Statement statement;
	if(prepareQuery(statement, q, "path, digest"))
	{
		while(statement.step())
		{
			String path;
			statement >> path;

			BinaryString digest;
			statement >> digest;

			if(pathAccessLevel(path) > q.mAccess)
				continue;

			targets.push_back(digest);
		}

		statement.finalize();
		return !targets.empty();
	}

	return false;
}

bool Indexer::query(const Query &q, Set<Resource> &resources)
{
	resources.clear();

	List<BinaryString> targets;
	query(q, targets);

	for(List<BinaryString>::iterator it = targets.begin();
		it != targets.end();
		++it)
	{
		resources.insert(Resource(*it));
	}

	return !resources.empty();
}

bool Indexer::query(const Query &q, Resource &resource)
{
	List<BinaryString> targets;
	query(q, targets);

	if(!targets.empty())
	{
		resource.fetch(*targets.begin());
		return true;
	}

	return false;
}

bool Indexer::process(String path, Resource &resource)
{
	// Sanitize path
	if(!path.empty() && path[path.size() - 1] == Directory::Separator)
		path.resize(path.size() - 1);
	if(path.empty()) path = "/";

	// Get name
	String name;
	if(path != "/") name = path.afterLast(Directory::Separator);
	else name = "/";

	// Don't process garbage files
	if(name == ".directory"
		|| name.toLower() == "thumbs.db"
		|| name.substr(0,7) == ".Trash-")
		return false;

	String realPath = this->realPath(path);
	Time   fileTime = File::Time(realPath);

	// This should be a background process, so sleep for a bit
	std::this_thread::sleep_for(milliseconds(100));

	// Recursively process if it's a directory
	bool isDirectory = false;
	if(path == "/")	// Top-level: Indexer directories
	{
		isDirectory = true;

		String tempFileName = File::TempName();
		File tempFile(tempFileName, File::Truncate);

		BinarySerializer serializer(&tempFile);
		Array<String> names;

		{
			std::unique_lock<std::mutex> lock(mMutex);
			mDirectories.getKeys(names);
		}

		// Iterate on directories
		for(int i=0; i<names.size(); ++i)
		try {
			String name = names[i];
			String subPath = "/" + name;
			String realSubPath = this->realPath(subPath);

			if(!Directory::Exist(realSubPath))
				Directory::Create(realSubPath);

			if(pathAccessLevel(subPath) != Resource::Public)
				continue;	// put only public directories in root

			Resource subResource;
			if(!process(subPath, subResource))
				continue;	// ignore this directory

			Time time = File::Time(realSubPath);
			fileTime = std::max(fileTime, time);

			serializer << subResource.getDirectoryRecord(time);
		}
		catch(const Exception &e)
		{
			LogWarn("Indexer::process", String("Indexing failed for directory ") + names[i] + ": " + e.what());
		}

		tempFile.close();
		realPath = Cache::Instance->move(tempFileName);
	}
	else if(Directory::Exist(realPath))
	{
		isDirectory = true;

		String tempFileName = File::TempName();
		File tempFile(tempFileName, File::Truncate);

		// Iterate on files and order them by name
		StringMap sorted;
		Directory dir(realPath);
		while(dir.nextFile())
		{
			String key = String(dir.fileIsDir() ? "0" : "1") + dir.fileName().toLower();
			sorted.insert(key, dir.fileName());
		}

		// Process ordered files
		BinarySerializer serializer(&tempFile);
		for(auto it = sorted.begin(); it != sorted.end(); ++it)
		{
			String subPath = path + '/' + it->second;
			String realSubPath = this->realPath(subPath);

			Resource subResource;
			if(!process(subPath, subResource))
				continue;	// ignore this file

			Time time = File::Time(realSubPath);
			Resource::DirectoryRecord record;
			*static_cast<Resource::MetaRecord*>(&record) = *static_cast<Resource::MetaRecord*>(subResource.mIndexRecord.get());
			record.digest = subResource.digest();
			record.time = time;
			serializer << record;

			fileTime = std::max(fileTime, time);
		}

		tempFile.close();
		realPath = Cache::Instance->move(tempFileName);
	}
	else {
		if(!File::Exist(realPath))
		{
			LogWarn("Indexer::process", String("Indexing failed: File does not exist: ") + realPath);
			return false;
		}
	}

	Time time(0);
	if(!get(path, resource, &time) || time < fileTime || path == "/")
	{
		LogInfo("Indexer::process", "Processing: " + path);

		resource.process(realPath, name, (isDirectory ? "directory" : "file"));
		notify(path, resource, fileTime);

		//LogDebug("Indexer::process", "Processed: digest is " + resource.digest().toString());
	}

	// Publish into DHT right now
	// Store will publish the blocks anyway
	Network::Instance->storeValue(resource.digest(), Network::Instance->overlay()->localNode());

	// Mark as seen
	Database::Statement statement = mDatabase->prepare("UPDATE resources SET seen=1 WHERE path=?1");
	statement.bind(1, path);
	statement.execute();

	return true;
}

bool Indexer::get(String path, Resource &resource, Time *time)
{
	// Sanitize path
	if(!path.empty() && path[path.size() - 1] == Directory::Separator)
		path.resize(path.size() - 1);
	if(path.empty()) path = "/";

	Database::Statement statement = mDatabase->prepare("SELECT digest, time FROM resources WHERE path = ?1 LIMIT 1");
	statement.bind(1, path);
	if(statement.step())
	{
		//LogDebug("Indexer::get", "Found in index: " + path);

		BinaryString digest;
		statement.value(0, digest);
		if(time) statement.value(1, *time);
		statement.finalize();

		try {
			resource.fetch(digest, true);	// local only
			return resource.isLocallyAvailable();
		}
		catch(const Exception &e)
		{
			// Should not happen, cache is bogus
			LogWarn("Indexer::get", e.what());
			return false;
		}
	}

	//LogDebug("Indexer::get", "Not found in index: " + path);
	statement.finalize();
	return false;
}

void Indexer::notify(String path, const Resource &resource, const Time &time)
{
	// Sanitize path
	if(!path.empty() && path[path.size() - 1] == Directory::Separator)
		path.resize(path.size() - 1);
	if(path.empty()) path = "/";

	String name;
	if(path == "/") name = "/";
	else name = path.afterLast('/');

	//LogDebug("Indexer::notify", "Notified: " + path);
	Assert(!name.empty());

	Database::Statement statement = mDatabase->prepare("INSERT OR IGNORE INTO names (name) VALUES (?1)");
	statement.bind(1, name);
	statement.execute();

	statement = mDatabase->prepare("INSERT OR REPLACE INTO resources (name_rowid, path, digest, time, seen) VALUES ((SELECT rowid FROM names WHERE name = ?1 LIMIT 1), ?2, ?3, ?4, 1)");
	statement.bind(1, name);
	statement.bind(2, path);
	statement.bind(3, resource.digest());
	statement.bind(4, time);
	statement.execute();

	// Resource has changed, re-publish it
	publish(prefix(), path);
}

bool Indexer::anounce(const Network::Link &link, const String &prefix, const String &path, List<BinaryString> &targets)
{
	// Not synchronized

	String cpath(path);
	String match = cpath.cut('?');

	Query q;
	q.setPath(cpath);
	q.setMatch(match);
	q.setAccessLevel((!link.remote.empty() && mUser->addressBook()->hasIdentifier(link.remote) ? Resource::Private : Resource::Public));
	q.setFromSelf(!link.remote.empty() && link.remote == mUser->identifier());

	return query(q, targets);
}

void Indexer::http(const String &prefix, Http::Request &request)
{
	// Not synchronized
	const String &url = request.url;
	if(mUser) mUser->setOnline();

	StringMap accessSelectMap;
	accessSelectMap["public"] = "Everyone";
	accessSelectMap["private"] = "Only contacts";
	accessSelectMap["personal"] = "Only me";

	try {
		if(prefix.afterLast('/') == "explore")
		{
			if(url != "/") throw 404;

			if(!Config::Get("user_global_shares").toBool())
				throw 404;

			String path;
			if(!request.get.get("path", path))
			{
#ifdef WINDOWS
				DWORD disks = GetLogicalDrives();
				if(!disks) throw 500;

				Http::Response response(request,200);
				response.send();

				Html page(response.stream);
				page.header(path);

				page.open("div",".box");
				page.open("table",".files");
				for(int i=0; i<25; ++i)
				{
					if(disks & (0x1 << i))
					{
						char letter = 0x41+i;
						String name = String(letter) + ":\\";
						String hrName = String(letter) + String(" drive");
						String link = prefix + "/?path=" + name.urlEncode();

						page.open("tr");
						page.open("td",".icon");
						page.image("/static/dir.png");
						page.close("td");
						page.open("td",".filename");
						page.link(link, hrName);
						page.close("td");
						page.open("td",".add");
						page.link(link+"&add=1", "share");
						page.close("td");
						page.close("tr");
					}
				}
				page.close("table");
				page.close("div");
				page.footer();
				return;
#else
				path = "/";
#endif
			}

			if(path[path.size()-1] != Directory::Separator)
				path+= Directory::Separator;

			if(!Directory::Exist(path)) throw 404;

			if(request.get.contains("add"))
			{
				path.resize(path.size()-1);
				String name = path.afterLast(Directory::Separator);
				name.remove(':');

				if(request.method == "POST")
				{
					if(!user()->checkToken(request.post["token"], "directory_add"))
						throw 403;

					String access;
					request.post.get("name", name);
					request.post.get("access", access);

					Resource::AccessLevel accessLevel;
					if(access == "personal") accessLevel = Resource::Personal;
					else if(access == "private") accessLevel = Resource::Private;
					else accessLevel = Resource::Public;

					try {
						if(name.empty()
							|| name.contains('/') || name.contains('\\')
							|| name.find("..") != String::NotFound)
								throw Exception("Invalid directory name");

						addDirectory(name, path, accessLevel);
					}
					catch(const Exception &e)
					{
						Http::Response response(request,200);
						response.send();

						Html page(response.stream);
						page.header("Error", false, prefix + url);
						page.text(e.what());
						page.footer();
						return;
					}

					Http::Response response(request,303);
					response.headers["Location"] = mUser->urlPrefix()+"/files/";
					response.send();
					return;
				}

				Http::Response response(request,200);
				response.send();

				Html page(response.stream);
				page.header("Add directory");
				page.openForm(prefix + url + "?path=" + path.urlEncode() + "&add=1", "post");
				page.input("hidden", "token", user()->generateToken("directory_add"));
				page.openFieldset("Add directory");
				page.label("", "Path"); page.text(path + Directory::Separator); page.br();
				page.label("name","Name"); page.input("text","name", name); page.br();
				page.label("access","Access"); page.select("access", accessSelectMap, "public"); page.br();
				page.label("add"); page.button("add","Add directory");
				page.closeFieldset();
				page.closeForm();
				page.footer();
				return;
			}

			Directory dir;
			try {
				dir.open(path);
			}
			catch(...)
			{
				throw 401;
			}

			Set<String> folders;
			while(dir.nextFile())
				if(dir.fileIsDir() && dir.fileName().at(0) != '.')
					folders.insert(dir.fileName());

			Http::Response response(request, 200);
			response.send();

			Html page(response.stream);
			page.header("Share directory");

			page.open("div",".box");
			if(folders.empty()) page.text("No subdirectories");
			else {
				std::set<String> existingPathsSet;
				for(auto it = mDirectories.begin(); it != mDirectories.end(); ++it)
				{
					existingPathsSet.insert(it->second.path);
				}

				page.open("table",".files");
				for(auto it = folders.begin(); it != folders.end(); ++it)
				{
					const String &name = *it;
					String childPath = path + name;
					String link = prefix + "/?path=" + childPath.urlEncode();

					page.open("tr");
					page.open("td",".icon");
					page.image("/static/dir.png");
					page.close("td");

					if(existingPathsSet.find(childPath) == existingPathsSet.end())
					{
						page.open("td",".filename");
						page.link(link, name);
						page.close("td");
						page.open("td",".add");
						page.link(link+"&add=1", "share");
						page.close("td");
					}
					else {
						page.open("td",".filename");
						page.text(name);
						page.close("td");
						page.open("td",".add");
						page.close("td");
					}

					page.close("tr");
				}
				page.close("table");
			}
			page.close("div");
			page.footer();
			return;

		} // prefix == "explore"

		if(request.method != "POST" && (request.get.contains("json")  || request.get.contains("playlist")))
		{
			Query q(url);
			Resource resource;
			if(!query(q, resource)) throw 404;

			Request req(resource);
			req.http(req.urlPrefix(), request);
			return;
		}

		// TODO: the following code should use JSON

		if(url == "/")
		{
			if(request.method == "POST")
			{
				if(!user()->checkToken(request.post["token"], "directory"))
					throw 403;

				String command = request.post["command"];
				if(command == "delete")
				{
					String name = request.post["argument"];
					if(!name.empty() && name != UploadDirectoryName)
					{
						removeDirectory(name);
						// TODO: delete files recursively
					}
				}
				else if(request.post.contains("name"))
				{
					String name = request.post["name"];
					String access = request.post["access"];

					Resource::AccessLevel accessLevel;
					if(access == "personal") accessLevel = Resource::Personal;
					else if(access == "private") accessLevel = Resource::Private;
					else accessLevel = Resource::Public;

					try {
						if(name.empty()
							|| name.contains('/') || name.contains('\\')
							|| name.find("..") != String::NotFound)
								throw Exception("Invalid directory name");

						addDirectory(name, "", accessLevel);
					}
					catch(const Exception &e)
					{
						Http::Response response(request,200);
						response.send();

						Html page(response.stream);
						page.header("Error", false, prefix + "/");
						page.text(e.what());
						page.footer();
						return;
					}
				}
				else throw 400;

				Http::Response response(request,303);
				response.headers["Location"] = prefix + "/";
				response.send();
				return;
			}

			String action;
			if(request.get.get("action", action))
			{
				String redirect = prefix + url;
				request.get.get("redirect", redirect);

				if(action == "refresh")
				{
					start();

					Http::Response response(request, 200);
					response.send();

					Html page(response.stream);
					page.header("Refreshing in background...", true, redirect);
					page.open("div", "notification");
					page.openLink("/");
					page.image("/static/refresh.png", "Refresh");
					page.closeLink();
					page.br();
					page.open("h1",".huge");
					page.text("Refreshing in background...");
					page.close("h1");
					page.close("div");
					page.footer();
					return;
				}

				Http::Response response(request,303);
				response.headers["Location"] = redirect;
				response.send();
				return;
			}

			Http::Response response(request,200);
			response.send();

			Html page(response.stream);
			page.header("Shared folders");

			Array<String> directories;
			getDirectories(directories);
			directories.prepend(UploadDirectoryName);

			page.open("div",".box");
			page.open("table",".files");

			for(int i=0; i<directories.size(); ++i)
			{
				String name;
				if(directories[i] == UploadDirectoryName) name = "Sent files";
				else name = directories[i];

				page.open("tr");
				page.open("td",".icon");
				page.image("/static/dir.png");
				page.close("td");
				page.open("td",".access");
				page.text("(");
				//if(isHiddenUrl(directories[i])) page.text("Invisible");
				//else {
					Resource::AccessLevel accessLevel = directoryAccessLevel(directories[i]);
					if(accessLevel == Resource::Personal) page.text("Personal");
					else if(accessLevel == Resource::Private) page.text("Private");
					else page.text("Public");
				//}
				page.text(")");
				page.close("td");
				page.open("td",".filename");
				page.link(directories[i], name);
				page.close("td");


				page.open("td",".actions");
				if(directories[i] != UploadDirectoryName)
				{
					page.openLink("#", ".deletelink");
					page.image("/static/delete.png", "Delete");
					page.closeLink();
				}
				page.close("td");
				page.close("tr");
			}

			page.close("table");
			page.close("div");

			page.openForm(prefix+url, "post", "executeForm");
			page.input("hidden", "token", user()->generateToken("directory"));
			page.input("hidden", "command");
			page.input("hidden", "argument");
			page.closeForm();

			page.javascript("function deleteDirectory(name) {\n\
				if(confirm('Do you really want to delete the shared directory '+name+' ?')) {\n\
					document.executeForm.command.value = 'delete';\n\
					document.executeForm.argument.value = name;\n\
					document.executeForm.submit();\n\
				}\n\
			}");

			page.javascript("$('.deletelink').css('cursor', 'pointer').click(function(event) {\n\
				var fileName = $(this).closest('tr').find('td.filename a').text();\n\
				deleteDirectory(fileName);\n\
				return false;\n\
			});");

			page.openForm(prefix+"/","post");
			page.input("hidden", "token", user()->generateToken("directory"));
			page.openFieldset("New directory");
			page.label("name","Name"); page.input("text","name"); page.br();
			page.label("access","Access"); page.select("access", accessSelectMap, "public"); page.br();
			page.label("add"); page.button("add","Create directory");
			page.label(""); page.link(mUser->urlPrefix()+"/explore/", "Add existing directory", ".button");
			page.br();
			page.closeFieldset();
			page.closeForm();

			page.footer();
		}
		else {
			String path;
			try {
				path = realPath(url);
			}
			catch(const Exception &e)
			{
				LogWarn("Indexer::http", e.what());
				throw 404;
			}

			if(path[path.size()-1] == Directory::Separator)
				path.resize(path.size()-1);

			if(Directory::Exist(path))
			{
				if(url[url.size()-1] != '/')
				{
					Http::Response response(request, 301);	// Moved Permanently
					response.headers["Location"] = prefix+url+"/";
					response.send();
					return;
				}

				if(request.method == "POST")
				{
					if(!user()->checkToken(request.post["token"], "directory"))
						throw 403;

					String command = request.post["command"];
					if(command == "delete")
					{
						String fileName = request.post["argument"];
						if(!fileName.empty())
						{
							String filePath = path + Directory::Separator + fileName;
							String fileUrl  = url + fileName;

							if(Directory::Exist(filePath))
							{
								if(Directory::Remove(filePath))
								{
									Database::Statement statement = mDatabase->prepare("DELETE FROM resources WHERE path = ?1");
									statement.bind(1, fileUrl);
									statement.execute();
								}
							}
							else if(File::Exist(filePath))
							{
								if(File::Remove(filePath))
								{
									Database::Statement statement = mDatabase->prepare("DELETE FROM resources WHERE path = ?1");
									statement.bind(1, fileUrl);
									statement.execute();
								}
							}

							// Recursively update parent directories
							String tmp = fileUrl.beforeLast('/');
							while(!tmp.empty())
							{
								update(tmp);
								if(!tmp.contains('/')) break;
								tmp = tmp.beforeLast('/');
							}
							update("/");
						}
					}
					else {
						std::set<Resource> resources;
						for(auto it = request.files.begin(); it != request.files.end(); ++it)
						{
							String fileName;
							if(!request.post.get(it->first, fileName)) continue;
							Assert(!fileName.empty());

							if(fileName.contains('/') || fileName.contains('\\')
									|| fileName.find("..") != String::NotFound)
										throw Exception("Invalid file name");

							TempFile *tempFile = it->second;
							tempFile->close();

							String filePath = path + Directory::Separator + fileName;
							String fileUrl  = url + fileName;

							File::Rename(tempFile->name(), filePath);

							LogInfo("Indexer::Http", String("Uploaded: ") + fileName);

							try {
								// Recursively update parent directories
								String tmp = fileUrl;
								while(!tmp.empty())
								{
									update(tmp);
									if(!tmp.contains('/')) break;
									tmp = tmp.beforeLast('/');
								}
								update("/");

								Query q(fileUrl);
								q.setFromSelf(true);
								Resource res;
								if(!query(q, res)) throw Exception("Query failed for " + filePath);

								resources.insert(res);
							}
							catch(const Exception &e)
							{
								LogWarn("Indexer::Http", String("Unable to get resource after upload: ") + e.what());
							}
						}

						if(request.get.contains("json"))
						{
							Http::Response response(request, 200);
							response.headers["Content-Type"] = "application/json";
							response.send();

							JsonSerializer json(response.stream);
							json << resources;
							return;
						}

						start();
					}

					Http::Response response(request,303);
					response.headers["Location"] = prefix+url;
					response.send();
					return;
				}

				Http::Response response(request, 200);
				response.send();

				Html page(response.stream);

				String directory = url.substr(1);
				directory.cut('/');

				String title;
				if(directory == UploadDirectoryName) title = "Sent files";
				else title = "Shared folder: " + request.url.substr(1,request.url.size()-2);

				page.header(title);

				page.openForm(prefix+url,"post", "uploadForm", true);
				page.input("hidden", "token", user()->generateToken("directory"));
				page.openFieldset("Add a file");
				page.label("file"); page.file("file", "Select a file"); page.br();
				page.label("send"); page.button("send","Send");
				page.closeFieldset();
				page.closeForm();
				page.div("","uploadMessage");

				page.javascript("document.uploadForm.send.style.display = 'none';\n\
				$(document.uploadForm.file).change(function() {\n\
					if(document.uploadForm.file.value != '') {\n\
						document.uploadForm.style.display = 'none';\n\
						$('#uploadMessage').html('<div class=\"box\">Uploading the file, please wait...</div>');\n\
						document.uploadForm.submit();\n\
					}\n\
				});");

				Map<String, StringMap> files;
				Directory dir(path);
				StringMap info;
				while(dir.nextFile())
				{
					if(dir.fileName() == ".directory"
						|| dir.fileName().toLower() == "thumbs.db"
						|| dir.fileName().substr(0,7) == ".Trash-")
						continue;

					dir.getFileInfo(info);
					if(info.get("type") == "directory") files.insert("0"+info.get("name").toLower()+info.get("name"),info);
					else files.insert("1"+info.get("name").toLower()+info.get("name"),info);
				}

				page.open("div", ".box");

				String desc;
				desc << files.size() << " files";
				page.span(desc, ".button");

				if(request.url[request.url.size()-1] == '/') page.openLink("..", ".button");
				else page.openLink(".", ".button");
				page.image("/static/arrow_up.png", "Parent");
				page.closeLink();

				page.br();

				if(files.empty()) page.div("No files", ".files");
				else {
					page.open("table", ".files");
					for(Map<String, StringMap>::iterator it = files.begin();
						it != files.end();
						++it)
					{
						StringMap &info = it->second;
						String name = info.get("name");
						String link = name;

						page.open("tr");
						page.open("td",".icon");
						if(info.get("type") == "directory") page.image("/static/dir.png");
						else page.image("/static/file.png");
						page.close("td");
						page.open("td",".filename");
						if(info.get("type") != "directory" && name.contains('.'))
							page.span(name.afterLast('.').toUpper(), ".type");
						page.link(link,name);
						page.close("td");
						page.open("td",".size");
						if(info.get("type") == "directory") page.text("directory");
						else page.text(String::hrSize(info.get("size")));
						page.close("td");
						page.open("td",".actions");

						page.openLink("#", ".linkdelete");
						page.image("/static/delete.png", "Delete", ".deletelink");
						page.closeLink();

						if(info.get("type") != "directory")
						{
							page.openLink(Http::AppendParam(link,"download"), ".downloadlink");
							page.image("/static/down.png", "Download");
							page.closeLink();

							if(Mime::IsAudio(name) || Mime::IsVideo(name))
							{
								page.openLink(Http::AppendParam(link,"play"), ".playlink");
								page.image("/static/play.png", "Play");
								page.closeLink();
							}
						}
						page.close("td");
						page.close("tr");
					}
					page.close("table");
					page.close("div");

					page.openForm(prefix+url, "post", "executeForm");
					page.input("hidden", "token", user()->generateToken("directory"));
					page.input("hidden", "command");
					page.input("hidden", "argument");
					page.closeForm();

					page.javascript("function deleteFile(name) {\n\
						if(confirm('Do you really want to delete '+name+' ?')) {\n\
							document.executeForm.command.value = 'delete';\n\
							document.executeForm.argument.value = name;\n\
							document.executeForm.submit();\n\
						}\n\
					}");

					page.javascript("$('.deletelink').css('cursor', 'pointer').click(function(event) {\n\
						var fileName = $(this).closest('tr').find('td.filename a').text();\n\
						deleteFile(fileName);\n\
						return false;\n\
					});");
				}

				page.footer();
			}
			else if(File::Exist(path))
			{
				if(request.get.contains("play"))
				{
					String host;
					if(!request.headers.get("Host", host))
					host = String("localhost:") + Config::Get("interface_port");

					Http::Response response(request, 200);
					response.headers["Content-Disposition"] = "attachment; filename=\"stream.m3u\"";
					response.headers["Content-Type"] = "audio/x-mpegurl";
					response.send();

					response.stream->writeLine("#EXTM3U");
					response.stream->writeLine(String("#EXTINF:-1, ") + path.afterLast(Directory::Separator));
					response.stream->writeLine("http://" + host + prefix + request.url);
					return;
				}

				Http::Response response(request,200);
				if(request.get.contains("download")) response.headers["Content-Type"] = "application/force-download";
				else response.headers["Content-Type"] = Mime::GetType(path);
				response.headers["Content-Length"] << File::Size(path);
				response.headers["Last-Modified"] = File::Time(path).toHttpDate();
				response.send();

				File file(path, File::Read);
				response.stream->write(file);
			}
			else throw 404;
		}
	}
	catch(const std::exception &e)
	{
		LogWarn("Indexer::http", e.what());
		throw 500;	// Httpd handles integer exceptions
	}
}

bool Indexer::prepareQuery(Database::Statement &statement, const Query &query, const String &fields)
{
	// Retrieve and prepare parameters
	String path = query.mPath;
	bool pattern = path.contains('*');
	if(pattern)
	{
		path.replace("\\", "\\\\");
		path.replace("%", "\\%");
		path.replace("*", "%");
	}

	String match = query.mMatch;
	match.replace("\\", "\\\\");
	match.replace("%", "\\%");
	match.replace("*", "%");

	BinaryString digest = query.mDigest;

	bool isFromSelf = (query.mAccess == Resource::Personal);
	int count = query.mCount;
	if(count <= 0 || count > 1000) count = 1000;	// Limit for security purposes

	// Build SQL request
	String sql;
	sql<<"SELECT "<<fields<<" FROM resources ";
	if(!match.empty()) sql<<"JOIN names ON names.rowid = name_rowid ";
	sql<<"WHERE digest IS NOT NULL ";

	if(!path.empty())
	{
		if(pattern) sql<<"AND path LIKE ? ESCAPE '\\' ";
		else sql<<"AND path = ? ";
	}

	if(!match.empty())			sql<<"AND names.name MATCH ? ";
	if(!digest.empty())			sql<<"AND digest = ? ";
	else if(path.empty() || !isFromSelf)	sql<<"AND path NOT LIKE '/\\_%' ESCAPE '\\' ";		// hidden files

	//if(query.mMinAge > 0) sql<<"AND time <= ? ";
	//if(query.mMaxAge > 0) sql<<"AND time >= ? ";

	sql<<"ORDER BY time DESC "; // Newer files first

	if(count > 0)
	{
		sql<<"LIMIT "<<String::number(count)<<" ";
		if(query.mOffset > 0) sql<<"OFFSET "<<String::number(query.mOffset)<<" ";
	}

	statement = mDatabase->prepare(sql);
	int parameter = 0;
	if(!path.empty())	statement.bind(++parameter, path);
	if(!match.empty())	statement.bind(++parameter, match);
	if(!digest.empty())	statement.bind(++parameter, query.mDigest);

	//if(query.mMinAge > 0)	statement.bind(++parameter, int64_t(Time::Now()-double(query.mMinAge)));
	//if(query.mMaxAge > 0)	statement.bind(++parameter, int64_t(Time::Now()-double(query.mMaxAge)));

	return true;
}

void Indexer::update(String path)
{
	if(!path.empty() && path[path.size() - 1] == '/')
		path.resize(path.size() - 1);
	if(path.empty()) path = "/";

	//LogDebug("Indexer::update", "Updating: " + path);

	try {
		if(path == "/")	// Top-level: Indexer directories
		{
			Array<String> names;

			{
				std::unique_lock<std::mutex> lock(mMutex);
				mDirectories.getKeys(names);
			}

			// Iterate on directories
			for(int i=0; i<names.size(); ++i)
			{
				String subpath = "/" + names[i];
				update(subpath);
			}
		}
		else {
			String realPath = this->realPath(path);
			if(Directory::Exist(realPath))
			{
				// Iterate on files
				Directory dir(realPath);
				while(dir.nextFile())
				{
					String subpath = path + '/' + dir.fileName();
					update(subpath);
				}
			}
		}

		Resource dummy;
		process(path, dummy);
	}
	catch(const Exception &e)
	{
		LogWarn("Indexer", String("Processing failed for ") + path + ": " + e.what());
	}
}

String Indexer::realPath(String path) const
{
	if(path.empty() || path == "/") return mBaseDirectory;
	if(path[0] == '/') path.ignore(1);

	// Do not accept the parent directory symbol as a security protection
	if(path.find("\\..") != String::NotFound || path.find("..\\") != String::NotFound) throw Exception("Invalid path: " + path);

	String directory = path;
	path = directory.cut('/');
	Entry entry;
	if(!mDirectories.get(directory, entry))
		throw Exception("Invalid path: unknown directory: " + directory);

	if(path.empty())
		return entry.path;

	if(Directory::Separator != '/') path.replace('/', Directory::Separator);
	return entry.path + Directory::Separator + path;
}

bool Indexer::isHiddenPath(String path) const
{
	if(path.empty()) return false;
	if(path[0] == '_') return true;
	if(path.size() >= 2 && path[0] == '/' && path[1] == '_') return true;
	return false;
}

Resource::AccessLevel Indexer::pathAccessLevel(String path) const
{
	if(path.empty() || path == "/") return Resource::Public;
	if(path[0] == '/') path.ignore(1);

	String directory = path;
	path = directory.cut('/');
	Entry entry;
	if(!mDirectories.get(directory, entry))
		throw Exception("Invalid path: unknown directory: " + directory);

	return entry.access;
}

void Indexer::run(void)
{
	{
		std::unique_lock<std::mutex> lock(mMutex);
		if(mRunning) return;
		mRunning = true;
	}

	try {
		LogDebug("Indexer::run", "Indexation started");

		// Invalidate all entries
		mDatabase->execute("UPDATE resources SET seen=0");

		// Update
		update("/");

		// Clean
		mDatabase->execute("DELETE FROM resources WHERE seen=0");	// TODO: delete from names

		LogDebug("Indexer::run", "Indexation finished");
	}
	catch(const std::exception &e)
	{
		LogWarn("Indexer::run", e.what());
	}

	{
		std::unique_lock<std::mutex> lock(mMutex);
		mRunning = false;
	}

	start(seconds(6*3600));
}

Indexer::Query::Query(const String &path) :
	mPath(path),
	mOffset(0), mCount(-1),
	mAccess(Resource::Private)
{

}

Indexer::Query::~Query(void)
{

}

void Indexer::Query::setPath(const String &path)
{
	mPath = path;
}

void Indexer::Query::setDigest(const BinaryString &digest)
{
	mDigest = digest;
}

void Indexer::Query::setRange(int first, int last)
{
	mOffset = std::max(first,0);
	mCount  = std::max(last-mOffset,0);
}

void Indexer::Query::setLimit(int count)
{
	mCount = count;
}

void Indexer::Query::setMatch(const String &match)
{
	mMatch = match;
}

void Indexer::Query::setAccessLevel(Resource::AccessLevel access)
{
	mAccess = access;
}

void Indexer::Query::setFromSelf(bool isFromSelf)
{
	if(isFromSelf)
	{
		if(mAccess == Resource::Private)
			mAccess = Resource::Personal;
	}
	else {
		// Other than self, may not access personal folders
		if(mAccess == Resource::Personal)
			mAccess = Resource::Private;
	}
}

void Indexer::Query::serialize(Serializer &s) const
{
	Object object;
	object.insert("path", mPath);
	object.insert("match", mMatch);
	object.insert("digest", mDigest);

	if(mOffset > 0) object.insert("offset", mOffset);
	if(mCount > 0)  object.insert("count", mCount);

	object.insert("access", (mAccess == Resource::Personal ? "personal" : (mAccess == Resource::Private ? "private" : "public")));

	s << object;
}

bool Indexer::Query::deserialize(Serializer &s)
{
	String strAccess;

	mPath.clear();
	mMatch.clear();
	mDigest.clear();
	mOffset = 0;
	mCount = 0;

	Object object;
	object.insert("path", mPath);
	object.insert("match", mMatch);
	object.insert("digest", mDigest);
	object.insert("offset", mOffset);
	object.insert("count", mCount);
	object.insert("access", strAccess);

	if(!(s >> object)) return false;

	if(strAccess== "personal") mAccess = Resource::Personal;
	else if(strAccess == "private") mAccess = Resource::Private;
	else mAccess = Resource::Public;

	return true;
}

bool Indexer::Query::isInlineSerializable(void) const
{
	return false;
}

Indexer::Entry::Entry(void)
{
	this->access = Resource::Public;
}

Indexer::Entry::Entry(const String &path, Resource::AccessLevel access)
{
	this->path = path;
	this->access = access;
}

Indexer::Entry::~Entry(void)
{

}

void Indexer::Entry::serialize(Serializer &s) const
{
	s << Object()
		.insert("path", path)
		.insert("access", String(access == Resource::Personal ? "personal" : (access == Resource::Private ? "private" : "public")));
}

bool Indexer::Entry::deserialize(Serializer &s)
{
	path.clear();

	String strAccess;

	if(!(s >> Object()
		.insert("path", path)
		.insert("access", strAccess)))
		return false;

	if(strAccess == "personal") access = Resource::Personal;
	else if(strAccess == "private") access = Resource::Private;
	else access = Resource::Public;

	return true;
}

bool Indexer::Entry::isInlineSerializable(void) const
{
	return false;
}

}
