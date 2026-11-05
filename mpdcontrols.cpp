#include "mpdcontrols.h"

#include <core/library/libraryinfo.h>
#include <core/library/musiclibrary.h>
#include <core/playlist/playlisthandler.h>
#include <core/playlist/playlist.h>
#include <gui/playlist/currentplaylistcontroller.h>
#include <gui/trackselectioncontroller.h>

#include <QMessageBox>
#include <QCheckBox>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QPushButton>
#include <QtConcurrent/QtConcurrentRun>

#include <mpd/client.h>

namespace {

struct MpdConfig
{
    QString host;
    unsigned port{6600};
    QString password;
};

MpdConfig readMpdConfig()
{
    MpdConfig config;
    config.host = qEnvironmentVariable("FOOYIN_MPD_HOST", "127.0.0.1");
    config.password = qEnvironmentVariable("FOOYIN_MPD_PASSWORD");

    bool ok = false;
    const uint port =
        qEnvironmentVariable("FOOYIN_MPD_PORT", "6600").toUInt(&ok);

    if (ok && port > 0 && port <= 65535) {
        config.port = port;
    }

    return config;
}

QString mpdError(mpd_connection* connection)
{
    const char* message = mpd_connection_get_error_message(connection);
    return message ? QString::fromUtf8(message)
                   : QStringLiteral("Unknown MPD error");
}

QStringList makeMpdUris(const Fooyin::TrackList& tracks,
                        Fooyin::MusicLibrary* library,
                        QString* error)
{
    if (!library) {
        *error = QStringLiteral("Fooyin music library is unavailable");
        return {};
    }

    QStringList uris;
    uris.reserve(static_cast<qsizetype>(tracks.size()));

    for (const Fooyin::Track& track : tracks) {
        if (track.isRemote() || track.isVirtual() || track.isInArchive()
            || track.hasCue() || track.isBoundedSegment()
            || track.subsong() != 0) {
            *error = QStringLiteral(
                "This playlist contains a remote, archive, cue, chapter, "
                "or subsong entry that cannot be mapped to an MPD file URI.");
            return {};
        }

        const QString filepath =
            QDir::cleanPath(QFileInfo(track.filepath()).absoluteFilePath());

        const auto libraryInfo = library->libraryForPath(filepath);
        if (!libraryInfo) {
            *error = QStringLiteral(
                "Fooyin has no configured library root for: %1").arg(filepath);
            return {};
        }

        const QString rootPath =
            QDir::cleanPath(QFileInfo(libraryInfo->path).absoluteFilePath());
        const QString relativePath = QDir(rootPath).relativeFilePath(filepath);

        // Guard against paths outside the configured root.
        if (relativePath == QStringLiteral("..")
            || relativePath.startsWith(QStringLiteral("../"))
            || QDir::isAbsolutePath(relativePath)) {
            *error = QStringLiteral(
                "Track is outside its fooyin library root: %1").arg(filepath);
            return {};
        }

        // Send a plain UTF-8 MPD database path, not an absolute file:// URL.
        uris.push_back(relativePath);
    }

    return uris;
}

QString runMpdOperation(const QStringList& uris,
                        bool toStoredPlaylist,
                        bool clearFirst,
                        const MpdConfig& config)
{
    const QByteArray host = config.host.toUtf8();
    mpd_connection* connection =
        mpd_connection_new(host.constData(), config.port, 5000);

    if (!connection) {
        return QStringLiteral("Could not allocate an MPD connection");
    }

    if (mpd_connection_get_error(connection) != MPD_ERROR_SUCCESS) {
        const QString error = mpdError(connection);
        mpd_connection_free(connection);
        return QStringLiteral("MPD connection failed: %1").arg(error);
    }

    if (!config.password.isEmpty()) {
        const QByteArray password = config.password.toUtf8();
        if (!mpd_run_password(connection, password.constData())) {
            const QString error = mpdError(connection);
            mpd_connection_free(connection);
            return QStringLiteral("MPD authentication failed: %1").arg(error);
        }
    }

    const QByteArray playlistName = QByteArrayLiteral("Filtered playlist");
    const QString clearTarget = toStoredPlaylist
        ? QStringLiteral("MPD playlist “Filtered playlist”")
        : QStringLiteral("MPD queue");

    bool wasCleared = false;

    const auto fail = [&](const QString& message) {
        const QString result = wasCleared
            ? QStringLiteral("%1 was cleared, but %2").arg(clearTarget, message)
            : message;
        mpd_connection_free(connection);
        return result;
    };

    if (toStoredPlaylist && clearFirst) {
        // Create the stored playlist if needed, then empty it.
        const QByteArray firstUri = uris.constFirst().toUtf8();
        if (!mpd_run_playlist_add(connection, playlistName.constData(),
                                  firstUri.constData())) {
            const QString error = mpdError(connection);
            mpd_connection_free(connection);
            return QStringLiteral("Could not prepare MPD playlist: %1").arg(error);
        }

        if (!mpd_run_playlist_clear(connection, playlistName.constData())) {
            const QString error = mpdError(connection);
            mpd_connection_free(connection);
            return QStringLiteral("Could not clear MPD playlist: %1").arg(error);
        }

        wasCleared = true;
    } else if (!toStoredPlaylist && clearFirst) {
        if (!mpd_run_clear(connection)) {
            const QString error = mpdError(connection);
            mpd_connection_free(connection);
            return QStringLiteral("Could not clear MPD queue: %1").arg(error);
        }

        wasCleared = true;
    }

    for (const QString& uri : uris) {
        const QByteArray uriUtf8 = uri.toUtf8();

        const bool succeeded = toStoredPlaylist
            ? mpd_run_playlist_add(connection, playlistName.constData(),
                                   uriUtf8.constData())
            : mpd_run_add(connection, uriUtf8.constData());

        if (!succeeded) {
            const QString error = mpdError(connection);
            return fail(QStringLiteral("MPD add failed: %1").arg(error));
        }
    }

    mpd_connection_free(connection);

    const QString result = QStringLiteral("Added %1 track(s) to %2")
        .arg(static_cast<qulonglong>(uris.size()))
        .arg(toStoredPlaylist ? QStringLiteral("Filtered playlist")
                              : QStringLiteral("MPD queue"));

    return wasCleared
        ? QStringLiteral("%1 cleared; %2").arg(clearTarget, result)
        : result;
}

} // namespace

