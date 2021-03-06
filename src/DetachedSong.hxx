/*
 * Copyright 2003-2017 The Music Player Daemon Project
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

#ifndef MPD_DETACHED_SONG_HXX
#define MPD_DETACHED_SONG_HXX

#include "check.h"
#include "tag/Tag.hxx"
#include "Chrono.hxx"
#include "Compiler.h"

#include <string>
#include <utility>

#include <time.h>

struct LightSong;
class Storage;
class Path;

class DetachedSong {
	friend DetachedSong DatabaseDetachSong(const Storage &db,
					       const LightSong &song);

	/**
	 * An UTF-8-encoded URI referring to the song file.  This can
	 * be one of:
	 *
	 * - an absolute URL with a scheme
	 *   (e.g. "http://example.com/foo.mp3")
	 *
	 * - an absolute file name
	 *
	 * - a file name relative to the music directory
	 */
	std::string uri;

	/**
	 * The "real" URI, the one to be used for opening the
	 * resource.  If this attribute is empty, then #uri shall be
	 * used.
	 *
	 * This attribute is used for songs from the database which
	 * have a relative URI.
	 */
	std::string real_uri;

	Tag tag;

	time_t mtime = 0;

	/**
	 * Start of this sub-song within the file.
	 */
	SongTime start_time = SongTime::zero();

	/**
	 * End of this sub-song within the file.
	 * Unused if zero.
	 */
	SongTime end_time = SongTime::zero();

public:
	explicit DetachedSong(const char *_uri)
		:uri(_uri) {}

	explicit DetachedSong(const std::string &_uri)
		:uri(_uri) {}

	explicit DetachedSong(std::string &&_uri)
		:uri(std::move(_uri)) {}

	template<typename U>
	DetachedSong(U &&_uri, Tag &&_tag)
		:uri(std::forward<U>(_uri)),
		 tag(std::move(_tag)) {}

	/**
	 * Copy data from a #LightSong instance.  Usually, you should
	 * call DatabaseDetachSong() instead, which initializes
	 * #real_uri properly using Storage::MapUTF8().
	 */
	explicit DetachedSong(const LightSong &other);

	gcc_noinline
	~DetachedSong() = default;

	/* these are declared because the user-defined destructor
	   above prevents them from being generated implicitly */
	explicit DetachedSong(const DetachedSong &) = default;
	DetachedSong(DetachedSong &&) = default;
	DetachedSong &operator=(DetachedSong &&) = default;

	gcc_pure
	explicit operator LightSong() const;

	gcc_pure
	const char *GetURI() const {
		return uri.c_str();
	}

	template<typename T>
	void SetURI(T &&_uri) {
		uri = std::forward<T>(_uri);
	}

	/**
	 * Does this object have a "real" URI different from the
	 * displayed URI?
	 */
	gcc_pure
	bool HasRealURI() const {
		return !real_uri.empty();
	}

	/**
	 * Returns "real" URI (#real_uri) and falls back to just
	 * GetURI().
	 */
	gcc_pure
	const char *GetRealURI() const {
		return (HasRealURI() ? real_uri : uri).c_str();
	}

	template<typename T>
	void SetRealURI(T &&_uri) {
		real_uri = std::forward<T>(_uri);
	}

	/**
	 * Returns true if both objects refer to the same physical
	 * song.
	 */
	gcc_pure
	bool IsSame(const DetachedSong &other) const {
		return uri == other.uri &&
			start_time == other.start_time &&
			end_time == other.end_time;
	}

	gcc_pure gcc_nonnull_all
	bool IsURI(const char *other_uri) const {
		return uri == other_uri;
	}

	gcc_pure
	bool IsRemote() const;

	gcc_pure
	bool IsFile() const {
		return !IsRemote();
	}

	gcc_pure
	bool IsAbsoluteFile() const;

	gcc_pure
	bool IsInDatabase() const;

	const Tag &GetTag() const {
		return tag;
	}

	Tag &WritableTag() {
		return tag;
	}

	void SetTag(const Tag &_tag) {
		tag = Tag(_tag);
	}

	void SetTag(Tag &&_tag) {
		tag = std::move(_tag);
	}

	void MoveTagFrom(DetachedSong &&other) {
		tag = std::move(other.tag);
	}

	/**
	 * Similar to the MoveTagFrom(), but move only the #TagItem
	 * array.
	 */
	void MoveTagItemsFrom(DetachedSong &&other) {
		tag.MoveItemsFrom(std::move(other.tag));
	}

	time_t GetLastModified() const {
		return mtime;
	}

	void SetLastModified(time_t _value) {
		mtime = _value;
	}

	SongTime GetStartTime() const {
		return start_time;
	}

	void SetStartTime(SongTime _value) {
		start_time = _value;
	}

	SongTime GetEndTime() const {
		return end_time;
	}

	void SetEndTime(SongTime _value) {
		end_time = _value;
	}

	gcc_pure
	SignedSongTime GetDuration() const;

	/**
	 * Update the #tag and #mtime.
	 *
	 * @return true on success
	 */
	bool Update();

	/**
	 * Load #tag and #mtime from a local file.
	 */
	bool LoadFile(Path path);
};

#endif
