// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "providers/youtube/YouTubeApi.hpp"

#include "common/network/NetworkRequest.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"

#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

#include <memory>

namespace chatterino {

using namespace Qt::Literals;

namespace {

QString formatApiError(const NetworkResult &res)
{
    auto message = res.parseJson()["error"_L1]
                      .toObject()["message"_L1]
                      .toString();
    if (!message.isEmpty())
    {
        return message;
    }
    return res.formatError();
}

}  // namespace

YouTubeApi *YouTubeApi::instance()
{
    static std::unique_ptr<YouTubeApi> api{new YouTubeApi};
    return api.get();
}

void YouTubeApi::getLiveChatId(const QString &videoId, Callback<QString> cb)
{
    QString url =
        u"https://www.googleapis.com/youtube/v3/videos?part=liveStreamingDetails&id="_s %
        QString::fromUtf8(QUrl::toPercentEncoding(videoId));

    NetworkRequest(url)
        .header("Authorization"_ba, "Bearer "_ba + this->authToken_)
        .onError([cb](const NetworkResult &res) {
            cb(makeUnexpected(formatApiError(res)));
        })
        .onSuccess([cb](const NetworkResult &res) {
            auto items = res.parseJson()["items"_L1].toArray();
            if (items.isEmpty())
            {
                cb(makeUnexpected(u"Video not found."_s));
                return;
            }
            auto liveChatId = items.at(0)
                                  .toObject()["liveStreamingDetails"_L1]
                                  .toObject()["activeLiveChatId"_L1]
                                  .toString();
            if (liveChatId.isEmpty())
            {
                cb(makeUnexpected(u"This video has no active live chat."_s));
                return;
            }
            cb(liveChatId);
        })
        .execute();
}

void YouTubeApi::findMessageId(const QString &liveChatId,
                               const QString &authorChannelId,
                               const QDateTime &timestamp,
                               const QString &messageText, Callback<QString> cb)
{
    QString url =
        u"https://www.googleapis.com/youtube/v3/liveChat/messages"
        u"?part=snippet,authorDetails&maxResults=2000&liveChatId="_s %
        QString::fromUtf8(QUrl::toPercentEncoding(liveChatId));

    NetworkRequest(url)
        .header("Authorization"_ba, "Bearer "_ba + this->authToken_)
        .onError([cb](const NetworkResult &res) {
            cb(makeUnexpected(formatApiError(res)));
        })
        .onSuccess([cb, authorChannelId, timestamp,
                    messageText](const NetworkResult &res) {
            auto items = res.parseJson()["items"_L1].toArray();

            QString bestId;
            qint64 bestDeltaMs = -1;

            for (const auto &itemVal : items)
            {
                auto item = itemVal.toObject();
                auto snippet = item["snippet"_L1].toObject();
                auto author = item["authorDetails"_L1].toObject();

                if (author["channelId"_L1].toString() != authorChannelId)
                {
                    continue;
                }
                if (snippet["displayMessage"_L1].toString() != messageText)
                {
                    continue;
                }

                auto published = QDateTime::fromString(
                    snippet["publishedAt"_L1].toString(), Qt::ISODateWithMs);
                if (!published.isValid())
                {
                    continue;
                }

                qint64 deltaMs = qAbs(published.msecsTo(timestamp));
                if (bestDeltaMs < 0 || deltaMs < bestDeltaMs)
                {
                    bestDeltaMs = deltaMs;
                    bestId = item["id"_L1].toString();
                }
            }

            if (bestId.isEmpty())
            {
                cb(makeUnexpected(
                    u"Couldn't find this message via the YouTube API - it "
                    u"may be too old, or this account may not have "
                    u"moderator access to this chat."_s));
                return;
            }

            cb(bestId);
        })
        .execute();
}

void YouTubeApi::deleteMessageById(const QString &messageId, Callback<void> cb)
{
    QString url =
        u"https://www.googleapis.com/youtube/v3/liveChat/messages?id="_s %
        QString::fromUtf8(QUrl::toPercentEncoding(messageId));

    NetworkRequest(url, NetworkRequestType::Delete)
        .header("Authorization"_ba, "Bearer "_ba + this->authToken_)
        .onError([cb](const NetworkResult &res) {
            cb(makeUnexpected(formatApiError(res)));
        })
        .onSuccess([cb](const NetworkResult & /*res*/) {
            cb(ExpectedStr<void>{});
        })
        .execute();
}

void YouTubeApi::setAuth(const QString &authToken)
{
    this->authToken_ = authToken.toUtf8();
}

YouTubeApi *getYouTubeApi()
{
    return YouTubeApi::instance();
}

}  // namespace chatterino
