// This file is part of Telegram Desktop MCP integration.
//
// For license and copyright information please follow this link:
// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

#include "local_llm.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace MCP {
namespace {

constexpr auto kGenerationTimeoutMs = 120000;
constexpr auto kModelsTimeoutMs = 5000;
constexpr auto kMaxTrainingExamples = 10;

[[nodiscard]] QString ReplyError(QNetworkReply *reply, const QByteArray &body) {
	const auto message = QJsonDocument::fromJson(body).object()
		.value("error").toObject().value("message").toString();
	return message.isEmpty() ? reply->errorString() : message;
}

} // namespace

LocalLLM::LocalLLM(QObject *parent)
: QObject(parent) {
}

void LocalLLM::setBaseUrl(const QString &url) {
	auto trimmed = url.trimmed();
	while (trimmed.endsWith('/')) {
		trimmed.chop(1);
	}
	// Accept the URL with or without the /v1 suffix.
	if (trimmed.endsWith("/v1")) {
		trimmed.chop(3);
	}
	_baseUrl = trimmed;
}

void LocalLLM::completeAsync(
		const QList<ChatMessage> &messages,
		std::function<void(CompletionResult)> done,
		double temperature) {
	if (!isConfigured()) {
		auto result = CompletionResult();
		result.error = "oMLX is not configured: set omlx_url with configure_chatbot";
		done(std::move(result));
		return;
	}
	auto list = QJsonArray();
	for (const auto &message : messages) {
		list.append(QJsonObject{
			{ "role", message.role },
			{ "content", message.content },
		});
	}
	auto body = QJsonObject{
		{ "messages", list },
		{ "temperature", temperature },
		{ "max_tokens", _maxTokens },
		{ "stream", false },
	};
	if (!_model.isEmpty()) {
		body["model"] = _model;
	}
	auto request = QNetworkRequest(QUrl(_baseUrl + "/v1/chat/completions"));
	request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
	request.setTransferTimeout(kGenerationTimeoutMs);
	const auto timer = std::make_shared<QElapsedTimer>();
	timer->start();
	const auto reply = _network.post(
		request,
		QJsonDocument(body).toJson(QJsonDocument::Compact));
	const auto weak = QPointer<LocalLLM>(this);
	QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
		reply->deleteLater();
		auto result = CompletionResult();
		result.durationSeconds = timer->elapsed() / 1000.0f;
		const auto bytes = reply->readAll();
		if (reply->error() != QNetworkReply::NoError) {
			result.error = ReplyError(reply, bytes);
		} else {
			const auto object = QJsonDocument::fromJson(bytes).object();
			const auto choices = object.value("choices").toArray();
			if (!choices.isEmpty()) {
				result.text = choices.first().toObject().value("message")
					.toObject().value("content").toString().trimmed();
			}
			result.model = object.value("model").toString();
			const auto usage = object.value("usage").toObject();
			result.promptTokens = usage.value("prompt_tokens").toInt();
			result.completionTokens = usage.value("completion_tokens").toInt();
			result.success = !result.text.isEmpty();
			if (!result.success) {
				result.error = "oMLX returned an empty answer";
			}
		}
		if (weak) {
			++weak->_stats.totalRequests;
			++(result.success
				? weak->_stats.successfulRequests
				: weak->_stats.failedRequests);
			weak->_stats.lastRequest = QDateTime::currentDateTime();
		}
		done(std::move(result));
	});
}

CompletionResult LocalLLM::complete(
		const QList<ChatMessage> &messages,
		double temperature) {
	auto result = CompletionResult();
	auto finished = false;
	auto loop = QEventLoop();
	completeAsync(messages, [&](CompletionResult value) {
		result = std::move(value);
		finished = true;
		loop.quit();
	}, temperature);
	if (!finished) {
		loop.exec(QEventLoop::ExcludeUserInputEvents);
	}
	return result;
}

CompletionResult LocalLLM::translate(
		const QString &text,
		const QString &targetLanguage,
		const QString &sourceLanguage) {
	const auto system = u"You are a professional translator. Output ONLY the "
		"translation, with no notes and no quotation marks."_q;
	const auto prompt = sourceLanguage.isEmpty()
		? QString("Translate to %1:\n\n%2").arg(targetLanguage, text)
		: QString("Translate from %1 to %2:\n\n%3").arg(
			sourceLanguage,
			targetLanguage,
			text);
	return complete({ { "system", system }, { "user", prompt } }, 0.3);
}

QList<ChatMessage> LocalLLM::ChatbotPrompt(
		const QString &input,
		const QString &personality,
		const QString &responseStyle,
		const QJsonArray &trainingExamples) {
	auto system = QString(
		"You are replying to a Telegram message on the account owner's behalf.\n"
		"Personality: %1\nResponse style: %2\n").arg(personality, responseStyle);
	auto used = 0;
	for (const auto &entry : trainingExamples) {
		const auto example = entry.toObject();
		const auto in = example.value("input").toString();
		const auto out = example.value("output").toString();
		if (in.isEmpty() || out.isEmpty()) {
			continue;
		}
		if (!used) {
			system += "\nExample exchanges to follow:\n";
		}
		system += QString("Message: %1\nReply: %2\n\n").arg(in, out);
		if (++used >= kMaxTrainingExamples) {
			break;
		}
	}
	system += "\nReply with the message text only.";
	return { { "system", system }, { "user", input } };
}

QJsonObject LocalLLM::listModels() {
	if (!isConfigured()) {
		return QJsonObject{ { "error", "oMLX is not configured" } };
	}
	auto request = QNetworkRequest(QUrl(_baseUrl + "/v1/models"));
	request.setTransferTimeout(kModelsTimeoutMs);
	const auto reply = _network.get(request);
	auto loop = QEventLoop();
	QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
	loop.exec(QEventLoop::ExcludeUserInputEvents);
	reply->deleteLater();
	const auto bytes = reply->readAll();
	if (reply->error() != QNetworkReply::NoError) {
		return QJsonObject{ { "error", ReplyError(reply, bytes) } };
	}
	auto models = QJsonArray();
	for (const auto &entry : QJsonDocument::fromJson(bytes).object()
			.value("data").toArray()) {
		models.append(entry.toObject().value("id").toString());
	}
	return QJsonObject{ { "models", models } };
}

} // namespace MCP
