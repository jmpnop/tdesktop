// Forum topic tools for the Telegram Desktop MCP integration.
//
// A forum splits one supergroup into topics, each with its own unread state.
// list_chats reports the group as a single row with one unread count, which
// hides where the unread messages actually are; these tools report the
// per-topic view the forum UI shows.
//
// For license and copyright information please follow this link:
// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

#include "mcp_server_includes.h"

#include "data/data_forum.h"
#include "data/data_forum_topic.h"
#include "base/random.h"

namespace MCP {

QJsonObject Server::toolListTopics(const QJsonObject &args) {
	const auto chatId = args["chat_id"].toVariant().toLongLong();
	const auto peer = resolvePeer(chatId);
	if (!peer) {
		return toolError(QString(
			"No chat %1 in this client's data").arg(chatId));
	}
	const auto channel = peer->asChannel();
	const auto forum = channel ? channel->forum() : nullptr;
	if (!forum) {
		return toolError(QString(
			"Chat %1 ('%2') is not a forum, so it has no topics"
			).arg(chatId).arg(peer->name()));
	}
	const auto unreadOnly = args.value("unread_only").toBool(false);
	const auto limit = clampLimit(args.value("limit").toInt(100), 100, 500);

	auto items = QJsonArray();
	auto total = 0;
	auto unreadTotal = 0;
	forum->enumerateTopics([&](not_null<Data::ForumTopic*> topic) {
		++total;
		const auto state = topic->chatListUnreadState();
		const auto unread = state.messages + state.marks;
		if (unread > 0) {
			++unreadTotal;
		}
		if ((unreadOnly && !unread) || items.size() >= limit) {
			return;
		}
		auto entry = QJsonObject();
		entry["topic_id"] = qint64(topic->rootId().bare);
		entry["title"] = topic->title();
		entry["general"] = topic->isGeneral();
		entry["closed"] = topic->closed();
		entry["pinned"] = topic->isPinnedDialog(FilterId());
		entry["unread_count"] = state.messages;
		entry["unread_marks"] = state.marks;
		entry["unread_mentions"] = state.mentions;
		entry["unread_reactions"] = state.reactions;
		entry["muted"] = topic->muted();
		// Without this a zero can mean either "nothing unread" or "this
		// client has not been told yet", and the caller cannot tell which.
		entry["unread_known"] = state.known;
		items.append(entry);
	});

	auto value = QJsonObject();
	value["success"] = true;
	value["chat_id"] = chatId;
	value["count"] = items.size();
	value["topics_total"] = total;
	value["topics_with_unread"] = unreadTotal;
	value["topics"] = items;
	return value;
}

// Wave 7 - forum topic create/edit/pin/delete (real MTP).
namespace {

// The topic id is the id of its service message, which only the returned updates carry.
[[nodiscard]] MsgId CreatedTopicId(const MTPUpdates &updates) {
	auto result = MsgId();
	const auto scan = [&](const MTPVector<MTPUpdate> &list) {
		for (const auto &update : list.v) {
			update.match([&](const MTPDupdateNewChannelMessage &data) {
				data.vmessage().match([&](const MTPDmessageService &message) {
					message.vaction().match([&](
							const MTPDmessageActionTopicCreate &) {
						result = message.vid().v;
					}, [](const auto &) {
					});
				}, [](const auto &) {
				});
			}, [](const auto &) {
			});
		}
	};
	updates.match([&](const MTPDupdates &data) {
		scan(data.vupdates());
	}, [&](const MTPDupdatesCombined &data) {
		scan(data.vupdates());
	}, [](const auto &) {
	});
	return result;
}

} // namespace

QJsonObject Server::toolCreateForumTopic(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto chatId = args["chat_id"].toVariant().toLongLong();
	const auto peer = resolvePeer(chatId);
	if (!peer || !peer->forum()) {
		return toolError(QString("Chat %1 is not a forum").arg(chatId));
	}
	const auto title = args["title"].toString().trimmed();
	if (title.isEmpty()) {
		return toolError("title is required");
	}
	const auto iconEmojiId = args.value("icon_emoji_id").toVariant().toULongLong();
	const auto hasColor = args.contains("icon_color");
	using Flag = MTPmessages_CreateForumTopic::Flag;
	const auto flags = (hasColor ? Flag::f_icon_color : Flag(0))
		| (iconEmojiId ? Flag::f_icon_emoji_id : Flag(0));
	const auto session = _session;
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPmessages_CreateForumTopic(
			MTP_flags(flags),
			peer->input(),
			MTP_string(title),
			MTP_int(args.value("icon_color").toInt()),
			MTP_long(iconEmojiId),
			MTP_long(base::RandomValue<uint64>()),
			MTPInputPeer()
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			auto value = QJsonObject();
			value["success"] = true;
			value["chat_id"] = chatId;
			value["title"] = title;
			if (const auto id = CreatedTopicId(result)) {
				value["topic_id"] = qint64(id.bare);
			}
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolEditForumTopic(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto chatId = args["chat_id"].toVariant().toLongLong();
	const auto topicId = args["topic_id"].toVariant().toLongLong();
	const auto peer = resolvePeer(chatId);
	if (!peer || !peer->forum()) {
		return toolError(QString("Chat %1 is not a forum").arg(chatId));
	}
	// Only the fields the caller names are sent; the rest keep their current value.
	using Flag = MTPmessages_EditForumTopic::Flag;
	auto flags = MTPmessages_EditForumTopic::Flags(0);
	if (args.contains("title")) flags |= Flag::f_title;
	if (args.contains("icon_emoji_id")) flags |= Flag::f_icon_emoji_id;
	if (args.contains("closed")) flags |= Flag::f_closed;
	if (args.contains("hidden")) flags |= Flag::f_hidden;
	if (flags == MTPmessages_EditForumTopic::Flags(0)) {
		return toolError("Nothing to change: pass at least one of title, "
			"icon_emoji_id, closed, hidden");
	}
	const auto session = _session;
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPmessages_EditForumTopic(
			MTP_flags(flags),
			peer->input(),
			MTP_int(topicId),
			MTP_string(args.value("title").toString()),
			MTP_long(args.value("icon_emoji_id").toVariant().toULongLong()),
			MTP_bool(args.value("closed").toBool()),
			MTP_bool(args.value("hidden").toBool())
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			auto value = QJsonObject();
			value["success"] = true;
			value["chat_id"] = chatId;
			value["topic_id"] = topicId;
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolPinForumTopic(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto chatId = args["chat_id"].toVariant().toLongLong();
	const auto topicId = args["topic_id"].toVariant().toLongLong();
	const auto pinned = args.value("pinned").toBool(true);
	const auto peer = resolvePeer(chatId);
	if (!peer || !peer->forum()) {
		return toolError(QString("Chat %1 is not a forum").arg(chatId));
	}
	const auto session = _session;
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPmessages_UpdatePinnedForumTopic(
			peer->input(),
			MTP_int(topicId),
			MTP_bool(pinned)
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			auto value = QJsonObject();
			value["success"] = true;
			value["chat_id"] = chatId;
			value["topic_id"] = topicId;
			value["pinned"] = pinned;
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolDeleteForumTopic(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto chatId = args["chat_id"].toVariant().toLongLong();
	const auto topicId = args["topic_id"].toVariant().toLongLong();
	const auto peer = resolvePeer(chatId);
	if (!peer || !peer->forum()) {
		return toolError(QString("Chat %1 is not a forum").arg(chatId));
	}
	if (topicId <= 1) {
		return toolError("The General topic cannot be deleted");
	}
	// Telegram deletes a topic's history in batches; repeat until no offset is left, as PeerMenuDeleteTopic does.
	const auto api = &_session->api();
	const auto step = std::make_shared<Fn<void()>>();
	return awaitMtp([&](auto done, auto fail) {
		const auto weak = std::weak_ptr<Fn<void()>>(step);
		*step = [=] {
			api->request(MTPmessages_DeleteTopicHistory(
				peer->input(),
				MTP_int(topicId)
			)).done([=](const MTPmessages_AffectedHistory &result) {
				if (api->applyAffectedHistory(peer, result) > 0) {
					if (const auto again = weak.lock()) {
						(*again)();
					}
					return;
				}
				if (const auto forum = peer->forum()) {
					forum->applyTopicDeleted(MsgId(topicId));
				}
				auto value = QJsonObject();
				value["success"] = true;
				value["chat_id"] = chatId;
				value["topic_id"] = topicId;
				done(value);
			}).fail([=](const MTP::Error &error) {
				fail(error.type());
			}).send();
		};
		(*step)();
	}, 60000);
}

} // namespace MCP
