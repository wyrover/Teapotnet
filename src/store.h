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

#ifndef ARC_STORE_H
#define ARC_STORE_H

#include "include.h"
#include "thread.h"
#include "synchronizable.h"
#include "identifier.h"
#include "file.h"
#include "map.h"
#include "http.h"
#include "interface.h"
#include "mutex.h"

namespace arc
{

class User;
  
class Store : protected Thread, protected Synchronizable, public HttpInterfaceable
{
public:
	struct Entry
	{
		Identifier 	hash;
		String		url;
		String		path;
		StringMap	info;
		File		*content;	// file content
	};
  
  	static bool GetResource(const Identifier &hash, Entry &entry, bool content = true);
	static const size_t ChunkSize;

	Store(User *user);
	~Store(void);
	
	User *user(void) const;
	String userName(void) const;
	
	void addDirectory(const String &name, const String &path);
	void removeDirectory(const String &name);

	void save(void) const;
	void refresh(void);

	bool get(const Identifier &identifier, Entry &entry, bool content = true);
	bool get(const String &url, Entry &entry, bool content = true);

	void http(const String &prefix, Http::Request &request);

private:
	void refreshDirectory(const String &dirUrl, const String &dirPath);
	String urlToPath(const String &url) const;
	void run(void);

	User *mUser;
	String mFileName;
	String mDatabasePath;
	StringMap mDirectories;
	
	static Map<Identifier,String> Resources;	// entry names by data hash
	static Mutex ResourcesMutex;
};

}

#endif
