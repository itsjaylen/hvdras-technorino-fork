// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

#include "messages/Emote.hpp"
#include "providers/twitch/TwitchBadge.hpp"

#include <IrcTagsRef>
#include <QString>
#include <QVariantMap>

#include <unordered_map>

namespace chatterino {

struct TwitchEmoteOccurrence {
    int start;
    int end;
    EmotePtr ptr;
    EmoteName name;
    /// True for an occurrence parsed from the `gifs` tag (Twitch's inline
    /// chat GIFs) rather than the `emotes` tag - splices into the message
    /// the same way a real emote does, but renders as a TwitchGifElement
    /// instead of an EmoteElement so it has its own visibility/size
    /// settings. See parseTwitchGifs.
    bool isGif = false;
    /// For a GIF occurrence, the same GIF at its original (full-size,
    /// undownsized) quality - tried if `ptr`'s downsized rendition fails to
    /// load (e.g. Giphy doesn't have a "downsized" variant for it), before
    /// giving up and falling back to text entirely. Null for real emote
    /// occurrences.
    EmotePtr fallbackPtr;

    bool operator==(const TwitchEmoteOccurrence &other) const
    {
        return std::tie(this->start, this->end, this->ptr, this->name) ==
               std::tie(other.start, other.end, other.ptr, other.name);
    }
};

/// @brief Parses the `badge-info` tag of an IRC message
///
/// The `badge-info` tag maps badge-names to a value. Subscriber badges, for
/// example, are mapped to the number of months the chatter is subscribed for.
///
/// **Example**:
/// `badge-info=subscriber/22` would be parsed as `{ subscriber => 22 }`
///
/// @param tags The tags of the IRC message
/// @returns A map of badge-names to their values
std::unordered_map<QString, QString> parseBadgeInfoTag(Communi::TagsRef tags);

/// @brief Parses the badges from the specified tag of an IRC message
///
/// All `badges`-type tags contain a comma separated list of key-value elements
/// which make up the name and version of each badge.
///
/// **Example**:
/// `badges=broadcaster/1,subscriber/18` would be parsed as
/// `[(broadcaster, 1), (subscriber, 18)]`
///
/// @param tags The tags of the IRC message
/// @param tagName The name of the tag to read badges from
/// @returns A list of badges (name and version)
std::vector<TwitchBadge> parseBadgeTag(Communi::TagsRef tags,
                                       const QString &tagName = "badges");

/// @brief Parses Twitch emotes in an IRC message
///
/// @param tags The tags of the IRC message
/// @param content The message text. This might be shortened due to skipping
///                content at the start. `messageOffset` describes this offset.
/// @param messageOffset The offset of `content` compared to the original
///                      message text. Used for calculating indices into the
///                      message. An offset of 3, for example, indicates that
///                      `content` excludes the first three characters of the
///                      original message (`@a foo` (original message) -> `foo`
///                      (content)).
/// @returns A list of emotes and their positions
std::vector<TwitchEmoteOccurrence> parseTwitchEmotes(Communi::TagsRef tags,
                                                     const QString &content,
                                                     int messageOffset);

/// @brief Parses Twitch's inline chat GIFs (the `gifs` tag) in an IRC message
///
/// The `gifs` tag is a comma-separated list of
/// `<start position>-<end position>|<gifID>|<gifURL>` entries, using the
/// same position convention as the `emotes` tag. The returned occurrences
/// have `isGif` set and splice into the message the same way emotes do.
///
/// @param tags The tags of the IRC message
/// @param content The message text - see parseTwitchEmotes for the same
///                messageOffset caveat.
/// @param messageOffset The offset of `content` compared to the original
///                      message text - see parseTwitchEmotes.
/// @returns A list of GIF occurrences and their positions
std::vector<TwitchEmoteOccurrence> parseTwitchGifs(Communi::TagsRef tags,
                                                   const QString &content,
                                                   int messageOffset);

}  // namespace chatterino
