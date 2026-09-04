// SPDX-FileCopyrightText: 2024 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/twitch/TwitchIrc.hpp"

#include "Application.hpp"
#include "common/Aliases.hpp"
#include "common/QLogging.hpp"
#include "controllers/emotes/EmoteController.hpp"
#include "messages/Image.hpp"
#include "providers/twitch/TwitchEmotes.hpp"
#include "util/IrcHelpers.hpp"

#include <mutex>
#include <unordered_map>

namespace {

using namespace chatterino;

void appendTwitchEmoteOccurrences(const QString &emote,
                                  std::vector<TwitchEmoteOccurrence> &vec,
                                  const std::vector<int> &correctPositions,
                                  const QString &originalMessage,
                                  int messageOffset)
{
    auto *app = getApp();
    if (!emote.contains(':'))
    {
        return;
    }

    auto parameters = emote.split(':');

    if (parameters.length() < 2)
    {
        return;
    }

    auto id = EmoteId{parameters.at(0)};

    auto occurrences = parameters.at(1).split(',');

    for (const QString &occurrence : occurrences)
    {
        auto coords = occurrence.split('-');

        if (coords.length() < 2)
        {
            return;
        }

        auto from = coords.at(0).toUInt() - messageOffset;
        auto to = coords.at(1).toUInt() - messageOffset;
        auto maxPositions = correctPositions.size();
        if (from > to || to >= maxPositions)
        {
            // Emote coords are out of range
            qCDebug(chatterinoTwitch)
                << "Emote coords" << from << "-" << to << "are out of range ("
                << maxPositions << ")";
            return;
        }

        auto start = correctPositions[from];
        auto end = correctPositions[to];
        if (start > end || start < 0 || end > originalMessage.length())
        {
            // Emote coords are out of range from the modified character positions
            qCDebug(chatterinoTwitch) << "Emote coords" << from << "-" << to
                                      << "are out of range after offsets ("
                                      << originalMessage.length() << ")";
            return;
        }

        auto name = EmoteName{originalMessage.mid(start, end - start + 1)};
        TwitchEmoteOccurrence emoteOccurrence{
            start,
            end,
            app->getEmotes()->getTwitchEmotes()->getOrCreateEmote(id, name),
            name,
        };
        if (emoteOccurrence.ptr == nullptr)
        {
            qCDebug(chatterinoTwitch)
                << "nullptr" << emoteOccurrence.name.string;
        }
        vec.push_back(std::move(emoteOccurrence));
    }
}

/// Appends one `<start>-<end>|<gifID>|<gifURL>` entry from the `gifs` tag.
/// Mirrors appendTwitchEmoteOccurrences' position handling exactly (same
/// tag convention), but builds a synthetic one-off Emote from the GIF URL
/// instead of looking one up in a known emote set, cached by gif ID so
/// repeated GIFs across messages reuse the same Emote/Image.
void appendTwitchGifOccurrence(const QString &gif,
                               std::vector<TwitchEmoteOccurrence> &vec,
                               const std::vector<int> &correctPositions,
                               const QString &originalMessage,
                               int messageOffset)
{
    auto parts = gif.split('|');
    if (parts.size() < 3)
    {
        return;
    }

    auto coords = parts.at(0).split('-');
    if (coords.length() < 2)
    {
        return;
    }

    auto from = coords.at(0).toUInt() - messageOffset;
    auto to = coords.at(1).toUInt() - messageOffset;
    auto maxPositions = correctPositions.size();
    if (from > to || to >= maxPositions)
    {
        qCDebug(chatterinoTwitch)
            << "GIF coords" << from << "-" << to << "are out of range ("
            << maxPositions << ")";
        return;
    }

    auto start = correctPositions[from];
    auto end = correctPositions[to];
    if (start > end || start < 0 || end > originalMessage.length())
    {
        qCDebug(chatterinoTwitch) << "GIF coords" << from << "-" << to
                                  << "are out of range after offsets ("
                                  << originalMessage.length() << ")";
        return;
    }

    auto gifId = parts.at(1);
    // Defensive: rejoin in case the URL itself ever contains a '|'.
    auto originalUrl = parts.mid(2).join('|');
    if (gifId.isEmpty() || originalUrl.isEmpty())
    {
        return;
    }

    // Giphy serves several size variants of the same GIF at the same path,
    // differing only in filename (e.g. "giphy-downsized.gif" instead of
    // the original "giphy.gif"). Twitch's GIF picker always sends the
    // original, full-size rendition, which for a typical multi-second,
    // many-frame GIF easily decodes to tens of MB - past the 20MB in-RAM
    // cap Image::actuallyLoad() enforces for every image in the app (which
    // silently marks the load as failed when hit), and slow to even
    // download in the first place. Prefer the downsized rendition when the
    // URL looks like a standard giphy media URL - but in practice it still
    // falls back to text close to half the time relying on this alone
    // (exact cause unconfirmed - Image's error logging didn't exist until
    // now), so TwitchGifElement also retries the original URL if this one
    // fails to load, before giving up and falling back to text.
    auto downsizedUrl = originalUrl;
    if (downsizedUrl.contains(QStringLiteral("giphy.com/media/")))
    {
        downsizedUrl.replace(QStringLiteral("/giphy.gif"),
                             QStringLiteral("/giphy-downsized.gif"));
    }

    auto name = EmoteName{originalMessage.mid(start, end - start + 1)};

    static std::unordered_map<EmoteId, std::weak_ptr<const Emote>> cache;
    static std::mutex cacheMutex;

    auto makeGifEmote = [&](const QString &url, const QString &idSuffix) {
        auto id = EmoteId{gifId + idSuffix};
        return cachedOrMakeEmotePtr(
            Emote{
                .name = name,
                .images = ImageSet{Image::fromUrl({url}, 1, {160, 120})},
                .tooltip = Tooltip{name.string},
                .id = id,
            },
            cache, cacheMutex, id);
    };

    auto emote = makeGifEmote(downsizedUrl, QString());
    auto fallbackEmote = downsizedUrl == originalUrl
                             ? nullptr
                             : makeGifEmote(originalUrl, QStringLiteral("-orig"));

    vec.push_back(TwitchEmoteOccurrence{
        start,
        end,
        emote,
        name,
        true,
        fallbackEmote,
    });
}

}  // namespace

