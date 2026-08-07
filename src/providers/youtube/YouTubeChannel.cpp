// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeChannel.hpp"

#include "Application.hpp"
#include "common/enums/MessageContext.hpp"
#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/Message.hpp"
#include "messages/MessageBuilder.hpp"
#include "messages/MessageElement.hpp"
#include "providers/youtube/YouTubeAccount.hpp"
#include "providers/youtube/YouTubeApi.hpp"
#include "singletons/Settings.hpp"

#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

using namespace Qt::Literals;
using namespace std::chrono_literals;

namespace chatterino {

namespace {

// YouTube red for author names
const QColor YOUTUBE_RED{0xFF, 0x00, 0x00};

// Minimum/maximum poll intervals (ms)
constexpr int MIN_POLL_MS = 3000;
constexpr int MAX_POLL_MS = 15000;
constexpr int DEFAULT_POLL_MS = 5000;
constexpr int ERROR_RETRY_MS = 10000;
// How long to wait before checking again for a live stream, after one ends
// or a channel handle isn't currently live.
constexpr int REDISCOVERY_RETRY_MS = 60000;

constexpr int PAGE_FETCH_TIMEOUT_MS = 15000;
constexpr int LIVE_CHAT_TIMEOUT_MS = 20000;

// Innertube client context sent with every request.
// clientVersion follows YouTube's YYYY-MMDD.HH.MM format.
constexpr const char *INNERTUBE_CLIENT_NAME = "WEB";
constexpr const char *INNERTUBE_CLIENT_VERSION = "2.20260101.00.00";
// Numeric ID for the WEB client (sent in X-YouTube-Client-Name header).
constexpr const char *INNERTUBE_CLIENT_NAME_NUM = "1";

// Headers that make requests look like a real browser
const char *USER_AGENT =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/124.0.0.0 Safari/537.36";

/// Reads the user-configurable min/max message stagger delay from
/// settings, defensively clamping to non-negative and min <= max.
std::pair<qint64, qint64> messageStaggerRangeMs()
{
    qint64 minMs =
        std::max(0, getSettings()->youtubeMessageStaggerMinMs.getValue());
    qint64 maxMs =
        std::max(0, getSettings()->youtubeMessageStaggerMaxMs.getValue());
    if (maxMs < minMs)
    {
        std::swap(minMs, maxMs);
    }
    return {minMs, maxMs};
}

/// Recursively search obj for the first string value at key.
QString findKey(const QJsonObject &obj, const QString &key)
{
    for (auto it = obj.begin(); it != obj.end(); ++it)
    {
        if (it.key() == key && it.value().isString())
        {
            return it.value().toString();
        }
        if (it.value().isObject())
        {
            auto found = findKey(it.value().toObject(), key);
            if (!found.isEmpty())
            {
                return found;
            }
        }
        if (it.value().isArray())
        {
            for (const auto &elem : it.value().toArray())
            {
                if (elem.isObject())
                {
                    auto found = findKey(elem.toObject(), key);
                    if (!found.isEmpty())
                    {
                        return found;
                    }
                }
            }
        }
    }
    return {};
}

/// Extract the live chat continuation token from ytInitialData.
/// Tries the standard liveChatRenderer path, then falls back to a recursive
/// search for any continuation string under a known continuation data key.
QString extractInitialContinuation(const QJsonObject &root)
{
    // Standard path: contents.twoColumnWatchNextResults.conversationBar
    //                .liveChatRenderer.continuations[0].<type>.continuation
    auto contents = root["contents"].toObject();
    auto two = contents["twoColumnWatchNextResults"].toObject();
    auto bar = two["conversationBar"].toObject();
    auto lcr = bar["liveChatRenderer"].toObject();
    if (!lcr.isEmpty())
    {
        auto continuations = lcr["continuations"].toArray();
        if (!continuations.isEmpty())
        {
            auto first = continuations[0].toObject();
            for (const auto *key :
                 {"reloadContinuationData", "timedContinuationData",
                  "invalidationContinuationData"})
            {
                auto inner = first[QLatin1String(key)].toObject();
                if (!inner.isEmpty())
                {
                    const auto cont = inner["continuation"].toString();
                    if (!cont.isEmpty())
                    {
                        return cont;
                    }
                }
            }
        }
    }

    // Fallback: recursively search for "continuation" inside any
    // *ContinuationData object, which is more robust against page restructures.
    return findKey(root, "continuation");
}

/// Extract the Innertube API key embedded in the page HTML.
QString extractApiKey(const QByteArray &body)
{
    static const QByteArray MARKER = "\"INNERTUBE_API_KEY\":\"";
    auto idx = body.indexOf(MARKER);
    if (idx == -1)
    {
        return {};
    }
    idx += static_cast<int>(MARKER.size());
    const auto end = body.indexOf('"', idx);
    if (end == -1)
    {
        return {};
    }
    return QString::fromUtf8(body.mid(idx, end - idx));
}

/// Extract the video owner channel's canonical URL path (e.g. "@somehandle"
/// or "channel/UCxxxx") from a watch page's browseEndpoint data, so a video
/// opened by a fixed video ID can still fall back to following that
/// channel's next live stream once this one ends.
QString extractChannelPath(const QByteArray &body)
{
    static const QByteArray MARKER = "\"canonicalBaseUrl\":\"";
    auto idx = body.indexOf(MARKER);
    if (idx == -1)
    {
        return {};
    }
    idx += static_cast<int>(MARKER.size());
    const auto end = body.indexOf('"', idx);
    if (end == -1)
    {
        return {};
    }

    QString path = QString::fromUtf8(body.mid(idx, end - idx));
    path.replace(u"\\/"_s, u"/"_s);
    if (path.startsWith(u'/'))
    {
        path.remove(0, 1);
    }
    return path;
}

/// Extract the `content` attribute of a `<meta property="X" content="Y">`
/// tag from page HTML (Open Graph title/image tags), decoding the handful
/// of HTML entities YouTube commonly escapes into these attributes.
QString extractMetaContent(const QByteArray &body, const QByteArray &property)
{
    const QByteArray marker = "property=\"" + property + "\" content=\"";
    auto idx = body.indexOf(marker);
    if (idx == -1)
    {
        return {};
    }
    idx += static_cast<int>(marker.size());
    const auto end = body.indexOf('"', idx);
    if (end == -1)
    {
        return {};
    }

    QString value = QString::fromUtf8(body.mid(idx, end - idx));
    value.replace(u"&amp;"_s, u"&"_s);
    value.replace(u"&quot;"_s, u"\""_s);
    value.replace(u"&#39;"_s, u"'"_s);
    value.replace(u"&lt;"_s, u"<"_s);
    value.replace(u"&gt;"_s, u">"_s);
    return value;
}

/// Parse ytInitialData JSON from page HTML.
/// Returns the JSON document, or null if not found / parse failed.
QJsonDocument extractYtInitialData(const QByteArray &body)
{
    static const QByteArray MARKER1 = "var ytInitialData = ";
    static const QByteArray MARKER2 = "ytInitialData = ";

    int idx = body.indexOf(MARKER1);
    if (idx != -1)
    {
        idx += static_cast<int>(MARKER1.size());
    }
    else
    {
        idx = body.indexOf(MARKER2);
        if (idx == -1)
        {
            return {};
        }
        idx += static_cast<int>(MARKER2.size());
    }

    auto endIdx = body.indexOf(";</script>", idx);
    if (endIdx == -1)
    {
        endIdx = body.indexOf(';', idx);
    }
    if (endIdx == -1)
    {
        return {};
    }

    return QJsonDocument::fromJson(body.mid(idx, endIdx - idx));
}

/// Extract text from a YouTube "runs" array (list of text/emoji run objects).
QString runsToText(const QJsonArray &runs)
{
    QString result;
    for (const auto &run : runs)
    {
        auto obj = run.toObject();
        if (obj.contains("text"_L1))
        {
            result += obj["text"].toString();
        }
        else if (obj.contains("emoji"_L1))
        {
            // Replace emoji with its shortcode if available
            auto emoji = obj["emoji"].toObject();
            auto shortcuts = emoji["shortcuts"].toArray();
            if (!shortcuts.isEmpty())
            {
                result += shortcuts[0].toString();
            }
            else
            {
                // Use the emojiId as fallback
                result += emoji["emojiId"].toString();
            }
        }
    }
    return result;
}

/// Returns a cached emote for a YouTube emoji run, built from its image
/// thumbnail. Returns nullptr if the run has no usable image (the caller
/// should fall back to the shortcode/emojiId text in that case).
EmotePtr getYouTubeEmoji(const QJsonObject &emoji)
{
    static QHash<QString, EmotePtr> cache;

    const auto emojiId = emoji["emojiId"].toString();
    if (emojiId.isEmpty())
    {
        return nullptr;
    }

    auto it = cache.constFind(emojiId);
    if (it != cache.constEnd())
    {
        return it.value();
    }

    const auto thumbnails =
        emoji["image"].toObject()["thumbnails"].toArray();
    if (thumbnails.isEmpty())
    {
        return nullptr;
    }
    const auto url = thumbnails.last().toObject()["url"].toString();
    if (url.isEmpty())
    {
        return nullptr;
    }

    QString name = emojiId;
    const auto shortcuts = emoji["shortcuts"].toArray();
    if (!shortcuts.isEmpty())
    {
        name = shortcuts[0].toString();
    }

    auto emote = std::make_shared<const Emote>(Emote{
        .name = {name},
        .images = ImageSet{Image::fromAutoscaledUrl({url}, 24)},
        .tooltip = Tooltip{name},
    });
    cache.insert(emojiId, emote);
    return emote;
}

/// Append a YouTube "runs" array (list of text/emoji run objects) to a
/// message as alternating text and emote elements, rendering emoji as real
/// images (falling back to their shortcode/emojiId as text when no image
/// is available).
void appendMessageRuns(MessageBuilder &builder, const QJsonArray &runs)
{
    for (const auto &runVal : runs)
    {
        auto run = runVal.toObject();
        if (run.contains("text"_L1))
        {
            builder.emplace<TextElement>(
                run["text"].toString(),
                MessageElementFlags{MessageElementFlag::Text},
                MessageColor::Text);
            continue;
        }

        if (!run.contains("emoji"_L1))
        {
            continue;
        }

        auto emoji = run["emoji"].toObject();
        if (auto emote = getYouTubeEmoji(emoji))
        {
            builder.emplace<EmoteElement>(emote, MessageElementFlag::Emote);
            continue;
        }

        QString fallback;
        const auto shortcuts = emoji["shortcuts"].toArray();
        if (!shortcuts.isEmpty())
        {
            fallback = shortcuts[0].toString();
        }
        else
        {
            fallback = emoji["emojiId"].toString();
        }
        if (!fallback.isEmpty())
        {
            builder.emplace<TextElement>(
                fallback, MessageElementFlags{MessageElementFlag::Text},
                MessageColor::Text);
        }
    }
}

/// Returns a cached badge emote for a fixed YouTube badge icon type
/// (moderator, verified, or channel owner), backed by bundled local icons.
EmotePtr getYouTubeIconBadge(const QString &iconType)
{
    static QHash<QString, EmotePtr> cache;
    auto it = cache.constFind(iconType);
    if (it != cache.constEnd())
    {
        return it.value();
    }

    QString asset;
    QString name;
    if (iconType == u"MODERATOR"_s)
    {
        asset = u"moderator"_s;
        name = u"Moderator"_s;
    }
    else if (iconType == u"VERIFIED"_s)
    {
        asset = u"verified"_s;
        name = u"Verified"_s;
    }
    else if (iconType == u"OWNER"_s)
    {
        asset = u"owner"_s;
        name = u"Channel Owner"_s;
    }
    else
    {
        return nullptr;
    }

    auto emote = std::make_shared<const Emote>(Emote{
        .name = {name},
        .images =
            ImageSet{
                Image::fromUrl(
                    {u":/badges/youtube-%1-18.png"_s.arg(asset)}, 1.0,
                    {18, 18}),
                Image::fromUrl(
                    {u":/badges/youtube-%1-36.png"_s.arg(asset)}, .5,
                    {36, 36}),
            },
        .tooltip = Tooltip{name},
    });
    cache.insert(iconType, emote);
    return emote;
}

/// Returns a cached badge emote for a per-channel custom badge image
/// (e.g. a channel membership badge) fetched from `url`.
EmotePtr getYouTubeCustomBadge(const QString &url, const QString &tooltip)
{
    static QHash<QString, EmotePtr> cache;
    auto it = cache.constFind(url);
    if (it != cache.constEnd())
    {
        return it.value();
    }

    auto emote = std::make_shared<const Emote>(Emote{
        .name = {tooltip},
        .images = ImageSet{Image::fromAutoscaledUrl({url}, 18)},
        .tooltip = Tooltip{tooltip},
    });
    cache.insert(url, emote);
    return emote;
}

/// Parse the `authorBadges` array of a liveChatTextMessageRenderer into
/// badge emotes ready to be emplaced into a message.
std::vector<std::pair<EmotePtr, MessageElementFlag>> parseAuthorBadges(
    const QJsonObject &renderer)
{
    std::vector<std::pair<EmotePtr, MessageElementFlag>> badges;

    const auto authorBadges = renderer["authorBadges"].toArray();
    for (const auto &badgeVal : authorBadges)
    {
        auto badgeRenderer =
            badgeVal.toObject()["liveChatAuthorBadgeRenderer"].toObject();
        if (badgeRenderer.isEmpty())
        {
            continue;
        }

        const auto tooltip = badgeRenderer["tooltip"].toString();

        const auto thumbnails = badgeRenderer["customThumbnail"]
                                     .toObject()["thumbnails"]
                                     .toArray();
        if (!thumbnails.isEmpty())
        {
            // The last entry is typically the highest resolution.
            const auto url = thumbnails.last().toObject()["url"].toString();
            if (!url.isEmpty())
            {
                if (auto emote = getYouTubeCustomBadge(
                        url, tooltip.isEmpty() ? u"Member"_s : tooltip))
                {
                    badges.emplace_back(emote,
                                        MessageElementFlag::BadgeSubscription);
                }
            }
            continue;
        }

        const auto iconType =
            badgeRenderer["icon"].toObject()["iconType"].toString();
        if (auto emote = getYouTubeIconBadge(iconType))
        {
            badges.emplace_back(emote,
                                MessageElementFlag::BadgeChannelAuthority);
        }
    }

    return badges;
}

/// Build a small system notice for a deleted message, matching the style of
/// Twitch/Kick's "A message from X was deleted: ..." notices.
MessagePtr makeYouTubeDeletionMessage(const MessagePtr &original)
{
    MessageBuilder builder;
    builder->flags.set(MessageFlag::System);
    builder->flags.set(MessageFlag::DoNotTriggerNotification);
    builder->flags.set(MessageFlag::ModerationAction);

    builder.emplace<TimestampElement>();
    builder.emplace<TextElement>(u"A message from"_s, MessageElementFlag::Text,
                                 MessageColor::System);
    builder.emplace<TextElement>(original->displayName,
                                 MessageElementFlag::Username,
                                 MessageColor::System, FontStyle::ChatMediumBold);
    builder.emplace<TextElement>(u"was deleted:"_s, MessageElementFlag::Text,
                                 MessageColor::System);

    auto text = original->messageText;
    const auto limit = getSettings()->deletedMessageLengthLimit.getValue();
    if (limit > 0 && text.length() > limit)
    {
        text = text.left(limit) + u"…"_s;
    }
    builder.emplace<TextElement>(text, MessageElementFlag::Text,
                                 MessageColor::Text);

    builder->messageText = original->messageText;
    builder->searchText = original->messageText;
    return builder.release();
}

/// Handle a `removeChatItemAction`: a single message was removed by a
/// moderator (or by the author themselves).
void handleMessageDeleted(Channel &channel, const QJsonObject &action)
{
    const auto targetItemId = action["targetItemId"].toString();
    if (targetItemId.isEmpty())
    {
        return;
    }

    auto msg = channel.findMessageByID(targetItemId);
    if (!msg)
    {
        qCWarning(chatterinoYoutube)
            << "removeChatItemAction targeted unknown message id"
            << targetItemId;
        return;
    }

    msg->flags.set(MessageFlag::Disabled);
    msg->flags.set(MessageFlag::InvalidReplyTarget);

    if (!getSettings()->hideDeletionActions)
    {
        channel.addMessage(makeYouTubeDeletionMessage(msg),
                           MessageContext::Original);
    }
}

/// Handle a `removeChatItemByAuthorAction`: all of a user's messages were
/// removed, e.g. as part of a ban/timeout.
void handleAuthorMessagesDeleted(Channel &channel, const QJsonObject &action)
{
    const auto externalChannelId = action["externalChannelId"].toString();
    if (externalChannelId.isEmpty())
    {
        return;
    }

    QString authorName;
    for (const auto &msg : channel.getMessageSnapshot())
    {
        if (msg->userID != externalChannelId)
        {
            continue;
        }

        if (authorName.isEmpty())
        {
            authorName = msg->displayName;
        }

        msg->flags.set(MessageFlag::Disabled);
        msg->flags.set(MessageFlag::InvalidReplyTarget);
    }

    if (authorName.isEmpty())
    {
        qCWarning(chatterinoYoutube)
            << "removeChatItemByAuthorAction targeted unknown author "
               "channel id"
            << externalChannelId;
        return;
    }

    if (!getSettings()->hideDeletionActions)
    {
        channel.addSystemMessage(
            u"YouTube: %1's messages were removed by a moderator."_s.arg(
                authorName));
    }
}

}  // namespace

namespace {

// A YouTube video ID is exactly 11 characters from [A-Za-z0-9_-].
bool looksLikeVideoId(const QString &s)
{
    if (s.size() != 11)
    {
        return false;
    }
    for (const QChar ch : s)
    {
        if (!ch.isLetterOrNumber() && ch != u'_' && ch != u'-')
        {
            return false;
        }
    }
    return true;
}

}  // namespace

YouTubeChannel::YouTubeChannel(const QString &nameOrHandle)
    : Channel(nameOrHandle, Type::YouTube)
    , videoId_(nameOrHandle)
{
}

void YouTubeChannel::initialize()
{
    const auto &nameOrHandle = this->videoId_;
    if (looksLikeVideoId(nameOrHandle))
    {
        this->addSystemMessage(
            u"YouTube: Connecting to live chat for video %1..."_s.arg(
                nameOrHandle));
        this->fetchWatchPage();
    }
    else
    {
        // Treat as channel handle — ensure it has the @ prefix
        this->handle_ = nameOrHandle.startsWith(u'@')
                            ? nameOrHandle
                            : u'@' + nameOrHandle;
        this->addSystemMessage(
            u"YouTube: Looking up live stream for %1..."_s.arg(this->handle_));
        this->fetchChannelLivePage(this->handle_);
    }
}

YouTubeChannel::~YouTubeChannel() = default;

const QString &YouTubeChannel::videoId() const
{
    return this->videoId_;
}

const QString &YouTubeChannel::title() const
{
    return this->title_;
}

const QString &YouTubeChannel::thumbnailUrl() const
{
    return this->thumbnailUrl_;
}

bool YouTubeChannel::canSendMessage() const
{
    return false;
}

bool YouTubeChannel::isLive() const
{
    return this->live_;
}

bool YouTubeChannel::canReconnect() const
{
    return true;
}

void YouTubeChannel::reconnect()
{
    if (this->handle_.isEmpty())
    {
        this->addSystemMessage(u"YouTube: Reconnecting..."_s);
        this->fetchWatchPage();
    }
    else
    {
        this->addSystemMessage(
            u"YouTube: Looking up live stream for %1..."_s.arg(
                this->handle_));
        this->fetchChannelLivePage(this->handle_);
    }
}

bool YouTubeChannel::hasModRights() const
{
    // We have no cheap way to know whether the logged-in account is
    // specifically a moderator/owner of *this* chat without an extra
    // quota-costing API call, so any non-anonymous YouTube login is treated
    // as having rights here. If it isn't actually one for this channel, the
    // API call itself will fail with a permission error.
    return !getApp()->getAccounts()->youtube.current()->isAnonymous();
}

void YouTubeChannel::deleteMessage(const QString &authorChannelId,
                                   const QDateTime &timestamp,
                                   const QString &messageText)
{
    auto weak = this->weak_from_this();
    auto reportError = [weak](const QString &error) {
        auto self = std::static_pointer_cast<YouTubeChannel>(weak.lock());
        if (!self)
        {
            return;
        }
        self->addSystemMessage(u"Failed to delete message: " % error);
    };

    auto findAndDelete = [weak, authorChannelId, timestamp, messageText,
                          reportError](const QString &liveChatId) {
        getYouTubeApi()->findMessageId(
            liveChatId, authorChannelId, timestamp, messageText,
            [weak, reportError](const ExpectedStr<QString> &res) {
                if (!weak.lock())
                {
                    return;
                }
                if (!res)
                {
                    reportError(res.error());
                    return;
                }
                getYouTubeApi()->deleteMessageById(
                    *res, [reportError](const ExpectedStr<void> &delRes) {
                        if (!delRes)
                        {
                            reportError(delRes.error());
                        }
                    });
            });
    };

    if (!this->liveChatId_.isEmpty())
    {
        findAndDelete(this->liveChatId_);
        return;
    }

    getYouTubeApi()->getLiveChatId(
        this->videoId_,
        [weak, findAndDelete, reportError](const ExpectedStr<QString> &res) {
            auto self = std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }
            if (!res)
            {
                reportError(res.error());
                return;
            }
            self->liveChatId_ = *res;
            findAndDelete(*res);
        });
}

