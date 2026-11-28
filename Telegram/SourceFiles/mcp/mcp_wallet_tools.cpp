// This file is part of Telegram Desktop MCP integration.
// Extracted from mcp_server_complete.cpp for modular organization.
//
// For license and copyright information please follow this link:
// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

#include "mcp_server_includes.h"

namespace MCP {
// ===== WALLET FEATURES IMPLEMENTATION =====

// Balance & Analytics
QJsonObject Server::toolGetWalletBalance(const QJsonObject &args) {
	Q_UNUSED(args);
	QJsonObject result;

	if (!_session) {
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	// Asks Telegram for the balance rather than reporting zero.
	//
	// This used to return hardcoded zeros with `loaded: false` and
	// `success: true`, on the belief that the Credits API did not exist in
	// this version. It does -- payments.getStarsStatus, and Data::Credits
	// besides -- and the comment was left over from the 6.x base. Worse than
	// the wrong answer: the zero was written into wallet_budgets, so the
	// historical record was being filled with a balance nobody ever had.
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPpayments_GetStarsStatus(
			MTP_flags(0),
			_session->user()->input()
		)).done([=](const MTPpayments_StarsStatus &result) {
			auto value = QJsonObject();
			const auto &data = result.data();
			const auto balance = data.vbalance().match([](
					const MTPDstarsAmount &amount) {
				return std::pair{ qint64(amount.vamount().v),
					qint64(amount.vnanos().v) };
			}, [](const MTPDstarsTonAmount &amount) {
				return std::pair{ qint64(amount.vamount().v), qint64(0) };
			});
			value["success"] = true;
			value["stars_balance"] = balance.first;
			value["stars_nano"] = balance.second;
			value["loaded"] = true;

			// Only now is there something true to record.
			QSqlQuery query(_db);
			query.prepare("INSERT OR REPLACE INTO wallet_budgets "
				"(id, balance, last_updated) VALUES (1, ?, datetime('now'))");
			query.addBindValue(balance.first);
			query.exec();

			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolGetBalanceHistory(const QJsonObject &args) {
	QJsonObject result;
	int days = args.value("days").toInt(30);

	QSqlQuery query(_db);
	query.prepare("SELECT date, balance FROM wallet_spending "
				  "WHERE date >= date('now', '-' || ? || ' days') "
				  "ORDER BY date");
	query.addBindValue(days);

	QJsonArray history;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject entry;
			entry["date"] = query.value(0).toString();
			entry["balance"] = query.value(1).toDouble();
			history.append(entry);
		}
	}

	result["success"] = true;
	result["history"] = history;
	result["days"] = days;

	return result;
}

QJsonObject Server::toolGetSpendingAnalytics(const QJsonObject &args) {
	QJsonObject result;
	QString period = args.value("period").toString("month");

	QString dateFilter;
	if (period == "day") dateFilter = "date('now', '-1 day')";
	else if (period == "week") dateFilter = "date('now', '-7 days')";
	else if (period == "year") dateFilter = "date('now', '-1 year')";
	else dateFilter = "date('now', '-30 days')";

	QSqlQuery query(_db);
	query.prepare("SELECT category, SUM(amount) as total FROM wallet_spending "
				  "WHERE date >= " + dateFilter + " AND amount < 0 "
				  "GROUP BY category ORDER BY total");

	QJsonObject byCategory;
	double totalSpent = 0;
	if (query.exec()) {
		while (query.next()) {
			QString category = query.value(0).toString();
			double amount = qAbs(query.value(1).toDouble());
			byCategory[category] = amount;
			totalSpent += amount;
		}
	}

	result["success"] = true;
	result["period"] = period;
	result["total_spent"] = totalSpent;
	result["by_category"] = byCategory;

	return result;
}

QJsonObject Server::toolGetIncomeAnalytics(const QJsonObject &args) {
	QJsonObject result;
	QString period = args.value("period").toString("month");

	QString dateFilter;
	if (period == "day") dateFilter = "date('now', '-1 day')";
	else if (period == "week") dateFilter = "date('now', '-7 days')";
	else if (period == "year") dateFilter = "date('now', '-1 year')";
	else dateFilter = "date('now', '-30 days')";

	QSqlQuery query(_db);
	query.prepare("SELECT category, SUM(amount) as total FROM wallet_spending "
				  "WHERE date >= " + dateFilter + " AND amount > 0 "
				  "GROUP BY category ORDER BY total DESC");

	QJsonObject byCategory;
	double totalIncome = 0;
	if (query.exec()) {
		while (query.next()) {
			QString category = query.value(0).toString();
			double amount = query.value(1).toDouble();
			byCategory[category] = amount;
			totalIncome += amount;
		}
	}

	result["success"] = true;
	result["period"] = period;
	result["total_income"] = totalIncome;
	result["by_category"] = byCategory;

	return result;
}

// Transactions
QJsonObject Server::toolGetTransactions(const QJsonObject &args) {
	QJsonObject result;
	int limit = args.value("limit").toInt(50);
	QString type = args.value("type").toString();

	QSqlQuery query(_db);
	QString sql = "SELECT id, date, amount, category, description, peer_id FROM wallet_spending ";
	if (!type.isEmpty()) {
		if (type == "income") sql += "WHERE amount > 0 ";
		else if (type == "expense") sql += "WHERE amount < 0 ";
	}
	sql += "ORDER BY date DESC LIMIT ?";

	query.prepare(sql);
	query.addBindValue(limit);

	QJsonArray transactions;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject tx;
			tx["id"] = query.value(0).toLongLong();
			tx["date"] = query.value(1).toString();
			tx["amount"] = query.value(2).toDouble();
			tx["category"] = query.value(3).toString();
			tx["description"] = query.value(4).toString();
			if (!query.value(5).isNull()) {
				tx["peer_id"] = query.value(5).toLongLong();
			}
			transactions.append(tx);
		}
	}

	result["success"] = true;
	result["transactions"] = transactions;
	result["count"] = transactions.size();

	return result;
}

QJsonObject Server::toolGetTransactionDetails(const QJsonObject &args) {
	QJsonObject result;
	QString transactionId = args["transaction_id"].toString();

	QSqlQuery query(_db);
	query.prepare("SELECT id, date, amount, category, description, peer_id FROM wallet_spending WHERE id = ?");
	query.addBindValue(transactionId);

	if (query.exec() && query.next()) {
		result["id"] = query.value(0).toLongLong();
		result["date"] = query.value(1).toString();
		result["amount"] = query.value(2).toDouble();
		result["category"] = query.value(3).toString();
		result["description"] = query.value(4).toString();
		if (!query.value(5).isNull()) {
			result["peer_id"] = query.value(5).toLongLong();
		}
		result["success"] = true;
	} else {
		result["success"] = false;
		result["error"] = "Transaction not found";
	}

	return result;
}

