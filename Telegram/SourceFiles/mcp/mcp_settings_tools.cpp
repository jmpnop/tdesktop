// This file is part of Telegram Desktop MCP integration.
// Extracted from mcp_server_complete.cpp for modular organization.
//
// For license and copyright information please follow this link:
// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

#include "mcp_server_includes.h"

#include "api/api_global_privacy.h"
#include "data/business/data_business_chatbots.h"
#include "data/business/data_business_common.h"
#include "wallet/wallet_session.h"
#include "base/unixtime.h"

namespace MCP {
// ============================================================================
// Privacy helper functions
// ============================================================================

namespace {

// The MCP names of the privacy keys; the TL constructors are what account.getPrivacy takes.
const std::vector<std::pair<QString, MTPInputPrivacyKey>> &PrivacyKeys() {
	static const auto result = std::vector<std::pair<QString, MTPInputPrivacyKey>>{
		{ "last_seen", MTP_inputPrivacyKeyStatusTimestamp() },
		{ "profile_photo", MTP_inputPrivacyKeyProfilePhoto() },
		{ "phone_number", MTP_inputPrivacyKeyPhoneNumber() },
		{ "added_by_phone", MTP_inputPrivacyKeyAddedByPhone() },
		{ "forwards", MTP_inputPrivacyKeyForwards() },
		{ "calls", MTP_inputPrivacyKeyPhoneCall() },
		{ "calls_p2p", MTP_inputPrivacyKeyPhoneP2P() },
		{ "invites", MTP_inputPrivacyKeyChatInvite() },
		{ "voice_messages", MTP_inputPrivacyKeyVoiceMessages() },
		{ "about", MTP_inputPrivacyKeyAbout() },
		{ "birthday", MTP_inputPrivacyKeyBirthday() },
		{ "gifts_auto_save", MTP_inputPrivacyKeyStarGiftsAutoSave() },
		{ "no_paid_messages", MTP_inputPrivacyKeyNoPaidMessages() },
		{ "saved_music", MTP_inputPrivacyKeySavedMusic() },
	};
	return result;
}

QStringList PrivacyKeyNames() {
	auto result = QStringList();
	for (const auto &[name, key] : PrivacyKeys()) {
		result.push_back(name);
	}
	return result;
}

std::optional<MTPInputPrivacyKey> PrivacyKeyToTL(const QString &name) {
	for (const auto &[known, key] : PrivacyKeys()) {
		if (known == name) {
			return key;
		}
	}
	return std::nullopt;
}

std::optional<MTPInputPrivacyRule> PrivacyOptionToTL(const QString &rule) {
	if (rule == "everybody" || rule == "everyone") {
		return MTP_inputPrivacyValueAllowAll();
	} else if (rule == "contacts") {
		return MTP_inputPrivacyValueAllowContacts();
	} else if (rule == "close_friends") {
		return MTP_inputPrivacyValueAllowCloseFriends();
	} else if (rule == "nobody") {
		return MTP_inputPrivacyValueDisallowAll();
	}
	return std::nullopt;
}

// Reports every rule Telegram returned, so exceptions are visible next to the base option.
QJsonObject DescribePrivacyRules(
		not_null<Main::Session*> session,
		const MTPVector<MTPPrivacyRule> &rules) {
	auto result = QJsonObject();
	auto rule = QString();
	auto allowUsers = QJsonArray();
	auto disallowUsers = QJsonArray();
	auto allowChats = QJsonArray();
	auto disallowChats = QJsonArray();
	const auto chatId = [&](const MTPlong &bare) {
		const auto channel = peerFromChannel(bare.v);
		return qint64((session->data().peerLoaded(channel)
			? channel
			: peerFromChat(bare.v)).value);
	};
	// Rules apply in order, so the first base option is the one in force.
	const auto setRule = [&](const QString &value) {
		if (rule.isEmpty()) {
			rule = value;
		}
	};
	for (const auto &entry : rules.v) {
		entry.match([&](const MTPDprivacyValueAllowAll &) {
			setRule(u"everybody"_q);
		}, [&](const MTPDprivacyValueAllowContacts &) {
			setRule(u"contacts"_q);
		}, [&](const MTPDprivacyValueAllowCloseFriends &) {
			setRule(u"close_friends"_q);
		}, [&](const MTPDprivacyValueDisallowAll &) {
			setRule(u"nobody"_q);
		}, [&](const MTPDprivacyValueDisallowContacts &) {
			result["disallow_contacts"] = true;
		}, [&](const MTPDprivacyValueAllowUsers &data) {
			for (const auto &id : data.vusers().v) {
				allowUsers.append(qint64(id.v));
			}
		}, [&](const MTPDprivacyValueDisallowUsers &data) {
			for (const auto &id : data.vusers().v) {
				disallowUsers.append(qint64(id.v));
			}
		}, [&](const MTPDprivacyValueAllowChatParticipants &data) {
			for (const auto &id : data.vchats().v) {
				allowChats.append(chatId(id));
			}
		}, [&](const MTPDprivacyValueDisallowChatParticipants &data) {
			for (const auto &id : data.vchats().v) {
				disallowChats.append(chatId(id));
			}
		}, [&](const MTPDprivacyValueAllowPremium &) {
			result["allow_premium"] = true;
		}, [&](const MTPDprivacyValueAllowBots &) {
			result["allow_bots"] = true;
		}, [&](const MTPDprivacyValueDisallowBots &) {
			result["disallow_bots"] = true;
		});
	}
	// A rule list that names no base option denies everyone it does not list.
	result["rule"] = rule.isEmpty() ? u"nobody"_q : rule;
	result["allow_user_ids"] = allowUsers;
	result["disallow_user_ids"] = disallowUsers;
	result["allow_chat_ids"] = allowChats;
	result["disallow_chat_ids"] = disallowChats;
	return result;
}

} // namespace

// ============================================================================
// PROFILE, PRIVACY, AND SECURITY SETTINGS IMPLEMENTATIONS
// These use real Telegram API integration
// ============================================================================

QJsonObject Server::toolGetProfileSettings(const QJsonObject &args) {
	Q_UNUSED(args);

	if (!_session) {
		return QJsonObject{
			{"error", "No active session"},
			{"status", "error"},
		};
	}

	const auto user = _session->user();
	if (!user) {
		return QJsonObject{
			{"error", "User data not available"},
			{"status", "error"},
		};
	}

	// Get birthday info
	const auto birthday = user->birthday();
	QJsonObject birthdayObj;
	if (birthday) {
		birthdayObj["day"] = birthday.day();
		birthdayObj["month"] = birthday.month();
		if (birthday.year()) {
			birthdayObj["year"] = birthday.year();
		}
	}

	return QJsonObject{
		{"first_name", user->firstName},
		{"last_name", user->lastName},
		{"username", user->username()},
		{"phone", user->phone()},
		{"bio", user->about()},
		{"birthday", birthdayObj},
		{"is_premium", user->isPremium()},
		{"status", "success"},
	};
}

QJsonObject Server::toolUpdateProfileName(const QJsonObject &args) {
	QString firstName = args["first_name"].toString();
	QString lastName = args["last_name"].toString();

	if (!_session) {
		return QJsonObject{
			{"error", "No active session"},
			{"status", "error"},
		};
	}

	if (firstName.isEmpty()) {
		return QJsonObject{
			{"error", "First name is required"},
			{"status", "error"},
		};
	}

	// Send MTP request to update profile name
	auto flags = MTPaccount_UpdateProfile::Flag::f_first_name
		| MTPaccount_UpdateProfile::Flag::f_last_name;

	_session->api().request(MTPaccount_UpdateProfile(
		MTP_flags(flags),
		MTP_string(firstName),
		MTP_string(lastName),
		MTPstring()
	)).done([session = _session](const MTPUser &user) {
		session->data().processUser(user);
	}).fail([](const MTP::Error &error) {
		qWarning() << "[MCP] Profile name update failed:" << error.type();
	}).send();

	return QJsonObject{
		{"first_name", firstName},
		{"last_name", lastName},
		{"status", "success"},
		{"note", "Profile name update request sent. Changes will appear shortly."},
	};
}

QJsonObject Server::toolUpdateProfileBio(const QJsonObject &args) {
	QString bio = args["bio"].toString();

	if (!_session) {
		return QJsonObject{
			{"error", "No active session"},
			{"status", "error"},
		};
	}

	// Use the API to save bio
	_session->api().saveSelfBio(bio);

	return QJsonObject{
		{"bio", bio},
		{"status", "success"},
		{"note", "Bio update initiated"},
	};
}

QJsonObject Server::toolUpdateProfileUsername(const QJsonObject &args) {
	const auto username = args["username"].toString().trimmed();
	if (!_session) {
		return toolError("No active session");
	}
	if (username.isEmpty()) {
		return toolError("username is required (pass an empty_ok flag is not "
			"supported; use the app to clear a username)");
	}

	// account.updateUsername needs no verification step -- the earlier claim
	// that this "requires interactive verification" confused it with changing
	// a phone number, which does. Telegram validates the name and answers
	// with the updated user, or with USERNAME_OCCUPIED / USERNAME_INVALID /
	// USERNAME_PURCHASE_AVAILABLE, all of which are worth passing through
	// verbatim rather than flattening into one failure.
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPaccount_UpdateUsername(
			MTP_string(username)
		)).done([=](const MTPUser &result) {
			QJsonObject value;
			value["success"] = true;
			value["username"] = username;
			result.match([&](const MTPDuser &data) {
				value["user_id"] = qint64(data.vid().v);
			}, [](const MTPDuserEmpty &) {});
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("account.updateUsername failed: " + error.type());
		}).send();
	});
}

QJsonObject Server::toolUpdateProfilePhone(const QJsonObject &args) {
	const auto phone = args["phone"].toString().trimmed();
	const auto codeHash = args.value("phone_code_hash").toString();
	const auto code = args.value("code").toString();

	if (!_session) {
		return toolError("No active session");
	}
	if (phone.isEmpty()) {
		return toolError("phone is required, in international format");
	}

	// Changing a number genuinely needs a confirmation code, so this is two
	// calls rather than one: the first sends the code and returns the hash,
	// the second passes the hash back with the code the user received. The
	// previous single-shot version simply declared the whole thing
	// unsupported.
	if (code.isEmpty() || codeHash.isEmpty()) {
		return awaitMtp([&](auto done, auto fail) {
			_session->api().request(MTPaccount_SendChangePhoneCode(
				MTP_string(phone),
				MTP_codeSettings(
					MTP_flags(0),
					MTPVector<MTPbytes>(),
					MTPstring(),
					MTPBool())
			)).done([=](const MTPauth_SentCode &result) {
				QJsonObject value;
				result.match([&](const MTPDauth_sentCode &data) {
					value["success"] = true;
					value["phone"] = phone;
					value["phone_code_hash"] = qs(data.vphone_code_hash());
					value["status"] = "code_sent";
					value["next_step"] = "Call update_profile_phone again with the "
						"same phone, this phone_code_hash, and the code received.";
				}, [&](const auto &) {
					value = toolError("Telegram answered with an unsupported "
						"sent-code type for this flow");
				});
				done(value);
			}).fail([=](const MTP::Error &error) {
				fail("account.sendChangePhoneCode failed: " + error.type());
			}).send();
		});
	}

	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPaccount_ChangePhone(
			MTP_string(phone),
			MTP_string(codeHash),
			MTP_string(code)
		)).done([=](const MTPUser &result) {
			QJsonObject value;
			value["success"] = true;
			value["phone"] = phone;
			value["status"] = "changed";
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("account.changePhone failed: " + error.type());
		}).send();
	});
}

QJsonObject Server::toolGetPrivacySettings(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	auto names = QStringList();
	if (args.contains("key")) {
		const auto key = args["key"].toString();
		if (!PrivacyKeyToTL(key)) {
			return toolError("Unknown privacy key '" + key + "'; one of: "
				+ PrivacyKeyNames().join(", "));
		}
		names.push_back(key);
	} else {
		names = PrivacyKeyNames();
	}
	const auto session = _session;
	const auto settings = std::make_shared<QJsonObject>();
	const auto left = std::make_shared<int>(names.size());
	return awaitMtp([&](auto done, auto fail) {
		for (const auto &name : names) {
			const auto key = *PrivacyKeyToTL(name);
			session->api().request(MTPaccount_GetPrivacy(
				key
			)).done([=](const MTPaccount_PrivacyRules &result) {
				const auto &data = result.data();
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				session->api().userPrivacy().apply(
					key.type(),
					data.vrules(),
					true);
				(*settings)[name] = DescribePrivacyRules(
					session,
					data.vrules());
				if (!--*left) {
					auto value = QJsonObject();
					value["success"] = true;
					value["settings"] = *settings;
					done(value);
				}
			}).fail([=](const MTP::Error &error) {
				fail(name + ": " + error.type());
			}).send();
		}
	});
}

QJsonObject Server::updatePrivacyOption(
		const QString &name,
		const QString &rule) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto key = PrivacyKeyToTL(name);
	const auto base = PrivacyOptionToTL(rule);
	if (!key || !base) {
		return toolError("rule must be everybody, contacts, close_friends "
			"or nobody");
	}
	const auto session = _session;
	// Only the base option changes; the exceptions already set are read back and kept.
	return awaitMtp([&](auto done, auto fail) {
		session->api().request(MTPaccount_GetPrivacy(
			*key
		)).done([=](const MTPaccount_PrivacyRules &current) {
			const auto &data = current.data();
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			const auto inputUsers = [&](const MTPVector<MTPlong> &ids) {
				auto result = QVector<MTPInputUser>();
				for (const auto &id : ids.v) {
					result.push_back(session->data().user(UserId(id.v))->inputUser());
				}
				return MTP_vector(result);
			};
			auto rules = QVector<MTPInputPrivacyRule>();
			for (const auto &entry : data.vrules().v) {
				entry.match([&](const MTPDprivacyValueAllowUsers &d) {
					rules.push_back(MTP_inputPrivacyValueAllowUsers(inputUsers(d.vusers())));
				}, [&](const MTPDprivacyValueDisallowUsers &d) {
					rules.push_back(MTP_inputPrivacyValueDisallowUsers(inputUsers(d.vusers())));
				}, [&](const MTPDprivacyValueAllowChatParticipants &d) {
					rules.push_back(MTP_inputPrivacyValueAllowChatParticipants(d.vchats()));
				}, [&](const MTPDprivacyValueDisallowChatParticipants &d) {
					rules.push_back(MTP_inputPrivacyValueDisallowChatParticipants(d.vchats()));
				}, [&](const MTPDprivacyValueDisallowContacts &) {
					rules.push_back(MTP_inputPrivacyValueDisallowContacts());
				}, [&](const MTPDprivacyValueAllowPremium &) {
					rules.push_back(MTP_inputPrivacyValueAllowPremium());
				}, [&](const MTPDprivacyValueAllowBots &) {
					rules.push_back(MTP_inputPrivacyValueAllowBots());
				}, [&](const MTPDprivacyValueDisallowBots &) {
					rules.push_back(MTP_inputPrivacyValueDisallowBots());
				}, [](const auto &) {
				});
			}
			rules.push_back(*base);
			session->api().request(MTPaccount_SetPrivacy(
				*key,
				MTP_vector(rules)
			)).done([=](const MTPaccount_PrivacyRules &result) {
				const auto &data = result.data();
				session->data().processUsers(data.vusers());
				session->data().processChats(data.vchats());
				session->api().userPrivacy().apply(
					key->type(),
					data.vrules(),
					true);
				auto value = DescribePrivacyRules(session, data.vrules());
				value["success"] = true;
				value["key"] = name;
				done(value);
			}).fail([=](const MTP::Error &error) {
				fail(error.type());
			}).send();
		}).fail([=](const MTP::Error &error) {
			fail("account.getPrivacy: " + error.type());
		}).send();
	});
}

