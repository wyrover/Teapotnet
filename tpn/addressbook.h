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

#ifndef TPN_ADDRESSBOOK_H
#define TPN_ADDRESSBOOK_H

#include "tpn/include.h"
#include "tpn/synchronizable.h"
#include "tpn/serializable.h"
#include "tpn/interface.h"
#include "tpn/http.h"
#include "tpn/core.h"
#include "tpn/address.h"
#include "tpn/socket.h"
#include "tpn/identifier.h"
#include "tpn/crypto.h"
#include "tpn/user.h"
#include "tpn/profile.h"
#include "tpn/mailqueue.h"
#include "tpn/scheduler.h"
#include "tpn/task.h"
#include "tpn/array.h"
#include "tpn/map.h"
#include "tpn/set.h"

namespace tpn
{

class User;
class Profile;
  
class AddressBook : private Synchronizable, public Serializable, public HttpInterfaceable, public Core::Listener
{
public:
	AddressBook(User *user);
	~AddressBook(void);
	
	User *user(void) const;
	String userName(void) const;
	
	void clear(void);
	void load(Stream &stream);
	void save(Stream &stream) const;
	void save(void) const;
	
	void sendContacts(const Identifier &peer) const;
	void sendContacts(void) const;
	
	void update(void);
	bool send(const Notification &notification);
	bool send(const Mail &mail);
	
	// Serializable
	void serialize(Serializer &s) const;
	bool deserialize(Serializer &s);
	bool isInlineSerializable(void) const;
	
	// HttpInterfaceable
	void http(const String &prefix, Http::Request &request);
	
	class MeetingPoint : public Serializable, public Core::Listener
	{
	public:
		MeetingPoint(void);
		MeetingPoint(	const Identifier &identifier,
				const String &tracker);
		virtual ~MeetingPoint(void);
		
		Identifier identifier(void) const;
		String tracker(void) const;
		uint32_t checksum(void) const;
		
		bool isFound(void) const;

		// Listener
		void seen(const Identifier &peer);
		bool recv(const Identifier &peer, const Notification &notification);
		
		// Serializable
		void serialize(Serializer &s) const;
		bool deserialize(Serializer &s);
		bool isInlineSerializable(void) const;
		
	protected:
		Identifier mIdentifier;
		String mTracker;
		bool mFound;
		
		friend class AddressBook;
	};
	
	class Contact
	{
	public:
		Contact(void);
		Contact(	AddressBook *addressBook,
				const String &uname,
				const String &name,
				const Rsa::PublicKey &pubKey);
		~Contact(void);
		
		const Rsa::PublicKey &publicKey(void) const;
		Identifier identifier(void) const;
		String uniqueName(void) const;
		String name(void) const;
		String urlPrefix(void) const;
		Profile *profile(void) const;
		
		bool isSelf(void) const;
		bool isConnected(void) const;
		bool isConnected(uint64_t number) const;
		
		void getInstanceNumbers(Array<uint64_t> &result) const;
		bool getInstanceIdentifier(uint64_t number, Identifier &result) const;
		bool getInstanceName(uint64_t number, String &result) const;
		bool getInstanceAddresses(uint64_t number, Array<Address> &result) const;
		
		bool send(const Notification &notification);
		bool send(const Mail &mail);
		
		// Core::Listener
		void seen(const Identifier &peer);
		bool recv(const Identifier &peer, const Notification &notification);
		
		// HttpInterfaceable
		void http(const String &prefix, Http::Request &request);
		
		// Serializable
		void serialize(Serializer &s) const;
		bool deserialize(Serializer &s);
		bool isInlineSerializable(void) const;
		
	private:
		class Instance : public Serializable
		{
			Instance(void);
			Instance(uint64_t number);
			~Instance();
			
			uint64_t number(void) const;
			String name(void) const;
			void setName(const String &name);
			Time lastSeen(void) const;
			void setSeen(void);
			
			void addAddress(const Address &addr);
			void addAddresses(const Set<Address> &addrs);
			void getAddresses(Set<Address> &result) const;
			
			// Serializable
			void serialize(Serializer &s) const;
			bool deserialize(Serializer &s);
			bool isInlineSerializable(void) const;
			
		private:
			uint64_t mNumber;
			String mName;
			
			typedef SerializableMap<Address, Time> AddressBlock;
			AddressBlock mAddrs;
			
			Time mLastSeen;
		};
	  
		AddressBook *mAddressBook;
		Profile *mProfile;
		
		String mUniqueName, mName;
		Rsa::PublicKey mPublicKey;
		
		SerializableList<MeetingPoint> mMeetingPoints;
		
		typedef SerializableMap<uint64_t, Instance> InstancesMap;
		InstancesMap mInstances;
		
		friend class AddressBook;
	};
	
	String addContact(const String &name, const Rsa::PublicKey &pubKey);	// returns uname
	bool removeContact(const String &uname);
	Contact *getContact(const String &uname);
	const Contact *getContact(const String &uname) const;
	void getContacts(Array<AddressBook::Contact*> &result);
	
	void setSelf(const Rsa::PublicKey &pubKey);
	Contact *getSelf(void);
	const Contact *getSelf(void) const;
	
private:
	bool publish(const Identifier &identifier);
	bool query(const Identifier &identifier, const String &tracker, Serializable &result);	
	
	User *mUser;
	String mFileName;
	SerializableMap<String, Contact> mContacts;	// Sorted by unique name
	SerializableMap<Identifier, Contact*> mContactsByIdentifier;
};

}

#endif
