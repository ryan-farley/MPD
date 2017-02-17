/*
 * Copyright 2003-2016 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"
#include "CurlStorage.hxx"
#include "storage/StoragePlugin.hxx"
#include "storage/StorageInterface.hxx"
#include "storage/FileInfo.hxx"
#include "storage/MemoryDirectoryReader.hxx"
#include "lib/curl/Global.hxx"
#include "lib/curl/Slist.hxx"
#include "lib/curl/Request.hxx"
#include "lib/curl/Handler.hxx"
#include "lib/expat/ExpatParser.hxx"
#include "fs/Traits.hxx"
#include "event/Call.hxx"
#include "event/DeferredMonitor.hxx"
#include "thread/Mutex.hxx"
#include "thread/Cond.hxx"
#include "util/ChronoUtil.hxx"
#include "util/RuntimeError.hxx"
#include "util/StringCompare.hxx"
#include "util/TimeParser.hxx"
#include "util/UriUtil.hxx"

#include <algorithm>
#include <memory>
#include <string>
#include <list>

#include <assert.h>

class CurlStorage final : public Storage {
	const std::string base;

	CurlGlobal *const curl;

public:
	CurlStorage(EventLoop &_loop, const char *_base)
		:base(_base),
		 curl(new CurlGlobal(_loop)) {}

	~CurlStorage() {
		BlockingCall(curl->GetEventLoop(), [this](){ delete curl; });
	}

	/* virtual methods from class Storage */
	StorageFileInfo GetInfo(const char *uri_utf8, bool follow) override;

	StorageDirectoryReader *OpenDirectory(const char *uri_utf8) override;

	std::string MapUTF8(const char *uri_utf8) const override;

	const char *MapToRelativeUTF8(const char *uri_utf8) const override;
};

std::string
CurlStorage::MapUTF8(const char *uri_utf8) const
{
	assert(uri_utf8 != nullptr);

	if (StringIsEmpty(uri_utf8))
		return base;

	char *uri_esc = curl_easy_escape(curl, uri_utf8, 0);	
	std::string uri = PathTraitsUTF8::Build(base.c_str(), uri_esc);
	curl_free(uri_esc);
	
	return uri;
}

const char *
CurlStorage::MapToRelativeUTF8(const char *uri_utf8) const
{
	// TODO: escape/unescape?

	return PathTraitsUTF8::Relative(base.c_str(), uri_utf8);
}

class BlockingHttpRequest : protected CurlResponseHandler, DeferredMonitor {
	std::exception_ptr postponed_error;

	bool done = false;

protected:
	CurlRequest request;

	Mutex mutex;
	Cond cond;

public:
	BlockingHttpRequest(CurlGlobal &curl, const char *uri)
		:DeferredMonitor(curl.GetEventLoop()),
		 request(curl, uri, *this) {
		// TODO: use CurlInputStream's configuration

		/* start the transfer inside the IOThread */
		DeferredMonitor::Schedule();
	}

	void Wait() {
		const std::lock_guard<Mutex> lock(mutex);
		while (!done)
			cond.wait(mutex);

		if (postponed_error)
			std::rethrow_exception(postponed_error);
	}

protected:
	void SetDone() {
		assert(!done);

		request.Stop();
		done = true;
		cond.signal();
	}

	void LockSetDone() {
		const std::lock_guard<Mutex> lock(mutex);
		SetDone();
	}

private:
	/* virtual methods from DeferredMonitor */
	void RunDeferred() final {
		assert(!done);

		request.Start();
	}

	/* virtual methods from CurlResponseHandler */
	void OnError(std::exception_ptr e) final {
		const std::lock_guard<Mutex> lock(mutex);
		postponed_error = std::move(e);
		SetDone();
	}
};

/**
 * The (relevant) contents of a "<D:response>" element.
 */
struct DavResponse {
	std::string href;
	unsigned status = 0;
	bool collection = false;
	std::chrono::system_clock::time_point mtime =
		std::chrono::system_clock::time_point::min();
	uint64_t length = 0;

	bool Check() const {
		return !href.empty();
	}
};

static unsigned
ParseStatus(const char *s)
{
	/* skip the "HTTP/1.1" prefix */
	const char *space = strchr(s, ' ');
	if (space == nullptr)
		return 0;

	return strtoul(space + 1, nullptr, 10);
}

static unsigned
ParseStatus(const char *s, size_t length)
{
	return ParseStatus(std::string(s, length).c_str());
}

static std::chrono::system_clock::time_point
ParseTimeStamp(const char *s)
{
	try {
		// TODO: make this more robust
		return ParseTimePoint(s, "%a, %d %b %Y %T %Z");
	} catch (const std::runtime_error &) {
		return std::chrono::system_clock::time_point::min();
	}
}

