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

#include "httpd.h"
#include "html.h"
#include "store.h"
#include "core.h"

namespace arc
{

Httpd::Httpd(int port) :
	mSock(port)
{

}

Httpd::~Httpd(void)
{
	mSock.close();	// useless
}

void Httpd::run(void)
{
	try {
		while(true)
		{
			Socket *sock = new Socket;
			mSock.accept(*sock);
			Handler *client = new Handler(this, sock);
			client->start(true); // client will destroy itself
		}
	}
	catch(const NetException &e)
	{
		return;
	}
}

Httpd::Request::Request(void)
{
	clear();
}

Httpd::Request::Request(const String &url, const String &method)
{
	clear();

	this->url = url;
	if(!method.empty()) this->method = method;
}

void Httpd::Request::send(Socket *sock)
{
	Assert(sock != NULL);
	this->sock = sock;
	Stream &stream = stream;

	String completeUrl(url);
	if(!get.empty())
	{
		if(completeUrl.find('?') == String::NotFound)
			completeUrl+= '?';

		for(	StringMap::iterator it = get.begin();
				it != get.end();
				++it)
		{
			completeUrl<<'&'<<it->first<<'='<<it->second;
		}
	}

	String postData;
	if(!post.empty())
	{
		for(	StringMap::iterator it = post.begin();
				it != post.end();
				++it)
		{
			if(!postData.empty()) postData<<'&';
			postData<<it->first<<'='<<it->second;
		}

		headers["Content-Length"] = "";
		headers["Content-Length"] << postData.size();
		headers["Content-Type"] = "application/x-www-form-urlencoded";
	}

	stream<<method<<" "<<completeUrl<<" HTTP/"<<version<<"\r\n";

	for(	StringMap::iterator it = headers.begin();
			it != headers.end();
			++it)
	{
		List<String> lines;
		it->second.remove('\r');
		it->second.explode(lines,'\n');
		for(	List<String>::iterator l = lines.begin();
				l != lines.end();
				++l)
		{
			stream<<it->first<<": "<<*l<<"\r\n";
		}
	}

	for(	StringMap::iterator it = cookies.begin();
			it != cookies.end();
			++it)
	{
		stream<<"Set-Cookie: "<< it->first<<'='<<it->second<<"\r\n";
	}

	stream<<"\r\n";

	if(!postData.empty())
		stream<<postData;
}

void Httpd::Request::recv(Socket *sock)
{
	Assert(sock != NULL);
	this->sock = sock;
	Stream &stream = stream;

	clear();

	// Read first line
	String line;
	if(!stream.readLine(line)) throw IOException("Connection closed");
	line.readString(method);
	line.readString(url);
	
	String protocol;
	line.readString(protocol);
	version = protocol.cut('/');

	if(url.empty() || version.empty() || protocol != "HTTP")
		throw 400;

	if(method != "GET" && method != "HEAD" && method != "POST")
		throw 405;

	// Read headers
	while(true)
	{
		String line;
		AssertIO(stream.readLine(line));
		if(line.empty()) break;

		String value = line.cut(':');
		line.trim();
		value.trim();
		headers.insert(line,value);
	}

	// Read cookies
	String cookie;
	if(headers.get("Cookie",cookie))
	{
		while(!cookie.empty())
		{
			String next = cookie.cut(';');
			String value = cookie.cut('=');
			cookie.trim();
			value.trim();
			cookies.insert(cookie,value);
			cookie = next;
		}
	}

	// Read URL variables
	String getData = url.cut('?');
	if(!getData.empty())
	{
		List<String> exploded;
		getData.explode(exploded,'&');
		for(	List<String>::iterator it = exploded.begin();
				it != exploded.end();
				++it)
		{
			String value = it->cut('=');
			get.insert(*it, value);
		}
	}

	// Read post variables
	if(method == "POST")
	{
		if(!headers.contains("Content-Length"))
			throw Exception("Missing Content-Length header in POST request");

		size_t size;
		String contentLength(headers["Content-Length"]);
		contentLength >> size;

		String data;
		if(stream.read(data, size) != size)
			throw IOException("Connection unexpectedly closed");

		if(headers.contains("Content-Type")
				&& headers["Content-Type"] == "application/x-www-form-urlencoded")
		{
			List<String> exploded;
			data.explode(exploded,'&');
			for(	List<String>::iterator it = exploded.begin();
					it != exploded.end();
					++it)
			{
				String value = it->cut('=');
				post.insert(*it, value);
			}
		}
	}
}

void Httpd::Request::clear(void)
{
	method = "GET";
	version = "1.0";
	url.clear();
	headers.clear();
	cookies.clear();
	get.clear();
	post.clear();
	sock = NULL;
}

Httpd::Response::Response(void)
{
	clear();
}

Httpd::Response::Response(const Request &request, int code)
{
	clear();

	this->code = code;
	this->version = request.version;
	this->sock = request.sock;
	this->headers["Content-Type"] = "text/html; charset=UTF-8";
}

void Httpd::Response::send(void)
{
	send(sock);
}

void Httpd::Response::send(Socket *sock)
{
	Assert(sock != NULL);
	this->sock = sock;
	Stream &stream = stream;
	
	if(version == "1.1" && code >= 200)
		headers["Connection"] = "Close";

	if(!headers.contains("Date"))
	{
		// TODO
		time_t rawtime;
		time(&rawtime);
		struct tm *timeinfo = localtime(&rawtime);
		char buffer[256];
		strftime(buffer, 256, "%a, %d %b %Y %H:%M:%S %Z", timeinfo);
		headers["Date"] = buffer;
	}

	if(message.empty())
	{
		switch(code)
		{
		case 100: message = "Continue";				break;
		case 200: message = "OK";					break;
		case 204: message = "No content";			break;
		case 206: message = "Partial Content";		break;
		case 301: message = "Moved Permanently"; 	break;
		case 302: message = "Found";				break;
		case 303: message = "See Other";			break;
		case 304: message = "Not Modified"; 		break;
		case 305: message = "Use Proxy"; 			break;
		case 307: message = "Temporary Redirect";	break;
		case 400: message = "Bad Request"; 			break;
		case 401: message = "Unauthorized";			break;
		case 403: message = "Forbidden"; 			break;
		case 404: message = "Not Found";			break;
		case 405: message = "Method Not Allowed";	break;
		case 406: message = "Not Acceptable";		break;
		case 408: message = "Request Timeout";		break;
		case 410: message = "Gone";								break;
		case 413: message = "Request Entity Too Large"; 		break;
		case 414: message = "Request-URI Too Long";				break;
		case 416: message = "Requested Range Not Satisfiable";	break;
		case 500: message = "Internal Server Error";			break;
		case 501: message = "Not Implemented";					break;
		case 502: message = "Bad Gateway";						break;
		case 503: message = "Service Unavailable";				break;
		case 504: message = "Gateway Timeout";					break;
		case 505: message = "HTTP Version Not Supported";		break;

		default:
			if(code < 300) message = "OK";
			else message = "Error";
			break;
		}
	}

	stream<<"HTTP/"<<version<<" "<<code<<" "<<message<<"\r\n";

	for(	StringMap::iterator it = headers.begin();
			it != headers.end();
			++it)
	{
		List<String> lines;
		it->second.remove('\r');
		it->second.explode(lines,'\n');
		for(	List<String>::iterator l = lines.begin();
				l != lines.end();
				++l)
		{
			stream<<it->first<<": "<<*l<<"\r\n";
		}
	}

	stream<<"\r\n";
}

void Httpd::Response::recv(Socket *sock)
{
	Assert(sock != NULL);
	this->sock = sock;
	Stream &stream = stream;

	clear();

	// Read first line
	String line;
	if(!stream.readLine(line)) throw IOException("Connection closed");

	String protocol;
	line.readString(protocol);
	version = protocol.cut('/');

	line.read(code);
	message = line;

	if(version.empty() || protocol != "HTTP")
		throw Exception("Invalid HTTP response");

	// Read headers
	while(true)
	{
		String line;
		AssertIO(stream.readLine(line));
		if(line.empty()) break;

		String value = line.cut(':');
		line.trim();
		value.trim();
		headers.insert(line,value);
	}
}

void Httpd::Response::clear(void)
{
	code = 200;
	version = "1.0";
	message.clear();
	version.clear();
}

Httpd::Handler::Handler(Httpd *httpd, Socket *sock) :
		mHttpd(httpd),
		mSock(sock)
{

}

Httpd::Handler::~Handler(void)
{
	delete mSock;	// deletion closes the socket
}

void Httpd::Handler::run(void)
{
	Request request;
	try {
		request.recv(mSock);

		String expect;
		if(request.headers.get("Expect",expect)
				&& expect.toLower() == "100-continue")
		{
			mSock->writeLine("HTTP/1.1 100 Continue\r\n\r\n");
		}

		process(request);
	}
	catch(int code)
	{
		Response response(request, code);
		response.headers["Content-Type"] = "text/html; charset=UTF-8";
		response.send();

		if(request.method != "HEAD")
		{
			Html page(response.sock);
			page.header(response.message);
			page.open("h1");
			page.text(String::number(response.code) + " - " + response.message);
			page.close("h1");
			page.footer();
		}
	}
}

void Httpd::Handler::process(Request &request)
{
	try {
		// TODO: registering system

		if(request.url.substr(0,6) == "/peers")
		{
			Core::Instance->http(request);
		}
		else {
			Store::Instance->http(request);
		}

		/*Response response(request);
		response.sock = mSock;

		// TODO

		response.send();
		if(request.method != "HEAD")
		{
			Html page(mSock);
			page.header("Test page");
			page.open("h1");
			page.text("Hello world !");
			page.close("h1");
			page.footer();
		}*/
	}
	catch(Exception &e)
	{
		throw 500;
	}
}

}