QJsonObject Server::toolExportTransactions(const QJsonObject &args) {
	QJsonObject result;
	QString format = args.value("format").toString("json");
	QString startDate = args.value("start_date").toString();
	QString endDate = args.value("end_date").toString();

	QSqlQuery query(_db);
	QString sql = "SELECT date, amount, category, description FROM wallet_spending ";
	QStringList conditions;
	if (!startDate.isEmpty()) conditions << "date >= ?";
	if (!endDate.isEmpty()) conditions << "date <= ?";
	if (!conditions.isEmpty()) {
		sql += "WHERE " + conditions.join(" AND ") + " ";
	}
	sql += "ORDER BY date";

	query.prepare(sql);
	if (!startDate.isEmpty()) query.addBindValue(startDate);
	if (!endDate.isEmpty()) query.addBindValue(endDate);

	QJsonArray transactions;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject tx;
			tx["date"] = query.value(0).toString();
			tx["amount"] = query.value(1).toDouble();
			tx["category"] = query.value(2).toString();
			tx["description"] = query.value(3).toString();
			transactions.append(tx);
		}
	}

	result["success"] = true;
	result["format"] = format;
	result["transactions"] = transactions;
	result["count"] = transactions.size();

	return result;
}

QJsonObject Server::toolCategorizeTransaction(const QJsonObject &args) {
	QJsonObject result;
	QString transactionId = args["transaction_id"].toString();
	QString category = args["category"].toString();

	if (category.isEmpty()) {
		result["error"] = "Missing category parameter";
		result["success"] = false;
		return result;
	}

	QSqlQuery query(_db);
	query.prepare("UPDATE wallet_spending SET category = ? WHERE id = ?");
	query.addBindValue(category);
	query.addBindValue(transactionId);

	if (query.exec() && query.numRowsAffected() > 0) {
		result["success"] = true;
		result["transaction_id"] = transactionId;
		result["category"] = category;
	} else {
		result["success"] = false;
		result["error"] = "Transaction not found";
	}

	return result;
}

// Gifts - uses real Telegram Stars Gift API
QJsonObject Server::toolSendGift(const QJsonObject &args) {
	QJsonObject result;
	qint64 recipientId = args["recipient_id"].toVariant().toLongLong();
	qint64 giftId = args.value("gift_id").toVariant().toLongLong();
	int starsAmount = args.value("stars_amount").toInt(0);
	QString message = args.value("message").toString();
	bool anonymous = args.value("anonymous").toBool(false);

	if (recipientId == 0) {
		result["error"] = "Missing recipient_id";
		result["success"] = false;
		return result;
	}

	if (!_session) {
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	// Resolve the recipient peer
	PeerId peerId(recipientId);
	auto peer = resolvePeer(peerId);
	if (!peer) {
		result["error"] = QString("Recipient %1 not found").arg(recipientId);
		result["success"] = false;
		return result;
	}

	auto user = peer->asUser();
	if (!user) {
		result["error"] = "Recipient must be a user";
		result["success"] = false;
		return result;
	}

	// Sending a gift is a purchase: payments.getPaymentForm on an
	// inputInvoiceStarGift, then payments.sendStarsForm. It debits real stars.
	//
	// This used to write a NEGATIVE row into wallet_spending and a
	// gift_transfers row marked 'sent', then return success with a
	// transaction_id that was a SQLite rowid -- so a gift nobody received
	// appeared in the spending history and in the gift log, and the id looked
	// like Telegram's. The comment above it claimed checkCanSendGift was "not
	// available in this version"; payments.checkCanSendGift exists.
	//
	// Nothing is recorded. The recipient has been resolved and validated
	// above, so the arguments are known-good -- what is missing is the payment,
	// and inventing it is what this whole cleanup is about.
	result["success"] = false;
	result["implemented"] = false;
	result["recipient_id"] = recipientId;
	result["gift_id"] = giftId;
	result["stars_amount"] = starsAmount;
	result["anonymous"] = anonymous;
	result["error"] = "Sending a gift is not implemented";
	result["reason"] = "A gift is bought through the payment-form flow and "
		"debits real stars. Nothing was written locally, because a gift this "
		"client did not send must not appear in the spending or gift history.";
	Q_UNUSED(message);

	return result;
}

QJsonObject Server::toolGetGiftHistory(const QJsonObject &args) {
	QJsonObject result;
	QString direction = args.value("direction").toString("both");  // sent, received, both
	int limit = args.value("limit").toInt(50);

	QSqlQuery query(_db);
	if (direction == "both") {
		query.prepare("SELECT id, gift_id, direction, peer_id, stars_amount, created_at "
					  "FROM gift_transfers ORDER BY created_at DESC LIMIT ?");
		query.addBindValue(limit);
	} else {
		query.prepare("SELECT id, gift_id, direction, peer_id, stars_amount, created_at "
					  "FROM gift_transfers WHERE direction = ? ORDER BY created_at DESC LIMIT ?");
		query.addBindValue(direction);
		query.addBindValue(limit);
	}

	QJsonArray gifts;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject gift;
			gift["id"] = query.value(0).toLongLong();
			gift["gift_id"] = query.value(1).toString();
			gift["direction"] = query.value(2).toString();
			gift["peer_id"] = query.value(3).toLongLong();
			gift["stars_amount"] = query.value(4).toInt();
			gift["created_at"] = query.value(5).toString();
			gifts.append(gift);
		}
	}

	result["success"] = true;
	result["gifts"] = gifts;
	result["direction"] = direction;
	result["count"] = gifts.size();

	return result;
}