static std::chrono::system_clock::time_point
ParseTimeStamp(const char *s, size_t length)
{
	return ParseTimeStamp(std::string(s, length).c_str());
}

static uint64_t
ParseU64(const char *s)
{
	return strtoull(s, nullptr, 10);
}

static uint64_t
ParseU64(const char *s, size_t length)
{
	return ParseU64(std::string(s, length).c_str());
}

/**
 * A WebDAV PROPFIND request.  Each "response" element will be passed
 * to OnDavResponse() (to be implemented by a derived class).
 */
class PropfindOperation : BlockingHttpRequest, CommonExpatParser {
	CurlSlist request_headers;

	enum class State {
		ROOT,
		RESPONSE,
		HREF,
		STATUS,
		TYPE,
		MTIME,
		LENGTH,
	} state = State::ROOT;

	DavResponse response;

public:
	PropfindOperation(CurlGlobal &_curl, const char *_uri, unsigned depth)
		:BlockingHttpRequest(_curl, _uri),
		 CommonExpatParser(ExpatNamespaceSeparator{'|'})
	{
		request.SetOption(CURLOPT_CUSTOMREQUEST, "PROPFIND");

		char buffer[40];
		sprintf(buffer, "depth: %u", depth);
		request_headers.Append(buffer);

		request.SetOption(CURLOPT_HTTPHEADER, request_headers.Get());

		request.SetOption(CURLOPT_POSTFIELDS,
				  "<?xml version=\"1.0\"?>\n"
				  "<a:propfind xmlns:a=\"DAV:\">"
				  "<a:prop><a:getcontenttype/></a:prop>"
				  "<a:prop><a:getcontentlength/></a:prop>"
				  "</a:propfind>");

		// TODO: send request body
	}

	using BlockingHttpRequest::Wait;

protected:
	virtual void OnDavResponse(DavResponse &&r) = 0;

private:
	void FinishResponse() {
		if (response.Check())
			OnDavResponse(std::move(response));
		response = DavResponse();
	}

	/* virtual methods from CurlResponseHandler */
	void OnHeaders(unsigned status,
		       std::multimap<std::string, std::string> &&headers) final {
		if (status != 207)
			throw FormatRuntimeError("Status %d from WebDAV server; expected \"207 Multi-Status\"",
						 status);

		auto i = headers.find("content-type");
		if (i == headers.end() ||
		    strncmp(i->second.c_str(), "text/xml", 8) != 0)
			throw std::runtime_error("Unexpected Content-Type from WebDAV server");
	}

	void OnData(ConstBuffer<void> _data) final {
		const auto data = ConstBuffer<char>::FromVoid(_data);
		Parse(data.data, data.size, false);
	}

	void OnEnd() final {
		Parse("", 0, true);
		LockSetDone();
	}

	/* virtual methods from CommonExpatParser */
	void StartElement(const XML_Char *name,
			  gcc_unused const XML_Char **attrs) final {
		switch (state) {
		case State::ROOT:
			if (strcmp(name, "DAV:|response") == 0)
				state = State::RESPONSE;
			break;

		case State::RESPONSE:
			if (strcmp(name, "DAV:|href") == 0)
				state = State::HREF;
			else if (strcmp(name, "DAV:|status") == 0)
				state = State::STATUS;
			else if (strcmp(name, "DAV:|resourcetype") == 0)
				state = State::TYPE;
			else if (strcmp(name, "DAV:|getlastmodified") == 0)
				state = State::MTIME;
			else if (strcmp(name, "DAV:|getcontentlength") == 0)
				state = State::LENGTH;
			break;

		case State::TYPE:
			if (strcmp(name, "DAV:|collection") == 0)
				response.collection = true;
			break;

		case State::HREF:
		case State::STATUS:
		case State::LENGTH:
		case State::MTIME:
			break;
		}
	}

	void EndElement(const XML_Char *name) final {
		switch (state) {
		case State::ROOT:
			break;

		case State::RESPONSE:
			if (strcmp(name, "DAV:|response") == 0) {
				FinishResponse();
				state = State::ROOT;
			}

			break;

		case State::HREF:
			if (strcmp(name, "DAV:|href") == 0)
				state = State::RESPONSE;
			break;

		case State::STATUS:
			if (strcmp(name, "DAV:|status") == 0)
				state = State::RESPONSE;
			break;

		case State::TYPE:
			if (strcmp(name, "DAV:|resourcetype") == 0)
				state = State::RESPONSE;
			break;

		case State::MTIME:
			if (strcmp(name, "DAV:|getlastmodified") == 0)
				state = State::RESPONSE;
			break;

		case State::LENGTH:
			if (strcmp(name, "DAV:|getcontentlength") == 0)
				state = State::RESPONSE;
			break;
		}
	}

