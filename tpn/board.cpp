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

#include "tpn/board.hpp"
#include "tpn/resource.hpp"
#include "tpn/cache.hpp"
#include "tpn/html.hpp"
#include "tpn/user.hpp"
#include "tpn/store.hpp"
#include "tpn/config.hpp"

#include "pla/jsonserializer.hpp"
#include "pla/binaryserializer.hpp"
#include "pla/object.hpp"

namespace tpn
{

Board::Board(const String &name, const String &secret, const String &displayName) :
	mName(name),
	mDisplayName(displayName),
	mSecret(secret),
	mHasNew(false),
	mUnread(0)
{
	if(!mName.empty() && mName[0] == '/') mName = mName.substr(1);
	Assert(!mName.empty());

	Interface::Instance->add(urlPrefix(), this);

	const String prefix = "/mail/" + mName;

	Set<BinaryString> digests;
	Store::Instance->retrieveValue(Store::Hash(prefix), digests);

	for(auto it = digests.begin(); it != digests.end(); ++it)
	{
		//LogDebug("Board", "Retrieved digest: " + it->toString());
		if(fetch(Network::Link::Null, prefix, "/", *it, false))
			incoming(Network::Link::Null, prefix, "/", *it);
	}

	publish(prefix);
	subscribe(prefix);
}

Board::~Board(void)
{
	Interface::Instance->remove(urlPrefix(), this);

	const String prefix = "/mail/" + mName;

	unpublish(prefix);
	unsubscribe(prefix);
}

String Board::urlPrefix(void) const
{
	std::unique_lock<std::mutex> lock(mMutex);
	return "/mail/" + mName;
}

bool Board::hasNew(void) const
{
	std::unique_lock<std::mutex> lock(mMutex);
	bool value = false;
	std::swap(mHasNew, value);
	return value;
}

int Board::unread(void) const
{
	// TODO: count unread
	return 0;
}

BinaryString Board::digest(void) const
{
	std::unique_lock<std::mutex> lock(mMutex);
	return mDigest;
}

bool Board::add(const Mail &mail, bool noIssue)
{
	{
		std::unique_lock<std::mutex> lock(mMutex);

		if(mail.empty() || mMails.contains(mail))
			return false;

		auto it = mMails.insert(mail);
		const Mail &m = *it.first;
		mUnorderedMails.append(&m);
	}

	const String prefix = "/mail/" + mName;

	if(!noIssue) issue(prefix, mail);
	process();
	publish(prefix);

	mCondition.notify_all();
	return true;
}

void Board::addMergeUrl(const String &url)
{
	std::unique_lock<std::mutex> lock(mMutex);
	mMergeUrls.insert(url);
}

void Board::removeMergeUrl(const String &url)
{
	std::unique_lock<std::mutex> lock(mMutex);
	mMergeUrls.erase(url);
}

void Board::process(void)
{
	try {
		std::unique_lock<std::mutex> lock(mMutex);

		// Write messages to temporary file
		String tempFileName = File::TempName();
		File tempFile(tempFileName, File::Truncate);
		BinarySerializer serializer(&tempFile);
		for(auto it = mMails.begin(); it != mMails.end(); ++it)
			serializer << *it;
		tempFile.close();

		// Move to cache
		Resource resource;
		resource.cache(tempFileName, mName, "mail", mSecret);

		// Retrieve digest and store it
		const String prefix = "/mail/" + mName;
		mDigest = resource.digest();
		Store::Instance->storeValue(Store::Hash(prefix), mDigest, Store::Permanent);

		LogDebug("Board::process", "Board processed: " + mDigest.toString());
	}
	catch(const Exception &e)
	{
		LogWarn("Board::process", String("Board processing failed: ") + e.what());
	}
}

bool Board::anounce(const Network::Link &link, const String &prefix, const String &path, List<BinaryString> &targets)
{
	std::unique_lock<std::mutex> lock(mMutex);
	targets.clear();

	if(mDigest.empty())
		return false;

	targets.push_back(mDigest);
	return true;
}

bool Board::incoming(const Network::Link &link, const String &prefix, const String &path, const BinaryString &target)
{
	{
		std::unique_lock<std::mutex> lock(mMutex);
		if(target == mDigest)
			return false;
	}

	if(fetch(link, prefix, path, target, true))
	{
		try {
			Resource resource(target, true);	// local only (already fetched)
			if(resource.type() != "mail")
				return false;

			bool complete = false;
			{
				std::unique_lock<std::mutex> lock(mMutex);

				Resource::Reader reader(&resource, mSecret);
				BinarySerializer serializer(&reader);
				Mail mail;
				unsigned count = 0;
				while(!!(serializer >> mail))
				{
					if(mail.empty())
						continue;

					if(!mMails.contains(mail))
					{
						auto it = mMails.insert(mail);
						const Mail &m = *it.first;
						mUnorderedMails.append(&m);

						++mUnread;
						mHasNew = true;
					}

					++count;
				}

				if(count == mMails.size())
				{
					mDigest = target;
					complete = true;
				}
			}

			if(!complete)
			{
				const String prefix = "/mail/" + mName;

				process();
				if(digest() != target)
					publish(prefix);
			}
		}
		catch(const Exception &e)
		{
			LogWarn("Board::incoming", e.what());
		}

		mCondition.notify_all();
	}

	return true;
}

bool Board::incoming(const Network::Link &link, const String &prefix, const String &path, const Mail &mail)
{
	if(add(mail, true))
	{
		++mUnread;
		mHasNew = true;
		return true;
	}

	return false;
}

void Board::http(const String &prefix, Http::Request &request)
{
	Assert(!request.url.empty());

	try {
		if(request.url == "/")
		{
			if(request.method == "POST")
			{
				if(request.post.contains("message") && !request.post["message"].empty())
				{
					Mail mail;
					mail.setContent(request.post["message"]);

					if(request.post.contains("parent"))
					{
						BinaryString parent;
						request.post["parent"].extract(parent);
						mail.setParent(parent);
					}

					if(request.post.contains("attachment"))
					{
						BinaryString attachment;
						request.post["attachment"].extract(attachment);
						if(!attachment.empty())
							mail.addAttachment(attachment);
					}

					if(request.post.contains("author"))
					{
						mail.setAuthor(request.post["author"]);
					}
					else {
						sptr<User> user = getAuthenticatedUser(request);
						if(user) {
							mail.setAuthor(user->name());
							mail.sign(user->identifier(), user->privateKey());
						}
					}

					add(mail);

					Http::Response response(request, 200);
					response.send();
				}

				throw 400;
			}

			if(request.get.contains("json"))
			{
				int next = 0;
				if(request.get.contains("next"))
					request.get["next"].extract(next);

				duration timeout = milliseconds(Config::Get("request_timeout").toInt());
				if(request.get.contains("timeout"))
					timeout = seconds(request.get["timeout"].toDouble());

				decltype(mUnorderedMails) temp;
				{
					std::unique_lock<std::mutex> lock(mMutex);

					if(next >= int(mUnorderedMails.size()))
					{
						mCondition.wait_for(lock, std::chrono::duration<double>(timeout), [this, next]() {
							return next < int(mUnorderedMails.size());
						});
					}

					temp.reserve(int(mUnorderedMails.size() - next));
					for(int i = next; i < int(mUnorderedMails.size()); ++i)
						temp.push_back(mUnorderedMails[i]);

					mUnread = 0;
					mHasNew = false;
				}

				Http::Response response(request, 200);
				response.headers["Content-Type"] = "application/json";
				response.send();

				JsonSerializer json(response.stream);
				json.setOptionalOutputMode(true);
				json << temp;
				return;
			}

			bool isPopup = request.get.contains("popup");
			bool isFrame = request.get.contains("frame");

			Http::Response response(request, 200);
			response.send();

			Html page(response.stream);

			String title;
			{
				std::unique_lock<std::mutex> lock(mMutex);
				title = (!mDisplayName.empty() ? mDisplayName : "Board " + mName);
			}

			page.header(title, isPopup || isFrame);

			if(!isFrame)
			{
				page.open("div","topmenu");
				if(isPopup) page.span(title, ".button");

// TODO: should be hidden in CSS
#ifndef ANDROID
				if(!isPopup)
				{
					String popupUrl = Http::AppendParam(request.fullUrl, "popup");
					page.raw("<a class=\"button\" href=\""+popupUrl+"\" target=\"_blank\" onclick=\"return popup('"+popupUrl+"','/');\">Popup</a>");
				}
#endif

				page.close("div");
			}

			page.open("div", ".replypanel");

			sptr<User> user = getAuthenticatedUser(request);
			if(user)
			{
				page.javascript("var TokenMail = '"+user->generateToken("mail")+"';\n\
						var TokenDirectory = '"+user->generateToken("directory")+"';\n\
						var TokenContact = '"+user->generateToken("contact")+"';\n\
						var UrlSelector = '"+user->urlPrefix()+"/myself/files/?json';\n\
						var UrlUpload = '"+user->urlPrefix()+"/files/_upload/?json';");

				page.raw("<a class=\"button\" href=\"#\" onclick=\"createFileSelector(UrlSelector, '#fileSelector', 'input.attachment', 'input.attachmentname', UrlUpload); return false;\"><img alt=\"File\" src=\"/static/paperclip.png\"></a>");
			}

			page.openForm("#", "post", "boardform");
			page.textarea("input");
			page.input("hidden", "attachment");
			page.input("hidden", "attachmentname");
			page.closeForm();
			page.close("div");
			page.div("","#attachedfile.attachedfile");
			page.div("", "#fileSelector.fileselector");

			if(isPopup) page.open("div", "board");
			else page.open("div", "board.box");

			page.open("div", "mail");
			page.open("p"); page.text("No messages"); page.close("p");
			page.close("div");

			page.close("div");

			page.javascript("function post() {\n\
					var message = $(document.boardform.input).val();\n\
					var attachment = $(document.boardform.attachment).val();\n\
					if(!message) return false;\n\
					var fields = {};\n\
					fields['message'] = message;\n\
					if(attachment) fields['attachment'] = attachment;\n\
					$.post('"+prefix+request.url+"', fields)\n\
						.fail(function(jqXHR, textStatus) {\n\
							alert('The message could not be sent.');\n\
						});\n\
					$(document.boardform.input).val('');\n\
					$(document.boardform.attachment).val('');\n\
					$(document.boardform.attachmentname).val('');\n\
					$('#attachedfile').hide();\n\
				}\n\
				$(document.boardform).submit(function() {\n\
					post();\n\
					return false;\n\
				});\n\
				$(document.boardform.attachment).change(function() {\n\
					$('#attachedfile').html('');\n\
					$('#attachedfile').hide();\n\
					var filename = $(document.boardform.attachmentname).val();\n\
					if(filename != '') {\n\
						$('#attachedfile').append('<img class=\"icon\" src=\"/static/file.png\">');\n\
						$('#attachedfile').append('<span class=\"filename\">'+filename+'</span>');\n\
						$('#attachedfile').show();\n\
					}\n\
					$(document.boardform.input).focus();\n\
					if($(document.boardform.input).val() == '') {\n\
						$(document.boardform.input).val(filename);\n\
						$(document.boardform.input).select();\n\
					}\n\
				});\n\
				$('#attachedfile').hide();");

			unsigned refreshPeriod = 2000;
			page.javascript("setMailReceiver('"+Http::AppendParam(request.fullUrl, "json")+"','#mail', "+String::number(refreshPeriod)+");");

			{
				std::unique_lock<std::mutex> lock(mMutex);
				for(auto &url : mMergeUrls)
					page.javascript("setMailReceiver('"+Http::AppendParam(url, "json")+"','#mail', "+String::number(refreshPeriod)+");");
			}

			page.footer();
			return;
		}
	}
	catch(const Exception &e)
	{
		LogWarn("AddressBook::http", e.what());
		throw 500;
	}

	throw 404;
}

}