void YouTubeChannel::setLive(bool live)
{
    if (this->live_ == live)
    {
        return;
    }
    this->live_ = live;
    this->liveStatusChanged.invoke();
}

void YouTubeChannel::scheduleRediscovery()
{
    auto weak = this->weak_from_this();
    QTimer::singleShot(REDISCOVERY_RETRY_MS, [weak] {
        auto self = std::static_pointer_cast<YouTubeChannel>(weak.lock());
        if (!self)
        {
            return;
        }
        if (self->handle_.isEmpty())
        {
            self->fetchWatchPage();
        }
        else
        {
            self->fetchChannelLivePage(self->handle_);
        }
    });
}

void YouTubeChannel::fetchChannelLivePage(const QString &handle)
{
    auto weak = this->weak_from_this();

    NetworkRequest(u"https://www.youtube.com/%1/live"_s.arg(handle))
        .header("User-Agent", USER_AGENT)
        .header("Accept-Language", "en-US,en;q=0.9")
        .followRedirects(true)
        .timeout(PAGE_FETCH_TIMEOUT_MS)
        .onSuccess([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }

            const auto &body = result.getData();

            // Extract video ID from the live page
            static const QByteArray VIDEO_ID_MARKER = "\"videoId\":\"";
            auto vidIdx = body.indexOf(VIDEO_ID_MARKER);
            if (vidIdx != -1)
            {
                vidIdx += static_cast<int>(VIDEO_ID_MARKER.size());
                auto vidEnd = body.indexOf('"', vidIdx);
                if (vidEnd != -1 && vidEnd - vidIdx == 11)
                {
                    self->videoId_ = QString::fromUtf8(body.mid(vidIdx, 11));
                }
            }

            self->apiKey_ = extractApiKey(body);
            self->title_ = extractMetaContent(body, "og:title");
            self->thumbnailUrl_ = extractMetaContent(body, "og:image");

            const auto doc = extractYtInitialData(body);
            if (doc.isNull())
            {
                self->addSystemMessage(
                    u"YouTube: Channel is not live. Will keep checking..."_s);
                self->scheduleRediscovery();
                return;
            }

            const auto continuation =
                extractInitialContinuation(doc.object());
            if (continuation.isEmpty())
            {
                self->addSystemMessage(
                    u"YouTube: Channel is not currently live. Will keep checking..."_s);
                self->scheduleRediscovery();
                return;
            }

            self->setLive(true);
            self->receivedFirstBatch_ = false;
            self->liveChatId_.clear();
            self->addSystemMessage(
                u"YouTube: Live chat found for %1, connecting..."_s.arg(
                    self->videoId_));
            self->fetchLiveChat(continuation);
        })
        .onError([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }
            qCWarning(chatterinoYoutube)
                << "Failed to fetch channel live page:"
                << result.formatError();
            self->addSystemMessage(
                u"YouTube: Failed to load channel (%1). Retrying..."_s.arg(
                    result.formatError()));
            QTimer::singleShot(ERROR_RETRY_MS, [weak] {
                auto self =
                    std::static_pointer_cast<YouTubeChannel>(weak.lock());
                if (self)
                {
                    self->fetchChannelLivePage(self->handle_);
                }
            });
        })
        .execute();
}