MpdControls::MpdControls(QObject* parent)
    : QObject{parent}
{}

void MpdControls::setFooyinServices(Fooyin::MusicLibrary* library)
{
    m_library = library;
}

void MpdControls::setGuiServices(
    Fooyin::CurrentPlaylistController* playlistSelection,
    Fooyin::TrackSelectionController* trackSelection)
{
    m_playlistSelection = playlistSelection;
    m_trackSelection = trackSelection;
}

bool MpdControls::tracksForCurrentPlaylist(Fooyin::TrackList* tracks,
                                           QString* error) const
{
    if (tracks) {
        tracks->clear();
    }
    if (error) {
        error->clear();
    }

    if (!tracks || !m_playlistSelection || !m_trackSelection) {
        if (error) {
            *error = QStringLiteral("Fooyin playlist selection is unavailable");
        }
        return false;
    }

    Fooyin::Playlist* playlist = m_playlistSelection->currentPlaylist();
    if (!playlist) {
        if (error) {
            *error = QStringLiteral("No playlist is selected in Fooyin");
        }
        return false;
    }

    const Fooyin::UId playlistId = m_playlistSelection->currentPlaylistId();
    const auto target = m_trackSelection->playlistSelectionTarget(playlistId);

    if (target
        && target->selection.playlistId
        && *target->selection.playlistId == playlistId
        && !target->selection.tracks.empty()) {
        // A selection in the currently selected playlist takes precedence.
        *tracks = target->selection.tracks;
    } else {
        // No selected rows: use every track in the UI-selected playlist.
        *tracks = playlist->tracks();
    }

    if (tracks->empty()) {
        if (error) {
            *error = QStringLiteral("The selected Fooyin playlist is empty");
        }
        return false;
    }

    return true;
}