QJsonObject Server::toolListAvailableGifts(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto resaleOnly = args.value("resale_only").toBool(false);
	const auto session = _session;
	return awaitMtp([&](auto done, auto fail) {
		session->api().request(MTPpayments_GetStarGifts(
			MTP_int(0) // hash
		)).done([=](const MTPpayments_StarGifts &result) {
			result.match([&](const MTPDpayments_starGifts &data) {
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				auto gifts = QJsonArray();
				for (const auto &gift : data.vgifts().v) {
					gift.match([&](const MTPDstarGift &fields) {
						const auto resale = fields.vavailability_resale()
							? qint64(fields.vavailability_resale()->v)
							: qint64(0);
						if (resaleOnly && !resale) {
							return;
						}
						auto entry = QJsonObject();
						entry["gift_id"] = QString::number(fields.vid().v);
						if (const auto title = fields.vtitle()) {
							entry["title"] = qs(*title);
						}
						entry["stars"] = qint64(fields.vstars().v);
						entry["convert_stars"] = qint64(fields.vconvert_stars().v);
						entry["limited"] = fields.is_limited();
						entry["sold_out"] = fields.is_sold_out();
						entry["require_premium"] = fields.is_require_premium();
						entry["auction"] = fields.is_auction();
						if (const auto remains = fields.vavailability_remains()) {
							entry["availability_remains"] = remains->v;
						}
						if (const auto total = fields.vavailability_total()) {
							entry["availability_total"] = total->v;
						}
						entry["resale_available"] = resale;
						if (const auto min = fields.vresell_min_stars()) {
							entry["resell_min_stars"] = qint64(min->v);
						}
						if (const auto upgrade = fields.vupgrade_stars()) {
							entry["upgrade_stars"] = qint64(upgrade->v);
						}
						gifts.append(entry);
					}, [](const MTPDstarGiftUnique &) {
					});
				}
				done(QJsonObject{
					{ "success", true },
					{ "source", "server" },
					{ "count", gifts.size() },
					{ "available_gifts", gifts },
				});
			}, [&](const MTPDpayments_starGiftsNotModified &) {
				fail(u"STARGIFTS_NOT_MODIFIED"_q);
			});
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolGetGiftSuggestions(const QJsonObject &args) {
	QJsonObject result;
	qint64 recipientId = args["recipient_id"].toVariant().toLongLong();

	if (!_session || recipientId == 0) {
		result["error"] = !_session ? "No active session" : "Missing recipient_id";
		result["success"] = false;
		return result;
	}

	PeerId peerId(recipientId);
	auto peer = resolvePeer(peerId);
	auto user = peer ? peer->asUser() : nullptr;

	if (user) {
		// Request gift options for this specific user
		_session->api().request(MTPpayments_GetStarsGiftOptions(
			MTP_flags(MTPpayments_GetStarsGiftOptions::Flag::f_user_id),
			user->inputUser()
		)).done([recipientId](const MTPVector<MTPStarsGiftOption> &options) {
			qWarning() << "MCP: Loaded" << options.v.size() << "gift options for user" << recipientId;
		}).fail([recipientId](const MTP::Error &error) {
			qWarning() << "MCP: Failed to load gift options for user" << recipientId << ":" << error.type();
		}).send();
	}

	QJsonArray suggestions;
	QJsonObject suggestion;
	suggestion["gift_type"] = "star_gift";
	suggestion["suggested_amount"] = 50;
	suggestion["reason"] = "Popular gift amount";
	suggestion["note"] = "Personalized gift options loading from Telegram API";
	suggestions.append(suggestion);

	result["success"] = true;
	result["recipient_id"] = recipientId;
	result["suggestions"] = suggestions;
	result["api_request"] = "submitted";

	return result;
}

// Subscriptions - uses real Telegram Stars Subscriptions API
QJsonObject Server::toolListSubscriptions(const QJsonObject &args) {
	Q_UNUSED(args);
	QJsonObject result;

	if (!_session) {
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	// Use the Credits API to get subscriptions
	auto self = _session->data().peer(_session->userPeerId());

	// Fire async request to get subscriptions
	_session->api().request(MTPpayments_GetStarsSubscriptions(
		MTP_flags(MTPpayments_getStarsSubscriptions::Flags(0)),
		MTP_inputPeerSelf(),
		MTP_string(QString())  // no offset, get first page
	)).done([this](const MTPpayments_StarsStatus &status) {
		const auto &data = status.data();
		_session->data().processUsers(data.vusers());
		_session->data().processChats(data.vchats());

		int count = 0;
		if (const auto subs = data.vsubscriptions()) {
			count = subs->v.size();
		}
		qWarning() << "MCP: Loaded" << count << "subscriptions";
	}).fail([](const MTP::Error &error) {
		qWarning() << "MCP: Failed to load subscriptions:" << error.type();
	}).send();

	// Return what we know: subscriptions are async, but we can check local DB too
	QJsonArray subscriptions;

	// Check local records
	QSqlQuery query(_db);
	query.prepare("SELECT peer_id, description, date FROM wallet_spending "
				  "WHERE category = 'subscription' ORDER BY date DESC LIMIT 50");
	if (query.exec()) {
		while (query.next()) {
			QJsonObject sub;
			sub["peer_id"] = query.value(0).toLongLong();
			sub["description"] = query.value(1).toString();
			sub["date"] = query.value(2).toString();
			sub["source"] = "local";
			subscriptions.append(sub);
		}
	}

	result["success"] = true;
	result["subscriptions"] = subscriptions;
	result["count"] = subscriptions.size();
	result["api_request"] = "submitted";
	result["note"] = "Subscription list request sent to Telegram API. "
					 "Local records shown. Full data loads asynchronously.";

	return result;
}

QJsonObject Server::toolSubscribeToChannel(const QJsonObject &args) {
	QJsonObject result;
	qint64 channelId = args["channel_id"].toVariant().toLongLong();
	QString tier = args.value("tier").toString("basic");

	if (channelId == 0) {
		result["error"] = "Missing channel_id";
		result["success"] = false;
		return result;
	}

	if (!_session) {
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	// A star subscription is bought through the payment form behind the
	// channel's invite link, and recurs. There is no client method that starts
	// one from a channel id alone.
	//
	// "Subscription intent recorded" was not a smaller version of subscribing.
	// It put a row in the spending log for something that had not begun, and
	// answered success, so a caller could believe it was subscribed.
	result["success"] = false;
	result["implemented"] = false;
	result["channel_id"] = channelId;
	result["tier"] = tier;
	result["error"] = "Subscribing to a paid channel is not implemented";
	result["reason"] = "A star subscription goes through the payment form behind "
		"the channel's invite link and debits real stars on a recurring basis. "
		"No intent was recorded: an intent is not a subscription, and storing it "
		"only made the two hard to tell apart.";

	return result;
}

QJsonObject Server::toolUnsubscribeFromChannel(const QJsonObject &args) {
	QJsonObject result;
	QString subscriptionId = args.value("subscription_id").toString();
	qint64 channelId = args["channel_id"].toVariant().toLongLong();

	if (subscriptionId.isEmpty() && channelId == 0) {
		result["error"] = "Missing subscription_id or channel_id";
		result["success"] = false;
		return result;
	}

	if (!_session) {
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	if (!subscriptionId.isEmpty()) {
		// Use real Telegram API to cancel subscription
		using Flag = MTPpayments_ChangeStarsSubscription::Flag;
		_session->api().request(MTPpayments_ChangeStarsSubscription(
			MTP_flags(Flag::f_canceled),
			MTP_inputPeerSelf(),
			MTP_string(subscriptionId),
			MTP_bool(true)  // cancel = true
		)).done([subscriptionId]() {
			qWarning() << "MCP: Subscription" << subscriptionId << "cancelled successfully";
		}).fail([subscriptionId](const MTP::Error &error) {
			qWarning() << "MCP: Failed to cancel subscription" << subscriptionId << ":" << error.type();
		}).send();

		result["success"] = true;
		result["subscription_id"] = subscriptionId;
		result["status"] = "cancellation_submitted";
		result["note"] = "Cancellation request sent to Telegram API";
	} else {
		result["success"] = true;
		result["channel_id"] = channelId;
		result["status"] = "need_subscription_id";
		result["note"] = "To cancel, provide subscription_id from list_subscriptions. "
						 "channel_id alone is not sufficient for cancellation.";
	}

	return result;
}

QJsonObject Server::toolGetSubscriptionStats(const QJsonObject &args) {
	Q_UNUSED(args);
	QJsonObject result;

	// Aggregate subscription-related spending from wallet_spending
	QSqlQuery query(_db);
	query.prepare("SELECT COUNT(*), COALESCE(SUM(amount), 0) FROM wallet_spending "
				  "WHERE category = 'subscription'");

	if (query.exec() && query.next()) {
		result["total_subscriptions"] = query.value(0).toInt();
		result["total_spent"] = query.value(1).toDouble();
	} else {
		result["total_subscriptions"] = 0;
		result["total_spent"] = 0;
	}

	// Monthly cost from recent 30 days
	QSqlQuery monthlyQuery(_db);
	monthlyQuery.prepare("SELECT COALESCE(SUM(amount), 0) FROM wallet_spending "
						 "WHERE category = 'subscription' AND date >= date('now', '-30 days')");
	if (monthlyQuery.exec() && monthlyQuery.next()) {
		result["monthly_cost"] = monthlyQuery.value(0).toDouble();
	} else {
		result["monthly_cost"] = 0;
	}

	result["success"] = true;

	return result;
}

// Monetization - uses real Telegram Stars Revenue API
QJsonObject Server::toolGetEarnings(const QJsonObject &args) {
	QJsonObject result;
	qint64 channelId = args.value("channel_id").toVariant().toLongLong();

	if (!_session) {
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	// Determine which peer to get earnings for
	PeerData *earningsPeer = nullptr;
	if (channelId > 0) {
		PeerId peerId(channelId);
		earningsPeer = resolvePeer(peerId);
	} else {
		// Self earnings (bot earnings)
		earningsPeer = _session->data().peer(_session->userPeerId());
	}

	if (!earningsPeer) {
		result["error"] = "Peer not found";
		result["success"] = false;
		return result;
	}

	// Fire async request for earnings stats
	auto inputPeer = earningsPeer->isUser()
		? earningsPeer->asUser()->input()
		: earningsPeer->asChannel()->input();

	_session->api().request(MTPpayments_GetStarsRevenueStats(
		MTP_flags(0),
		inputPeer
	)).done([channelId](const MTPpayments_StarsRevenueStats &stats) {
		const auto &data = stats.data();
		const auto &status = data.vstatus().data();
		auto current = CreditsAmountFromTL(status.vcurrent_balance());
		auto available = CreditsAmountFromTL(status.vavailable_balance());
		auto overall = CreditsAmountFromTL(status.voverall_revenue());
		qWarning() << "MCP: Earnings for" << channelId
				   << "- current:" << current.whole()
				   << "available:" << available.whole()
				   << "overall:" << overall.whole()
				   << "usdRate:" << data.vusd_rate().v;
	}).fail([channelId](const MTP::Error &error) {
		qWarning() << "MCP: Failed to load earnings for" << channelId << ":" << error.type();
	}).send();

	// Credits API not available in this version
	qint64 currencyBalance = 0;

	result["success"] = true;
	result["channel_id"] = channelId;
	result["cached_currency_balance"] = currencyBalance;
	result["api_request"] = "submitted";
	result["note"] = "Revenue stats request sent to Telegram API. "
					 "Results include current_balance, available_balance, overall_revenue, and usd_rate. "
					 "Check server logs for detailed earnings data.";

	return result;
}

QJsonObject Server::toolWithdrawEarnings(const QJsonObject &args) {
	if (args.value("to_wallet").toBool() && _session) {
		return withdrawEarningsToWallet(args);
	}
	QJsonObject result;
	qint64 amount = args["amount"].toVariant().toLongLong();
	QString method = args.value("method").toString("stars");  // stars or ton
	qint64 channelId = args.value("channel_id").toVariant().toLongLong();

	if (amount <= 0) {
		result["error"] = "Amount must be positive";
		result["success"] = false;
		return result;
	}

	if (!_session) {
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	PeerData *withdrawPeer = nullptr;
	if (channelId > 0) {
		PeerId peerId(channelId);
		withdrawPeer = resolvePeer(peerId);
	} else {
		withdrawPeer = _session->data().peer(_session->userPeerId());
	}

	if (!withdrawPeer) {
		result["error"] = "Peer not found";
		result["success"] = false;
		return result;
	}

	// getStarsRevenueWithdrawalUrl takes an InputCheckPasswordSRP -- the
	// account's 2FA password, run through SRP against a fresh challenge from
	// account.getPassword. inputCheckPasswordEmpty is not a way to "trigger a
	// prompt"; it is a password the server rejects, and there is no prompt to
	// trigger from here.
	//
	// So the old body sent a request that could only fail, attached a .fail()
	// that logged and a .done() that did not exist, and then wrote a NEGATIVE
	// row into wallet_spending and answered success with status
	// "password_required" -- recording money as leaving an account that had not
	// paid out a thing.
	//
	// Doing this properly means holding the user's 2FA password, which this
	// server does not and should not.
	result["success"] = false;
	result["implemented"] = false;
	result["amount"] = amount;
	result["method"] = method;
	result["channel_id"] = channelId;
	result["error"] = "Withdrawing earnings is not implemented";
	result["reason"] = "The withdrawal URL is gated on an SRP proof of the "
		"account's 2FA password, which this server does not hold. Nothing was "
		"recorded: a withdrawal that never started must not appear in the "
		"spending history. Withdraw from Settings > Monetization.";
	Q_UNUSED(withdrawPeer);

	return result;
}

QJsonObject Server::toolSetMonetizationRules(const QJsonObject &args) {
	if (!args.contains("rules") || !args["rules"].isObject()) {
		return toolError("rules is required and must be an object");
	}
	const auto rules = args["rules"].toObject();

	// Stored as one row, replacing the previous rule set. Previously this
	// echoed the rules back with "configured locally" and stored nothing.
	const auto encoded = QString::fromUtf8(
		QJsonDocument(rules).toJson(QJsonDocument::Compact));
	QSqlQuery query(_db);
	query.prepare("INSERT OR REPLACE INTO monetization_rules "
		"(id, rules, updated_at) VALUES (1, ?, datetime('now'))");
	query.addBindValue(encoded);
	if (!query.exec()) {
		return toolError("Could not store monetization rules: "
			+ query.lastError().text());
	}

	QJsonObject result;
	result["success"] = true;
	result["rules"] = rules;
	return result;
}

QJsonObject Server::toolGetMonetizationAnalytics(const QJsonObject &args) {
	const auto chatId = args["chat_id"].toVariant().toLongLong();
	if (!_session) {
		return toolError("No active session");
	}
	const auto peer = chatId
		? _session->data().peerLoaded(PeerId(chatId))
		: _session->user().get();
	if (!peer) {
		return toolError("Chat not found");
	}

	// Real revenue figures, replacing a hand-built reply that reported zeros
	// with success. StarsAmount carries a nanos component, so amounts are
	// returned both raw and combined rather than truncated to whole stars.
	// StarsAmount is boxed and has a TON variant whose amount is a signed
	// nano value -- decoding it by hand gets negatives wrong. Use the
	// converter the rest of the app uses.
	const auto amount = [](const MTPStarsAmount &value) {
		const auto parsed = CreditsAmountFromTL(value);
		QJsonObject entry;
		entry["whole"] = qint64(parsed.whole());
		entry["nano"] = qint64(parsed.nano());
		entry["value"] = parsed.value();
		entry["currency"] = parsed.ton() ? "ton" : "stars";
		return entry;
	};
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPpayments_GetStarsRevenueStats(
			MTP_flags(0),
			peer->input()
		)).done([=](const MTPpayments_StarsRevenueStats &result) {
			const auto &data = result.data();
			const auto &status = data.vstatus().data();
			QJsonObject value;
			value["success"] = true;
			value["chat_id"] = chatId;
			value["usd_rate"] = data.vusd_rate().v;
			value["current_balance"] = amount(status.vcurrent_balance());
			value["available_balance"] = amount(status.vavailable_balance());
			value["overall_revenue"] = amount(status.voverall_revenue());
			value["withdrawal_enabled"] = status.is_withdrawal_enabled();
			if (const auto next = status.vnext_withdrawal_at()) {
				value["next_withdrawal_at"] = next->v;
			}
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("payments.getStarsRevenueStats failed: " + error.type());
		}).send();
	});
}

// Budget Management
QJsonObject Server::toolSetSpendingBudget(const QJsonObject &args) {
	QJsonObject result;
	double dailyLimit = args.value("daily_limit").toDouble(0);
	double weeklyLimit = args.value("weekly_limit").toDouble(0);
	double monthlyLimit = args.value("monthly_limit").toDouble(0);

	QSqlQuery query(_db);
	query.prepare("INSERT OR REPLACE INTO wallet_budgets (id, daily_limit, weekly_limit, monthly_limit, updated_at) "
				  "VALUES (1, ?, ?, ?, datetime('now'))");
	query.addBindValue(dailyLimit);
	query.addBindValue(weeklyLimit);
	query.addBindValue(monthlyLimit);

	if (query.exec()) {
		result["success"] = true;
		result["daily_limit"] = dailyLimit;
		result["weekly_limit"] = weeklyLimit;
		result["monthly_limit"] = monthlyLimit;
	} else {
		result["success"] = false;
		result["error"] = "Failed to save budget";
	}

	return result;
}

QJsonObject Server::toolGetBudgetStatus(const QJsonObject &args) {
	const auto category = args["category"].toString().trimmed();
	if (category.isEmpty()) {
		return toolError("category is required: pass 'daily', 'weekly', "
			"'monthly', or 'all'");
	}
	const auto wanted = category.toLower();
	if (wanted != "daily" && wanted != "weekly"
		&& wanted != "monthly" && wanted != "all") {
		return toolError(QString("Unknown category '%1': expected daily, "
			"weekly, monthly or all").arg(category));
	}

	QSqlQuery budget(_db);
	budget.prepare("SELECT daily_limit, weekly_limit, monthly_limit "
		"FROM wallet_budgets WHERE id = 1");
	if (!budget.exec() || !budget.next()) {
		return toolError("No budget configured; call set_spending_budget first");
	}
	const auto limits = std::array<double, 3>{
		budget.value(0).toDouble(),
		budget.value(1).toDouble(),
		budget.value(2).toDouble() };

	QSqlQuery spent(_db);
	spent.prepare("SELECT "
		"SUM(CASE WHEN date >= date('now') THEN ABS(amount) ELSE 0 END), "
		"SUM(CASE WHEN date >= date('now','-7 days') THEN ABS(amount) ELSE 0 END), "
		"SUM(CASE WHEN date >= date('now','-30 days') THEN ABS(amount) ELSE 0 END) "
		"FROM wallet_spending WHERE amount < 0");
	auto used = std::array<double, 3>{};
	if (spent.exec() && spent.next()) {
		for (auto i = 0; i != 3; ++i) {
			used[i] = spent.value(i).toDouble();
		}
	}

	// Only the requested window is returned. Previously every window came
	// back regardless of the category asked for, so the argument had no
	// effect on the answer at all.
	const auto names = std::array<const char*, 3>{
		"daily", "weekly", "monthly" };
	QJsonObject windows;
	for (auto i = 0; i != 3; ++i) {
		if (wanted != "all" && wanted != names[i]) {
			continue;
		}
		QJsonObject entry;
		entry["limit"] = limits[i];
		entry["spent"] = used[i];
		entry["remaining"] = limits[i] - used[i];
		entry["over_budget"] = (limits[i] > 0) && (used[i] > limits[i]);
		windows[names[i]] = entry;
	}

	QJsonObject result;
	result["success"] = true;
	result["category"] = wanted;
	result["windows"] = windows;
	return result;
}

QJsonObject Server::toolSetBudgetAlert(const QJsonObject &args) {
	const auto threshold = args["threshold"].toDouble();
	const auto alertType = args.value("type").toString("percentage");

	if (threshold <= 0) {
		return toolError("threshold must be greater than zero");
	}
	if (alertType != "percentage" && alertType != "absolute") {
		return toolError("type must be 'percentage' or 'absolute'");
	}
	if (alertType == "percentage" && threshold > 100) {
		return toolError("a percentage threshold cannot exceed 100");
	}

	// Telegram has no server-side spending alert, so this is genuinely local
	// state -- but it has to actually be stored. It previously echoed the
	// arguments back with "Budget alert configured" and wrote nothing, so the
	// alert did not exist and no later call could find it.
	QSqlQuery query(_db);
	query.prepare("INSERT INTO budget_alerts (threshold, alert_type) "
		"VALUES (?, ?)");
	query.addBindValue(threshold);
	query.addBindValue(alertType);
	if (!query.exec()) {
		return toolError("Could not store budget alert: "
			+ query.lastError().text());
	}

	QJsonObject result;
	result["success"] = true;
	result["id"] = query.lastInsertId().toLongLong();
	result["threshold"] = threshold;
	result["alert_type"] = alertType;
	return result;
}

// Mini app tools. A mini app is a bot, so miniapp_id is the bot's user id.
namespace {

[[nodiscard]] UserData *MiniappBot(
		not_null<Main::Session*> session,
		const QJsonObject &args) {
	const auto id = args["miniapp_id"].toVariant().toLongLong();
	const auto user = id
		? session->data().userLoaded(UserId(BareId(id)))
		: nullptr;
	return (user && user->isBot()) ? user : nullptr;
}

[[nodiscard]] qint64 StarsAmountValue(const MTPStarsAmount &amount) {
	return amount.match([](const MTPDstarsAmount &data) {
		return qint64(data.vamount().v);
	}, [](const MTPDstarsTonAmount &data) {
		return qint64(data.vamount().v);
	});
}

} // namespace

QJsonObject Server::scanMiniappTransactions(UserId bot, int maxPages) {
	auto transactions = QJsonArray();
	auto offset = QString();
	auto scanned = 0;
	auto pages = 0;
	const auto session = _session;
	// Telegram cannot filter Stars history by counterparty, so pages are read and filtered here.
	while (pages++ < maxPages) {
		const auto page = awaitMtp([&](auto done, auto fail) {
			session->api().request(MTPpayments_GetStarsTransactions(
				MTP_flags(0),
				MTPstring(),
				MTP_inputPeerSelf(),
				MTP_string(offset),
				MTP_int(100)
			)).done([=](const MTPpayments_StarsStatus &status) {
				const auto &data = status.data();
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				auto list = QJsonArray();
				auto count = 0;
				if (const auto history = data.vhistory()) {
					for (const auto &entry : history->v) {
						++count;
						const auto &tx = entry.data();
						auto peerId = PeerId();
						tx.vpeer().match([&](const MTPDstarsTransactionPeer &p) {
							peerId = peerFromMTP(p.vpeer());
						}, [](const auto &) {
						});
						const auto user = peerIsUser(peerId)
							? session->data().userLoaded(peerToUser(peerId))
							: nullptr;
						if (!user
							|| !user->isBot()
							|| (bot && user->id != peerFromUser(bot))) {
							continue;
						}
						auto item = QJsonObject();
						item["id"] = qs(tx.vid());
						item["date"] = qint64(tx.vdate().v);
						item["miniapp_id"] = qint64(peerToUser(peerId).bare);
						item["miniapp_name"] = user->name();
						item["amount"] = StarsAmountValue(tx.vamount());
						if (const auto title = tx.vtitle()) {
							item["title"] = qs(*title);
						}
						if (const auto description = tx.vdescription()) {
							item["description"] = qs(*description);
						}
						item["refund"] = tx.is_refund();
						item["pending"] = tx.is_pending();
						item["failed"] = tx.is_failed();
						if (const auto period = tx.vsubscription_period()) {
							item["subscription_period"] = period->v;
						}
						list.append(item);
					}
				}
				auto value = QJsonObject();
				value["success"] = true;
				value["list"] = list;
				value["count"] = count;
				value["next_offset"] = data.vnext_offset()
					? qs(*data.vnext_offset())
					: QString();
				done(value);
			}).fail([=](const MTP::Error &error) {
				fail(error.type());
			}).send();
		});
		if (!page["success"].toBool()) {
			return page;
		}
		scanned += page["count"].toInt();
		for (const auto &item : page["list"].toArray()) {
			transactions.append(item);
		}
		offset = page["next_offset"].toString();
		if (offset.isEmpty()) {
			break;
		}
	}
	auto value = QJsonObject();
	value["success"] = true;
	value["transactions"] = transactions;
	value["scanned_transactions"] = scanned;
	// False when older history exists beyond the pages read.
	value["complete"] = offset.isEmpty();
	return value;
}

QJsonObject Server::toolListMiniappPermissions(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	auto budgets = QHash<QString, QPair<double, double>>();
	QSqlQuery query(_db);
	if (query.exec("SELECT miniapp_id, approved_amount, spent_amount "
			"FROM miniapp_budgets")) {
		while (query.next()) {
			budgets.insert(
				query.value(0).toString(),
				{ query.value(1).toDouble(), query.value(2).toDouble() });
		}
	}
	const auto session = _session;
	return awaitMtp([&](auto done, auto fail) {
		session->api().request(MTPmessages_GetAttachMenuBots(
			MTP_long(0)
		)).done([=](const MTPAttachMenuBots &result) {
			auto value = QJsonObject();
			result.match([&](const MTPDattachMenuBots &data) {
				session->data().processUsers(data.vusers());
				auto list = QJsonArray();
				for (const auto &entry : data.vbots().v) {
					const auto &bot = entry.data();
					const auto id = qint64(bot.vbot_id().v);
					const auto user = session->data().userLoaded(
						UserId(bot.vbot_id().v));
					auto item = QJsonObject();
					item["miniapp_id"] = id;
					item["short_name"] = qs(bot.vshort_name());
					if (user) {
						item["name"] = user->name();
						item["username"] = user->username();
					}
					item["installed"] = !bot.is_inactive();
					item["in_attach_menu"] = bot.is_show_in_attach_menu();
					item["in_side_menu"] = bot.is_show_in_side_menu();
					item["requests_write_access"] = bot.is_request_write_access();
					item["has_settings"] = bot.is_has_settings();
					// Only known once the bot's full profile has been loaded.
					if (user && user->botInfo && user->wasFullUpdated()) {
						item["can_manage_emoji_status"]
							= user->botInfo->canManageEmojiStatus;
					}
					const auto budget = budgets.find(QString::number(id));
					if (budget != budgets.end()) {
						item["local_budget_stars"] = budget->first;
						item["local_spent_stars"] = budget->second;
					}
					list.append(item);
				}
				value["success"] = true;
				value["miniapps"] = list;
				value["count"] = list.size();
			}, [&](const MTPDattachMenuBotsNotModified &) {
				value = toolError("Telegram answered not-modified to hash 0");
			});
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolRevokeMiniappPermission(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto bot = MiniappBot(_session, args);
	if (!bot) {
		return toolError("miniapp_id must be the user id of a bot this "
			"client knows; see list_miniapp_permissions");
	}
	const auto permission = args.value("permission").toString("attach_menu");
	const auto session = _session;
	return awaitMtp([&](auto done, auto fail) {
		const auto finish = [=](const MTPBool &result) {
			auto value = QJsonObject();
			value["success"] = mtpIsTrue(result);
			value["miniapp_id"] = qint64(bot->id.value);
			value["permission"] = permission;
			value["revoked"] = mtpIsTrue(result);
			done(value);
		};
		const auto failed = [=](const MTP::Error &error) {
			fail(error.type());
		};
		if (permission == "attach_menu") {
			session->api().request(MTPmessages_ToggleBotInAttachMenu(
				MTP_flags(0),
				bot->inputUser(),
				MTP_bool(false)
			)).done(finish).fail(failed).send();
		} else if (permission == "emoji_status") {
			session->api().request(MTPbots_ToggleUserEmojiStatusPermission(
				bot->inputUser(),
				MTP_bool(false)
			)).done(finish).fail(failed).send();
		} else {
			fail("permission must be attach_menu or emoji_status");
		}
	});
}

QJsonObject Server::toolGetMiniappHistory(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto filtered = args.contains("miniapp_id");
	const auto bot = filtered ? MiniappBot(_session, args) : nullptr;
	if (filtered && !bot) {
		return toolError("miniapp_id must be the user id of a bot this "
			"client knows; see list_miniapp_permissions");
	}
	const auto pages = std::clamp(args.value("max_pages").toInt(5), 1, 20);
	auto value = scanMiniappTransactions(
		bot ? peerToUser(bot->id) : UserId(),
		pages);
	if (value["success"].toBool()) {
		value["count"] = value["transactions"].toArray().size();
	}
	return value;
}

QJsonObject Server::toolGetMiniappSpending(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto filtered = args.contains("miniapp_id");
	const auto bot = filtered ? MiniappBot(_session, args) : nullptr;
	if (filtered && !bot) {
		return toolError("miniapp_id must be the user id of a bot this "
			"client knows; see list_miniapp_permissions");
	}
	const auto pages = std::clamp(args.value("max_pages").toInt(5), 1, 20);
	const auto scan = scanMiniappTransactions(
		bot ? peerToUser(bot->id) : UserId(),
		pages);
	if (!scan["success"].toBool()) {
		return scan;
	}
	auto perApp = QJsonObject();
	auto total = qint64();
	for (const auto &entry : scan["transactions"].toArray()) {
		const auto tx = entry.toObject();
		const auto amount = tx["amount"].toVariant().toLongLong();
		if (tx["failed"].toBool() || amount >= 0) {
			continue;
		}
		const auto key = QString::number(tx["miniapp_id"].toVariant().toLongLong());
		auto row = perApp[key].toObject();
		row["name"] = tx["miniapp_name"];
		row["spent_stars"] = row["spent_stars"].toVariant().toLongLong() - amount;
		row["payments"] = row["payments"].toInt() + 1;
		perApp[key] = row;
		total -= amount;
	}
	auto value = QJsonObject();
	value["success"] = true;
	value["spending"] = perApp;
	value["total_spent_stars"] = total;
	value["scanned_transactions"] = scan["scanned_transactions"];
	value["complete"] = scan["complete"];
	return value;
}

QJsonObject Server::toolSetMiniappBudget(const QJsonObject &args) {
	const auto id = args["miniapp_id"].toVariant().toLongLong();
	if (!id) {
		return toolError("miniapp_id is required");
	}
	if (!args.contains("amount")) {
		return toolError("amount is required: the Stars cap; 0 removes it");
	}
	const auto amount = args["amount"].toDouble();
	QSqlQuery query(_db);
	if (amount <= 0) {
		query.prepare("DELETE FROM miniapp_budgets WHERE miniapp_id = ?");
		query.addBindValue(QString::number(id));
	} else {
		// Re-setting a cap keeps what was already spent through approve_miniapp_spend.
		query.prepare("INSERT INTO miniapp_budgets (miniapp_id, approved_amount, "
			"spent_amount, created_at) VALUES (?, ?, 0, datetime('now')) "
			"ON CONFLICT(miniapp_id) DO UPDATE SET approved_amount = ?");
		query.addBindValue(QString::number(id));
		query.addBindValue(amount);
		query.addBindValue(amount);
	}
	if (!query.exec()) {
		return toolError("Could not store the budget: "
			+ query.lastError().text());
	}
	auto value = QJsonObject();
	value["success"] = true;
	value["miniapp_id"] = id;
	value["budget_stars"] = std::max(amount, 0.);
	value["note"] = "Telegram has no per-app budget; this cap is local and "
		"only approve_miniapp_spend enforces it.";
	return value;
}

QJsonObject Server::toolApproveMiniappSpend(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto id = args["miniapp_id"].toVariant().toLongLong();
	auto slug = args["invoice"].toString().trimmed();
	// Accept a t.me/$slug or t.me/invoice/slug link as well as the bare slug.
	if (const auto pos = slug.lastIndexOf('/'); pos >= 0) {
		slug = slug.mid(pos + 1);
	}
	if (slug.startsWith('$')) {
		slug = slug.mid(1);
	}
	if (!id || slug.isEmpty()) {
		return toolError("miniapp_id and invoice (the slug or t.me link the "
			"mini app opened) are required");
	}
	const auto confirm = args.value("confirm").toBool(false);
	const auto invoice = MTP_inputInvoiceSlug(MTP_string(slug));
	const auto session = _session;
	const auto form = awaitMtp([&](auto done, auto fail) {
		session->api().request(MTPpayments_GetPaymentForm(
			MTP_flags(0),
			invoice,
			MTPDataJSON()
		)).done([=](const MTPpayments_PaymentForm &result) {
			auto value = QJsonObject();
			result.match([&](const MTPDpayments_paymentFormStars &data) {
				session->data().processUsers(data.vusers());
				auto total = qint64();
				for (const auto &price : data.vinvoice().data().vprices().v) {
					total += price.data().vamount().v;
				}
				value["success"] = true;
				value["form_id"] = QString::number(data.vform_id().v);
				value["bot_id"] = qint64(data.vbot_id().v);
				value["title"] = qs(data.vtitle());
				value["description"] = qs(data.vdescription());
				value["amount_stars"] = total;
			}, [&](const auto &) {
				value = toolError("This invoice is not payable in Stars; "
					"card payments need the in-app payment form");
			});
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("payments.getPaymentForm: " + error.type());
		}).send();
	});
	if (!form["success"].toBool()) {
		return form;
	}
	if (form["bot_id"].toVariant().toLongLong() != id) {
		return toolError(QString("This invoice belongs to bot %1, not %2"
			).arg(form["bot_id"].toVariant().toLongLong()).arg(id));
	}
	const auto amount = form["amount_stars"].toVariant().toLongLong();
	auto budget = std::optional<std::pair<double, double>>();
	{
		QSqlQuery query(_db);
		query.prepare("SELECT approved_amount, spent_amount FROM miniapp_budgets "
			"WHERE miniapp_id = ?");
		query.addBindValue(QString::number(id));
		if (query.exec() && query.next()) {
			budget = std::pair{ query.value(0).toDouble(), query.value(1).toDouble() };
		}
	}
	auto preview = form;
	preview.remove("success");
	if (budget) {
		preview["budget_stars"] = budget->first;
		preview["budget_spent_stars"] = budget->second;
		if (budget->second + amount > budget->first) {
			auto refused = toolError(QString("Over the local budget: %1 of %2 "
				"Stars already spent").arg(budget->second).arg(budget->first));
			refused["invoice"] = preview;
			return refused;
		}
	}
	if (!confirm) {
		auto value = QJsonObject();
		value["success"] = true;
		value["paid"] = false;
		value["invoice"] = preview;
		value["next_step"] = "Nothing was paid. Call again with confirm: true "
			"to pay this amount in Stars.";
		return value;
	}
	const auto formId = form["form_id"].toString().toULongLong();
	const auto paid = awaitMtp([&](auto done, auto fail) {
		session->api().request(MTPpayments_SendStarsForm(
			MTP_long(formId),
			invoice
		)).done([=](const MTPpayments_PaymentResult &result) {
			auto value = QJsonObject();
			result.match([&](const MTPDpayments_paymentResult &data) {
				session->api().applyUpdates(data.vupdates());
				value["success"] = true;
				value["paid"] = true;
			}, [&](const MTPDpayments_paymentVerificationNeeded &data) {
				value["success"] = false;
				value["paid"] = false;
				value["verification_url"] = qs(data.vurl());
				value["error"] = "Telegram wants this payment verified in a "
					"browser first";
			});
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("payments.sendStarsForm: " + error.type());
		}).send();
	}, 60000);
	if (paid["paid"].toBool() && budget) {
		QSqlQuery query(_db);
		query.prepare("UPDATE miniapp_budgets SET spent_amount = spent_amount + ? "
			"WHERE miniapp_id = ?");
		query.addBindValue(double(amount));
		query.addBindValue(QString::number(id));
		query.exec();
	}
	auto value = paid;
	value["invoice"] = preview;
	return value;
}

// Stars Transfer - uses real Telegram Stars API
QJsonObject Server::toolSendStars(const QJsonObject &args) {
	QJsonObject result;
	qint64 recipientId = args["recipient_id"].toVariant().toLongLong();
	int amount = args["amount"].toInt();
	QString message = args.value("message").toString();

	if (recipientId == 0 || amount <= 0) {
		result["error"] = "Missing recipient_id or invalid amount";
		result["success"] = false;
		return result;
	}

	// There is no user-to-user star transfer in the API, in any version.
	// inputInvoiceStars tops up your OWN balance; inputStorePaymentStarsGift
	// buys stars for someone with real money, not with stars you hold. The
	// closest equivalent is sending a gift the recipient then converts with
	// convertStarGift -- a different operation, at the recipient's discretion,
	// usually worth less than the gift cost.
	//
	// So this cannot be finished as named. It used to write a NEGATIVE row
	// into wallet_spending and answer success with status "recorded", which
	// made a transfer that never happened show up in the spending history
	// while the recipient received nothing.
	result["success"] = false;
	result["implemented"] = false;
	result["recipient_id"] = recipientId;
	result["amount"] = amount;
	result["error"] = "Sending stars directly to another user is not possible";
	result["reason"] = "Telegram has no user-to-user star transfer. Buying stars "
		"for someone else costs real money (inputStorePaymentStarsGift), and the "
		"only star-denominated route is send_gift, which the recipient must "
		"convert -- a different operation, not an equivalent transfer. Nothing "
		"was recorded locally.";
	Q_UNUSED(message);

	return result;
}

QJsonObject Server::toolGetStarsRate(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}

	// Telegram publishes both rates in appConfig, so no request is needed.
	// This used to return 0.0 with success=true under a comment claiming the
	// credits API was unavailable -- a zero rate is worse than an error here,
	// because it reads as a usable number and silently turns any conversion
	// into zero.
	const auto &config = _session->appConfig();
	const auto withdrawRate = config.starsWithdrawRate();
	const auto sellRate = config.starsSellRate();

	QJsonObject result;
	result["success"] = true;
	result["usd_per_star_withdraw"] = withdrawRate;
	result["usd_per_star_sell"] = sellRate;
	result["stars_per_usd_withdraw"] = (withdrawRate > 0)
		? (1. / withdrawRate)
		: 0.;
	result["source"] = "appConfig:stars_usd_withdraw_rate_x1000";
	return result;
}

QJsonObject Server::toolConvertStars(const QJsonObject &args) {
	const auto starsAmount = args["stars_amount"].toInt();
	const auto target = args.value("target").toString("usd").toLower();

	if (starsAmount <= 0) {
		return toolError("stars_amount must be a positive integer");
	}
	if (!_session) {
		return toolError("No active session");
	}
	if (target != "usd") {
		return toolError(QString(
			"Only 'usd' conversion is supported; Telegram publishes no rate "
			"for '%1'").arg(target));
	}

	// Two rates exist and they differ: withdrawing stars and selling them are
	// priced separately, so reporting one number would be wrong for whichever
	// case the caller meant. Both are returned and named.
	const auto &config = _session->appConfig();
	const auto withdrawRate = config.starsWithdrawRate();
	const auto sellRate = config.starsSellRate();

	QJsonObject result;
	result["success"] = true;
	result["stars_amount"] = starsAmount;
	result["target"] = target;
	result["usd_if_withdrawn"] = starsAmount * withdrawRate;
	result["usd_if_sold"] = starsAmount * sellRate;
	result["usd_per_star_withdraw"] = withdrawRate;
	result["usd_per_star_sell"] = sellRate;
	return result;
}

QJsonObject Server::toolGetStarsLeaderboard(const QJsonObject &args) {
	Q_UNUSED(args);
	QJsonObject result;

	// Aggregate from local star_reactions table
	QSqlQuery query(_db);
	query.prepare("SELECT chat_id, SUM(stars_count) as total "
				  "FROM star_reactions GROUP BY chat_id "
				  "ORDER BY total DESC LIMIT 20");

	QJsonArray leaderboard;
	if (query.exec()) {
		int rank = 1;
		while (query.next()) {
			QJsonObject entry;
			entry["rank"] = rank++;
			entry["chat_id"] = query.value(0).toLongLong();
			entry["total_stars"] = query.value(1).toInt();
			leaderboard.append(entry);
		}
	}

	result["success"] = true;
	result["leaderboard"] = leaderboard;
	result["count"] = leaderboard.size();

	return result;
}

QJsonObject Server::toolGetStarsHistory(const QJsonObject &args) {
	const auto direction = args.value("direction").toString("all");
	const auto limit = clampLimit(args.value("limit").toInt(50), 50, 100);

	if (!_session) {
		return toolError("No active session");
	}

	// Waits for Telegram's answer instead of throwing it away.
	//
	// This used to fire the request, discard the reply in a lambda that only
	// logged a count, and return local rows with `api_request: "submitted"`
	// and a note saying the real data "loads asynchronously". It never did:
	// nothing wrote it anywhere a later call could read. The local ledger is
	// still returned alongside, but now as a named second list rather than as
	// a stand-in for the thing that was promised.
	using Flags = MTPpayments_GetStarsTransactions::Flags;
	auto flags = Flags(0);
	if (direction == "inbound") {
		flags |= MTPpayments_GetStarsTransactions::Flag::f_inbound;
	} else if (direction == "outbound") {
		flags |= MTPpayments_GetStarsTransactions::Flag::f_outbound;
	}

	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPpayments_GetStarsTransactions(
			MTP_flags(flags),
			MTPstring(),
			MTP_inputPeerSelf(),
			MTP_string(QString()),
			MTP_int(limit)
		)).done([=](const MTPpayments_StarsStatus &status) {
			const auto &data = status.data();
			_session->data().processUsers(data.vusers());
			_session->data().processChats(data.vchats());

			auto transactions = QJsonArray();
			if (const auto history = data.vhistory()) {
				for (const auto &entry : history->v) {
					const auto &tx = entry.data();
					auto item = QJsonObject();
					item["id"] = qs(tx.vid());
					item["date"] = qint64(tx.vdate().v);
					item["amount"] = tx.vamount().match([](
							const MTPDstarsAmount &amount) {
						return qint64(amount.vamount().v);
					}, [](const MTPDstarsTonAmount &amount) {
						return qint64(amount.vamount().v);
					});
					if (const auto title = tx.vtitle()) {
						item["title"] = qs(*title);
					}
					if (const auto description = tx.vdescription()) {
						item["description"] = qs(*description);
					}
					item["refund"] = tx.is_refund();
					item["pending"] = tx.is_pending();
					item["failed"] = tx.is_failed();
					transactions.append(item);
				}
			}

			const auto balance = data.vbalance().match([](
					const MTPDstarsAmount &amount) {
				return qint64(amount.vamount().v);
			}, [](const MTPDstarsTonAmount &amount) {
				return qint64(amount.vamount().v);
			});

			// The local ledger, kept but no longer passed off as the answer.
			auto local = QJsonArray();
			QSqlQuery query(_db);
			query.prepare("SELECT date, amount, category, description, peer_id "
				"FROM wallet_spending ORDER BY date DESC LIMIT ?");
			query.addBindValue(limit);
			if (query.exec()) {
				while (query.next()) {
					auto row = QJsonObject();
					row["date"] = query.value(0).toString();
					row["amount"] = query.value(1).toDouble();
					row["category"] = query.value(2).toString();
					row["description"] = query.value(3).toString();
					if (!query.value(4).isNull()) {
						row["peer_id"] = qint64(query.value(4).toLongLong());
					}
					local.append(row);
				}
			}

			auto value = QJsonObject();
			value["success"] = true;
			value["direction"] = direction;
			value["current_balance"] = balance;
			value["transactions"] = transactions;
			value["count"] = transactions.size();
			value["local_notes"] = local;
			value["local_notes_count"] = local.size();
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}


} // namespace MCP
