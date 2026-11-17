// Client for the local oMLX server, used by the chatbot and translation tools.
//
// oMLX serves an OpenAI-compatible /v1/chat/completions. The base URL is an
// explicit setting: nothing is probed or guessed, because an unrelated model
// server on a common port would otherwise receive the account's messages.
//
// This file is part of Telegram Desktop MCP integration.
//
// For license and copyright information please follow this link:
// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QDateTime>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonArray>
#include <QtNetwork/QNetworkAccessManager>

#include <functional>

namespace MCP {

// A single message in chat format.
struct ChatMessage {
	QString role; // "system", "user", "assistant"
	QString content;
};

struct CompletionResult {
	QString text;
	QString model;
	int promptTokens = 0;
	int completionTokens = 0;
	float durationSeconds = 0.0f;
	bool success = false;
	QString error;
};

class LocalLLM : public QObject {
	Q_OBJECT

public:
	explicit LocalLLM(QObject *parent = nullptr);

	// An empty URL means not configured; every call then fails without a request.
	void setBaseUrl(const QString &url);
	void setModel(const QString &model) { _model = model; }
	[[nodiscard]] QString baseUrl() const { return _baseUrl; }
	[[nodiscard]] QString model() const { return _model; }
	[[nodiscard]] bool isConfigured() const { return !_baseUrl.isEmpty(); }

	// Never blocks: `done` runs on the main thread when the answer or an error arrives.
	void completeAsync(
		const QList<ChatMessage> &messages,
		std::function<void(CompletionResult)> done,
		double temperature = 0.7);

	// Waits for the answer in a nested loop that ignores user input; for tool calls only.
	CompletionResult complete(
		const QList<ChatMessage> &messages,
		double temperature = 0.7);

	CompletionResult translate(
		const QString &text,
		const QString &targetLanguage,
		const QString &sourceLanguage = QString());

	// The prompt the chatbot answers with: persona, style and up to 10 few-shot examples.
	[[nodiscard]] static QList<ChatMessage> ChatbotPrompt(
		const QString &input,
		const QString &personality,
		const QString &responseStyle,
		const QJsonArray &trainingExamples);

	// Lists models from GET /v1/models; an error string instead when that fails.
	[[nodiscard]] QJsonObject listModels();

	struct Stats {
		int totalRequests = 0;
		int successfulRequests = 0;
		int failedRequests = 0;
		QDateTime lastRequest;
	};
	[[nodiscard]] Stats stats() const { return _stats; }

private:
	QNetworkAccessManager _network;
	QString _baseUrl;
	QString _model;
	int _maxTokens = 1024;
	Stats _stats;

};

} // namespace MCP
