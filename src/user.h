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

#ifndef ARC_USER_H
#define ARC_USER_H

#include "include.h"
#include "thread.h"
#include "http.h"
#include "interface.h"
#include "identifier.h"
#include "addressbook.h"
#include "core.h"
#include "map.h"

namespace arc
{

class User : public Thread, protected Synchronizable, public Core::Listener, public HttpInterfaceable
{
public:
	static User *Authenticate(const String &name, const String &password);
	
	User(const String &name, const String &password = "");
	~User(void);
	
	const String &name(void) const;
	String profilePath(void) const;
	
	void message(const Message &message);
	void http(const String &prefix, Http::Request &request);
	
private:
	void run(void);
	
	String mName;
	Identifier mHash;
	AddressBook *mAddressBook;
	
	static Map<Identifier, User*> UsersMap;
};

}

#endif