void YouTubeChannel::fetchWatchPage()
{
    auto weak = this->weak_from_this();

    NetworkRequest(u"https://www.youtube.com/watch?v=%1"_s.arg(this->videoId_))
        .header("User-Agent", USER_AGENT)
        .header("Accept-Language", "en-US,en;q=0.9")
        .followRedirects(true)
        .timeout(PAGE_FETCH_TIMEOUT_MS)
        .onSuccess([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }

            const auto &body = result.getData();

            self->apiKey_ = extractApiKey(body);
            self->title_ = extractMetaContent(body, "og:title");
            self->thumbnailUrl_ = extractMetaContent(body, "og:image");
            if (self->handle_.isEmpty())
            {
                // Learn the owning channel's path so that once this video's
                // stream ends, rediscovery/reconnect can search for a new
                // live stream on the same channel instead of only ever
                // re-checking this one dead video.
                self->handle_ = extractChannelPath(body);
            }

            const auto doc = extractYtInitialData(body);
            if (doc.isNull())
            {
                self->addSystemMessage(
                    u"YouTube: No live chat data found. Will keep checking..."_s);
                self->scheduleRediscovery();
                return;
            }

            const auto continuation =
                extractInitialContinuation(doc.object());
            if (continuation.isEmpty())
            {
                self->addSystemMessage(
                    u"YouTube: No live chat available. Will keep checking..."_s);
                self->scheduleRediscovery();
                return;
            }

            self->setLive(true);
            self->receivedFirstBatch_ = false;
            self->liveChatId_.clear();
            self->addSystemMessage(u"YouTube: Live chat found, connecting..."_s);
            self->fetchLiveChat(continuation);
        })
        .onError([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }
            qCWarning(chatterinoYoutube)
                << "Failed to fetch YouTube watch page:"
                << result.formatError();
            self->addSystemMessage(
                u"YouTube: Failed to load page (%1). Retrying..."_s.arg(
                    result.formatError()));
            QTimer::singleShot(ERROR_RETRY_MS, [weak] {
                auto self =
                    std::static_pointer_cast<YouTubeChannel>(weak.lock());
                if (self)
                {
                    self->fetchWatchPage();
                }
            });
        })
        .execute();
}