QJsonObject Server::toolSetPrivacy(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto name = args["key"].toString();
	const auto key = PrivacyKeyToTL(name);
	if (!key) {
		return toolError("Unknown privacy key '" + name + "'; one of: "
			+ PrivacyKeyNames().join(", "));
	}
	const auto rule = args["rule"].toString();
	auto rules = QVector<MTPInputPrivacyRule>();
	const auto users = [&](const QString &field, QString *error) {
		auto result = QVector<MTPInputUser>();
		for (const auto &id : args[field].toArray()) {
			const auto peer = resolvePeer(id.toVariant().toLongLong());
			const auto user = peer ? peer->asUser() : nullptr;
			if (!user) {
				*error = QString("%1: no user %2 in this client's data"
					).arg(field).arg(id.toVariant().toLongLong());
				return result;
			}
			result.push_back(user->inputUser());
		}
		return result;
	};
	const auto chats = [&](const QString &field, QString *error) {
		auto result = QVector<MTPlong>();
		for (const auto &id : args[field].toArray()) {
			const auto peer = resolvePeer(id.toVariant().toLongLong());
			if (!peer || peer->isUser()) {
				*error = QString("%1: no group %2 in this client's data"
					).arg(field).arg(id.toVariant().toLongLong());
				return result;
			}
			result.push_back(peerToBareMTPInt(peer->id));
		}
		return result;
	};
	auto error = QString();
	// Exceptions first, the base option last, the order the official client sends.
	if (const auto list = users("allow_user_ids", &error); !list.isEmpty()) {
		rules.push_back(MTP_inputPrivacyValueAllowUsers(MTP_vector(list)));
	}
	if (const auto list = chats("allow_chat_ids", &error); !list.isEmpty()) {
		rules.push_back(MTP_inputPrivacyValueAllowChatParticipants(
			MTP_vector(list)));
	}
	if (args["allow_premium"].toBool()) {
		rules.push_back(MTP_inputPrivacyValueAllowPremium());
	}
	if (args["allow_bots"].toBool()) {
		rules.push_back(MTP_inputPrivacyValueAllowBots());
	}
	if (const auto list = users("disallow_user_ids", &error); !list.isEmpty()) {
		rules.push_back(MTP_inputPrivacyValueDisallowUsers(MTP_vector(list)));
	}
	if (const auto list = chats("disallow_chat_ids", &error); !list.isEmpty()) {
		rules.push_back(MTP_inputPrivacyValueDisallowChatParticipants(
			MTP_vector(list)));
	}
	if (args["disallow_bots"].toBool()) {
		rules.push_back(MTP_inputPrivacyValueDisallowBots());
	}
	if (!error.isEmpty()) {
		return toolError(error);
	}
	if (const auto base = PrivacyOptionToTL(rule)) {
		rules.push_back(*base);
	} else {
		return toolError("rule must be everybody, contacts, close_friends "
			"or nobody");
	}
	const auto session = _session;
	return awaitMtp([&](auto done, auto fail) {
		session->api().request(MTPaccount_SetPrivacy(
			*key,
			MTP_vector(rules)
		)).done([=](const MTPaccount_PrivacyRules &result) {
			const auto &data = result.data();
			session->data().processUsers(data.vusers());
			session->data().processChats(data.vchats());
			session->api().userPrivacy().apply(
				key->type(),
				data.vrules(),
				true);
			auto value = DescribePrivacyRules(session, data.vrules());
			value["success"] = true;
			value["key"] = name;
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolUpdateLastSeenPrivacy(const QJsonObject &args) {
	return updatePrivacyOption(u"last_seen"_q, args["rule"].toString());
}

QJsonObject Server::toolUpdateProfilePhotoPrivacy(const QJsonObject &args) {
	return updatePrivacyOption(u"profile_photo"_q, args["rule"].toString());
}

QJsonObject Server::toolUpdatePhoneNumberPrivacy(const QJsonObject &args) {
	return updatePrivacyOption(u"phone_number"_q, args["rule"].toString());
}

QJsonObject Server::toolUpdateForwardsPrivacy(const QJsonObject &args) {
	return updatePrivacyOption(u"forwards"_q, args["rule"].toString());
}

QJsonObject Server::toolUpdateBirthdayPrivacy(const QJsonObject &args) {
	return updatePrivacyOption(u"birthday"_q, args["rule"].toString());
}

QJsonObject Server::toolUpdateAboutPrivacy(const QJsonObject &args) {
	return updatePrivacyOption(u"about"_q, args["rule"].toString());
}

QJsonObject Server::toolGetBlockedUsers(const QJsonObject &args) {
	Q_UNUSED(args);

	if (!_session) {
		return QJsonObject{
			{"error", "No active session"},
			{"status", "error"},
		};
	}

	// Trigger reload to get fresh data
	_session->api().blockedPeers().reload();

	// Also query any locally cached blocked user info from our DB
	QJsonArray blockedArray;
	if (_db.isOpen()) {
		QSqlQuery query(_db);
		if (query.exec("SELECT user_id, username, blocked_at FROM blocked_users ORDER BY blocked_at DESC")) {
			while (query.next()) {
				QJsonObject blocked;
				blocked["user_id"] = query.value(0).toLongLong();
				blocked["username"] = query.value(1).toString();
				blocked["blocked_at"] = QDateTime::fromSecsSinceEpoch(
					query.value(2).toLongLong()).toString(Qt::ISODate);
				blockedArray.append(blocked);
			}
		}
	}

	return QJsonObject{
		{"status", "success"},
		{"blocked_users", blockedArray},
		{"count", blockedArray.size()},
		{"note", "Blocked list reload initiated. Cached data shown. "
			"Use block_user/unblock_user tools to manage blocked users."},
	};
}

QJsonObject Server::toolGetSecuritySettings(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	const auto session = _session;
	const auto value = std::make_shared<QJsonObject>();
	const auto left = std::make_shared<int>(4);
	// Four independent reads; the answer goes out once all of them are back.
	return awaitMtp([&](auto done, auto fail) {
		const auto finish = [=] {
			if (!--*left) {
				(*value)["success"] = true;
				done(*value);
			}
		};
		session->api().request(MTPaccount_GetPassword(
		)).done([=](const MTPaccount_Password &result) {
			const auto &data = result.data();
			(*value)["two_step_enabled"] = data.is_has_password();
			(*value)["two_step_has_recovery_email"] = data.is_has_recovery();
			(*value)["two_step_has_hint"] = bool(data.vhint());
			(*value)["login_email_set"] = bool(data.vlogin_email_pattern());
			if (const auto reset = data.vpending_reset_date()) {
				(*value)["two_step_pending_reset_date"] = qint64(reset->v);
			}
			finish();
		}).fail([=](const MTP::Error &error) {
			fail("account.getPassword: " + error.type());
		}).send();
		session->api().request(MTPaccount_GetAccountTTL(
		)).done([=](const MTPAccountDaysTTL &result) {
			(*value)["account_self_destruct_days"] = result.data().vdays().v;
			finish();
		}).fail([=](const MTP::Error &error) {
			fail("account.getAccountTTL: " + error.type());
		}).send();
		session->api().request(MTPmessages_GetDefaultHistoryTTL(
		)).done([=](const MTPDefaultHistoryTTL &result) {
			(*value)["default_message_ttl_seconds"] = result.data().vperiod().v;
			finish();
		}).fail([=](const MTP::Error &error) {
			fail("messages.getDefaultHistoryTTL: " + error.type());
		}).send();
		session->api().request(MTPaccount_GetAuthorizations(
		)).done([=](const MTPaccount_Authorizations &result) {
			const auto &data = result.data();
			(*value)["active_sessions"] = int(data.vauthorizations().v.size());
			(*value)["inactive_session_ttl_days"]
				= data.vauthorization_ttl_days().v;
			finish();
		}).fail([=](const MTP::Error &error) {
			fail("account.getAuthorizations: " + error.type());
		}).send();
	});
}

QJsonObject Server::toolGetActiveSessions(const QJsonObject &args) {
	Q_UNUSED(args);

	if (!_session) {
		return QJsonObject{
			{"error", "No active session"},
			{"status", "error"},
		};
	}

	// Reload authorizations
	_session->api().authorizations().reload();

	// Get current list (may be empty if not yet loaded)
	const auto list = _session->api().authorizations().list();

	QJsonArray sessions;
	for (const auto &entry : list) {
		QJsonObject sessionObj;
		sessionObj["hash"] = QString::number(entry.hash);
		sessionObj["name"] = entry.name;
		sessionObj["platform"] = entry.platform;
		sessionObj["system"] = entry.system;
		sessionObj["info"] = entry.info;
		sessionObj["ip"] = entry.ip;
		sessionObj["location"] = entry.location;
		sessionObj["active"] = entry.active;
		sessionObj["is_current"] = (entry.hash == 0);
		sessions.append(sessionObj);
	}

	return QJsonObject{
		{"sessions", sessions},
		{"total", _session->api().authorizations().total()},
		{"status", "success"},
	};
}

QJsonObject Server::toolTerminateSession(const QJsonObject &args) {
	qint64 hash = args["hash"].toVariant().toLongLong();

	if (!_session) {
		return QJsonObject{
			{"error", "No active session"},
			{"status", "error"},
		};
	}

	if (hash == 0) {
		return QJsonObject{
			{"error", "Cannot terminate current session"},
			{"status", "error"},
		};
	}

	// Request session termination
	_session->api().authorizations().requestTerminate(
		[](const MTPBool &) { /* success */ },
		[](const MTP::Error &) { /* fail */ },
		static_cast<uint64>(hash)
	);

	return QJsonObject{
		{"session_hash", QString::number(hash)},
		{"status", "initiated"},
		{"note", "Session termination request sent"},
	};
}

QJsonObject Server::toolUpdateAutoDeletePeriod(const QJsonObject &args) {
	int period = args["period"].toInt();

	if (!_session) {
		return QJsonObject{
			{"error", "No active session"},
			{"status", "error"},
		};
	}

	// Validate period (must be 0, 86400, 604800, or 2592000)
	if (period != 0 && period != 86400 && period != 604800 && period != 2592000) {
		return QJsonObject{
			{"error", "Invalid period. Must be 0 (disabled), 86400 (1 day), 604800 (1 week), or 2592000 (1 month)"},
			{"period", period},
			{"status", "error"},
		};
	}

	// Update auto-delete period
	_session->api().selfDestruct().updateDefaultHistoryTTL(period);

	return QJsonObject{
		{"period", period},
		{"period_description", period == 0 ? "disabled" : (period == 86400 ? "1 day" : (period == 604800 ? "1 week" : "1 month"))},
		{"status", "success"},
		{"note", "Auto-delete period update initiated"},
	};
}

// ============================================================================
// TRANSLATION TOOLS - delegate to mcp_premium_tools.cpp implementations
// ============================================================================

QJsonObject Server::toolGetTranslationLanguages(const QJsonObject &args) {
	Q_UNUSED(args);
	QJsonArray languages;
	// Common Telegram-supported languages
	const char* langCodes[] = {
		"en", "es", "fr", "de", "it", "pt", "ru", "zh", "ja", "ko",
		"ar", "hi", "tr", "pl", "nl", "uk", "cs", "sv", "da", "fi",
		"no", "hu", "ro", "bg", "hr", "sk", "sl", "lt", "lv", "et"
	};
	const char* langNames[] = {
		"English", "Spanish", "French", "German", "Italian", "Portuguese",
		"Russian", "Chinese", "Japanese", "Korean", "Arabic", "Hindi",
		"Turkish", "Polish", "Dutch", "Ukrainian", "Czech", "Swedish",
		"Danish", "Finnish", "Norwegian", "Hungarian", "Romanian",
		"Bulgarian", "Croatian", "Slovak", "Slovenian", "Lithuanian",
		"Latvian", "Estonian"
	};
	for (int i = 0; i < 30; ++i) {
		QJsonObject lang;
		lang["code"] = QString::fromLatin1(langCodes[i]);
		lang["name"] = QString::fromLatin1(langNames[i]);
		languages.append(lang);
	}
	QJsonObject result;
	result["success"] = true;
	result["languages"] = languages;
	result["count"] = languages.size();
	return result;
}

QJsonObject Server::toolAutoTranslateChat(const QJsonObject &args) {
	qint64 chatId = args["chat_id"].toVariant().toLongLong();
	QString targetLanguage = args["target_language"].toString();
	bool enabled = args.value("enabled").toBool(true);

	if (targetLanguage.isEmpty()) {
		QJsonObject result;
		result["error"] = "Missing target_language parameter";
		result["success"] = false;
		return result;
	}

	QSqlQuery query(_db);
	query.prepare("INSERT OR REPLACE INTO auto_translate_config (chat_id, target_language, enabled, updated_at) "
				  "VALUES (?, ?, ?, datetime('now'))");
	query.addBindValue(chatId);
	query.addBindValue(targetLanguage);
	query.addBindValue(enabled);

	QJsonObject result;
	if (query.exec()) {
		result["success"] = true;
		result["chat_id"] = chatId;
		result["target_language"] = targetLanguage;
		result["enabled"] = enabled;
	} else {
		result["success"] = false;
		result["error"] = "Failed to save auto-translate config";
	}
	return result;
}

QJsonObject Server::toolTranslateMessages(const QJsonObject &args) {
	// Delegate to the translate_message implementation in premium tools
	return toolTranslateMessage(args);
}

// ============================================================================
// VOICE/VIDEO TOOLS - delegate or provide local tracking
// ============================================================================

QJsonObject Server::toolGenerateVoiceMessage(const QJsonObject &args) {
	// Delegate to TTS tool in business tools
	return toolTextToSpeech(args);
}

QJsonObject Server::toolListVoicePresets(const QJsonObject &args) {
	// Delegate to voice personas in business tools
	return toolListVoicePersonas(args);
}

QJsonObject Server::toolGetTranscriptionStatus(const QJsonObject &args) {
	QString transcriptionId = args["transcription_id"].toString();
	qint64 messageId = args.value("message_id").toVariant().toLongLong();

	QJsonObject result;
	if (_voiceTranscription && messageId > 0) {
		auto stored = _voiceTranscription->getStoredTranscription(messageId);
		if (stored.success) {
			result["success"] = true;
			result["status"] = "completed";
			result["text"] = stored.text;
			result["language"] = stored.language;
			result["confidence"] = stored.confidence;
			return result;
		}
	}

	result["success"] = true;
	result["transcription_id"] = transcriptionId;
	result["status"] = "pending";
	result["note"] = "Transcription not yet available";
	return result;
}

QJsonObject Server::toolGenerateVideoCircle(const QJsonObject &args) {
	// Delegate to video tool in business tools
	return toolTextToVideo(args);
}

QJsonObject Server::toolConfigureVideoAvatar(const QJsonObject &args) {
	// Delegate to avatar upload in business tools
	return toolUploadAvatarSource(args);
}

// ============================================================================
// GREETING TOOLS - delegate to business tools implementations
// ============================================================================

QJsonObject Server::toolConfigureGreeting(const QJsonObject &args) {
	return toolSetGreetingMessage(args);
}

QJsonObject Server::toolGetGreetingConfig(const QJsonObject &args) {
	return toolGetGreetingMessage(args);
}

QJsonObject Server::toolGetGreetingStats(const QJsonObject &args) {
	Q_UNUSED(args);
	QSqlQuery query(_db);
	query.prepare("SELECT greetings_sent, updated_at FROM greeting_config WHERE id = 1");

	QJsonObject result;
	if (query.exec() && query.next()) {
		result["greetings_sent"] = query.value(0).toInt();
		result["last_updated"] = query.value(1).toString();
		result["success"] = true;
	} else {
		result["greetings_sent"] = 0;
		result["success"] = true;
		result["note"] = "No greeting configured";
	}
	return result;
}

// ============================================================================
// AWAY MESSAGE TOOLS - delegate to business tools implementations
// ============================================================================

QJsonObject Server::toolConfigureAwayMessage(const QJsonObject &args) {
	return toolSetAwayMessage(args);
}

// ============================================================================
// QUICK REPLY TOOLS - delegate to business tools implementations
// ============================================================================

QJsonObject Server::toolSendQuickReply(const QJsonObject &args) {
	return toolUseQuickReply(args);
}

QJsonObject Server::toolEditQuickReply(const QJsonObject &args) {
	return toolUpdateQuickReply(args);
}

// ============================================================================
// BUSINESS LOCATION - local SQLite storage
// ============================================================================

QJsonObject Server::toolSetBusinessLocation(const QJsonObject &args) {
	QString address = args["address"].toString();
	double latitude = args.value("latitude").toDouble(0);
	double longitude = args.value("longitude").toDouble(0);

	if (address.isEmpty()) {
		QJsonObject result;
		result["error"] = "Missing address parameter";
		result["success"] = false;
		return result;
	}

	QSqlQuery query(_db);
	query.prepare("INSERT OR REPLACE INTO business_location (id, address, latitude, longitude, updated_at) "
				  "VALUES (1, ?, ?, ?, datetime('now'))");
	query.addBindValue(address);
	query.addBindValue(latitude);
	query.addBindValue(longitude);

	QJsonObject result;
	if (query.exec()) {
		result["success"] = true;
		result["address"] = address;
		result["latitude"] = latitude;
		result["longitude"] = longitude;
	} else {
		result["success"] = false;
		result["error"] = "Failed to save business location";
	}
	return result;
}

QJsonObject Server::toolGetBusinessLocation(const QJsonObject &args) {
	Q_UNUSED(args);
	QSqlQuery query(_db);
	query.prepare("SELECT address, latitude, longitude, updated_at FROM business_location WHERE id = 1");

	QJsonObject result;
	if (query.exec() && query.next()) {
		result["address"] = query.value(0).toString();
		result["latitude"] = query.value(1).toDouble();
		result["longitude"] = query.value(2).toDouble();
		result["updated_at"] = query.value(3).toString();
		result["success"] = true;
	} else {
		result["success"] = true;
		result["note"] = "No business location configured";
	}
	return result;
}

// ============================================================================
// CHAT RULES - delegate to premium tools implementations
// ============================================================================

QJsonObject Server::toolCreateChatRule(const QJsonObject &args) {
	return toolSetChatRules(args);
}

QJsonObject Server::toolListChatRules(const QJsonObject &args) {
	return toolGetChatRules(args);
}

QJsonObject Server::toolDeleteChatRule(const QJsonObject &args) {
	qint64 ruleId = args["rule_id"].toVariant().toLongLong();
	QString ruleName = args.value("rule_name").toString();

	QSqlQuery query(_db);
	if (ruleId > 0) {
		query.prepare("DELETE FROM chat_rules WHERE id = ?");
		query.addBindValue(ruleId);
	} else if (!ruleName.isEmpty()) {
		query.prepare("DELETE FROM chat_rules WHERE rule_name = ?");
		query.addBindValue(ruleName);
	} else {
		QJsonObject result;
		result["error"] = "Provide rule_id or rule_name";
		result["success"] = false;
		return result;
	}

	QJsonObject result;
	if (query.exec() && query.numRowsAffected() > 0) {
		result["success"] = true;
		result["deleted"] = true;
	} else {
		result["success"] = false;
		result["error"] = "Rule not found";
	}
	return result;
}

QJsonObject Server::toolExecuteChatRules(const QJsonObject &args) {
	// Matches the rules and CARRIES OUT the actions, which is what "execute"
	// has to mean. This forwarded to test_chat_rules, a dry-run matcher whose
	// answer is `would_trigger` -- so a caller asking to execute rules got a
	// report of what would have happened and nothing happened at all.
	const auto chatId = args["chat_id"].toVariant().toLongLong();
	const auto message = args["test_message"].toString();
	if (!chatId) {
		return toolError("chat_id is required and must be non-zero");
	}
	if (message.isEmpty()) {
		return toolError("test_message is required: the text to run the rules "
			"against");
	}

	const auto matched = toolTestChatRules(args);
	if (!matched.value("success").toBool()) {
		return matched;
	}
	const auto rules = matched.value("matched_rules").toArray();

	QJsonArray performed;
	for (const auto &entry : rules) {
		const auto rule = entry.toObject();
		const auto actions = rule.value("actions").toObject();
		QJsonObject done;
		done["rule_name"] = rule.value("rule_name");

		// Only actions we can actually carry out are reported as performed.
		// Anything else is named as unsupported rather than counted, so the
		// result cannot overstate what happened.
		if (actions.contains(u"reply"_q)) {
			QJsonObject send;
			send["chat_id"] = chatId;
			send["text"] = actions.value("reply").toString();
			const auto sent = toolSendMessage(send);
			done["reply"] = sent.value("success").toBool();
			if (!sent.value("success").toBool()) {
				done["reply_error"] = sent.value("error");
			}
		}
		if (actions.contains(u"tag"_q)) {
			QJsonObject tag;
			tag["chat_id"] = chatId;
			tag["tag"] = actions.value("tag").toString();
			done["tagged"] = toolAddMessageTag(tag).value("success").toBool();
		}
		const auto unsupported = [&] {
			QJsonArray out;
			for (auto i = actions.begin(); i != actions.end(); ++i) {
				if (i.key() != u"reply"_q && i.key() != u"tag"_q) {
					out.append(i.key());
				}
			}
			return out;
		}();
		if (!unsupported.isEmpty()) {
			done["unsupported_actions"] = unsupported;
		}
		performed.append(done);
	}

	QJsonObject result;
	result["success"] = true;
	result["chat_id"] = chatId;
	result["rules_matched"] = rules.size();
	result["performed"] = performed;
	return result;
}

// ============================================================================
// TAG TOOLS - delegate to premium tools implementations
// ============================================================================

QJsonObject Server::toolGetTaggedMessages(const QJsonObject &args) {
	QString tag = args["tag"].toString();
	if (tag.isEmpty()) {
		return toolGetMessageTags(args);
	}
	return toolSearchByTag(args);
}

// ============================================================================
// PAID MESSAGES - local SQLite storage
// ============================================================================

QJsonObject Server::toolConfigurePaidMessages(const QJsonObject &args) {
	qint64 chatId = args["chat_id"].toVariant().toLongLong();
	int pricePerMessage = args["price"].toInt();
	bool enabled = args.value("enabled").toBool(true);

	QSqlQuery query(_db);
	query.prepare("INSERT OR REPLACE INTO paid_message_config (chat_id, price, enabled, updated_at) "
				  "VALUES (?, ?, ?, datetime('now'))");
	query.addBindValue(chatId);
	query.addBindValue(pricePerMessage);
	query.addBindValue(enabled);

	QJsonObject result;
	if (query.exec()) {
		result["success"] = true;
		result["chat_id"] = chatId;
		result["price"] = pricePerMessage;
		result["enabled"] = enabled;
	} else {
		result["success"] = false;
		result["error"] = "Failed to configure paid messages";
	}
	return result;
}

QJsonObject Server::toolGetPaidMessagesStats(const QJsonObject &args) {
	Q_UNUSED(args);
	QSqlQuery query(_db);
	query.prepare("SELECT SUM(price * unlocks), SUM(unlocks), COUNT(*) FROM paid_content");

	QJsonObject result;
	if (query.exec() && query.next()) {
		result["total_revenue"] = query.value(0).toInt();
		result["total_unlocks"] = query.value(1).toInt();
		result["total_posts"] = query.value(2).toInt();
		result["success"] = true;
	} else {
		result["total_revenue"] = 0;
		result["total_unlocks"] = 0;
		result["success"] = true;
	}
	return result;
}

// ============================================================================
// AD FILTER - delegate to premium tools
// ============================================================================

QJsonObject Server::toolGetFilteredAds(const QJsonObject &args) {
	return toolGetAdFilterStats(args);
}

QJsonObject Server::toolSearchTransactions(const QJsonObject &args) {
	QString searchQuery = args["query"].toString();
	QString category = args.value("category").toString();
	int limit = args.value("limit").toInt(50);

	QSqlQuery query(_db);
	QString sql = "SELECT id, date, amount, category, description FROM wallet_spending WHERE 1=1 ";
	if (!searchQuery.isEmpty()) {
		sql += "AND description LIKE ? ";
	}
	if (!category.isEmpty()) {
		sql += "AND category = ? ";
	}
	sql += "ORDER BY date DESC LIMIT ?";

	query.prepare(sql);
	if (!searchQuery.isEmpty()) query.addBindValue("%" + searchQuery + "%");
	if (!category.isEmpty()) query.addBindValue(category);
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
			transactions.append(tx);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["transactions"] = transactions;
	result["count"] = transactions.size();
	return result;
}

QJsonObject Server::toolGetTopupOptions(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	// payments.getStarsTopupOptions returns the real purchasable amounts.
	// This previously returned a hardcoded list, which went stale silently
	// whenever Telegram changed its pricing.
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPpayments_GetStarsTopupOptions(
		)).done([=](const MTPVector<MTPStarsTopupOption> &result) {
			QJsonArray options;
			for (const auto &option : result.v) {
				const auto &data = option.data();
				QJsonObject entry;
				entry["stars"] = qint64(data.vstars().v);
				entry["currency"] = qs(data.vcurrency());
				entry["amount"] = qint64(data.vamount().v);
				entry["extended"] = data.is_extended();
				if (const auto product = data.vstore_product()) {
					entry["store_product"] = qs(*product);
				}
				options.append(entry);
			}
			QJsonObject value;
			value["success"] = true;
			value["options"] = options;
			value["count"] = options.size();
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("payments.getStarsTopupOptions failed: " + error.type());
		}).send();
	});
}

// ============================================================================
// WALLET BUDGET/ALERT TOOLS - delegate to wallet tools
// ============================================================================

QJsonObject Server::toolSetWalletBudget(const QJsonObject &args) {
	return toolSetSpendingBudget(args);
}

QJsonObject Server::toolConfigureWalletAlerts(const QJsonObject &args) {
	return toolSetBudgetAlert(args);
}

QJsonObject Server::toolGetWithdrawalStatus(const QJsonObject &args) {
	Q_UNUSED(args);
	QSqlQuery query(_db);
	query.prepare("SELECT balance, last_updated FROM wallet_budgets WHERE id = 1");

	QJsonObject result;
	result["success"] = true;
	if (query.exec() && query.next()) {
		result["balance"] = query.value(0).toDouble();
		result["last_updated"] = query.value(1).toString();
		result["withdrawal_available"] = query.value(0).toDouble() > 0;
	} else {
		result["balance"] = 0;
		result["withdrawal_available"] = false;
	}
	result["note"] = "Withdrawals processed via Telegram Stars/Fragment";
	return result;
}

namespace {

[[nodiscard]] QString WalletKindName(Wallet::TransferItem::Kind kind) {
	using Kind = Wallet::TransferItem::Kind;
	switch (kind) {
	case Kind::Transfer: return "transfer";
	case Kind::Collectible: return "collectible";
	case Kind::Onramp: return "onramp";
	case Kind::PeerTransfer: return "peer_transfer";
	case Kind::KeyChange: return "key_change";
	}
	return "transfer";
}

[[nodiscard]] QString OnchainGiftActionName(Wallet::TransferOnchainGift action) {
	using Action = Wallet::TransferOnchainGift;
	switch (action) {
	case Action::None: return "none";
	case Action::Buy: return "buy";
	case Action::List: return "list";
	case Action::Delist: return "delist";
	case Action::Reprice: return "reprice";
	case Action::OfferCreate: return "offer_create";
	case Action::OfferAcceptBuyer: return "offer_accept_buyer";
	case Action::OfferAcceptSeller: return "offer_accept_seller";
	case Action::OfferCancel: return "offer_cancel";
	case Action::OfferDecline: return "offer_decline";
	case Action::OfferExpire: return "offer_expire";
	}
	return "none";
}

} // namespace

// Telegram has no call to report a wallet transaction, so the mark stays in this server's database.
QJsonObject Server::toolMarkTransactionSpam(const QJsonObject &args) {
	const auto id = args.value("transaction_id").toString().trimmed();
	if (id.isEmpty()) {
		return toolError("transaction_id is required (from get_history)");
	}
	const auto spam = args.value("spam").toBool(true);
	QSqlQuery query(_db);
	if (spam) {
		query.prepare("INSERT OR REPLACE INTO wallet_spam_marks "
			"(transaction_id, marked_at) VALUES (?, ?)");
		query.addBindValue(id);
		query.addBindValue(qint64(base::unixtime::now()));
	} else {
		query.prepare("DELETE FROM wallet_spam_marks WHERE transaction_id = ?");
		query.addBindValue(id);
	}
	if (!query.exec()) {
		return toolError(query.lastError().text());
	}
	return QJsonObject{
		{ "success", true },
		{ "transaction_id", id },
		{ "marked_spam_locally", spam },
		{ "scope", "local" },
		{ "note", "Saved in this client only; Telegram has no API to report "
			"a wallet transaction." },
	};
}

QJsonObject Server::toolCreateCryptoPayment(const QJsonObject &args) {
	const auto amount = args["amount"].toDouble();
	const auto currency = args.value("currency").toString("TON");
	const auto recipient = args["recipient"].toString();
	const auto comment = args.value("comment").toString();
	const auto action = args.value("action").toString("send");
	const auto limit = clampLimit(args.value("limit").toInt(20), 20, 100);

	if (!_session) {
		return toolError("No active session");
	}

	// WHY: bind the app's real non-custodial Wallet::Session, not a second
	// wallet -- this tool once drove a phantom tonsdk subprocess with its own
	// seed in MCP SQLite. Reads report the real wallet; writes are refused.
	auto &wallet = _session->wallet();
	wallet.ensureLoaded();

	const auto presenceName = [](Wallet::Presence p) -> QString {
		switch (p) {
		case Wallet::Presence::Unknown: return "unknown";
		case Wallet::Presence::Provisioning: return "provisioning";
		case Wallet::Presence::Missing: return "missing";
		case Wallet::Presence::Unavailable: return "unavailable";
		case Wallet::Presence::AddressUnreadable: return "address_unreadable";
		case Wallet::Presence::Ready: return "ready";
		}
		return "unknown";
	};

	const auto address = wallet.address();

	QJsonObject result;
	if (action == "get_address") {
		result["success"] = true;
		result["action"] = action;
		result["presence"] = presenceName(wallet.presence());
		result["has_wallet"] = address.has_value();
		if (address) {
			result["address"] = *address;
			result["address_bounceable"] = wallet.addressFriendly(true);
		}
		return result;
	}

	if (action == "get_balance") {
		result["success"] = true;
		result["action"] = action;
		result["presence"] = presenceName(wallet.presence());
		result["balance_nano"] = qint64(wallet.balanceNano());
		if (address) {
			result["address"] = *address;
		}
		// ponytail: cached balance -- await balanceNanoValue() for a settled read.
		result["note"] = "balance_nano is the last synced wallet balance in "
			"nanoTON (1 TON = 1e9 nanoTON); presence says whether it is ready.";
		return result;
	}

	if (action == "get_history") {
		const auto &history = wallet.history();
		auto marked = QSet<QString>();
		QSqlQuery marks(_db);
		if (marks.exec("SELECT transaction_id FROM wallet_spam_marks")) {
			while (marks.next()) {
				marked.insert(marks.value(0).toString());
			}
		}
		auto list = QJsonArray();
		for (const auto &item : history) {
			if (list.size() >= limit) {
				break;
			}
			auto tx = QJsonObject();
			tx["id"] = item.id;
			tx["incoming"] = item.incoming;
			tx["amount_nano"] = qint64(item.amountNano);
			if (!item.counterparty.isEmpty()) {
				tx["counterparty"] = item.counterparty;
			}
			if (!item.comment.isEmpty() && !item.commentEncrypted) {
				tx["comment"] = item.comment;
			}
			if (item.date) {
				tx["date"] = qint64(*item.date);
			}
			tx["kind"] = WalletKindName(item.kind);
			if (item.onchainGift != Wallet::TransferOnchainGift::None) {
				tx["onchain_gift"] = OnchainGiftActionName(item.onchainGift);
			}
			if (!item.collectible.isEmpty()) {
				tx["collectible"] = item.collectible;
			}
			tx["suspected_spam"] = item.suspectedSpam;
			tx["marked_spam_locally"] = marked.contains(item.id);
			list.append(tx);
		}
		result["success"] = true;
		result["action"] = action;
		result["transactions"] = list;
		result["count"] = list.size();
		return result;
	}

	// Write actions are refused rather than faked.
	result["success"] = false;
	result["implemented"] = false;
	result["action"] = action;
	if (action == "create_wallet" || action == "import_wallet") {
		result["error"] = "Creating or importing a wallet here is not supported";
		result["reason"] = "The app already owns one non-custodial TON wallet, "
			"whose seed phrase is created and restored only through the wallet "
			"panel behind the device vault. A second seed minted by this server "
			"-- the old \"mnemonics\" path -- would be a wallet the user never "
			"sees and cannot recover.";
	} else {
		result["error"] = "Sending TON is not implemented";
		result["recipient"] = recipient;
		result["amount"] = amount;
		result["currency"] = currency;
		if (!comment.isEmpty()) {
			result["comment"] = comment;
		}
		result["reason"] = "A transfer must be signed with the wallet's vault key "
			"authorization (passcode / seed unlock), which this server does not "
			"and should not hold. Send from the in-app wallet. Use action "
			"get_address, get_balance or get_history for read-only access.";
	}
	return result;
}

QJsonObject Server::toolGenerateFinancialReport(const QJsonObject &args) {
	QString period = args.value("period").toString("month");

	QJsonObject report;
	report["generated_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
	report["period"] = period;

	// Income
	QJsonObject incomeResult = toolGetIncomeAnalytics(args);
	report["total_income"] = incomeResult["total_income"];
	report["income_by_category"] = incomeResult["by_category"];

	// Spending
	QJsonObject spendingResult = toolGetSpendingAnalytics(args);
	report["total_spent"] = spendingResult["total_spent"];
	report["spending_by_category"] = spendingResult["by_category"];

	// Budget status
	// "all" explicitly: get_budget_status now requires a category, so passing
	// an empty object returns an error object that would land in the report
	// where the budget belongs.
	QJsonObject budgetArgs;
	budgetArgs["category"] = QStringLiteral("all");
	report["budget_status"] = toolGetBudgetStatus(budgetArgs);

	QJsonObject result;
	result["success"] = true;
	result["report"] = report;
	return result;
}

// ============================================================================
// COLLECTIBLES/PORTFOLIO - delegate to stars tools
// ============================================================================

QJsonObject Server::toolGetCollectiblesPortfolio(const QJsonObject &args) {
	return toolGetPortfolio(args);
}

// Both of these read a gift_collections / collection_items pair that no server
// ever populated, so they described collections this client had invented. They
// go to Telegram now; there is nothing a local table can add, because
// collection membership is server state.
QJsonObject Server::toolGetCollectionDetails(const QJsonObject &args) {
	QJsonObject result;
	const auto collectionId = args["collection_id"].toInt();

	if (collectionId <= 0) {
		result["error"] = "Missing or invalid collection_id";
		result["success"] = false;
		return result;
	}

	if (!_session) {
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	return awaitMtp([&](auto done, auto fail) {
		const auto self = _session->data().peer(_session->userPeerId());
		_session->api().request(MTPpayments_GetStarGiftCollections(
			self->input(),
			MTP_long(0)
		)).done([=](const MTPpayments_StarGiftCollections &result) {
			auto value = QJsonObject();
			auto found = false;
			result.match([&](const MTPDpayments_starGiftCollections &data) {
				for (const auto &entry : data.vcollections().v) {
					const auto &collection = entry.data();
					if (collection.vcollection_id().v != collectionId) {
						continue;
					}
					found = true;
					value["collection_id"] = collection.vcollection_id().v;
					value["title"] = qs(collection.vtitle());
					value["gifts_count"] = collection.vgifts_count().v;
				}
			}, [](const MTPDpayments_starGiftCollectionsNotModified &) {
			});
			value["success"] = found;
			if (!found) {
				value["error"] = "Collection not found";
				value["collection_id"] = collectionId;
			}
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

// How much of this account's gift shelf sits in one collection.
//
// The old version divided one orphaned table by another and, since nothing
// writes to either any more, would have reported 0% forever. This is a ratio
// over two numbers Telegram actually returns; it is arithmetic on real
// figures, not an estimate.
QJsonObject Server::toolGetCollectionCompletion(const QJsonObject &args) {
	QJsonObject result;
	const auto collectionId = args["collection_id"].toInt();

	if (collectionId <= 0) {
		result["error"] = "Missing or invalid collection_id";
		result["success"] = false;
		return result;
	}

	if (!_session) {
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	return awaitMtp([&](auto done, auto fail) {
		const auto self = _session->data().peer(_session->userPeerId());
		_session->api().request(MTPpayments_GetStarGiftCollections(
			self->input(),
			MTP_long(0)
		)).done([=](const MTPpayments_StarGiftCollections &collections) {
			auto inCollection = -1;
			collections.match([&](
					const MTPDpayments_starGiftCollections &data) {
				for (const auto &entry : data.vcollections().v) {
					const auto &collection = entry.data();
					if (collection.vcollection_id().v == collectionId) {
						inCollection = collection.vgifts_count().v;
					}
				}
			}, [](const MTPDpayments_starGiftCollectionsNotModified &) {
			});

			if (inCollection < 0) {
				auto value = QJsonObject();
				value["success"] = false;
				value["collection_id"] = collectionId;
				value["error"] = "Collection not found";
				done(value);
				return;
			}

			// Second hop: the total is the account's whole saved-gift count,
			// which only payments.getSavedStarGifts knows.
			_session->api().request(MTPpayments_GetSavedStarGifts(
				MTP_flags(0),
				self->input(),
				MTPint(), // collection_id: unset, we want every gift
				MTP_string(QString()),
				MTP_int(1) // one row is enough; `count` is what we need
			)).done([=](const MTPpayments_SavedStarGifts &saved) {
				const auto total = saved.data().vcount().v;
				auto value = QJsonObject();
				value["success"] = true;
				value["collection_id"] = collectionId;
				value["gifts_in_collection"] = inCollection;
				value["gifts_owned_total"] = total;
				value["completion_percentage"] = total > 0
					? (double(inCollection) / total * 100.0)
					: 0.0;
				done(value);
			}).fail([=](const MTP::Error &error) {
				fail(error.type());
			}).send();
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

// ============================================================================
// AUCTION TOOLS - delegate to stars tools
// ============================================================================

QJsonObject Server::toolListActiveAuctions(const QJsonObject &args) {
	return toolListAuctions(args);
}

QJsonObject Server::toolPlaceAuctionBid(const QJsonObject &args) {
	return toolPlaceBid(args);
}

QJsonObject Server::toolGetAuctionDetails(const QJsonObject &args) {
	return toolGetAuctionStatus(args);
}

QJsonObject Server::toolCreateAuctionAlert(const QJsonObject &args) {
	QString auctionId = args["auction_id"].toString();
	double priceThreshold = args.value("price_threshold").toDouble(0);

	QSqlQuery query(_db);
	query.prepare("INSERT INTO price_alerts (gift_type, target_price, direction, triggered, created_at) "
				  "VALUES (?, ?, 'below', 0, datetime('now'))");
	query.addBindValue("auction:" + auctionId);
	query.addBindValue(priceThreshold);

	QJsonObject result;
	if (query.exec()) {
		result["success"] = true;
		result["alert_id"] = query.lastInsertId().toLongLong();
		result["auction_id"] = auctionId;
		result["price_threshold"] = priceThreshold;
	} else {
		result["success"] = false;
		result["error"] = "Failed to create auction alert";
	}
	return result;
}

QJsonObject Server::toolGetAuctionAlerts(const QJsonObject &args) {
	Q_UNUSED(args);
	QSqlQuery query(_db);
	query.prepare("SELECT id, gift_type, target_price, direction, triggered, created_at FROM price_alerts "
				  "WHERE gift_type LIKE 'auction:%' ORDER BY created_at DESC");

	QJsonArray alerts;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject alert;
			alert["id"] = query.value(0).toLongLong();
			alert["auction_id"] = query.value(1).toString().mid(8); // Remove "auction:" prefix
			alert["price_threshold"] = query.value(2).toDouble();
			alert["direction"] = query.value(3).toString();
			alert["triggered"] = query.value(4).toBool();
			alert["created_at"] = query.value(5).toString();
			alerts.append(alert);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["alerts"] = alerts;
	result["count"] = alerts.size();
	return result;
}

// ============================================================================
// FRAGMENT/MARKETPLACE - delegate or local tracking
// ============================================================================

QJsonObject Server::toolGetFragmentListings(const QJsonObject &args) {
	return toolListMarketplace(args);
}

QJsonObject Server::toolUpdateListing(const QJsonObject &args) {
	const auto slug = args.value("slug").toString().trimmed();
	const auto msgId = args.value("msg_id").toInt();
	const auto price = qint64(args.value("price").toVariant().toLongLong());
	if (!_session) {
		return toolError("No active session");
	}
	if (slug.isEmpty() && !msgId) {
		return toolError("either slug or msg_id is required to identify the "
			"saved gift being relisted");
	}
	if (price < 0) {
		return toolError("price must not be negative");
	}

	// Sets the real resale price. This used to validate listing_id and then
	// return success with the note "Listing updates require marketplace API",
	// having changed nothing.
	const auto gift = slug.isEmpty()
		? MTP_inputSavedStarGiftUser(MTP_int(msgId))
		: MTP_inputSavedStarGiftSlug(MTP_string(slug));
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPpayments_UpdateStarGiftPrice(
			gift,
			StarsAmountToTL(CreditsAmount(price))
		)).done([=](const MTPUpdates &result) {
			_session->api().applyUpdates(result);
			QJsonObject value;
			value["success"] = true;
			value["price"] = price;
			if (!slug.isEmpty()) value["slug"] = slug;
			if (msgId) value["msg_id"] = msgId;
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("payments.updateStarGiftPrice failed: " + error.type());
		}).send();
	});
}

QJsonObject Server::toolGetMarketTrends(const QJsonObject &args) {
	QString giftType = args.value("gift_type").toString();
	int days = args.value("days").toInt(7);

	QSqlQuery query(_db);
	query.prepare("SELECT date, AVG(price) as avg_price, COUNT(*) as volume "
				  "FROM price_history WHERE date >= date('now', '-' || ? || ' days') "
				  "GROUP BY date ORDER BY date");
	query.addBindValue(days);

	QJsonArray trends;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject point;
			point["date"] = query.value(0).toString();
			point["avg_price"] = query.value(1).toDouble();
			point["volume"] = query.value(2).toInt();
			trends.append(point);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["trends"] = trends;
	result["days"] = days;
	return result;
}

QJsonObject Server::toolCreatePriceAlert(const QJsonObject &args) {
	return toolSetPriceAlert(args);
}

QJsonObject Server::toolBacktestStrategy(const QJsonObject &args) {
	QString strategy = args["strategy"].toString();
	int days = args.value("days").toInt(30);
	double initialInvestment = args.value("initial_investment").toDouble(1000);
	QString giftType = args.value("gift_type").toString();

	// Fetch historical prices
	QSqlQuery query(_db);
	if (giftType.isEmpty()) {
		query.prepare("SELECT date, price, gift_type FROM price_history "
					  "WHERE date >= date('now', '-' || ? || ' days') "
					  "ORDER BY date ASC");
		query.addBindValue(days);
	} else {
		query.prepare("SELECT date, price, gift_type FROM price_history "
					  "WHERE gift_type = ? AND date >= date('now', '-' || ? || ' days') "
					  "ORDER BY date ASC");
		query.addBindValue(giftType);
		query.addBindValue(days);
	}

	QJsonObject result;
	result["strategy"] = strategy;
	result["days"] = days;
	result["initial_investment"] = initialInvestment;

	if (!query.exec()) {
		result["success"] = false;
		result["error"] = "Failed to query price history";
		return result;
	}

	QVector<double> prices;
	QJsonArray pricePoints;
	while (query.next()) {
		double price = query.value(1).toDouble();
		prices.append(price);
		QJsonObject pt;
		pt["date"] = query.value(0).toString();
		pt["price"] = price;
		pricePoints.append(pt);
	}

	if (prices.size() < 2) {
		result["success"] = true;
		result["final_value"] = initialInvestment;
		result["profit_loss"] = 0.0;
		result["trades"] = 0;
		result["note"] = "Insufficient price data for backtesting (need 2+ data points)";
		return result;
	}

	// Simulate strategies
	double portfolio = initialInvestment;
	double holdings = 0;
	int trades = 0;
	double maxDrawdown = 0;
	double peakValue = initialInvestment;

	if (strategy == "buy_and_hold") {
		holdings = initialInvestment / prices.first();
		portfolio = holdings * prices.last();
		trades = 1;
	} else if (strategy == "moving_average") {
		// Buy when price > 5-day MA, sell when below
		bool holding = false;
		double cash = initialInvestment;
		for (int i = 5; i < prices.size(); ++i) {
			double ma5 = 0;
			for (int j = i - 5; j < i; ++j) ma5 += prices[j];
			ma5 /= 5.0;

			if (!holding && prices[i] > ma5) {
				holdings = cash / prices[i];
				cash = 0;
				holding = true;
				trades++;
			} else if (holding && prices[i] < ma5) {
				cash = holdings * prices[i];
				holdings = 0;
				holding = false;
				trades++;
			}

			double currentValue = holding ? holdings * prices[i] : cash;
			if (currentValue > peakValue) peakValue = currentValue;
			double drawdown = (peakValue - currentValue) / peakValue;
			if (drawdown > maxDrawdown) maxDrawdown = drawdown;
		}
		portfolio = holdings > 0 ? holdings * prices.last() : cash;
	} else {
		// Default: buy_and_hold
		holdings = initialInvestment / prices.first();
		portfolio = holdings * prices.last();
		trades = 1;
	}

	result["success"] = true;
	result["final_value"] = portfolio;
	result["profit_loss"] = portfolio - initialInvestment;
	result["return_percent"] = (portfolio - initialInvestment) / initialInvestment * 100.0;
	result["trades"] = trades;
	result["max_drawdown_percent"] = maxDrawdown * 100.0;
	result["data_points"] = prices.size();
	result["price_range"] = pricePoints;
	return result;
}

// ============================================================================
// STAR REACTIONS - delegate to stars tools
// ============================================================================

QJsonObject Server::toolGetReactionStats(const QJsonObject &args) {
	return toolGetReactionAnalytics(args);
}

QJsonObject Server::toolGetStarReactionsReceived(const QJsonObject &args) {
	Q_UNUSED(args);
	QSqlQuery query(_db);
	query.prepare("SELECT chat_id, message_id, stars_count, created_at FROM star_reactions "
				  "ORDER BY created_at DESC LIMIT 100");

	QJsonArray reactions;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject r;
			r["chat_id"] = query.value(0).toLongLong();
			r["message_id"] = query.value(1).toLongLong();
			r["stars_count"] = query.value(2).toInt();
			r["created_at"] = query.value(3).toString();
			reactions.append(r);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["reactions"] = reactions;
	result["count"] = reactions.size();
	return result;
}

QJsonObject Server::toolGetStarReactionsSent(const QJsonObject &args) {
	Q_UNUSED(args);
	// Sent reactions tracked locally
	QSqlQuery query(_db);
	query.prepare("SELECT chat_id, message_id, stars_count, created_at FROM star_reactions "
				  "ORDER BY created_at DESC LIMIT 100");

	QJsonArray reactions;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject r;
			r["chat_id"] = query.value(0).toLongLong();
			r["message_id"] = query.value(1).toLongLong();
			r["stars_count"] = query.value(2).toInt();
			r["created_at"] = query.value(3).toString();
			reactions.append(r);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["reactions"] = reactions;
	result["count"] = reactions.size();
	return result;
}

QJsonObject Server::toolGetTopSupporters(const QJsonObject &args) {
	int limit = args.value("limit").toInt(10);

	// Aggregate from star_reactions by chat_id (content where reactions were received)
	// and from gift_transfers for gift-based support
	QJsonArray supporters;

	// Stars received per chat
	QSqlQuery query(_db);
	query.prepare("SELECT chat_id, SUM(stars_count) as total_stars, COUNT(*) as reaction_count "
				  "FROM star_reactions GROUP BY chat_id ORDER BY total_stars DESC LIMIT ?");
	query.addBindValue(limit);

	if (query.exec()) {
		int rank = 1;
		while (query.next()) {
			QJsonObject supporter;
			supporter["rank"] = rank++;
			supporter["chat_id"] = query.value(0).toLongLong();
			supporter["total_stars"] = query.value(1).toInt();
			supporter["reaction_count"] = query.value(2).toInt();
			supporter["source"] = "star_reactions";
			supporters.append(supporter);
		}
	}

	// Also include gift senders
	QSqlQuery giftQuery(_db);
	giftQuery.prepare("SELECT peer_id, SUM(stars_amount) as total_gifted, COUNT(*) as gift_count "
					  "FROM gift_transfers WHERE direction = 'received' "
					  "GROUP BY peer_id ORDER BY total_gifted DESC LIMIT ?");
	giftQuery.addBindValue(limit);

	if (giftQuery.exec()) {
		while (giftQuery.next()) {
			QJsonObject supporter;
			supporter["peer_id"] = giftQuery.value(0).toLongLong();
			supporter["total_gifted"] = giftQuery.value(1).toInt();
			supporter["gift_count"] = giftQuery.value(2).toInt();
			supporter["source"] = "gifts";
			supporters.append(supporter);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["supporters"] = supporters;
	result["count"] = supporters.size();
	return result;
}

// ============================================================================
// STAR RATING - local tracking
// ============================================================================

QJsonObject Server::toolGetStarRatingDetails(const QJsonObject &args) {
	Q_UNUSED(args);
	QSqlQuery query(_db);
	query.prepare("SELECT SUM(stars_count), COUNT(*) FROM star_reactions");

	QJsonObject result;
	if (query.exec() && query.next()) {
		int totalStars = query.value(0).toInt();
		int reactionCount = query.value(1).toInt();
		result["total_stars_received"] = totalStars;
		result["reaction_count"] = reactionCount;
		result["avg_stars_per_reaction"] = reactionCount > 0 ? (double)totalStars / reactionCount : 0;
	}
	result["success"] = true;
	return result;
}

QJsonObject Server::toolSimulateRatingChange(const QJsonObject &args) {
	int additionalStars = args["additional_stars"].toInt();
	int additionalReactions = args.value("additional_reactions").toInt(1);

	QSqlQuery query(_db);
	query.prepare("SELECT SUM(stars_count), COUNT(*) FROM star_reactions");

	QJsonObject result;
	if (query.exec() && query.next()) {
		int currentStars = query.value(0).toInt();
		int currentCount = query.value(1).toInt();
		int newStars = currentStars + additionalStars;
		int newCount = currentCount + additionalReactions;
		result["current_avg"] = currentCount > 0 ? (double)currentStars / currentCount : 0;
		result["simulated_avg"] = newCount > 0 ? (double)newStars / newCount : 0;
		result["change"] = (newCount > 0 ? (double)newStars / newCount : 0) - (currentCount > 0 ? (double)currentStars / currentCount : 0);
	}
	result["success"] = true;
	return result;
}

QJsonObject Server::toolGetRatingHistory(const QJsonObject &args) {
	int days = args.value("days").toInt(30);

	QSqlQuery query(_db);
	query.prepare("SELECT date(created_at) as day, SUM(stars_count), COUNT(*) "
				  "FROM star_reactions WHERE created_at >= date('now', '-' || ? || ' days') "
				  "GROUP BY day ORDER BY day");
	query.addBindValue(days);

	QJsonArray history;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject entry;
			entry["date"] = query.value(0).toString();
			entry["stars"] = query.value(1).toInt();
			entry["reactions"] = query.value(2).toInt();
			history.append(entry);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["history"] = history;
	result["days"] = days;
	return result;
}

// ============================================================================
// PROFILE GIFTS - local tracking
// ============================================================================

QJsonObject Server::toolGetProfileGifts(const QJsonObject &args) {
	Q_UNUSED(args);
	QSqlQuery query(_db);
	query.prepare("SELECT gift_type, quantity, avg_price FROM portfolio WHERE quantity > 0");

	QJsonArray gifts;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject gift;
			gift["gift_type"] = query.value(0).toString();
			gift["quantity"] = query.value(1).toInt();
			gift["value"] = query.value(2).toDouble();
			gifts.append(gift);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["gifts"] = gifts;
	result["count"] = gifts.size();
	return result;
}

QJsonObject Server::toolUpdateGiftDisplay(const QJsonObject &args) {
	const auto chatId = args.value("chat_id").toVariant().toLongLong();
	const auto msgIds = args.value("msg_ids").toArray();
	if (!_session) {
		return toolError("No active session");
	}
	if (msgIds.isEmpty()) {
		return toolError("msg_ids is required: pass the saved-gift message ids "
			"to pin, in the order they should appear");
	}
	const auto peer = chatId
		? _session->data().peerLoaded(PeerId(chatId))
		: _session->user().get();
	if (!peer) {
		return toolError("Chat not found");
	}

	auto gifts = QVector<MTPInputSavedStarGift>();
	gifts.reserve(msgIds.size());
	for (const auto &value : msgIds) {
		gifts.push_back(MTP_inputSavedStarGiftUser(MTP_int(value.toInt())));
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPpayments_ToggleStarGiftsPinnedToTop(
			peer->input(),
			MTP_vector<MTPInputSavedStarGift>(gifts)
		)).done([=](const MTPBool &result) {
			QJsonObject value;
			value["success"] = mtpIsTrue(result);
			value["pinned_count"] = msgIds.size();
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("payments.toggleStarGiftsPinnedToTop failed: " + error.type());
		}).send();
	});
}

QJsonObject Server::toolReorderProfileGifts(const QJsonObject &args) {
	const auto chatId = args.value("chat_id").toVariant().toLongLong();
	const auto order = args.value("collection_ids").toArray();
	if (!_session) {
		return toolError("No active session");
	}
	if (order.isEmpty()) {
		return toolError("collection_ids is required: Telegram reorders gift "
			"collections, not individual gifts. Use update_gift_display to pin "
			"specific gifts to the top.");
	}
	const auto peer = chatId
		? _session->data().peerLoaded(PeerId(chatId))
		: _session->user().get();
	if (!peer) {
		return toolError("Chat not found");
	}

	auto ids = QVector<MTPint>();
	ids.reserve(order.size());
	for (const auto &value : order) {
		ids.push_back(MTP_int(value.toInt()));
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPpayments_ReorderStarGiftCollections(
			peer->input(),
			MTP_vector<MTPint>(ids)
		)).done([=](const MTPBool &result) {
			QJsonObject value;
			value["success"] = mtpIsTrue(result);
			value["collection_count"] = order.size();
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("payments.reorderStarGiftCollections failed: " + error.type());
		}).send();
	});
}

QJsonObject Server::toolToggleGiftNotifications(const QJsonObject &args) {
	const auto chatId = args["chat_id"].toVariant().toLongLong();
	const auto enabled = args.value("enabled").toBool(true);
	if (!_session) {
		return toolError("No active session");
	}
	if (!chatId) {
		return toolError("chat_id is required and must be non-zero");
	}
	const auto peer = resolvePeer(chatId);
	const auto channel = peer ? peer->asChannel() : nullptr;
	if (!channel) {
		return toolError("Chat not found");
	}

	// Only broadcast channels have this setting. Telegram rejects a megagroup
	// with PEER_ID_INVALID, which reads as a malformed chat_id and sends the
	// caller looking at the wrong thing -- measured directly: the same call
	// succeeds on a broadcast channel and fails on a supergroup created
	// moments apart. canManageGifts() does not separate them, since it only
	// asks whether we can post.
	if (!channel->isBroadcast()) {
		return toolError(QString(
			"Chat %1 is a supergroup; gift notifications exist only for "
			"broadcast channels").arg(chatId));
	}
	if (!channel->canManageGifts()) {
		return toolError("This account cannot manage gifts for that channel");
	}

	using Flag = MTPpayments_ToggleChatStarGiftNotifications::Flag;
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPpayments_ToggleChatStarGiftNotifications(
			MTP_flags(enabled ? Flag::f_enabled : Flag(0)),
			channel->input()
		)).done([=](const MTPBool &result) {
			QJsonObject value;
			value["success"] = mtpIsTrue(result);
			value["chat_id"] = chatId;
			value["enabled"] = enabled;
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("payments.toggleChatStarGiftNotifications failed: "
				+ error.type());
		}).send();
	});
}

// ============================================================================
// GIFT INVESTMENT/ANALYTICS - delegate to stars tools
// ============================================================================

QJsonObject Server::toolGetGiftInvestmentAdvice(const QJsonObject &args) {
	Q_UNUSED(args);

	// Reports what this client has actually recorded, and nothing else. The
	// previous version returned a fixed sentence -- "monitor market trends and
	// diversify" -- alongside real numbers, which reads as analysis of them and
	// is not. Price movement is a fact we can compute from recorded purchases
	// and observed prices; advice is not something we are positioned to give.
	QSqlQuery query(_db);
	query.prepare("SELECT p.gift_type, p.quantity, p.avg_price, "
		"p.current_value, "
		"(SELECT MIN(price) FROM price_history h WHERE h.gift_type = p.gift_type), "
		"(SELECT MAX(price) FROM price_history h WHERE h.gift_type = p.gift_type), "
		"(SELECT COUNT(*) FROM price_history h WHERE h.gift_type = p.gift_type) "
		"FROM portfolio p WHERE p.quantity > 0");
	if (!query.exec()) {
		return toolError("Could not read portfolio: "
			+ query.lastError().text());
	}

	QJsonArray holdings;
	auto totalPaid = 0.;
	auto totalNow = 0.;
	while (query.next()) {
		const auto quantity = query.value(1).toInt();
		const auto avgPrice = query.value(2).toDouble();
		const auto current = query.value(3).toDouble();
		const auto samples = query.value(6).toInt();
		totalPaid += avgPrice * quantity;
		totalNow += current;

		QJsonObject entry;
		entry["gift_type"] = query.value(0).toString();
		entry["quantity"] = quantity;
		entry["avg_paid"] = avgPrice;
		entry["current_value"] = current;
		entry["change"] = current - (avgPrice * quantity);
		// Only meaningful with recorded history; say how much there is rather
		// than implying a range exists when we have seen one price.
		entry["price_samples"] = samples;
		if (samples > 1) {
			entry["observed_low"] = query.value(4).toDouble();
			entry["observed_high"] = query.value(5).toDouble();
		}
		holdings.append(entry);
	}

	QJsonObject result;
	result["success"] = true;
	result["holdings"] = holdings;
	result["total_paid"] = totalPaid;
	result["total_current_value"] = totalNow;
	result["total_change"] = totalNow - totalPaid;
	result["note"] = "Observed from locally recorded purchases and prices. "
		"Not a valuation and not advice.";
	return result;
}

QJsonObject Server::toolGetPortfolioPerformance(const QJsonObject &args) {
	return toolGetPortfolioValue(args);
}

// ============================================================================
// STAR GIFTS - delegate to stars tools or local tracking
// ============================================================================

QJsonObject Server::toolListStarGifts(const QJsonObject &args) {
	return toolListAvailableGifts(args);
}

QJsonObject Server::toolGetStarGiftDetails(const QJsonObject &args) {
	return toolGetGiftPriceHistory(args);
}

QJsonObject Server::toolBrowseGiftMarketplace(const QJsonObject &args) {
	return toolListMarketplace(args);
}

QJsonObject Server::toolGetGiftDetails(const QJsonObject &args) {
	const auto slug = args["slug"].toString().trimmed();
	if (!_session) {
		return toolError("No active session");
	}
	if (slug.isEmpty()) {
		return toolError("slug is required: unique gifts are addressed by slug, "
			"not by numeric id");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPpayments_GetUniqueStarGift(
			MTP_string(slug)
		)).done([=](const MTPpayments_UniqueStarGift &result) {
			const auto &data = result.data();
			QJsonObject value;
			value["success"] = true;
			data.vgift().match([&](const MTPDstarGiftUnique &gift) {
				value["id"] = qint64(gift.vid().v);
				value["gift_id"] = qint64(gift.vgift_id().v);
				value["title"] = qs(gift.vtitle());
				value["slug"] = qs(gift.vslug());
				value["num"] = gift.vnum().v;
				value["availability_issued"] = gift.vavailability_issued().v;
				value["availability_total"] = gift.vavailability_total().v;
				value["attribute_count"] = gift.vattributes().v.size();
				if (const auto owner = gift.vowner_name()) {
					value["owner_name"] = qs(*owner);
				}
			}, [&](const MTPDstarGift &gift) {
				value["id"] = qint64(gift.vid().v);
				value["note"] = "slug resolved to a non-unique gift";
			});
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("payments.getUniqueStarGift failed: " + error.type());
		}).send();
	});
}

QJsonObject Server::toolGetUpgradeOptions(const QJsonObject &args) {
	const auto giftId = args["gift_id"].toVariant().toLongLong();
	if (!_session) {
		return toolError("No active session");
	}
	if (!giftId) {
		return toolError("gift_id is required and must be non-zero");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPpayments_GetStarGiftUpgradePreview(
			MTP_long(giftId)
		)).done([=](const MTPpayments_StarGiftUpgradePreview &result) {
			const auto &data = result.data();
			QJsonArray prices;
			for (const auto &price : data.vprices().v) {
				const auto &pd = price.data();
				QJsonObject entry;
				entry["upgrade_stars"] = qint64(pd.vupgrade_stars().v);
				entry["date"] = pd.vdate().v;
				prices.append(entry);
			}
			QJsonObject value;
			value["success"] = true;
			value["gift_id"] = giftId;
			value["prices"] = prices;
			value["sample_attribute_count"] = data.vsample_attributes().v.size();
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("payments.getStarGiftUpgradePreview failed: " + error.type());
		}).send();
	});
}

QJsonObject Server::toolGetGiftTransferHistory(const QJsonObject &args) {
	int limit = args.value("limit").toInt(50);

	QSqlQuery query(_db);
	query.prepare("SELECT id, gift_id, direction, peer_id, stars_amount, created_at "
				  "FROM gift_transfers ORDER BY created_at DESC LIMIT ?");
	query.addBindValue(limit);

	QJsonArray transfers;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject transfer;
			transfer["id"] = query.value(0).toLongLong();
			transfer["gift_id"] = query.value(1).toString();
			transfer["direction"] = query.value(2).toString();
			transfer["peer_id"] = query.value(3).toLongLong();
			transfer["stars_amount"] = query.value(4).toInt();
			transfer["created_at"] = query.value(5).toString();
			transfers.append(transfer);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["transfers"] = transfers;
	result["count"] = transfers.size();
	return result;
}

QJsonObject Server::toolGetGiftAnalytics(const QJsonObject &args) {
	return toolGetReactionAnalytics(args);
}

QJsonObject Server::toolGetUniqueGiftAnalytics(const QJsonObject &args) {
	const auto giftType = args["gift_type"].toString().trimmed();
	if (giftType.isEmpty()) {
		return toolError("gift_type is required");
	}

	// Reports the requested gift type. It used to aggregate the whole
	// portfolio and return that under any gift_type the caller passed.
	QSqlQuery query(_db);
	query.prepare("SELECT quantity, avg_price, current_value "
		"FROM portfolio WHERE gift_type = ?");
	query.addBindValue(giftType);
	if (!query.exec()) {
		return toolError("Could not read portfolio: "
			+ query.lastError().text());
	}

	QJsonObject result;
	result["success"] = true;
	result["gift_type"] = giftType;
	if (query.next()) {
		result["quantity"] = query.value(0).toInt();
		result["avg_price"] = query.value(1).toDouble();
		result["current_value"] = query.value(2).toDouble();
		result["held"] = true;
	} else {
		result["held"] = false;
		result["note"] = "No holding recorded locally for this gift type";
	}
	return result;
}

// ============================================================================
// SUBSCRIPTION TOOLS - delegate to wallet tools
// ============================================================================

QJsonObject Server::toolGetSubscriptionAlerts(const QJsonObject &args) {
	Q_UNUSED(args);
	QSqlQuery query(_db);
	query.prepare("SELECT id, gift_type, target_price, direction, triggered, created_at FROM price_alerts "
				  "WHERE gift_type LIKE 'sub:%' ORDER BY created_at DESC");

	QJsonArray alerts;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject alert;
			alert["id"] = query.value(0).toLongLong();
			alert["subscription"] = query.value(1).toString().mid(4);
			alert["threshold"] = query.value(2).toDouble();
			alert["triggered"] = query.value(4).toBool();
			alert["created_at"] = query.value(5).toString();
			alerts.append(alert);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["alerts"] = alerts;
	return result;
}

QJsonObject Server::toolCancelSubscription(const QJsonObject &args) {
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
		using Flag = MTPpayments_ChangeStarsSubscription::Flag;
		_session->api().request(MTPpayments_ChangeStarsSubscription(
			MTP_flags(Flag::f_canceled),
			MTP_inputPeerSelf(),
			MTP_string(subscriptionId),
			MTP_bool(true)
		)).done([subscriptionId]() {
			qWarning() << "MCP: Subscription" << subscriptionId << "cancelled successfully";
		}).fail([subscriptionId](const MTP::Error &error) {
			qWarning() << "MCP: Failed to cancel subscription" << subscriptionId << ":" << error.type();
		}).send();

		result["success"] = true;
		result["subscription_id"] = subscriptionId;
		result["status"] = "cancellation_submitted";
		result["api_request"] = "submitted";
		result["note"] = "Cancellation request sent via MTPpayments_ChangeStarsSubscription";
	} else {
		result["success"] = true;
		result["channel_id"] = channelId;
		result["status"] = "need_subscription_id";
		result["note"] = "To cancel, provide subscription_id from list_subscriptions. "
						 "channel_id alone is not sufficient for cancellation.";
	}

	return result;
}

// ============================================================================
// PAID CONTENT - delegate to stars tools
// ============================================================================

QJsonObject Server::toolGetUnlockedContent(const QJsonObject &args) {
	return toolListPurchasedContent(args);
}

QJsonObject Server::toolGetPaidContentEarnings(const QJsonObject &args) {
	return toolGetPaidContentStats(args);
}

QJsonObject Server::toolGetPaidMediaStats(const QJsonObject &args) {
	return toolGetPaidContentStats(args);
}

// ============================================================================
// CHANNEL EARNINGS - local tracking
// ============================================================================

QJsonObject Server::toolGetChannelEarnings(const QJsonObject &args) {
	qint64 channelId = args["channel_id"].toVariant().toLongLong();

	QSqlQuery query(_db);
	query.prepare("SELECT SUM(amount) FROM wallet_spending WHERE peer_id = ? AND amount > 0");
	query.addBindValue(channelId);

	QJsonObject result;
	if (query.exec() && query.next()) {
		result["channel_id"] = channelId;
		result["total_earnings"] = query.value(0).toDouble();
	}
	result["success"] = true;
	return result;
}

QJsonObject Server::toolGetAllChannelsEarnings(const QJsonObject &args) {
	Q_UNUSED(args);
	QSqlQuery query(_db);
	query.prepare("SELECT peer_id, SUM(amount) as total FROM wallet_spending "
				  "WHERE amount > 0 AND peer_id IS NOT NULL "
				  "GROUP BY peer_id ORDER BY total DESC");

	QJsonArray earnings;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject entry;
			entry["channel_id"] = query.value(0).toLongLong();
			entry["total_earnings"] = query.value(1).toDouble();
			earnings.append(entry);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["earnings"] = earnings;
	return result;
}

QJsonObject Server::toolGetEarningsChart(const QJsonObject &args) {
	int days = args.value("days").toInt(30);

	QSqlQuery query(_db);
	query.prepare("SELECT date, SUM(amount) FROM wallet_spending "
				  "WHERE amount > 0 AND date >= date('now', '-' || ? || ' days') "
				  "GROUP BY date ORDER BY date");
	query.addBindValue(days);

	QJsonArray chart;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject point;
			point["date"] = query.value(0).toString();
			point["earnings"] = query.value(1).toDouble();
			chart.append(point);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["chart"] = chart;
	result["days"] = days;
	return result;
}

// ============================================================================
// GIVEAWAY TOOLS - local tracking
// ============================================================================

QJsonObject Server::toolListGiveaways(const QJsonObject &args) {
	int limit = args.value("limit").toInt(50);
	QString status = args.value("status").toString();

	QSqlQuery query(_db);
	if (status.isEmpty()) {
		query.prepare("SELECT id, type, stars_amount, winner_count, channel_id, status, created_at "
					  "FROM giveaways ORDER BY created_at DESC LIMIT ?");
		query.addBindValue(limit);
	} else {
		query.prepare("SELECT id, type, stars_amount, winner_count, channel_id, status, created_at "
					  "FROM giveaways WHERE status = ? ORDER BY created_at DESC LIMIT ?");
		query.addBindValue(status);
		query.addBindValue(limit);
	}

	QJsonArray giveaways;
	if (query.exec()) {
		while (query.next()) {
			QJsonObject g;
			g["id"] = query.value(0).toLongLong();
			g["type"] = query.value(1).toString();
			g["stars_amount"] = query.value(2).toInt();
			g["winner_count"] = query.value(3).toInt();
			g["channel_id"] = query.value(4).toLongLong();
			g["status"] = query.value(5).toString();
			g["created_at"] = query.value(6).toString();
			giveaways.append(g);
		}
	}

	QJsonObject result;
	result["success"] = true;
	result["giveaways"] = giveaways;
	result["count"] = giveaways.size();
	return result;
}

QJsonObject Server::toolGetGiveawayOptions(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPpayments_GetStarsGiveawayOptions(
		)).done([=](const MTPVector<MTPStarsGiveawayOption> &result) {
			QJsonArray options;
			for (const auto &option : result.v) {
				const auto &data = option.data();
				QJsonObject entry;
				entry["stars"] = qint64(data.vstars().v);
				entry["currency"] = qs(data.vcurrency());
				entry["amount"] = qint64(data.vamount().v);
				entry["yearly_boosts"] = data.vyearly_boosts().v;
				entry["is_default"] = data.is_default();
				entry["extended"] = data.is_extended();
				QJsonArray winners;
				for (const auto &w : data.vwinners().v) {
					const auto &wd = w.data();
					QJsonObject entry2;
					entry2["users"] = wd.vusers().v;
					entry2["per_user_stars"] = qint64(wd.vper_user_stars().v);
					entry2["is_default"] = wd.is_default();
					winners.append(entry2);
				}
				entry["winners_options"] = winners;
				options.append(entry);
			}
			QJsonObject value;
			value["success"] = true;
			value["options"] = options;
			value["count"] = options.size();
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail("payments.getStarsGiveawayOptions failed: " + error.type());
		}).send();
	});
}

