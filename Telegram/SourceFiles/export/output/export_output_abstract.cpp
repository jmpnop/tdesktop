/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/output/export_output_abstract.h"

#include "export/output/export_output_html_and_json.h"
#include "export/output/export_output_html.h"
#include "export/output/export_output_json.h"

#include <QtCore/QDir>
#include <QtCore/QDate>
#include <QtCore/QTime>
#include <QtCore/QDateTime>
#include <QtCore/QRegularExpression>

namespace Export {
namespace Output {

namespace {

// Sanitize name for filesystem use
QString SanitizeForFilesystem(const QString &name) {
	if (name.isEmpty()) {
		return QString("Unknown");
	}
	QString result = name;
	// Replace characters not allowed in filenames
	static const QRegularExpression invalidChars(
		QString::fromLatin1("[<>:\"/\\\\|?*\\x00-\\x1f]"));
	result.replace(invalidChars, QString("_"));
	// Replace spaces with underscores for cleaner paths
	result.replace(' ', '_');
	// Remove leading/trailing dots and spaces
	while (result.startsWith('.') || result.startsWith('_')) {
		result = result.mid(1);
	}
	while (result.endsWith('.') || result.endsWith('_')) {
		result.chop(1);
	}
	// Limit length to avoid filesystem issues
	if (result.length() > 50) {
		result = result.left(50);
	}
	return result.isEmpty() ? QString("Export") : result;
}

} // namespace

QString NormalizePath(const Settings &settings) {
	// If resuming into an existing export directory, reuse it as-is
	if (!settings.resumeExportDir.isEmpty()) {
		QDir dir(settings.resumeExportDir);
		if (dir.exists()) {
			auto result = dir.absolutePath();
			if (!result.endsWith('/')) {
				result += '/';
			}
			qWarning() << "[Export] Reusing existing export dir:" << result;
			return result;
		}
		qWarning() << "[Export] Resume dir gone, creating fresh:"
		           << settings.resumeExportDir;
	}

	QDir folder(settings.path);
	const auto path = folder.absolutePath();
	auto result = path.endsWith('/') ? path : (path + '/');
	if (!folder.exists() && !settings.forceSubPath) {
		return result;
	}
	const auto mode = QDir::AllEntries | QDir::NoDotAndDotDot;
	const auto list = folder.entryInfoList(mode);
	if (list.isEmpty() && !settings.forceSubPath) {
		return result;
	}

	// Format: Type-Name-DDMMYYYY-HHMMSS
	// Example: Chat-John_Doe-10022026-143025
	const auto now = QDateTime::currentDateTime();
	const auto dateStr = now.toString(QString::fromLatin1("ddMMyyyy"));
	const auto timeStr = now.toString(QString::fromLatin1("HHmmss"));

	QString base;
	if (settings.onlySinglePeer()
		&& !settings.singlePeerType.isEmpty()
		&& !settings.singlePeerName.isEmpty()) {
		// Use new naming: Type-Name-DDMMYYYY-HHMMSS
		const auto type = settings.singlePeerType;
		const auto name = SanitizeForFilesystem(settings.singlePeerName);
		base = QString("%1-%2-%3-%4").arg(type, name, dateStr, timeStr);
	} else if (settings.onlySinglePeer()) {
		// Fallback for single peer without name info
		base = QString("ChatExport_%1").arg(now.date().toString(Qt::ISODate));
	} else {
		// Full data export
		base = QString("DataExport_%1").arg(now.date().toString(Qt::ISODate));
	}

	const auto add = [&](int i) {
		return base + (i ? " (" + QString::number(i) + ')' : QString());
	};
	auto index = 0;
	while (QDir(result + add(index)).exists()) {
		++index;
	}
	result += add(index) + '/';
	return result;
}

std::unique_ptr<AbstractWriter> CreateWriter(Format format) {
	switch (format) {
	case Format::Html: return std::make_unique<HtmlWriter>();
	case Format::Json: return std::make_unique<JsonWriter>();
	case Format::HtmlAndJson: return std::make_unique<HtmlAndJsonWriter>();
	// Markdown variants: fall back to Json (Markdown not yet implemented as AbstractWriter)
	// TODO: Implement proper MarkdownWriter when needed
	case Format::Markdown: return std::make_unique<JsonWriter>();
	case Format::HtmlAndMarkdown: return std::make_unique<HtmlWriter>();
	case Format::JsonAndMarkdown: return std::make_unique<JsonWriter>();
	case Format::All: return std::make_unique<HtmlAndJsonWriter>();
	}
	Unexpected("Format in Export::Output::CreateWriter.");
}

} // namespace Output
} // namespace Export
