// On-chain gift and wallet-withdrawal tools for the Telegram Desktop MCP integration.
//
// Buying, listing, repricing and offering on-chain gifts ends in a TON
// transaction the local wallet signs behind the vault unlock, so those stay
// in the app. What is here needs no wallet key: reading sale state, declining
// an offer, and the two moves Telegram gates on the temporary 2FA password.
//
// For license and copyright information please follow this link:
// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

#include "mcp_server_includes.h"

#include "main/main_session.h"

namespace MCP {
namespace {

constexpr auto kTmpPasswordMissing = "No temporary 2FA password is cached. "
	"Telegram issues one for about 10 minutes after the 2FA password is "
	"entered in the app (for example when transferring a gift); enter it "
	"there, then call again. This server never holds the 2FA password.";

[[nodiscard]] QJsonObject UniqueGiftJson(const MTPDstarGiftUnique &gift) {
	auto result = QJsonObject();
	result["gift_id"] = QString::number(gift.vid().v);
	result["title"] = qs(gift.vtitle());
	result["slug"] = qs(gift.vslug());
	result["num"] = gift.vnum().v;
	result["onchain_sale"] = gift.is_onchain_sale();
	result["fixed_sale"] = gift.is_fixed_sale();
	result["my_sale"] = gift.is_my_sale();
	result["resale_ton_only"] = gift.is_resale_ton_only();
	if (const auto address = gift.vgift_address()) {
		result["gift_address"] = qs(*address);
	}
	if (const auto owner = gift.vowner_address()) {
		result["owner_address"] = qs(*owner);
	}
	if (const auto minimum = gift.voffer_min_amount()) {
		result["offer_min_amount_nano"] = qint64(minimum->v);
	}
	if (const auto fee = gift.vfee_bps()) {
		result["fee_bps"] = fee->v;
	}
	return result;
}

} // namespace

QJsonObject Server::toolListOnchainGifts(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto offset = args.value("offset").toString();
	const auto limit = clampLimit(args.value("limit").toInt(50), 50, 100);
	const auto session = _session;
	return awaitMtp([&](auto done, auto fail) {
		session->api().request(MTPpayments_GetSavedStarGifts(
			MTP_flags(MTPpayments_GetSavedStarGifts::Flag::f_exclude_unlimited),
			MTP_inputPeerSelf(),
			MTP_int(0), // collection_id
			MTP_string(offset),
			MTP_int(limit)
		)).done([=](const MTPpayments_SavedStarGifts &result) {
			const auto &data = result.data();
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			auto gifts = QJsonArray();
			for (const auto &saved : data.vgifts().v) {
				const auto &fields = saved.data();
				fields.vgift().match([&](const MTPDstarGiftUnique &gift) {
					auto entry = UniqueGiftJson(gift);
					entry["on_chain"] = gift.vgift_address().has_value();
					if (const auto msgId = fields.vmsg_id()) {
						entry["msg_id"] = msgId->v;
					}
					if (const auto at = fields.vcan_export_at()) {
						entry["can_withdraw_at"] = at->v;
					}
					gifts.append(entry);
				}, [](const MTPDstarGift &) {
				});
			}
			auto value = QJsonObject();
			value["success"] = true;
			value["gifts"] = gifts;
			value["count"] = gifts.size();
			if (const auto next = data.vnext_offset()) {
				value["next_offset"] = qs(*next);
			}
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolDeclineOnchainGiftOffer(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto msgId = args.value("offer_msg_id").toInt();
	if (msgId <= 0) {
		return toolError("offer_msg_id is required: the id of the offer "
			"service message in the chat with the buyer");
	}
	if (!args.value("confirm").toBool(false)) {
		return QJsonObject{
			{ "success", true },
			{ "declined", false },
			{ "offer_msg_id", msgId },
			{ "next_step", "Nothing was sent. Call again with confirm: true "
				"to decline this offer; the buyer's TON is refunded." },
		};
	}
	const auto session = _session;
	return awaitMtp([&](auto done, auto fail) {
		session->api().request(MTPpayments_DeclineStarGiftOnchainOffer(
			MTP_int(msgId)
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			done(QJsonObject{
				{ "success", true },
				{ "declined", true },
				{ "offer_msg_id", msgId },
			});
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolWithdrawGiftOnchain(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto slug = args.value("slug").toString();
	const auto msgId = args.value("msg_id").toInt();
	if (slug.isEmpty() && msgId <= 0) {
		return toolError("Pass slug or msg_id of a unique gift you own");
	}
	const auto keep = args.value("keep_on_profile").toBool(true);
	const auto password = _session->validTmpPassword();
	auto preview = QJsonObject{
		{ "slug", slug },
		{ "msg_id", msgId },
		{ "keep_on_profile", keep },
		{ "tmp_password_cached", !password.isEmpty() },
	};
	if (password.isEmpty()) {
		auto error = toolError(kTmpPasswordMissing);
		error["request"] = preview;
		return error;
	}
	if (!args.value("confirm").toBool(false)) {
		return QJsonObject{
			{ "success", true },
			{ "withdrawn", false },
			{ "request", preview },
			{ "next_step", "Nothing was sent. Call again with confirm: true "
				"to move this gift to the TON blockchain." },
		};
	}
	using Flag = MTPpayments_WithdrawStarGift::Flag;
	const auto input = (msgId > 0)
		? MTP_inputSavedStarGiftUser(MTP_int(msgId))
		: MTP_inputSavedStarGiftSlug(MTP_string(slug));
	const auto session = _session;
	return awaitMtp([&](auto done, auto fail) {
		session->api().request(MTPpayments_WithdrawStarGift(
			MTP_flags(keep ? Flag::f_keep_on_profile : Flag()),
			input,
			MTP_bytes(password)
		)).done([=](const MTPBool &result) {
			done(QJsonObject{
				{ "success", mtpIsTrue(result) },
				{ "withdrawn", mtpIsTrue(result) },
				{ "request", preview },
			});
		}).fail([=](const MTP::Error &error) {
			if (error.type().startsWith(u"PASSWORD_"_q)) {
				session->dropTmpPassword(password);
			}
			fail(error.type());
		}).send();
	}, 30000);
}

QJsonObject Server::withdrawEarningsToWallet(const QJsonObject &args) {
	const auto ton = (args.value("method").toString("ton") != "stars");
	const auto amount = args.value("amount").toVariant().toLongLong();
	const auto channelId = args.value("channel_id").toVariant().toLongLong();
	const auto peer = channelId ? resolvePeer(channelId) : nullptr;
	if (channelId && !peer) {
		return toolError(QString("No chat %1 in this client's data"
			).arg(channelId));
	}
	const auto password = _session->validTmpPassword();
	auto preview = QJsonObject{
		{ "to_wallet", true },
		{ "currency", ton ? "ton" : "stars" },
		{ "amount", amount ? QJsonValue(amount) : QJsonValue("all") },
		{ "channel_id", channelId },
		{ "tmp_password_cached", !password.isEmpty() },
	};
	if (password.isEmpty()) {
		auto error = toolError(kTmpPasswordMissing);
		error["request"] = preview;
		return error;
	}
	if (!args.value("confirm").toBool(false)) {
		return QJsonObject{
			{ "success", true },
			{ "withdrawn", false },
			{ "request", preview },
			{ "next_step", "Nothing was sent. Call again with confirm: true "
				"to withdraw to this account's TON wallet." },
		};
	}
	using Flag = MTPpayments_withdrawStarsRevenueToWallet::Flag;
	const auto flags = (ton ? Flag::f_ton : Flag())
		| (amount ? Flag::f_amount : Flag());
	const auto input = peer ? peer->input() : MTP_inputPeerSelf();
	const auto session = _session;
	return awaitMtp([&](auto done, auto fail) {
		session->api().request(MTPpayments_WithdrawStarsRevenueToWallet(
			MTP_flags(flags),
			input,
			MTP_long(amount),
			MTP_bytes(password)
		)).done([=](const MTPBool &result) {
			done(QJsonObject{
				{ "success", mtpIsTrue(result) },
				{ "withdrawn", mtpIsTrue(result) },
				{ "request", preview },
			});
		}).fail([=](const MTP::Error &error) {
			if (error.type().startsWith(u"PASSWORD_"_q)) {
				session->dropTmpPassword(password);
			}
			fail(error.type());
		}).send();
	}, 30000);
}

} // namespace MCP