QJsonObject Server::toolGetGiveawayStats(const QJsonObject &args) {
	const auto giveawayId = args["giveaway_id"].toVariant().toLongLong();
	if (!giveawayId) {
		return toolError("giveaway_id is required and must be non-zero");
	}

	// Reports the giveaway asked for. It previously aggregated every giveaway
	// in the table and returned those totals whatever id was passed, so the
	// numbers described the wrong thing whenever more than one existed.
	QSqlQuery query(_db);
	query.prepare("SELECT type, stars_amount, winner_count, channel_id, "
		"status, created_at FROM giveaways WHERE id = ?");
	query.addBindValue(giveawayId);
	if (!query.exec()) {
		return toolError("Could not read giveaway: "
			+ query.lastError().text());
	}
	if (!query.next()) {
		return toolError(QString("No giveaway recorded locally with id %1")
			.arg(giveawayId));
	}

	QJsonObject result;
	result["success"] = true;
	result["giveaway_id"] = giveawayId;
	result["type"] = query.value(0).toString();
	result["stars_amount"] = query.value(1).toInt();
	result["winner_count"] = query.value(2).toInt();
	result["channel_id"] = query.value(3).toLongLong();
	result["status"] = query.value(4).toString();
	result["created_at"] = query.value(5).toString();
	return result;
}