	void CharacterData(const XML_Char *s, int len) final {
		switch (state) {
		case State::ROOT:
		case State::RESPONSE:
		case State::TYPE:
			break;

		case State::HREF:
			response.href.assign(s, len);
			break;

		case State::STATUS:
			response.status = ParseStatus(s, len);
			break;

		case State::MTIME:
			response.mtime = ParseTimeStamp(s, len);
			break;

		case State::LENGTH:
			response.length = ParseU64(s, len);
			break;
		}
	}
};

/**
 * Obtain information about a single file using WebDAV PROPFIND.
 */
class HttpGetInfoOperation final : public PropfindOperation {
	StorageFileInfo info;

public:
	HttpGetInfoOperation(CurlGlobal &curl, const char *uri)
		:PropfindOperation(curl, uri, 0),
		 info(StorageFileInfo::Type::OTHER) {
	}

	const StorageFileInfo &Perform() {
		Wait();
		return info;
	}

protected:
	/* virtual methods from PropfindOperation */
	void OnDavResponse(DavResponse &&r) override {
		if (r.status != 200)
			return;

		info.type = r.collection
			? StorageFileInfo::Type::DIRECTORY
			: StorageFileInfo::Type::REGULAR;
		info.size = r.length;
		info.mtime = r.mtime;
	}
};

StorageFileInfo
CurlStorage::GetInfo(const char *uri_utf8, gcc_unused bool follow)
{
	char *uri_esc = curl_easy_escape(curl, uri_utf8, 0);
	std::string uri = base;
	uri += uri_esc;
	curl_free(uri_esc);

	return HttpGetInfoOperation(*curl, uri.c_str()).Perform();
}

gcc_pure
static const char *
UriPathOrSlash(const char *uri)
{
	const char *path = uri_get_path(uri);
	if (path == nullptr)
		path = "/";
	return path;
}

/**
 * Obtain a directory listing using WebDAV PROPFIND.
 */
class HttpListDirectoryOperation final : public PropfindOperation {
	const std::string base_path;

	MemoryStorageDirectoryReader::List entries;

public:
	HttpListDirectoryOperation(CurlGlobal &curl, const char *uri)
		:PropfindOperation(curl, uri, 1),
		 base_path(UriPathOrSlash(uri)) {}

	StorageDirectoryReader *Perform() {
		Wait();
		return ToReader();
	}

private:
	StorageDirectoryReader *ToReader() {
		return new MemoryStorageDirectoryReader(std::move(entries));
	}

	/**
	 * Convert a "href" attribute (which may be an absolute URI)
	 * to the base file name.
	 */
	gcc_pure
	StringView HrefToEscapedName(const char *href) const {
		const char *path = uri_get_path(href);
		if (path == nullptr)
			return nullptr;

		path = StringAfterPrefix(path, base_path.c_str());
		if (path == nullptr || *path == 0)
			return nullptr;

		const char *slash = strchr(path, '/');
		if (slash == nullptr)
			/* regular file */
			return path;
		else if (slash[1] == 0)
			/* trailing slash: collection; strip the slash */
			return {path, slash};
		else
			/* strange, better ignore it */
			return nullptr;
	}

protected:
	/* virtual methods from PropfindOperation */
	void OnDavResponse(DavResponse &&r) override {
		if (r.status != 200)
			return;

		const auto escaped_name = HrefToEscapedName(r.href.c_str());
		if (escaped_name.IsNull())
			return;

		// TODO: unescape
		const auto name = escaped_name;

		entries.emplace_front(std::string(name.data, name.size));

		auto &info = entries.front().info;
		info = StorageFileInfo(r.collection
				       ? StorageFileInfo::Type::DIRECTORY
				       : StorageFileInfo::Type::REGULAR);
		info.size = r.length;
		info.mtime = r.mtime;
	}
};

StorageDirectoryReader *
CurlStorage::OpenDirectory(const char *uri_utf8)
{
	char *uri_esc = curl_easy_escape(curl, uri_utf8, 0);
	std::string uri = base;
	uri += uri_esc;
	curl_free(uri_esc);

	/* collection URIs must end with a slash */
	if (uri.back() != '/')
		uri.push_back('/');

	return HttpListDirectoryOperation(*curl, uri.c_str()).Perform();
}

static Storage *
CreateCurlStorageURI(EventLoop &event_loop, const char *uri)
{
	if (strncmp(uri, "http://", 7) != 0 &&
	    strncmp(uri, "https://", 8) != 0)
		return nullptr;

	return new CurlStorage(event_loop, uri);
}

const StoragePlugin curl_storage_plugin = {
	"curl",
	CreateCurlStorageURI,
};
