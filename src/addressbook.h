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

#ifndef ARC_ADDRESSBOOK_H
#define ARC_ADDRESSBOOK_H

#include "include.h"
#include "http.h"
#include "interface.h"
#include "address.h"
#include "socket.h"
#include "identifier.h"
#include "core.h"
#include "message.h"
#include "array.h"
#include "map.h"

namespace arc
{

class User;
  
class AddressBook : public Synchronizable, public HttpInterfaceable
{
public:
	AddressBook(User *user);
	~AddressBook(void);
	
	const String &name(void) const;
	
	void load(Stream &stream);
	void save(Stream &stream) const;
	void autosave(void) const;
	
	void update(void);
	void http(const String &prefix, Http::Request &request);

	
	class Contact : protected Synchronizable, public Serializable, public HttpInterfaceable, public Core::Listener
	{
	public:
	  	Contact(	AddressBook *addressBook,
				const String &uname,
				const String &name,
	     			const String &tracker,
	     			const ByteString &secret);
		Contact(AddressBook *addressBook);
		~Contact(void);
	    	
		const String &uniqueName(void) const;
		const String &name(void) const;
		const String &tracker(void) const;
		const Identifier &peering(void) const;
		const Identifier &remotePeering(void) const;
		uint32_t peeringChecksum(void) const;
		String urlPrefix(void) const;
		
		void update(void);
		
		void message(const Message &message);
		void http(const String &prefix, Http::Request &request);
		
		void serialize(Stream &s) const;
		void deserialize(Stream &s);
		
	private:
	  	AddressBook *mAddressBook;
		String mUniqueName, mName, mTracker;
		Identifier mPeering, mRemotePeering;
		ByteString mSecret;
		
		SerializableArray<Address> mAddrs;
		Deque<Message> mMessages;
	};
	
	const Identifier &addContact(String name, const ByteString &secret);
	void removeContact(const Identifier &peering);
	const Contact *getContact(const Identifier &peering);
	
private:
	static bool publish(const Identifier &remotePeering);
	static bool query(const Identifier &peering, const String &tracker, Array<Address> &addrs);
	
	String mName;
	String mFileName;
	Map<Identifier, Contact*> mContacts;		// Sorted by peering
	Map<String, Contact*> mContactsByUniqueName;	// Sorted by unique name
};

}

#endif