void MpdControls::addTracks(const Fooyin::TrackList& tracks,
                            bool toStoredPlaylist,
                            bool clearFirst)
{
    if (m_busy) {
        emit statusChanged(QStringLiteral("An MPD operation is already running"));
        return;
    }

    if (tracks.empty()) {
        emit statusChanged(QStringLiteral("There are no tracks to add"));
        return;
    }

    QString error;
    const QStringList uris = makeMpdUris(tracks, m_library, &error);
    if (!error.isEmpty()) {
        emit statusChanged(error);
        return;
    }

    const MpdConfig config = readMpdConfig();

    m_busy = true;
    emit busyChanged(true);

    auto* watcher = new QFutureWatcher<QString>(this);

    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher]() {
                m_busy = false;
                emit busyChanged(false);
                emit statusChanged(watcher->result());
                watcher->deleteLater();
            });

    watcher->setFuture(QtConcurrent::run(
        [uris, toStoredPlaylist, clearFirst, config]() {
            return runMpdOperation(uris, toStoredPlaylist, clearFirst, config);
        }));
}

MpdControlsWidget::MpdControlsWidget(MpdControls* controls, QWidget* parent)
    : FyWidget{parent}
    , m_controls{controls}
    , m_queueButton{new QPushButton(tr("MPD queue add"), this)}
    , m_playlistButton{new QPushButton(tr("MPD playlist add"), this)}
    , m_clearCheckBox{new QCheckBox(tr("MPD clear"), this)}
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_queueButton);
    layout->addWidget(m_playlistButton);
    layout->addWidget(m_clearCheckBox);

    m_clearCheckBox->setChecked(false);

    connect(m_queueButton, &QPushButton::clicked, this,
            [this]() { add(false); });
    connect(m_playlistButton, &QPushButton::clicked, this,
            [this]() { add(true); });

    connect(m_controls, &MpdControls::busyChanged, this,
            [this](bool busy) {
                m_queueButton->setDisabled(busy);
                m_playlistButton->setDisabled(busy);
            });

    connect(m_controls, &MpdControls::statusChanged,
            this, &MpdControlsWidget::showResult);
}

void MpdControlsWidget::add(bool toStoredPlaylist)
{
    Fooyin::TrackList tracks;
    QString error;

    if (!m_controls->tracksForCurrentPlaylist(&tracks, &error)) {
        showResult(error);
        return;
    }

    if (tracks.size() > 100) {
        const QString destination = toStoredPlaylist
            ? tr("the MPD playlist “Filtered playlist”")
            : tr("the MPD queue");

        const auto answer = QMessageBox::question(
            this,
            tr("Confirm MPD addition"),
            tr("Add %1 tracks to %2?")
                .arg(static_cast<qulonglong>(tracks.size()))
                .arg(destination),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel);

        if (answer != QMessageBox::Yes) {
            return;
        }
    }

    const bool clearFirst = m_clearCheckBox->isChecked();

    // Consume the one-shot clear option only when proceeding with the add.
    m_clearCheckBox->setChecked(false);

    m_controls->addTracks(tracks, toStoredPlaylist, clearFirst);
}

void MpdControlsWidget::showResult(const QString& message)
{
    QMessageBox dialog(QMessageBox::Information,
                       tr("MPD"),
                       message,
                       QMessageBox::Ok,
                       this);
    dialog.setDefaultButton(QMessageBox::Ok);
    dialog.setEscapeButton(QMessageBox::Ok);
    dialog.exec();
}

QString MpdControlsWidget::name() const
{
    return tr("MPD controls");
}

QString MpdControlsWidget::layoutName() const
{
    return QStringLiteral("MPDControls");
}
