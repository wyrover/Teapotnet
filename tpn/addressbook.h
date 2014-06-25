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
#include "tpn/http.h"
#include "tpn/interface.h"
#include "tpn/address.h"
#include "tpn/socket.h"
#include "tpn/identifier.h"
#include "tpn/mailqueue.h"
#include "tpn/core.h"
#include "tpn/user.h"
#include "tpn/profile.h"
#include "tpn/scheduler.h"
#include "tpn/task.h"
#include "tpn/array.h"
#include "tpn/map.h"
#include "tpn/set.h"

namespace tpn
{

class User;
class Profile;
  
class AddressBook : private Synchronizable, public HttpInterfaceable, public Core::Listener
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
	
	void sendContacts(const Identifier &peering) const;
	void sendContacts(void) const;
	
	void update(void);
	bool send(const Notification &notification);
	bool send(const Mail &mail);
	
	void http(const String &prefix, Http::Request &request);
	
	typedef SerializableMap<Address, Time> AddressBlock;
	typedef SerializableMap<String, AddressBlock> AddressMap;
	
	class Contact : public Serializable, public HttpInterfaceable, public Task
	{
	public:
	  	Contact(	AddressBook *addressBook,
				const String &uname,
				const String &name,
				const String &tracker,
				const String &secret);
		Contact(AddressBook *addressBook);
		~Contact(void);

		String uniqueName(void) const;
		String name(void) const;
		String tracker(void) const;
		Identifier peering(void) const;
		Identifier remotePeering(void) const;
		Time time(void) const;
		uint32_t peeringChecksum(void) const;
		String urlPrefix(void) const;
		
		Profile *profile(void) const;

		bool isSelf(void) const;
		bool isFound(void) const;
		bool isConnected(void) const;
		bool isConnected(const String &instance) const;
		bool isOnline(void) const;
		String status(void) const;
		AddressMap addresses(void) const;
		bool isDeleted(void) const;
		void setDeleted(void);
		void getInstancesNames(Array<String> &array);
		
		bool addAddresses(const AddressMap &map);
		
		// These functions return true if addr is successfully connected
		bool connectAddress(const Address &addr, const String &instance, bool save = true);
		bool connectAddresses(const AddressMap &map, bool save = true, bool shuffle = false);
		
		void update(bool alternate = false);
		void createProfile(void);
		
		void connected(const Identifier &peering, bool incoming);
		void disconnected(const Identifier &peering);
		bool notification(const Identifier &peering, Notification *notification);
		bool request(const Identifier &peering, Request *request);
		bool send(const Notification &notification);
		bool send(const Mail &mail);
		
		void http(const String &prefix, Http::Request &request);
		void copy(const Contact *contact);
		
		// Serializable
		void serialize(Serializer &s) const;
		bool deserialize(Serializer &s);
		bool isInlineSerializable(void) const;
		
	private:
		BinaryString secret(void) const;
		void run(void);
		
		MailQueue::Selection selectMails(bool privateOnly = false) const;
		void sendMails(const Identifier &peering, const MailQueue::Selection &selection, int offset, int count) const;
		void sendMailsChecksum(const Identifier &peering, const MailQueue::Selection &selection, int offset, int count, bool recursion) const;
		void sendUnread(const Identifier &peering) const;
		void sendPassed(const Identifier &peering) const;
		
	  	AddressBook *mAddressBook;
		String mUniqueName, mName, mTracker;
		Identifier mPeering, mRemotePeering;
		BinaryString mSecret;
		Time mTime;
		bool mDeleted;
		
		bool mFound;
		AddressMap mAddrs;
		Set<String> mExcludedInstances;
		Set<String> mOnlineInstances;
		
		Profile *mProfile;
	};
	
	Identifier addContact(String name, const String &secret);
	void removeContact(const Identifier &peering);
	Contact *getContact(const Identifier &peering);
	const Contact *getContact(const Identifier &peering) const;
	Contact *getContactByUniqueName(const String &uname);
	const Contact *getContactByUniqueName(const String &uname) const;
	void getContacts(Array<Contact *> &array);
	
	Identifier setSelf(const String &secret);
	Contact *getSelf(void);
	const Contact *getSelf(void) const;
	
private:
	static const double StartupDelay;	// time before starting connections (useful for UPnP)
	static const double UpdateInterval;	// for contacts connection attemps
	static const double UpdateStep;		// between contacts
	static const int MaxChecksumDistance;	// for mail synchronization
	
	void registerContact(Contact *contact, int ordinal = 0);
	void unregisterContact(Contact *contact);
	bool publish(const Identifier &remotePeering);
	bool query(const Identifier &peering, const String &tracker, AddressMap &output, bool alternate = false);	
	
	User *mUser;
	String mUserName;
	String mFileName;
	Map<Identifier, Contact*> mContacts;			// Sorted by identifier
	Map<String, Contact*> mContactsByUniqueName;		// Sorted by unique name
	Scheduler mScheduler;
	
	Set<String> mBogusTrackers;
};

}

#endif