void YouTubeChannel::fetchLiveChat(const QString &continuation)
{
    auto weak = this->weak_from_this();

    const QJsonObject requestBody{
        {"context",
         QJsonObject{{"client",
                      QJsonObject{
                          {"clientName", INNERTUBE_CLIENT_NAME},
                          {"clientVersion", INNERTUBE_CLIENT_VERSION},
                          {"hl", "en"},
                          {"gl", "US"},
                      }}}},
        {"continuation", continuation},
    };

    // YouTube requires the API key as a query parameter to return JSON.
    // The key is extracted from the watch page; fall back to the known
    // public key if extraction failed.
    const auto key = this->apiKey_.isEmpty()
                         ? u"AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8"_s
                         : this->apiKey_;

    const auto url =
        u"https://www.youtube.com/youtubei/v1/live_chat/get_live_chat?key=%1"_s
            .arg(key);
    const auto referer =
        u"https://www.youtube.com/watch?v=%1"_s.arg(this->videoId_);

    NetworkRequest(url, NetworkRequestType::Post)
        .json(requestBody)
        .header("User-Agent", USER_AGENT)
        .header("Accept-Language", "en-US,en;q=0.9")
        .header("Origin", "https://www.youtube.com")
        .header("Referer", referer)
        .header("X-YouTube-Client-Name", INNERTUBE_CLIENT_NAME_NUM)
        .header("X-YouTube-Client-Version", INNERTUBE_CLIENT_VERSION)
        .timeout(LIVE_CHAT_TIMEOUT_MS)
        .onSuccess([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }

            const auto root = result.parseJson();
            if (root.isEmpty())
            {
                qCWarning(chatterinoYoutube)
                    << "Empty/invalid JSON from live chat API (status"
                    << result.status().value_or(0) << ")";
                self->addSystemMessage(
                    u"YouTube: Chat API returned an invalid response. Retrying..."_s);
                QTimer::singleShot(ERROR_RETRY_MS, [weak] {
                    auto self =
                        std::static_pointer_cast<YouTubeChannel>(weak.lock());
                    if (self)
                    {
                        self->fetchWatchPage();
                    }
                });
                return;
            }

            auto cc =
                root["continuationContents"].toObject()["liveChatContinuation"]
                    .toObject();
            if (cc.isEmpty())
            {
                qCWarning(chatterinoYoutube)
                    << "No liveChatContinuation in response";
                // Stream may have ended
                self->setLive(false);
                self->addSystemMessage(
                    u"YouTube: Live chat ended. Will keep checking for a new stream..."_s);
                self->scheduleRediscovery();
                return;
            }

            // Parse next continuation & timeout
            QString nextContinuation;
            int timeoutMs = DEFAULT_POLL_MS;

            const auto continuations = cc["continuations"].toArray();
            if (!continuations.isEmpty())
            {
                auto contObj = continuations[0].toObject();
                // Try invalidationContinuationData → continuation
                for (const auto &key :
                     {"invalidationContinuationData",
                      "timedContinuationData", "liveChatReplayContinuationData",
                      "reloadContinuationData"})
                {
                    auto inner = contObj[key].toObject();
                    if (!inner.isEmpty())
                    {
                        nextContinuation = inner["continuation"].toString();
                        if (inner.contains("timeoutMs"_L1))
                        {
                            timeoutMs =
                                std::clamp(inner["timeoutMs"].toInt(),
                                           MIN_POLL_MS, MAX_POLL_MS);
                        }
                        break;
                    }
                }
            }

            // Parse chat messages
            std::vector<std::pair<qint64, MessagePtr>> pendingMessages;
            const auto actions = cc["actions"].toArray();
            for (const auto &actionVal : actions)
            {
                auto action = actionVal.toObject();

                // Note: the live polling endpoint uses removeChatItemAction /
                // removeChatItemByAuthorAction, not the markChatItemAs...
                // actions documented for chat replay - confirmed by logging
                // raw action keys against a real moderation event.
                if (auto deleteAction =
                        action["removeChatItemAction"].toObject();
                    !deleteAction.isEmpty())
                {
                    handleMessageDeleted(*self, deleteAction);
                    continue;
                }

                if (auto banAction =
                        action["removeChatItemByAuthorAction"].toObject();
                    !banAction.isEmpty())
                {
                    handleAuthorMessagesDeleted(*self, banAction);
                    continue;
                }

                auto addChatItemAction = action["addChatItemAction"].toObject();
                if (addChatItemAction.isEmpty())
                {
                    qCWarning(chatterinoYoutube)
                        << "Unhandled live chat action with keys"
                        << action.keys();
                    continue;
                }

                auto addItem = addChatItemAction["item"].toObject();

                // Only handle regular text messages
                auto renderer =
                    addItem["liveChatTextMessageRenderer"].toObject();
                if (renderer.isEmpty())
                {
                    qCWarning(chatterinoYoutube)
                        << "Unhandled addChatItemAction item with keys"
                        << addItem.keys();
                    continue;
                }

                QString authorName =
                    renderer["authorName"].toObject()["simpleText"].toString();
                if (authorName.startsWith(u'@'))
                {
                    authorName.remove(0, 1);
                }
                const auto messageRuns =
                    renderer["message"].toObject()["runs"].toArray();
                const QString messageText = runsToText(messageRuns);
                const qint64 timestampUsec =
                    renderer["timestampUsec"].toString().toLongLong();

                if (authorName.isEmpty() || messageText.isEmpty())
                {
                    continue;
                }

                MessageBuilder builder;
                builder->id = renderer["id"].toString();
                builder->channelName = self->getName();
                builder->platform = MessagePlatform::YouTube;
                builder->loginName = authorName;
                builder->displayName = authorName;
                builder->userID =
                    renderer["authorExternalChannelId"].toString();
                builder->messageText = messageText;
                builder->searchText = authorName % u": "_s % messageText;

                builder
                    .emplace<TextElement>(u"#"_s % self->getName(),
                                          MessageElementFlag::ChannelName,
                                          MessageColor::System)
                    ->setLink({Link::JumpToChannel,
                              u":youtube:"_s % self->getName()});

                if (timestampUsec > 0)
                {
                    builder->serverReceivedTime =
                        QDateTime::fromMSecsSinceEpoch(timestampUsec / 1000);
                }

                builder.emplace<TimestampElement>(
                    builder->serverReceivedTime.isValid()
                        ? builder->serverReceivedTime.toLocalTime().time()
                        : QTime::currentTime());

                for (const auto &[emote, flag] : parseAuthorBadges(renderer))
                {
                    builder.emplace<BadgeElement>(emote, flag);
                }

                builder
                    .emplace<TextElement>(
                        authorName + ':',
                        MessageElementFlags{MessageElementFlag::Username},
                        MessageColor{YOUTUBE_RED}, FontStyle::ChatMediumBold)
                    ->setLink({Link::UserInfo, authorName});

                appendMessageRuns(builder, messageRuns);

                pendingMessages.emplace_back(timestampUsec, builder.release());
            }

            // YouTube's actions array isn't reliably in chronological order
            // (likely multiple chat shards merged without a strict global
            // order) - sort so stagger deltas are never negative and
            // fillInMissingMessages' ascending-order assumption holds.
            std::stable_sort(pendingMessages.begin(), pendingMessages.end(),
                             [](const auto &a, const auto &b) {
                                 return a.first < b.first;
                             });

            if (!self->receivedFirstBatch_)
            {
                // The first poll of a connection is a catch-up batch of
                // messages that already happened (chat history), not new
                // arrivals. Use the same batch/history insert path Kick
                // uses for its channel history backfill (fillInMissingMessages)
                // instead of addMessage, so it's treated as history rather
                // than a stream of live messages.
                self->receivedFirstBatch_ = true;
                std::vector<MessagePtr> historyMessages;
                historyMessages.reserve(pendingMessages.size());
                for (auto &pending : pendingMessages)
                {
                    historyMessages.push_back(pending.second);
                }
                self->fillInMissingMessages(historyMessages);
            }
            else
            {
                // Later polls, though, can still bundle several seconds'
                // worth of genuinely new messages into one batch - display
                // those one at a time, staggered by real relative timing
                // (clamped to the user-configurable range), instead of all
                // at once.
                const auto [minStaggerMs, maxStaggerMs] =
                    messageStaggerRangeMs();
                qint64 cumulativeDelayMs = 0;
                qint64 prevTimestampUsec = 0;
                bool firstMessage = true;
                for (auto &[timestampUsec, msg] : pendingMessages)
                {
                    if (firstMessage)
                    {
                        self->addMessage(msg, MessageContext::Original);
                        firstMessage = false;
                        prevTimestampUsec = timestampUsec;
                        continue;
                    }

                    qint64 deltaMs = minStaggerMs;
                    if (timestampUsec > 0 && prevTimestampUsec > 0)
                    {
                        deltaMs = (timestampUsec - prevTimestampUsec) / 1000;
                    }
                    deltaMs =
                        std::clamp(deltaMs, minStaggerMs, maxStaggerMs);
                    cumulativeDelayMs += deltaMs;
                    if (timestampUsec > 0)
                    {
                        prevTimestampUsec = timestampUsec;
                    }

                    QTimer::singleShot(cumulativeDelayMs, [weak, msg] {
                        auto self = std::static_pointer_cast<YouTubeChannel>(
                            weak.lock());
                        if (self)
                        {
                            self->addMessage(msg, MessageContext::Original);
                        }
                    });
                }
            }

            if (nextContinuation.isEmpty())
            {
                self->setLive(false);
                self->addSystemMessage(
                    u"YouTube: Live chat ended. Will keep checking for a new stream..."_s);
                self->scheduleRediscovery();
                return;
            }

            self->scheduleNextPoll(nextContinuation, timeoutMs);
        })
        .onError([weak](const NetworkResult &result) {
            auto self =
                std::static_pointer_cast<YouTubeChannel>(weak.lock());
            if (!self)
            {
                return;
            }
            qCWarning(chatterinoYoutube)
                << "Live chat request failed:" << result.formatError();
            // On error, re-fetch the watch page to get a fresh token
            QTimer::singleShot(ERROR_RETRY_MS, [weak] {
                auto self =
                    std::static_pointer_cast<YouTubeChannel>(weak.lock());
                if (self)
                {
                    self->fetchWatchPage();
                }
            });
        })
        .execute();
}

void YouTubeChannel::scheduleNextPoll(const QString &continuation,
                                      int timeoutMs)
{
    auto weak = this->weak_from_this();
    QTimer::singleShot(timeoutMs, [weak, continuation] {
        auto self = std::static_pointer_cast<YouTubeChannel>(weak.lock());
        if (self)
        {
            self->fetchLiveChat(continuation);
        }
    });
}

}  // namespace chatterino