// ============================================================================
// BLOCK/UNBLOCK - real Telegram API
// ============================================================================

QJsonObject Server::toolBlockUser(const QJsonObject &args) {
	qint64 userId = args["user_id"].toVariant().toLongLong();

	if (!_session) {
		QJsonObject result;
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	if (userId == 0) {
		QJsonObject result;
		result["error"] = "Missing user_id parameter";
		result["success"] = false;
		return result;
	}

	auto peer = resolvePeer(userId);
	if (!peer) {
		QJsonObject result;
		result["error"] = "User not found";
		result["success"] = false;
		return result;
	}

	_session->api().blockedPeers().block(peer);

	QJsonObject result;
	result["success"] = true;
	result["user_id"] = userId;
	result["status"] = "blocked";
	return result;
}

QJsonObject Server::toolUnblockUser(const QJsonObject &args) {
	qint64 userId = args["user_id"].toVariant().toLongLong();

	if (!_session) {
		QJsonObject result;
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	if (userId == 0) {
		QJsonObject result;
		result["error"] = "Missing user_id parameter";
		result["success"] = false;
		return result;
	}

	auto peer = resolvePeer(userId);
	if (!peer) {
		QJsonObject result;
		result["error"] = "User not found";
		result["success"] = false;
		return result;
	}

	_session->api().blockedPeers().unblock(peer);

	QJsonObject result;
	result["success"] = true;
	result["user_id"] = userId;
	result["status"] = "unblocked";
	return result;
}

// ============================================================================
// TAG TOOLS - delegate to premium tools implementations
// ============================================================================

QJsonObject Server::toolTagMessage(const QJsonObject &args) {
	return toolAddMessageTag(args);
}

QJsonObject Server::toolListTags(const QJsonObject &args) {
	return toolGetMessageTags(args);
}

QJsonObject Server::toolDeleteTag(const QJsonObject &args) {
	return toolRemoveMessageTag(args);
}

// ============================================================================
// TASK TOOLS - delegate to premium tools
// ============================================================================

QJsonObject Server::toolCreateTask(const QJsonObject &args) {
	return toolCreateTaskFromMessage(args);
}

// ============================================================================
// AWAY TOOLS - delegate to business tools
// ============================================================================

QJsonObject Server::toolGetAwayConfig(const QJsonObject &args) {
	return toolGetAwayMessage(args);
}

QJsonObject Server::toolSetAwayNow(const QJsonObject &args) {
	const auto message = args["message"].toString();
	if (message.isEmpty()) {
		return toolError("message is required: away mode needs the text to "
			"reply with");
	}

	// Stores the message it was given. It previously flipped the enabled flag
	// and ignored the argument entirely, so callers got away mode with
	// whatever text happened to be configured before -- or an error claiming
	// nothing was configured while holding the text to configure it with.
	QSqlQuery query(_db);
	query.prepare("INSERT INTO away_config (id, enabled, message, updated_at) "
		"VALUES (1, 1, ?, datetime('now')) "
		"ON CONFLICT(id) DO UPDATE SET "
		"enabled = 1, message = excluded.message, "
		"updated_at = datetime('now')");
	query.addBindValue(message);
	if (!query.exec()) {
		return toolError("Could not enable away mode: "
			+ query.lastError().text());
	}

	QJsonObject result;
	result["success"] = true;
	result["enabled"] = true;
	result["message"] = message;
	return result;
}

QJsonObject Server::toolGetAwayStats(const QJsonObject &args) {
	Q_UNUSED(args);
	QSqlQuery query(_db);
	query.prepare("SELECT away_sent, updated_at FROM away_config WHERE id = 1");

	QJsonObject result;
	if (query.exec() && query.next()) {
		result["away_replies_sent"] = query.value(0).toInt();
		result["last_updated"] = query.value(1).toString();
		result["success"] = true;
	} else {
		result["away_replies_sent"] = 0;
		result["success"] = true;
		result["note"] = "No away message configured";
	}
	return result;
}

// ============================================================================
// BUSINESS HOURS - delegate to business tools
// ============================================================================

QJsonObject Server::toolIsOpenNow(const QJsonObject &args) {
	return toolCheckBusinessStatus(args);
}

// ============================================================================
// VOICE CLONE - local storage
// ============================================================================

QJsonObject Server::toolCloneVoice(const QJsonObject &args) {
	QString name = args["name"].toString();
	QString audioSample = args["audio_sample"].toString();
	if (audioSample.isEmpty()) {
		audioSample = args["audio_path"].toString();
	}

	QJsonObject result;

	if (name.isEmpty()) {
		result["error"] = "Missing name parameter";
		result["success"] = false;
		return result;
	}

	if (audioSample.isEmpty()) {
		result["error"] = "Missing audio_sample/audio_path parameter - "
						  "provide path to a WAV file with the voice to clone";
		result["success"] = false;
		return result;
	}

	if (!QFile::exists(audioSample)) {
		result["error"] = QString("Audio sample file not found: %1").arg(audioSample);
		result["success"] = false;
		return result;
	}

	// Store the clone sample reference
	QSqlQuery sampleQuery(_db);
	sampleQuery.prepare("INSERT OR REPLACE INTO voice_clone_samples "
						"(name, sample_path, provider, language, created_at) "
						"VALUES (?, ?, 'coqui', 'en', datetime('now'))");
	sampleQuery.addBindValue(name);
	sampleQuery.addBindValue(audioSample);
	sampleQuery.exec();

	// Create/update voice persona pointing to this clone
	QSqlQuery query(_db);
	query.prepare("INSERT OR REPLACE INTO voice_persona "
				  "(name, voice_id, pitch, speed, provider, sample_path, created_at) "
				  "VALUES (?, 'xtts_v2_clone', 1.0, 1.0, 'coqui', ?, datetime('now'))");
	query.addBindValue(name);
	query.addBindValue(audioSample);

	if (query.exec()) {
		// Test the clone if TTS service is available
		if (_textToSpeech && _textToSpeech->isRunning()) {
			auto testResult = _textToSpeech->cloneAndSynthesize(
				"This is a voice clone test.", audioSample, 1.0);
			if (testResult.success) {
				result["test_synthesis"] = "success";
				result["test_duration"] = testResult.durationSeconds;
			} else {
				result["test_synthesis"] = "failed";
				result["test_error"] = testResult.error;
				result["note"] = "Clone registered but test synthesis failed. "
								 "Ensure Coqui TTS with XTTS-v2 is installed: "
								 "pip install TTS";
			}
		}

		result["success"] = true;
		result["name"] = name;
		result["sample_path"] = audioSample;
		result["provider"] = "coqui";
		result["status"] = "clone_registered";
	} else {
		result["success"] = false;
		result["error"] = "Failed to register voice clone: " + query.lastError().text();
	}
	return result;
}

// ============================================================================
// GIFTS - delegate to wallet/stars tools
// ============================================================================

QJsonObject Server::toolListGifts(const QJsonObject &args) {
	return toolListAvailableGifts(args);
}

QJsonObject Server::toolGetStarRating(const QJsonObject &args) {
	return toolGetStarRatingDetails(args);
}

// ============================================================================
// FINANCIAL - delegate to wallet tools
// ============================================================================

QJsonObject Server::toolGetTaxSummary(const QJsonObject &args) {
	const auto year = args["year"].toInt();
	if (year < 1970 || year > 2100) {
		return toolError("year is required and must be a four-digit calendar "
			"year");
	}

	// Scoped to the requested year. This used to sum every transaction ever
	// recorded and return it under whatever year the caller asked for, which
	// is the one thing a tax summary must never do.
	const auto from = QString::number(year) + "-01-01";
	const auto to = QString::number(year) + "-12-31";

	QSqlQuery query(_db);
	query.prepare("SELECT "
		"SUM(CASE WHEN amount > 0 THEN amount ELSE 0 END), "
		"SUM(CASE WHEN amount < 0 THEN ABS(amount) ELSE 0 END), "
		"COUNT(*) "
		"FROM wallet_spending WHERE date(date) BETWEEN date(?) AND date(?)");
	query.addBindValue(from);
	query.addBindValue(to);

	double income = 0, expense = 0;
	int count = 0;
	if (!query.exec()) {
		return toolError("Could not read transactions: "
			+ query.lastError().text());
	}
	if (query.next()) {
		income = query.value(0).toDouble();
		expense = query.value(1).toDouble();
		count = query.value(2).toInt();
	}

	QJsonObject result;
	result["success"] = true;
	result["year"] = year;
	result["total_income"] = income;
	result["total_expenses"] = expense;
	result["net_income"] = income - expense;
	result["transaction_count"] = count;
	result["note"] = "Locally tracked transactions only; nothing here is "
		"synced from Telegram and it is not tax advice";
	return result;
}

// ============================================================================
// STAR GIFTS - delegate to stars tools
// ============================================================================

QJsonObject Server::toolSendStarGift(const QJsonObject &args) {
	return toolSendGift(args);
}

QJsonObject Server::toolTransferGift(const QJsonObject &args) {
	QString giftId = args["gift_id"].toString();
	qint64 recipientId = args["recipient_id"].toVariant().toLongLong();

	QJsonObject result;

	if (giftId.isEmpty() || recipientId == 0) {
		result["error"] = "Missing gift_id or recipient_id";
		result["success"] = false;
		return result;
	}

	if (!_session) {
		result["error"] = "No active session";
		result["success"] = false;
		return result;
	}

	auto recipient = resolvePeer(recipientId);
	if (!recipient) {
		result["error"] = QString("Recipient %1 not found").arg(recipientId);
		result["success"] = false;
		return result;
	}

	// Determine input gift ID - check if it's a slug (unique gift) or numeric message ID
	MTPInputSavedStarGift inputGift = giftId.toLongLong() > 0
		? MTP_inputSavedStarGiftUser(MTP_int(giftId.toInt()))
		: MTP_inputSavedStarGiftSlug(MTP_string(giftId));

	_session->api().request(MTPpayments_TransferStarGift(
		inputGift,
		recipient->input()
	)).done([giftId, recipientId](const MTPUpdates &updates) {
		qWarning() << "MCP: Gift" << giftId << "transferred to" << recipientId << "successfully";
	}).fail([giftId, recipientId](const MTP::Error &error) {
		qWarning() << "MCP: Failed to transfer gift" << giftId << "to" << recipientId << ":" << error.type();
	}).send();

	// Record transfer in local DB
	QSqlQuery query(_db);
	query.prepare("INSERT INTO gift_transfers (gift_id, from_user, to_user, transfer_date, status) "
				  "VALUES (?, ?, ?, datetime('now'), 'submitted')");
	query.addBindValue(giftId);
	query.addBindValue(_session->userId().bare);
	query.addBindValue(recipientId);
	query.exec();

	result["success"] = true;
	result["gift_id"] = giftId;
	result["recipient_id"] = recipientId;
	result["status"] = "transfer_submitted";
	result["api_request"] = "submitted";
	result["note"] = "Transfer request sent via MTPpayments_TransferStarGift";
	return result;
}

QJsonObject Server::toolCancelListing(const QJsonObject &args) {
	return toolDelistGift(args);
}

// ============================================================================
// WAVE 1 - account.* security/content tools (real MTP, read-only + toggles)
// ============================================================================

QJsonObject Server::toolGetSensitiveContentSetting(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPaccount_GetContentSettings(
		)).done([=](const MTPaccount_ContentSettings &result) {
			const auto &data = result.data();
			auto value = QJsonObject();
			value["success"] = true;
			value["sensitive_enabled"] = data.is_sensitive_enabled();
			value["sensitive_can_change"] = data.is_sensitive_can_change();
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolSetSensitiveContentEnabled(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	if (!args.contains("enabled")) {
		return toolError("enabled is required (true to show sensitive content)");
	}
	const auto enabled = args["enabled"].toBool();
	using Flags = MTPaccount_SetContentSettings::Flags;
	auto flags = Flags(0);
	if (enabled) {
		flags |= MTPaccount_SetContentSettings::Flag::f_sensitive_enabled;
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPaccount_SetContentSettings(
			MTP_flags(flags)
		)).done([=]() {
			auto value = QJsonObject();
			value["success"] = true;
			value["sensitive_enabled"] = enabled;
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolGetConnectedWebsites(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPaccount_GetWebAuthorizations(
		)).done([=](const MTPaccount_WebAuthorizations &result) {
			const auto &data = result.data();
			_session->data().processUsers(data.vusers());
			auto list = QJsonArray();
			for (const auto &auth : data.vauthorizations().v) {
				const auto &w = auth.data();
				auto entry = QJsonObject();
				// hash is a 64-bit id returned as a string so JSON keeps it intact.
				entry["hash"] = QString::number(w.vhash().v);
				entry["bot_id"] = qint64(w.vbot_id().v);
				entry["domain"] = qs(w.vdomain());
				entry["browser"] = qs(w.vbrowser());
				entry["platform"] = qs(w.vplatform());
				entry["ip"] = qs(w.vip());
				entry["region"] = qs(w.vregion());
				entry["date_created"] = qint64(w.vdate_created().v);
				entry["date_active"] = qint64(w.vdate_active().v);
				list.append(entry);
			}
			auto value = QJsonObject();
			value["success"] = true;
			value["websites"] = list;
			value["count"] = list.size();
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

// Wave 3 - account security & global privacy (real MTP).
QJsonObject Server::toolGet2faStatus(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPaccount_GetPassword(
		)).done([=](const MTPaccount_Password &result) {
			const auto &data = result.data();
			auto value = QJsonObject();
			value["success"] = true;
			value["has_password"] = data.is_has_password();
			value["has_recovery"] = data.is_has_recovery();
			if (const auto hint = data.vhint()) {
				value["hint"] = qs(*hint);
			}
			if (const auto email = data.vlogin_email_pattern()) {
				value["login_email_pattern"] = qs(*email);
			}
			if (const auto unconf = data.vemail_unconfirmed_pattern()) {
				value["email_unconfirmed_pattern"] = qs(*unconf);
			}
			if (const auto reset = data.vpending_reset_date()) {
				value["pending_reset_date"] = qint64(reset->v);
			}
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolGetAccountSelfDestruct(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPaccount_GetAccountTTL(
		)).done([=](const MTPAccountDaysTTL &result) {
			auto value = QJsonObject();
			value["success"] = true;
			value["days"] = result.data().vdays().v;
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolSetAccountSelfDestruct(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto days = args["days"].toInt();
	if (days <= 0) {
		return toolError("days must be a positive number of days of inactivity "
			"before the account is deleted");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPaccount_SetAccountTTL(
			MTP_accountDaysTTL(MTP_int(days))
		)).done([=]() {
			auto value = QJsonObject();
			value["success"] = true;
			value["days"] = days;
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolTerminateAllOtherSessions(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPauth_ResetAuthorizations(
		)).done([=]() {
			auto value = QJsonObject();
			value["success"] = true;
			value["note"] = "All other sessions (devices) have been logged out.";
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolGetGlobalPrivacy(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPaccount_GetGlobalPrivacySettings(
		)).done([=](const MTPGlobalPrivacySettings &result) {
			const auto &d = result.data();
			auto value = QJsonObject();
			value["success"] = true;
			value["archive_and_mute_new_noncontact_peers"]
				= d.is_archive_and_mute_new_noncontact_peers();
			value["keep_archived_unmuted"] = d.is_keep_archived_unmuted();
			value["keep_archived_folders"] = d.is_keep_archived_folders();
			value["hide_read_marks"] = d.is_hide_read_marks();
			value["new_noncontact_peers_require_premium"]
				= d.is_new_noncontact_peers_require_premium();
			value["display_gifts_button"] = d.is_display_gifts_button();
			value["noncontact_peers_paid_stars"] = qint64(
				d.vnoncontact_peers_paid_stars().value_or_empty());
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolSetGlobalPrivacy(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	using Flag = MTPDglobalPrivacySettings::Flag;
	const auto toggles = std::vector<std::pair<QString, Flag>>{
		{ "archive_and_mute_new_noncontact_peers",
			Flag::f_archive_and_mute_new_noncontact_peers },
		{ "keep_archived_unmuted", Flag::f_keep_archived_unmuted },
		{ "keep_archived_folders", Flag::f_keep_archived_folders },
		{ "hide_read_marks", Flag::f_hide_read_marks },
		{ "new_noncontact_peers_require_premium",
			Flag::f_new_noncontact_peers_require_premium },
		{ "display_gifts_button", Flag::f_display_gifts_button },
	};
	const auto hasStars = args.contains("noncontact_peers_paid_stars");
	auto named = false;
	for (const auto &[field, flag] : toggles) {
		named |= args.contains(field);
	}
	if (!named && !hasStars) {
		return toolError("Nothing to change: pass at least one setting "
			"reported by get_global_privacy");
	}
	const auto stars = args["noncontact_peers_paid_stars"].toVariant().toLongLong();
	const auto session = _session;
	// The method replaces the whole object, so unnamed settings are read first and sent back unchanged.
	return awaitMtp([&](auto done, auto fail) {
		session->api().request(MTPaccount_GetGlobalPrivacySettings(
		)).done([=](const MTPGlobalPrivacySettings &current) {
			const auto &d = current.data();
			auto flags = d.vflags().v;
			for (const auto &[field, flag] : toggles) {
				if (args.contains(field)) {
					flags = args[field].toBool() ? (flags | flag) : (flags & ~flag);
				}
			}
			auto paid = d.vnoncontact_peers_paid_stars().value_or_empty();
			if (hasStars) {
				paid = stars;
			}
			flags = paid ? (flags | Flag::f_noncontact_peers_paid_stars)
				: (flags & ~Flag::f_noncontact_peers_paid_stars);
			session->api().request(MTPaccount_SetGlobalPrivacySettings(
				MTP_globalPrivacySettings(
					MTP_flags(flags),
					MTP_long(paid),
					d.vdisallowed_gifts()
						? *d.vdisallowed_gifts()
						: MTPDisallowedGiftsSettings())
			)).done([=](const MTPGlobalPrivacySettings &result) {
				session->api().globalPrivacy().reload();
				const auto &r = result.data();
				auto value = QJsonObject();
				value["success"] = true;
				value["archive_and_mute_new_noncontact_peers"]
					= r.is_archive_and_mute_new_noncontact_peers();
				value["keep_archived_unmuted"] = r.is_keep_archived_unmuted();
				value["keep_archived_folders"] = r.is_keep_archived_folders();
				value["hide_read_marks"] = r.is_hide_read_marks();
				value["new_noncontact_peers_require_premium"]
					= r.is_new_noncontact_peers_require_premium();
				value["display_gifts_button"] = r.is_display_gifts_button();
				value["noncontact_peers_paid_stars"] = qint64(
					r.vnoncontact_peers_paid_stars().value_or_empty());
				done(value);
			}).fail([=](const MTP::Error &error) {
				fail(error.type());
			}).send();
		}).fail([=](const MTP::Error &error) {
			fail("account.getGlobalPrivacySettings: " + error.type());
		}).send();
	});
}

QJsonObject Server::toolGetConnectedBots(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPaccount_GetConnectedBots(
		)).done([=](const MTPaccount_ConnectedBots &result) {
			const auto &d = result.data();
			_session->data().processUsers(d.vusers());
			auto list = QJsonArray();
			for (const auto &bot : d.vconnected_bots().v) {
				const auto &b = bot.data();
				auto o = QJsonObject();
				o["bot_id"] = qint64(b.vbot_id().v);
				if (const auto device = b.vdevice()) {
					o["device"] = qs(*device);
				}
				if (const auto date = b.vdate()) {
					o["connected_date"] = qint64(date->v);
				}
				if (const auto loc = b.vlocation()) {
					o["location"] = qs(*loc);
				}
				list.append(o);
			}
			auto value = QJsonObject();
			value["success"] = true;
			value["connected_bots"] = list;
			value["count"] = list.size();
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

namespace {

[[nodiscard]] std::optional<Data::ChatbotsPermissions> BotPermissionsFromArgs(
		const QJsonValue &value,
		QString *error) {
	using Right = Data::ChatbotsPermission;
	static const auto names = std::vector<std::pair<QString, Right>>{
		{ "reply", Right::ReplyToMessages },
		{ "mark_read", Right::MarkAsRead },
		{ "delete_sent", Right::DeleteSent },
		{ "delete_received", Right::DeleteReceived },
		{ "edit_name", Right::EditName },
		{ "edit_bio", Right::EditBio },
		{ "edit_profile_photo", Right::EditUserpic },
		{ "edit_username", Right::EditUsername },
		{ "view_gifts", Right::ViewGifts },
		{ "sell_gifts", Right::SellGifts },
		{ "gift_settings", Right::GiftSettings },
		{ "transfer_gifts", Right::TransferGifts },
		{ "transfer_stars", Right::TransferStars },
		{ "manage_stories", Right::ManageStories },
	};
	// The bot always reads messages it is connected to; the rest are opt-in.
	auto result = Data::ChatbotsPermissions(Right::ViewMessages);
	if (value.isUndefined()) {
		return result | Right::ReplyToMessages | Right::MarkAsRead;
	}
	for (const auto &entry : value.toArray()) {
		const auto name = entry.toString();
		const auto i = ranges::find(names, name, &std::pair<QString, Right>::first);
		if (i == end(names)) {
			auto known = QStringList();
			for (const auto &[label, right] : names) {
				known.push_back(label);
			}
			*error = "Unknown permission '" + name + "'; one of: "
				+ known.join(", ");
			return std::nullopt;
		}
		result |= i->second;
	}
	return result;
}

[[nodiscard]] Data::BusinessChatTypes BusinessChatTypesFromArgs(
		const QJsonArray &list) {
	using Type = Data::BusinessChatType;
	auto result = Data::BusinessChatTypes();
	for (const auto &entry : list) {
		const auto name = entry.toString();
		if (name == "new_chats") result |= Type::NewChats;
		else if (name == "existing_chats") result |= Type::ExistingChats;
		else if (name == "contacts") result |= Type::Contacts;
		else if (name == "non_contacts") result |= Type::NonContacts;
	}
	return result;
}

} // namespace

QJsonObject Server::toolConnectBusinessBot(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto peer = resolvePeer(args["bot_id"].toVariant().toLongLong());
	const auto bot = peer ? peer->asUser() : nullptr;
	if (!bot || !bot->isBot()) {
		return toolError("bot_id must be the user id of a bot this client knows");
	}
	auto error = QString();
	const auto permissions = BotPermissionsFromArgs(
		args.value("permissions"),
		&error);
	if (!permissions) {
		return toolError(error);
	}
	const auto collect = [&](const QString &field) {
		auto result = std::vector<not_null<UserData*>>();
		for (const auto &id : args[field].toArray()) {
			const auto p = resolvePeer(id.toVariant().toLongLong());
			const auto user = p ? p->asUser() : nullptr;
			if (!user) {
				error = QString("%1: no user %2 in this client's data"
					).arg(field).arg(id.toVariant().toLongLong());
				return result;
			}
			result.push_back(user);
		}
		return result;
	};
	const auto only = (args.value("mode").toString("all_except") == "only");
	auto recipients = Data::BusinessRecipients();
	recipients.allButExcluded = !only;
	auto &chosen = only ? recipients.included : recipients.excluded;
	chosen.types = BusinessChatTypesFromArgs(args["chat_types"].toArray());
	chosen.list = collect("user_ids");
	if (only) {
		recipients.excluded.list = collect("exclude_user_ids");
	}
	if (!error.isEmpty()) {
		return toolError(error);
	}
	if (only && recipients.included.empty()) {
		return toolError("mode \"only\" needs chat_types or user_ids to "
			"say which chats the bot may handle");
	}
	auto settings = Data::ChatbotsSettings{
		.bot = bot,
		.recipients = recipients,
		.permissions = *permissions,
	};
	const auto session = _session;
	auto &chatbots = _session->data().chatbots();
	const auto describe = [=] {
		auto value = QJsonObject();
		value["success"] = true;
		value["bot_id"] = qint64(bot->id.value);
		value["bot_name"] = bot->name();
		value["mode"] = only ? "only" : "all_except";
		return value;
	};
	if (chatbots.loaded() && chatbots.current() == settings) {
		auto value = describe();
		value["changed"] = false;
		return value;
	}
	return awaitMtp([&](auto done, auto fail) {
		if (chatbots.loaded()) {
			// Through the client's own cache, so Settings > Business shows the change.
			chatbots.save(settings, [=] {
				done(describe());
			}, [=](const QString &type) {
				fail(type);
			});
			return;
		}
		// Not cached yet; the next load reads the new state from Telegram.
		using Flag = MTPaccount_UpdateConnectedBot::Flag;
		session->api().request(MTPaccount_UpdateConnectedBot(
			MTP_flags(Flag::f_rights),
			Data::ToMTP(settings.permissions),
			bot->inputUser(),
			Data::ForBotsToMTP(settings.recipients)
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			done(describe());
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolDisconnectBusinessBot(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto peer = resolvePeer(args["bot_id"].toVariant().toLongLong());
	const auto bot = peer ? peer->asUser() : nullptr;
	if (!bot || !bot->isBot()) {
		return toolError("bot_id must be the user id of a bot this client "
			"knows; see get_connected_bots");
	}
	const auto session = _session;
	auto &chatbots = _session->data().chatbots();
	const auto describe = [=] {
		auto value = QJsonObject();
		value["success"] = true;
		value["bot_id"] = qint64(bot->id.value);
		value["disconnected"] = true;
		return value;
	};
	return awaitMtp([&](auto done, auto fail) {
		if (chatbots.loaded() && chatbots.current().bot == bot) {
			chatbots.save({}, [=] {
				done(describe());
			}, [=](const QString &type) {
				fail(type);
			});
			return;
		}
		using Flag = MTPaccount_UpdateConnectedBot::Flag;
		session->api().request(MTPaccount_UpdateConnectedBot(
			MTP_flags(Flag::f_deleted),
			MTPBusinessBotRights(),
			bot->inputUser(),
			MTP_inputBusinessBotRecipients(
				MTP_flags(0),
				MTPVector<MTPInputUser>(),
				MTPVector<MTPInputUser>())
		)).done([=](const MTPUpdates &result) {
			session->api().applyUpdates(result);
			done(describe());
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}

QJsonObject Server::toolDisconnectWebsite(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto hash = args["hash"].toVariant().toLongLong();
	if (!hash) {
		return toolError("hash is required: the website hash from "
			"get_connected_websites");
	}
	return awaitMtp([&](auto done, auto fail) {
		_session->api().request(MTPaccount_ResetWebAuthorization(
			MTP_long(hash)
		)).done([=]() {
			auto value = QJsonObject();
			value["success"] = true;
			value["hash"] = QString::number(hash);
			done(value);
		}).fail([=](const MTP::Error &error) {
			fail(error.type());
		}).send();
	});
}


} // namespace MCP