namespace chatterino {

std::unordered_map<QString, QString> parseBadgeInfoTag(Communi::TagsRef tags)
{
    std::unordered_map<QString, QString> infoMap;

    auto infoIt = tags.get("badge-info");
    if (!infoIt)
    {
        return infoMap;
    }

    auto info = infoIt->split(',', Qt::SkipEmptyParts);

    for (const QString &badge : info)
    {
        infoMap.emplace(slashKeyValue(badge));
    }

    return infoMap;
}

std::vector<TwitchBadge> parseBadgeTag(Communi::TagsRef tags,
                                       const QString &tagName)
{
    std::vector<TwitchBadge> b;

    auto badgesIt = tags.get(tagName);
    if (!badgesIt)
    {
        return b;
    }

    auto badges = badgesIt->split(',', Qt::SkipEmptyParts);

    for (const QString &badge : badges)
    {
        if (!badge.contains('/'))
        {
            continue;
        }

        auto pair = slashKeyValue(badge);
        b.emplace_back(TwitchBadge{pair.first, pair.second});
    }

    return b;
}

std::vector<TwitchEmoteOccurrence> parseTwitchEmotes(Communi::TagsRef tags,
                                                     const QString &content,
                                                     int messageOffset)
{
    // Twitch emotes
    std::vector<TwitchEmoteOccurrence> twitchEmotes;

    auto emotesTag = tags.get("emotes");

    if (!emotesTag)
    {
        return twitchEmotes;
    }

    QStringList emoteString = emotesTag->split('/');
    std::vector<int> correctPositions;
    for (int i = 0; i < content.size(); ++i)
    {
        if (!content.at(i).isLowSurrogate())
        {
            correctPositions.push_back(i);
        }
    }
    for (const QString &emote : emoteString)
    {
        appendTwitchEmoteOccurrences(emote, twitchEmotes, correctPositions,
                                     content, messageOffset);
    }

    return twitchEmotes;
}

std::vector<TwitchEmoteOccurrence> parseTwitchGifs(Communi::TagsRef tags,
                                                   const QString &content,
                                                   int messageOffset)
{
    std::vector<TwitchEmoteOccurrence> gifs;

    auto gifsTag = tags.get("gifs");
    if (!gifsTag)
    {
        return gifs;
    }

    QStringList gifStrings = gifsTag->split(',');
    std::vector<int> correctPositions;
    for (int i = 0; i < content.size(); ++i)
    {
        if (!content.at(i).isLowSurrogate())
        {
            correctPositions.push_back(i);
        }
    }
    for (const QString &gif : gifStrings)
    {
        appendTwitchGifOccurrence(gif, gifs, correctPositions, content,
                                  messageOffset);
    }

    return gifs;
}

}  // namespace chatterino
