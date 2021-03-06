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

#include "tpn/portmapping.hpp"
#include "tpn/html.hpp"
#include "tpn/network.hpp"

#include "pla/scheduler.hpp"
#include "pla/http.hpp"
#include "pla/jsonserializer.hpp"
#include "pla/object.hpp"
#include "pla/random.hpp"

namespace tpn
{

PortMapping *PortMapping::Instance = NULL;

PortMapping::PortMapping(void) :
	mProtocol(NULL),
	mEnabled(false)
{

}

PortMapping::~PortMapping(void)
{
	disable();
}

void PortMapping::enable(void)
{
	std::unique_lock<std::mutex> lock(mMutex);
	mEnabled = true;
	mAlarm.schedule(seconds(600.), [this]()
	{
		run();
	});
}

void PortMapping::disable(void)
{
	std::unique_lock<std::mutex> lock(mMutex);
	mEnabled = false;
	mAlarm.cancel();

	if(mProtocol)
	{
		for(auto it = mMap.begin(); it != mMap.end(); ++it)
		{
			mProtocol->remove(it->first.protocol, it->first.port, it->second.external);
		}

		mProtocol.reset();
	}
}

bool PortMapping::isEnabled(void) const
{
	std::unique_lock<std::mutex> lock(mMutex);
	return mEnabled;
}

bool PortMapping::isAvailable(void) const
{
	std::unique_lock<std::mutex> lock(mMutex);
	return !mExternalHost.empty();
}

String PortMapping::getExternalHost(void) const
{
	std::unique_lock<std::mutex> lock(mMutex);
	return mExternalHost;
}

Address PortMapping::getExternalAddress(Protocol protocol, uint16_t internal) const
{
	uint16_t external = internal;
	get(protocol, internal, external);
	return Address(mExternalHost, external);
}

void PortMapping::add(Protocol protocol, uint16_t internal, uint16_t suggested)
{
	remove(protocol, internal);

	std::unique_lock<std::mutex> lock(mMutex);

	Entry &entry = mMap[Descriptor(protocol, internal)];
	entry.suggested = suggested;
	entry.external = suggested;

	if(mProtocol) mProtocol->add(protocol, internal, entry.external);
}

void PortMapping::remove(Protocol protocol, uint16_t internal)
{
	std::unique_lock<std::mutex> lock(mMutex);

	Descriptor descriptor(protocol, internal);
	Entry entry;
	if(mMap.get(descriptor, entry))
	{
		mMap.erase(descriptor);
		if(mProtocol) mProtocol->remove(protocol, internal, entry.external);
	}
}

bool PortMapping::get(Protocol protocol, uint16_t internal, uint16_t &external) const
{
	std::unique_lock<std::mutex> lock(mMutex);
	if(!mProtocol) return false;

	external = internal;

	Entry entry;
	if(mMap.get(Descriptor(protocol, internal), entry)) return false;
	if(!entry.external) return false;

	external = entry.external;
	return true;
}

void PortMapping::run(void)
{
	std::unique_lock<std::mutex> lock(mMutex);
	if(!mEnabled) return;

	Set<Address> addresses;
	Network::Instance->overlay()->getAddresses(addresses);

	bool hasIpv4 = false;
	bool hasPublicIpv4 = false;
	auto it = addresses.begin();
	while(it != addresses.end())
	{
		hasIpv4|= it->isIpv4();
		hasPublicIpv4|= (it->isIpv4() && it->isPublic());
		++it;
	}

	if(!hasIpv4 || hasPublicIpv4)
	{
		mProtocol.reset();
		return;
	}

	LogDebug("PortMapping", "Potential NAT detected");

	if(mProtocol)
	{
		bool success = false;
		try {
			if(mProtocol->check(mExternalHost) && !mExternalHost.empty())
				success = true;
		}
		catch(const Exception &e)
		{
			LogWarn("PortMapping", e.what());
		}

		if(!success) mProtocol.reset();
	}

	if(!mProtocol)
	{
		LogDebug("PortMapping", "Probing protocols...");

		mExternalHost.clear();
		for(int i=0; i<2; ++i)	// TODO: FreeboxAPI is disabled
		{
			try {
				switch(i)
				{
					case 0: mProtocol = std::make_shared<NatPMP>(); 	break;
					case 1: mProtocol = std::make_shared<UPnP>();		break;
					case 2: mProtocol = std::make_shared<FreeboxAPI>();	break;
				}

				if(mProtocol->check(mExternalHost) && !mExternalHost.empty())
					break;
			}
			catch(const Exception &e)
			{
				LogWarn("PortMapping", e.what());
			}

			mProtocol = NULL;
		}

		if(mProtocol) LogInfo("PortMapping", "Port mapping is available, external address is " + mExternalHost);
		else LogInfo("PortMapping", "Port mapping is not available");
	}

	if(mProtocol)
	{
		for(auto it = mMap.begin(); it != mMap.end(); ++it)
		{
			if(!it->second.external) it->second.external = it->second.suggested;
			if(!mProtocol->add(it->first.protocol, it->first.port, it->second.external))
				LogWarn("PortMapping", String("Mapping failed for ") + (it->first.protocol == TCP ? "TCP" : "UDP") + " port " + String::number(it->second.suggested));
		}
	}
}


PortMapping::NatPMP::NatPMP(void)
{
	mSock.bind(5350, true, AF_INET);
	mGatewayAddr.set("255.255.255.255", 5351, AF_INET, SOCK_DGRAM);
}

PortMapping::NatPMP::~NatPMP(void)
{

}

bool PortMapping::NatPMP::check(String &host)
{
	LogDebug("PortMapping::NatPMP", "Trying NAT-PMP...");

	BinaryString query;
	query.writeBinary(uint8_t(0));	// version
	query.writeBinary(uint8_t(0));	// op

	int attempts = 3;
	duration timeout = milliseconds(250.);
	for(int i=0; i<attempts; ++i)
	{
		BinaryString dgram = query;
		mSock.write(dgram, mGatewayAddr);

		using clock = std::chrono::steady_clock;
		std::chrono::time_point<clock> end = clock::now() + std::chrono::duration_cast<clock::duration>(timeout);

		while(clock::now() < end)
		{
			Address sender;
			duration left = end - clock::now();
			if(!mSock.read(dgram, sender, left)) break;

			if(!sender.isPrivate()) continue;

			LogDebug("PortMapping::NatPMP", String("Got response from ") + sender.toString());
			if(parse(dgram, 0))
			{
				LogDebug("PortMapping", "NAT-PMP is available");
				mGatewayAddr = sender;
				host = mExternalHost;
				return true;
			}
		}

		timeout*= 2;
	}

	//LogDebug("PortMapping::NatPMP", "NAT-PMP is not available");
	return false;
}

bool PortMapping::NatPMP::add(Protocol protocol, uint16_t internal, uint16_t &external)
{
	return request((protocol == TCP ? 2 : 1), internal, external, 3600, &external);
}

bool PortMapping::NatPMP::remove(Protocol protocol, uint16_t internal, uint16_t external)
{
	return request((protocol == TCP ? 2 : 1), internal, external, 0, NULL);
}

bool PortMapping::NatPMP::request(uint8_t op, uint16_t internal, uint16_t suggested, uint32_t lifetime, uint16_t *external)
{
	if(!op) return false;

	BinaryString query;
	query.writeBinary(uint8_t(0));	// version
	query.writeBinary(op);		// op
	query.writeBinary(uint16_t(0));	// reserved
	query.writeBinary(internal);
	query.writeBinary(suggested);
	query.writeBinary(lifetime);

	const int attempts = 3;
	duration timeout = milliseconds(250.);
	for(int i=0; i<attempts; ++i)
	{
		BinaryString dgram = query;
		mSock.write(dgram, mGatewayAddr);

		using clock = std::chrono::steady_clock;
		std::chrono::time_point<clock> end = clock::now() + std::chrono::duration_cast<clock::duration>(timeout);

		while(clock::now() < end)
		{
			Address sender;
			duration left = end - clock::now();
			if(!mSock.read(dgram, sender, left)) break;

			if(!sender.isPrivate()) continue;

			if(parse(dgram, op, internal))
				return true;
		}

		timeout*= 2;
	}

	return false;
}

bool PortMapping::NatPMP::parse(BinaryString &dgram, uint8_t reqOp, uint16_t reqInternal, uint16_t *retExternal)
{
	uint8_t version;
	uint8_t op;
	uint16_t result;
	uint32_t time;
	if(!dgram.readBinary(version))  return false;
	if(!dgram.readBinary(op))	return false;
	if(!dgram.readBinary(result))	return false;
	if(!dgram.readBinary(time))	return false;

	if(reqOp != op - 128) return false;
	if(result != 0) return false;

	switch(op)
	{
		case 128:	// address
		{
			uint8_t a,b,c,d;
			if(!dgram.readBinary(a)) return false;
			if(!dgram.readBinary(b)) return false;
			if(!dgram.readBinary(c)) return false;
			if(!dgram.readBinary(d)) return false;

			if(mExternalHost.empty()) LogDebug("PortMapping::NatPMP", "NAT-PMP compliant gateway found");

			mExternalHost.clear();
			mExternalHost<<a<<'.'<<b<<'.'<<c<<'.'<<d;
			return true;
		}

		case 129:	// UDP object
		case 130:	// TCP object
		{
			uint16_t internal;
			uint16_t external;
			uint32_t lifetime;
			if(!dgram.readBinary(internal)) return false;
		  	if(!dgram.readBinary(external)) return false;
			if(!dgram.readBinary(lifetime)) return false;

			if(!internal) return false;
			if(reqInternal && (reqInternal != internal)) return false;

			if(retExternal) *retExternal = external;
			return true;
		}
	}

	return false;
}


PortMapping::UPnP::UPnP(void)
{
	mSock.bind(1900, true, AF_INET);
}

PortMapping::UPnP::~UPnP(void)
{

}

bool PortMapping::UPnP::check(String &host)
{
	LogDebug("PortMapping::UPnP", "Trying UPnP...");

	Address addr;
	addr.set("239.255.255.250", 1900, AF_INET, SOCK_DGRAM);

	String message;
	message << "M-SEARCH * HTTP/1.1\r\n";
	message << "HOST: "<<addr<<"\r\n";
	message << "MAN: ssdp:discover\r\n";
	message << "MX: 10\r\n";
	message << "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n";

	int attempts = 3;
	duration timeout = milliseconds(250.);
	for(int i=0; i<attempts; ++i)
	{
		BinaryString dgram(message);
		mSock.write(dgram, addr);

		using clock = std::chrono::steady_clock;
		std::chrono::time_point<clock> end = clock::now() + std::chrono::duration_cast<clock::duration>(timeout);

		while(clock::now() < end)
		{
			Address sender;
			duration left = end - clock::now();
			if(!mSock.read(dgram, sender, left)) break;

			if(!sender.isPrivate()) continue;

			LogDebug("PortMapping::UPnP", String("Got response from ") + sender.toString());
			try {
				if(parse(dgram))
				{
					LogDebug("PortMapping::UPnP", "UPnP is available");
					mGatewayAddr = sender;
					host = mExternalHost;
					return true;
				}
			}
			catch(const Exception &e)
			{
				// Nothing to do
			}
		}

		timeout*= 2;
	}

	//LogDebug("PortMapping::UPnP", "UPnP is not available");
	return false;
}

bool PortMapping::UPnP::add(Protocol protocol, uint16_t internal, uint16_t &external)
{
	if(mControlUrl.empty()) return false;
	if(!external) external = 1024 + Random().readInt() % (49151 - 1024);

	unsigned duration = 3600;	// 1h
	unsigned attempts = 20;

	uint32_t gen = 0;
	for(int i=0; i<attempts; ++i)
	{
		Http::Request request(mControlUrl, "POST");

		String host;
		request.headers.get("Host", host);
		Socket sock(host, seconds(10.));
		Address localAddr = sock.getLocalAddress();

		String content = "<?xml version=\"1.0\"?>\r\n\
<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n\
<s:Body>\r\n\
<m:AddPortMapping xmlns:m=\"urn:schemas-upnp-org:service:WANIPConnection:1\">\r\n\
<NewRemoteHost></NewRemoteHost>\r\n\
<NewExternalPort>" + String::number(external) + "</NewExternalPort>\r\n\
<NewProtocol>"+(protocol == TCP ? String("TCP") : String("UDP"))+"</NewProtocol>\r\n\
<NewInternalPort>" + String::number(internal) + "</NewInternalPort>\r\n\
<NewInternalClient>"+Html::escape(localAddr.host())+"</NewInternalClient>\r\n\
<NewEnabled>1</NewEnabled>\r\n\
<NewPortMappingDescription>"+Html::escape(String(APPNAME))+"</NewPortMappingDescription>\r\n\
<NewLeaseDuration>"+String::number(duration)+"</NewLeaseDuration>\r\n\
</m:AddPortMapping>\r\n\
</s:Body>\r\n\
</s:Envelope>\r\n";

		request.headers["Content-Leng.hpp"] << content.size();
		request.headers["Content-Type"] = "text/xml; charset=\"utf-8\"";
		request.headers["Soapaction"] = "urn:schemas-upnp-org:service:WANIPConnection:1#AddPortMapping";

		request.send(&sock);
		sock.write(content);

		Http::Response response;
		response.recv(&sock);

		if(response.code == 200)
		{
			sock.clear();
			return true;
		}

		String result;
		Stream &stream = result;
		sock.read(stream);

		String strErrorCode = extract(result, "errorCode");
		if(strErrorCode.empty())
		{
			LogWarn("PortMapping::UPnP", String("AddPortMapping: Unknown error"));
			return false;
		}

		int errorCode = 0;
		strErrorCode.extract(errorCode);

		if(errorCode == 718)
		{
			// The port mapping entry specified conflicts with a object assigned previously to another client

			if(i == attempts-2)
			{
				// The device is probably bogus, and the object is actually assigned to us
				remove(protocol, internal, external);
			}
			else {
				if(localAddr.isIpv4())
				{
					if(i == 0) gen = localAddr.host().dottedToInt(256) + external;
					uint32_t rnd = gen = uint32_t(22695477*gen + 1); rnd = rnd >> 17;
					external = 1024 + rnd;
				}
				else {
					external = 1024 + Random().readInt() % (49151 - 1024);
				}
			}
			continue;
		}

		if(duration && errorCode == 725)
		{
			// The NAT implementation only supports permanent lease times on port mapping
			duration = 0;
			continue;
		}


		LogWarn("PortMapping::UPnP", String("AddPortMapping: Error code " + String::number(errorCode)));
		return false;
	}

	LogWarn("PortMapping::UPnP", String("AddPortMapping: Reached max number of attempts, giving up"));
	return false;
}

bool PortMapping::UPnP::remove(Protocol protocol, uint16_t internal, uint16_t external)
{
	if(mControlUrl.empty()) return false;

	Http::Request request(mControlUrl, "POST");

	String host;
	request.headers.get("Host", host);
	Socket sock(host, seconds(10.));

	String content = "<?xml version=\"1.0\"?>\r\n\
<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n\
<s:Body>\r\n\
<m:DeletePortMapping xmlns:m=\"urn:schemas-upnp-org:service:WANIPConnection:1\">\r\n\
<NewRemoteHost></NewRemoteHost>\r\n\
<NewExternalPort>" + String::number(external) + "</NewExternalPort>\r\n\
<NewProtocol>"+(protocol == TCP ? String("TCP") : String("UDP"))+"</NewProtocol>\r\n\
</m:DeletePortMapping>\r\n\
</s:Body>\r\n\
</s:Envelope>\r\n";

	request.headers["Content-Leng.hpp"] << content.size();
	request.headers["Content-Type"] = "text/xml; charset=\"utf-8\"";
	request.headers["Soapaction"] = "urn:schemas-upnp-org:service:WANIPConnection:1#DeletePortMapping";

	request.send(&sock);
	sock.write(content);

	Http::Response response;
	response.recv(&sock);
	sock.clear();

	return (response.code == 200);
}

bool PortMapping::UPnP::parse(BinaryString &dgram)
{
	String message(dgram.begin(), dgram.end());

	StringMap headers;
	String line;
	while(message.readLine(line))
	{
		String content = line.cut(':');
		headers[line.toUpper()] = content.trimmed();
		line.clear();
	}

	String st;
	headers.get("ST", st);
	headers.get("NT", st);
	if(st.find("device:InternetGatewayDevice") == String::NotFound) return false;

	String server;
	if(headers.get("SERVER", server))
		LogDebug("PortMapping::UPnP", "Found device: " + server);

	String location;
	if(!headers.get("LOCATION", location)) return false;

	String protocol = location;
	String host = protocol.cut(':');
	while(!host.empty() && host[0] == '/') host.ignore();
	host.cut('/');
	String baseUrl = protocol + "://" + host;

	String result;
	if(Http::Get(location, &result, NULL, 2, true) != 200) return false;

	size_t pos = result.find("urn:schemas-upnp-org:service:WANIPConnection");
	if(pos == String::NotFound) return false;

	mControlUrl = extract(result, "controlURL", pos);
	if(mControlUrl.empty()) return false;
	if(mControlUrl[0] == '/') mControlUrl = baseUrl + mControlUrl;

	String content = "<?xml version=\"1.0\"?>\r\n\
<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n\
<s:Body><m:GetExternalIPAddress xmlns:m=\"urn:schemas-upnp-org:service:WANIPConnection:1\"></m:GetExternalIPAddress></s:Body>\r\n\
</s:Envelope>\r\n";

	Http::Request request(mControlUrl, "POST");
	request.headers["Content-Leng.hpp"] << content.size();
	request.headers["Content-Type"] = "text/xml; charset=\"utf-8\"";
	request.headers["Soapaction"] = "urn:schemas-upnp-org:service:WANIPConnection:1#GetExternalIPAddress";

	request.headers.get("Host", host);

	Socket sock(host, seconds(10.));
	request.send(&sock);
	sock.write(content);

	result.clear();
	Stream &stream = result;

	Http::Response response;
	response.recv(&sock);
	sock.read(stream);

	if(response.code != 200) return false;

	mExternalHost = extract(result, "NewExternalIPAddress");
	return !mExternalHost.empty();
}

String PortMapping::UPnP::extract(String &xml, const String &field, size_t pos)
{
	String beginTag = "<" + field + ">";
	String endTag = "</" + field + ">";

	size_t begin = xml.find(beginTag, pos);
	if(begin == String::NotFound) return "";
	begin+= beginTag.size();

	size_t end = xml.find(endTag, begin);
	if(end == String::NotFound) end = xml.size();

	String result = xml.substr(begin, end - begin);
	result.trim();
	return result;
}

PortMapping::FreeboxAPI::FreeboxAPI(void)
{

}

PortMapping::FreeboxAPI::~FreeboxAPI(void)
{

}

bool PortMapping::FreeboxAPI::check(String &host)
{
	LogDebug("PortMapping::FreeboxAPI", "Trying Freebox API...");

	// TODO: This is not the right method to check freebox API

	const String baseUrl = "http://mafreebox.freebox.fr";
	Http::Request request(baseUrl + "/api_version", "GET");

	String hhost;
	request.headers.get("Host", hhost);

	Socket sock(hhost, seconds(2.));
	sock.connect(hhost);
	request.send(&sock);
	mLocalAddr = sock.getLocalAddress();

	Http::Response response;
	response.recv(&sock);
	if(response.code != 200) return false;

	String apiBaseUrl, apiVersion;
	JsonSerializer(&sock) >> Object()
		.insert("api_base_url", apiBaseUrl)
		.insert("api_version", apiVersion);

	if(apiBaseUrl.empty()) return false;
	if(apiVersion.empty()) return false;

	LogDebug("PortMapping::FreeboxAPI", "Found Freebox Server");
	mFreeboxUrl = baseUrl + apiBaseUrl + "v" + apiVersion.before('.');

	FreeboxResponse fbr;
	if(!get("/connection/", fbr)) return false;
	if(!fbr.success) return false;

	return fbr.result.get("ipv4", host);
}

bool PortMapping::FreeboxAPI::add(Protocol protocol, uint16_t internal, uint16_t &external)
{
	Object object;
	object.insert("enabled", "true");
	object.insert("comment", APPNAME);
	object.insert("lan_port", internal);
	object.insert("wan_port_end", external);
	object.insert("wan_port_start", external);
	object.insert("lan_ip", mLocalAddr.host());
	object.insert("ip_proto", (protocol == TCP ? "tcp" : "udp"));
	object.insert("src_ip", "0.0.0.0");

	FreeboxResponse fbr;
	if(!put("/api/v1/fw/redir/", object, fbr)) return false;
	return fbr.success;
}

bool PortMapping::FreeboxAPI::remove(Protocol protocol, uint16_t internal, uint16_t external)
{
	return false;
}

bool PortMapping::FreeboxAPI::get(const String &url, FreeboxResponse &response)
{
	if(mFreeboxUrl.empty()) return false;

	Http::Request request(mFreeboxUrl + url, "GET");

	String host;
	request.headers.get("Host", host);

	Socket sock(host, seconds(10.));
	request.send(&sock);

	Http::Response hresponse;
	hresponse.recv(&sock);
	if(hresponse.code != 200) return false;

	JsonSerializer(&sock) >> response;
	return true;
}

bool PortMapping::FreeboxAPI::put(const String &url, Serializable &data, FreeboxResponse &response)
{
	if(mFreeboxUrl.empty()) return false;

	Http::Request request(mFreeboxUrl + url, "PUT");

	String post;
	JsonSerializer(&post) << data;

	String host;
	request.headers.get("Host", host);
	request.headers["Content-Leng.hpp"] << post.size();
	request.headers["Content-Type"] = "application/json; charset=\"utf-8\"";

	Socket sock;
	sock.connect(host);
	request.send(&sock);
	sock.write(post);

	Http::Response hresponse;
	hresponse.recv(&sock);
	if(hresponse.code != 200) return false;

	JsonSerializer(&sock) >> response;
	return true;
}

PortMapping::FreeboxAPI::FreeboxResponse::FreeboxResponse(void) :
	success(false)
{

}

void PortMapping::FreeboxAPI::FreeboxResponse::serialize(Serializer &s) const
{
	s << Object()
		.insert("success", success)
		.insert("error_code", errorCode)
		.insert("message", message)
		.insert("result", result);
}

bool PortMapping::FreeboxAPI::FreeboxResponse::deserialize(Serializer &s)
{
	success = false;
	errorCode.clear();
	message.clear();
	result.clear();

	return !!(s >> Object()
		.insert("success", success)
		.insert("error_code", errorCode)
		.insert("message", message)
		.insert("result", result));
}

bool PortMapping::FreeboxAPI::FreeboxResponse::isInlineSerializable(void) const
{
	return false;
}

}
