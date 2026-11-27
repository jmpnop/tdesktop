// Story posting, upload and draft tools for the Telegram Desktop MCP integration.
//
// Posting goes through Api::Stories, the same uploader the story editor
// feeds, so a story sent here shows up in the stories list with its upload
// progress and can be cancelled from the app as well. Media is prepared the
// way the editor prepares an untouched file: a compliant file is sent as is,
// anything else is fitted on a 9:16 canvas and transcoded.
//
// For license and copyright information please follow this link:
// https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL

#include "mcp_server_includes.h"

#include "api/api_stories.h"
#include "api/api_user_privacy.h"
#include "data/data_stories.h"
#include "data/data_story_drafts.h"
#include "media/media_video_canvas.h"
#include "media/media_video_encode.h"
#include "media/media_video_frames.h"
#include "core/mime_type.h"
#include "base/unixtime.h"

#include <QtGui/QImageReader>

namespace MCP {
namespace {

constexpr auto kStillMusicDuration = crl::time(15'000);

[[nodiscard]] QSize StoryCanvas() {
	return QSize(Api::kStoryVideoWidth, Api::kStoryVideoHeight);
}

[[nodiscard]] Media::Encode::CanvasBackground BlackCanvas() {
	return { .top = QColor(0, 0, 0), .bottom = QColor(0, 0, 0) };
}

[[nodiscard]] QImage FitStoryImage(const QImage &image) {
	return Media::Encode::FitOnCanvas(image, StoryCanvas(), BlackCanvas());
}

[[nodiscard]] bool IsVideoFile(const QString &path) {
	return Core::MimeTypeForFile(QFileInfo(path)).name().startsWith(
		u"video/"_q);
}

// Mirrors the editor's passthrough test: anything else is transcoded.
[[nodiscard]] bool VideoPassthrough(
		const QString &path,
		const Media::Encode::SourceInfo &info) {
	const auto size = info.display;
	return (Core::MimeTypeForFile(QFileInfo(path)).name() == u"video/mp4"_q)
		&& info.videoH264
		&& info.videoYuv420
		&& !info.rotation
		&& (!info.hasAudio || info.audioRemuxable)
		&& (info.duration > 0)
		&& (info.duration <= Api::kStoryVideoMaxDuration)
		&& (info.fileSize <= Api::kStoryVideoMaxFileSize)
		&& ((size == QSize(Api::kStoryVideoWidth, Api::kStoryVideoHeight))
			|| (size == QSize(Api::kStoryPhotoWidth, Api::kStoryPhotoHeight)))
		&& Media::Encode::IsFaststartMp4(path);
}

[[nodiscard]] QJsonObject DraftJson(const Data::StoryDraft &draft) {
	auto result = QJsonObject{
		{ "draft_id", QString::number(draft.id) },
		{ "date", qint64(draft.date) },
		{ "peer_id", QString::number(draft.peer.value) },
		{ "video", draft.video },
		{ "width", draft.dimensions.width() },
		{ "height", draft.dimensions.height() },
		{ "caption", draft.caption.text },
		{ "period", draft.period },
		{ "has_music", !draft.audioPath.isEmpty() },
		{ "edited_in_app", !draft.scene.isEmpty() },
	};
	if (draft.video) {
		result["duration_ms"] = qint64(draft.duration);
	}
	return result;
}

} // namespace

QJsonObject Server::toolPostStory(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto path = args.value("file_path").toString();
	if (path.isEmpty() || !QFileInfo(path).isFile()) {
		return toolError("file_path must name a local photo or video file");
	}
	const auto chatId = args.value("chat_id").toVariant().toLongLong();
	const auto peer = chatId
		? resolvePeer(chatId)
		: static_cast<PeerData*>(_session->user().get());
	if (!peer) {
		return toolError(QString("No chat %1 in this client's data"
			).arg(chatId));
	}
	const auto periodHours = args.value("period_hours").toInt(24);
	if (periodHours != 6
		&& periodHours != 12
		&& periodHours != 24
		&& periodHours != 48) {
		return toolError("period_hours must be 6, 12, 24 or 48");
	}

	auto privacy = Api::UserPrivacy::Rule();
	const auto privacyName = args.value("privacy").toString("everyone");
	using Option = Api::UserPrivacy::Option;
	if (privacyName == "everyone") {
		privacy.option = Option::Everyone;
	} else if (privacyName == "contacts") {
		privacy.option = Option::Contacts;
	} else if (privacyName == "close_friends") {
		privacy.option = Option::CloseFriends;
	} else if (privacyName == "selected") {
		privacy.option = Option::Nobody;
	} else {
		return toolError("privacy must be everyone, contacts, close_friends "
			"or selected");
	}
	auto error = QString();
	const auto collect = [&](const QString &field) {
		auto result = std::vector<not_null<PeerData*>>();
		for (const auto &id : args.value(field).toArray()) {
			const auto user = resolvePeer(id.toVariant().toLongLong());
			if (!user || !user->isUser()) {
				error = QString("%1: no user %2 in this client's data"
					).arg(field).arg(id.toVariant().toLongLong());
				break;
			}
			result.push_back(user);
		}
		return result;
	};
	privacy.always.peers = collect("allow_user_ids");
	privacy.never.peers = collect("disallow_user_ids");
	if (!error.isEmpty()) {
		return toolError(error);
	}
	if (privacy.option == Option::Nobody && privacy.always.peers.empty()) {
		return toolError("privacy selected needs allow_user_ids");
	}

	const auto musicPath = args.value("music_path").toString();
	if (!musicPath.isEmpty() && !QFileInfo(musicPath).isFile()) {
		return toolError("music_path must name a local audio file");
	}
	auto music = std::optional<Api::StoryMusicFile>();
	auto musicTracks = std::vector<Media::Encode::MusicTrack>();
	if (!musicPath.isEmpty()) {
		const auto probed = Media::Encode::ProbeSource(musicPath);
		music = Api::StoryMusicFile{
			.path = musicPath,
			.title = args.value("music_title").toString(
				QFileInfo(musicPath).completeBaseName()),
			.performer = args.value("music_performer").toString(),
			.duration = probed.duration,
		};
		musicTracks.push_back({ .path = musicPath, .loop = true });
	}

	auto media = Api::SendStoryMedia(Api::SendStoryPhoto());
	auto kind = QString();
	if (IsVideoFile(path)) {
		const auto info = Media::Encode::ProbeSource(path);
		if (info.empty()) {
			return toolError("Could not read the video file");
		}
		const auto cover = FitStoryImage(Media::Video::ExtractFrame(
			path,
			QByteArray(),
			0,
			StoryCanvas()));
		if (musicTracks.empty() && VideoPassthrough(path, info)) {
			media = Api::SendStoryVideo{
				.cover = cover,
				.path = path,
				.hasAudio = info.hasAudio,
			};
			kind = "video";
		} else {
			auto source = std::make_shared<Media::Encode::VideoSource>(
				Media::Encode::VideoSource{
					.path = path,
					.exactSize = StoryCanvas(),
					.till = std::min(info.duration, Api::kStoryVideoMaxDuration),
					.removeAudio = !musicTracks.empty(),
					.canvas = BlackCanvas(),
					.music = musicTracks,
				});
			media = Api::SendStoryVideo{
				.source = std::move(source),
				.cover = cover,
				.music = music,
			};
			kind = "video_transcoded";
		}
	} else {
		auto reader = QImageReader(path);
		reader.setAutoTransform(true);
		const auto image = reader.read();
		if (image.isNull()) {
			return toolError("Could not read the image file: "
				+ reader.errorString());
		}
		if (musicTracks.empty()) {
			media = Api::SendStoryPhoto{ .image = image };
			kind = "photo";
		} else {
			const auto base = FitStoryImage(image);
			const auto length = (music->duration > 0)
				? std::min(music->duration, Api::kStoryVideoMaxDuration)
				: kStillMusicDuration;
			auto job = std::make_shared<Media::Encode::Job>(
				Media::Encode::Job{
					.source = Media::Encode::StillSource{
						.base = base,
						.duration = length,
						.music = musicTracks,
					},
				});
			media = Api::SendStoryVideo{
				.cover = base,
				.job = std::move(job),
				.music = music,
			};
			kind = "photo_with_music";
		}
	}

	auto &stories = _session->api().stories();
	stories.send(Api::StoryToSend{
		.peer = peer,
		.media = std::move(media),
		.caption = TextWithTags{ args.value("caption").toString() },
		.privacy = std::move(privacy),
		.period = periodHours * 3600,
		.pinned = args.value("pinned").toBool(true),
		.noForwards = args.value("protect_content").toBool(false),
	});
	const auto &uploads = stories.uploads();
	return QJsonObject{
		{ "success", true },
		{ "queued", true },
		{ "upload_id", uploads.empty()
			? QString()
			: QString::number(uploads.back().id) },
		{ "peer_id", QString::number(peer->id.value) },
		{ "media", kind },
		{ "privacy", privacyName },
		{ "period_hours", periodHours },
		{ "next_step", "The story uploads in the background; follow it with "
			"list_story_uploads." },
	};
}

QJsonObject Server::toolListStoryUploads(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	auto list = QJsonArray();
	for (const auto &upload : _session->api().stories().uploads()) {
		auto entry = QJsonObject{
			{ "upload_id", QString::number(upload.id) },
			{ "peer_id", QString::number(upload.peer->id.value) },
			{ "progress", upload.progress },
			{ "preparing", upload.preparing },
			{ "failed", !upload.error.isEmpty() },
		};
		if (!upload.error.isEmpty()) {
			entry["error"] = upload.error;
		}
		if (upload.editId) {
			entry["edit_story_id"] = upload.editId;
		}
		list.append(entry);
	}
	return QJsonObject{
		{ "success", true },
		{ "uploads", list },
		{ "count", list.size() },
	};
}

QJsonObject Server::toolCancelStoryUpload(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto id = args.value("upload_id").toVariant().toString().toULongLong();
	auto &stories = _session->api().stories();
	const auto &uploads = stories.uploads();
	const auto found = ranges::any_of(uploads, [&](const auto &upload) {
		return upload.id == id;
	});
	if (!found) {
		return toolError("No story upload with this upload_id "
			"(see list_story_uploads)");
	}
	stories.remove(id);
	return QJsonObject{
		{ "success", true },
		{ "upload_id", QString::number(id) },
		{ "cancelled", true },
	};
}

QJsonObject Server::toolListStoryDrafts(const QJsonObject &args) {
	Q_UNUSED(args);
	if (!_session) {
		return toolError("No active session");
	}
	auto list = QJsonArray();
	for (const auto &draft : _session->data().stories().drafts().list()) {
		list.append(DraftJson(draft));
	}
	return QJsonObject{
		{ "success", true },
		{ "drafts", list },
		{ "count", list.size() },
		{ "note", "Drafts are kept on this device for 7 days." },
	};
}

QJsonObject Server::toolSaveStoryDraft(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto path = args.value("file_path").toString();
	if (path.isEmpty() || !QFileInfo(path).isFile()) {
		return toolError("file_path must name a local photo or video file");
	}
	auto draft = Data::StoryDraft();
	draft.date = base::unixtime::now();
	draft.peer = _session->userPeerId();
	draft.caption = TextWithTags{ args.value("caption").toString() };
	draft.period = args.value("period_hours").toInt(24) * 3600;
	auto media = Data::StoryDraftMedia();
	if (IsVideoFile(path)) {
		const auto info = Media::Encode::ProbeSource(path);
		if (info.empty()) {
			return toolError("Could not read the video file");
		}
		draft.video = true;
		draft.dimensions = info.display;
		draft.duration = info.duration;
		draft.thumbnail = Media::Video::ExtractFrame(
			path,
			QByteArray(),
			0,
			StoryCanvas());
		media.videoPath = path;
	} else {
		auto reader = QImageReader(path);
		reader.setAutoTransform(true);
		media.photo = reader.read();
		if (media.photo.isNull()) {
			return toolError("Could not read the image file: "
				+ reader.errorString());
		}
		draft.dimensions = media.photo.size();
		draft.thumbnail = media.photo.scaled(
			StoryCanvas(),
			Qt::KeepAspectRatio,
			Qt::SmoothTransformation);
	}
	const auto id = _session->data().stories().drafts().save(
		std::move(draft),
		media);
	if (!id) {
		return toolError("The draft could not be saved");
	}
	return QJsonObject{
		{ "success", true },
		{ "draft_id", QString::number(id) },
		{ "note", "Privacy and pinning are chosen when the draft is posted "
			"from the app; drafts do not store them." },
	};
}

QJsonObject Server::toolDeleteStoryDraft(const QJsonObject &args) {
	if (!_session) {
		return toolError("No active session");
	}
	const auto id = args.value("draft_id").toVariant().toString().toULongLong();
	auto &drafts = _session->data().stories().drafts();
	if (!drafts.find(id)) {
		return toolError("No story draft with this draft_id "
			"(see list_story_drafts)");
	}
	drafts.remove(id);
	return QJsonObject{
		{ "success", true },
		{ "draft_id", QString::number(id) },
		{ "deleted", true },
	};
}

} // namespace MCP
